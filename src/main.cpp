// Lask Manager - Details tab: open instantly, list processes, end the selected one.
//
// Thread model (see docs/design.md): the UI thread owns the window and only ever reads the newest
// snapshot; the sampler thread does the one kernel query per tick; the enrichment thread fills in
// per-process facts for the rows on screen; kills run on a throwaway worker.

#include "facts.h"
#include "snapshot.h"
#include "theme.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr UINT kMsgSnapshot = WM_APP + 1;
constexpr UINT kMsgKillDone = WM_APP + 2;
constexpr UINT kMsgFacts = WM_APP + 3;
constexpr UINT_PTR kTimerDeferredAdopt = 1;
constexpr int kListId = 100;
constexpr int kFilterId = 101;
constexpr unsigned kDefaultIntervalMs = 1000;
constexpr int kLookahead = 16;   // rows above and below the visible page whose facts are fetched too
constexpr uint32_t kEvictEvery = 16; // ticks between sweeps of the UI-side fact cache

enum Command : int { kCmdEndTask = 1001, kCmdRefresh, kCmdRestartAdmin, kCmdFind, kCmdColumnBase = 1100 };

// Every column the Details grid can show. App::columns maps the ListView's sub-item indices to these;
// Name is always sub-item 0, the item column, and cannot be hidden.
enum class Column : int {
    Name,
    Pid,
    Status,
    User,
    Cpu,
    Memory,
    Description,
    CommandLine,
    ImagePath,
    Elevated,
    Architecture,
    Threads,
    Handles,
    Priority,
    Count
};

struct ColumnSpec {
    const wchar_t* title;
    int width;     // at 96 dpi
    bool numeric;  // right-aligned, biggest first on the first click
    bool visible;  // shown by default
};

constexpr ColumnSpec kColumns[] = {
    {L"Name", 220, false, true},          {L"PID", 64, true, true},
    {L"Status", 84, false, true},         {L"User name", 110, false, true},
    {L"CPU", 60, true, true},             {L"Memory", 96, true, true},
    {L"Description", 240, false, true},   {L"Command line", 360, false, false},
    {L"Image path", 300, false, false},   {L"Elevated", 72, false, false},
    {L"Architecture", 92, false, false},  {L"Threads", 70, true, false},
    {L"Handles", 76, true, false},        {L"Priority", 92, false, false},
};
static_assert(std::size(kColumns) == static_cast<size_t>(Column::Count));

constexpr int Index(Column c) {
    return static_cast<int>(c);
}

// UI-side copy of what the enrichment thread found for one process identity.
struct FactEntry {
    std::shared_ptr<const lask::ProcessFacts> facts;
    int image = 0;         // index into App::images; 0 is the generic application icon
    uint32_t lastSeen = 0; // App::tick when the process was last in a snapshot
};

struct ViewRow {
    const lask::ProcessRow* row; // points into App::current, rebuilt whenever it changes
    double cpu;                  // percent of all logical CPUs; < 0 until two snapshots exist
    const FactEntry* entry;      // null until the enrichment thread has reported on this process
};

struct KillResult {
    uint32_t pid;
    int64_t createTime;
    DWORD error;
};

struct App {
    HWND hwnd = nullptr;
    HWND list = nullptr;
    HWND filter = nullptr;
    HFONT font = nullptr;
    int fontHeight = 16;
    HBRUSH background = nullptr;
    HBRUSH surface = nullptr;
    HIMAGELIST images = nullptr;
    int iconSize = 16;
    UINT dpi = 96;
    bool elevated = false;
    lask::Theme theme;
    lask::Sampler sampler;
    lask::Enricher enricher;
    std::shared_ptr<const lask::Snapshot> current;
    std::shared_ptr<const lask::Snapshot> previous;
    std::vector<ViewRow> view;
    std::unordered_map<lask::Identity, FactEntry, lask::IdentityHash> facts;
    std::unordered_map<std::wstring, int> iconByPath;
    uint32_t tick = 0;
    int wantedTop = -1;
    Column sortColumn = Column::Cpu;
    bool sortDescending = true;
    std::vector<Column> columns; // by ListView sub-item index; display order is the ListView's own
    bool columnVisible[Index(Column::Count)] = {};
    int columnWidth[Index(Column::Count)] = {}; // at 96 dpi, remembered while a column is hidden
    bool menuOpen = false;
    wchar_t filterText[128] = {};
    wchar_t text[160] = {}; // LVN_GETDISPINFO scratch; the control copies it before the next request
};

App g;

int Scale(int px) {
    return MulDiv(px, static_cast<int>(g.dpi), 96);
}

template <class T>
int Compare(const T& a, const T& b) {
    return a < b ? -1 : (b < a ? 1 : 0);
}

void FillSolid(HDC dc, const RECT& rc, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

// ---- cell values -------------------------------------------------------------------------------

Column ColumnAt(int subItem) {
    return subItem >= 0 && subItem < static_cast<int>(g.columns.size()) ? g.columns[subItem] : Column::Count;
}

const lask::ProcessFacts* FactsOf(const ViewRow& v) {
    return v.entry ? v.entry->facts.get() : nullptr;
}

const wchar_t* MachineName(USHORT machine) {
    switch (machine) {
    case IMAGE_FILE_MACHINE_AMD64: return L"x64";
    case IMAGE_FILE_MACHINE_I386: return L"x86";
    case IMAGE_FILE_MACHINE_ARM64: return L"ARM64";
    case IMAGE_FILE_MACHINE_ARMNT: return L"ARM";
    default: return L"";
    }
}

const wchar_t* PriorityName(int32_t basePriority) {
    switch (basePriority) {
    case 4: return L"Idle";
    case 6: return L"Below normal";
    case 8: return L"Normal";
    case 10: return L"Above normal";
    case 13: return L"High";
    case 24: return L"Realtime";
    default: return L"";
    }
}

// Text of a non-numeric column. The pointer stays valid until the view or the fact cache changes,
// which only happens on this thread between paints.
const wchar_t* StringValue(const ViewRow& v, Column c) {
    const lask::ProcessFacts* f = FactsOf(v);
    switch (c) {
    case Column::Name: return v.row->name;
    case Column::Status: return v.row->suspended ? L"Suspended" : L"Running";
    case Column::User: return f ? f->user.c_str() : L"";
    case Column::Description: return f ? f->description.c_str() : L"";
    case Column::CommandLine: return f ? f->commandLine.c_str() : L"";
    case Column::ImagePath: return f ? f->path.c_str() : L"";
    case Column::Elevated: return !f || f->elevated < 0 ? L"" : (f->elevated ? L"Yes" : L"No");
    case Column::Architecture: return f ? MachineName(f->machine) : L"";
    case Column::Priority: return v.row->pid ? PriorityName(v.row->basePriority) : L"";
    default: return L"";
    }
}

int CompareRows(const ViewRow& a, const ViewRow& b, Column c) {
    switch (c) {
    case Column::Pid: return Compare(a.row->pid, b.row->pid);
    case Column::Cpu: return Compare(a.cpu, b.cpu);
    case Column::Memory: return Compare(a.row->privateWorkingSet, b.row->privateWorkingSet);
    case Column::Threads: return Compare(a.row->threadCount, b.row->threadCount);
    case Column::Handles: return Compare(a.row->handleCount, b.row->handleCount);
    case Column::Priority: return Compare(a.row->basePriority, b.row->basePriority);
    default: return _wcsicmp(StringValue(a, c), StringValue(b, c));
    }
}

bool Contains(const wchar_t* haystack, const wchar_t* needle) {
    return haystack && *haystack && FindStringOrdinal(FIND_FROMSTART, haystack, -1, needle, -1, TRUE) >= 0;
}

// The filter matches what the user can see: name, PID, and any fact already fetched. A process
// whose description has never been on screen matches only by name until it has been.
bool MatchesFilter(const ViewRow& v) {
    const wchar_t* needle = g.filterText;
    if (!needle[0]) return true;
    if (Contains(v.row->name, needle)) return true;
    wchar_t pid[16];
    swprintf_s(pid, L"%u", v.row->pid);
    if (Contains(pid, needle)) return true;
    const lask::ProcessFacts* f = FactsOf(v);
    return f && (Contains(f->description.c_str(), needle) || Contains(f->user.c_str(), needle) ||
                 Contains(f->path.c_str(), needle) || Contains(f->commandLine.c_str(), needle));
}

// ---- view: the sorted, filtered, rate-annotated list the grid shows --------------------------

void SortView() {
    const Column column = g.sortColumn;
    const bool descending = g.sortDescending;
    std::sort(g.view.begin(), g.view.end(), [column, descending](const ViewRow& a, const ViewRow& b) {
        int c = CompareRows(a, b, column);
        if (c == 0) c = Compare(a.row->pid, b.row->pid); // deterministic order for ties
        return descending ? c > 0 : c < 0;
    });
}

FactEntry* FindFacts(const lask::ProcessRow& r) {
    auto it = g.facts.find({r.pid, r.createTime});
    return it == g.facts.end() ? nullptr : &it->second;
}

void RebuildView() {
    g.view.clear();
    if (!g.current) return;
    const lask::Snapshot& cur = *g.current;
    ++g.tick;

    std::unordered_map<uint32_t, const lask::ProcessRow*> previousByPid;
    double denominator = 0.0;
    if (g.previous) {
        const uint64_t elapsed = cur.elapsed100ns(*g.previous);
        if (elapsed) denominator = static_cast<double>(elapsed) * cur.logicalCpus;
        previousByPid.reserve(g.previous->rows.size());
        for (const auto& r : g.previous->rows) previousByPid.emplace(r.pid, &r);
    }

    g.view.reserve(cur.rows.size());
    for (const auto& r : cur.rows) {
        double cpu = -1.0;
        if (denominator > 0.0) {
            if (auto it = previousByPid.find(r.pid); it != previousByPid.end() && it->second->createTime == r.createTime) {
                const uint64_t delta = r.cpuTime >= it->second->cpuTime ? r.cpuTime - it->second->cpuTime : 0;
                cpu = std::clamp(100.0 * static_cast<double>(delta) / denominator, 0.0, 100.0);
            }
        }
        FactEntry* entry = FindFacts(r);
        if (entry) entry->lastSeen = g.tick;
        const ViewRow v{&r, cpu, entry};
        if (MatchesFilter(v)) g.view.push_back(v);
    }

    // Facts of processes that have exited. Every live one was just stamped, filtered out or not,
    // so nothing erased here is referenced by the view.
    if (g.tick % kEvictEvery == 0)
        std::erase_if(g.facts, [](const auto& kv) { return kv.second.lastSeen != g.tick; });
    SortView();
}

std::optional<lask::Identity> SelectedIdentity() {
    const int i = ListView_GetNextItem(g.list, -1, LVNI_SELECTED);
    if (i < 0 || i >= static_cast<int>(g.view.size())) return std::nullopt;
    return lask::Identity{g.view[i].row->pid, g.view[i].row->createTime};
}

int IndexOf(const lask::Identity& id) {
    for (size_t i = 0; i < g.view.size(); ++i)
        if (g.view[i].row->pid == id.pid && g.view[i].row->createTime == id.createTime) return static_cast<int>(i);
    return -1;
}

void RestoreSelection(const std::optional<lask::Identity>& selected) {
    if (!selected) return;
    const int idx = IndexOf(*selected);
    ListView_SetItemState(g.list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    if (idx >= 0) ListView_SetItemState(g.list, idx, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

void RedrawVisibleRows() {
    const int top = ListView_GetTopIndex(g.list);
    const int page = ListView_GetCountPerPage(g.list);
    ListView_RedrawItems(g.list, top, top + page);
}

// Tells the enrichment thread which processes are on screen: the visible page first, then a few
// rows of lookahead below and above, so scrolling a little finds the facts already there.
void PublishWanted() {
    if (!g.enricher.running() || g.view.empty()) return;
    const int count = static_cast<int>(g.view.size());
    const int top = std::clamp(ListView_GetTopIndex(g.list), 0, count - 1);
    const int end = std::min(count, top + ListView_GetCountPerPage(g.list) + 1);
    g.wantedTop = top;

    std::vector<lask::Identity> ids;
    ids.reserve(static_cast<size_t>(end - top + 2 * kLookahead));
    auto add = [&ids](int i) { ids.push_back({g.view[i].row->pid, g.view[i].row->createTime}); };
    for (int i = top; i < end; ++i) add(i);
    for (int i = end; i < std::min(count, end + kLookahead); ++i) add(i);
    for (int i = top - 1; i >= std::max(0, top - kLookahead); --i) add(i);

    const uint32_t kinds = lask::kFactBasic | (g.columnVisible[Index(Column::CommandLine)] ? lask::kFactCommandLine : 0u);
    g.enricher.want(std::move(ids), kinds, g.iconSize);
}

void PublishWantedIfScrolled() {
    if (ListView_GetTopIndex(g.list) != g.wantedTop) PublishWanted();
}

// Only the visible rows are invalidated; the control asks for their text through LVN_GETDISPINFO.
void ApplyView() {
    const auto selected = SelectedIdentity();
    RebuildView();
    ListView_SetItemCountEx(g.list, static_cast<int>(g.view.size()), LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
    RestoreSelection(selected);
    RedrawVisibleRows();
    PublishWanted();
}

bool IsInteracting() {
    return g.menuOpen || (GetKeyState(VK_LBUTTON) & 0x8000) || (GetKeyState(VK_RBUTTON) & 0x8000);
}

void AdoptLatestSnapshot() {
    auto latest = g.sampler.latest();
    if (!latest || latest == g.current) return;
    g.previous = std::move(g.current);
    g.current = std::move(latest);
    // Enrichment starts with the second tick, so icons and descriptions never delay the list.
    if (g.previous && !g.enricher.running()) g.enricher.start(g.hwnd, kMsgFacts);
    ApplyView();
}

// A snapshot that lands while the mouse is down or a menu is open waits: the row under the cursor
// must not move.
void OnSnapshot() {
    if (IsInteracting()) {
        SetTimer(g.hwnd, kTimerDeferredAdopt, 100, nullptr);
        return;
    }
    AdoptLatestSnapshot();
}

// ---- facts and icons -----------------------------------------------------------------------------

void CreateImageList() {
    const int size = GetSystemMetricsForDpi(SM_CXSMICON, g.dpi);
    if (g.images && size == g.iconSize) return; // the worker only resends icons when the size changes
    g.iconSize = size;
    HIMAGELIST images = ImageList_Create(g.iconSize, g.iconSize, ILC_COLOR32 | ILC_MASK, 64, 64);
    auto generic = static_cast<HICON>(
        LoadImageW(nullptr, IDI_APPLICATION, IMAGE_ICON, g.iconSize, g.iconSize, LR_SHARED));
    ImageList_AddIcon(images, generic); // index 0: processes without an icon, or not fetched yet
    ListView_SetImageList(g.list, images, LVSIL_SMALL);
    if (g.images) ImageList_Destroy(g.images);
    g.images = images;

    // Every icon has to come again at the new size; the enrichment thread notices the size change.
    g.iconByPath.clear();
    for (auto& [id, entry] : g.facts) entry.image = 0;
}

void OnFacts(lask::FactsBatch* batch) {
    for (const lask::FactsUpdate& u : *batch) {
        const std::wstring& path = u.facts->path;
        if (u.icon && !g.iconByPath.contains(path)) {
            const int image = ImageList_AddIcon(g.images, u.icon);
            if (image >= 0) {
                g.iconByPath.emplace(path, image);
                for (auto& [id, entry] : g.facts)
                    if (entry.facts && entry.facts->path == path) entry.image = image;
            }
        }
        FactEntry& entry = g.facts[u.id];
        entry.facts = u.facts;
        entry.lastSeen = g.tick;
        const auto icon = g.iconByPath.find(path);
        entry.image = icon == g.iconByPath.end() ? 0 : icon->second;
    }
    lask::DestroyBatch(batch); // the image list keeps its own copies of the icons

    // unordered_map never moves its elements, so rows that already point at an entry stay valid.
    for (ViewRow& v : g.view)
        if (!v.entry) v.entry = FindFacts(*v.row);
    RedrawVisibleRows();
}

// ---- grid -------------------------------------------------------------------------------------

void FormatMemory(uint64_t bytes, wchar_t* out, size_t count) {
    swprintf_s(out, count, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

void OnGetDispInfo(NMLVDISPINFOW* info) {
    LVITEMW& item = info->item;
    if (item.iItem < 0 || item.iItem >= static_cast<int>(g.view.size())) {
        if (item.mask & LVIF_TEXT) item.pszText = const_cast<wchar_t*>(L"");
        return;
    }
    const ViewRow& v = g.view[item.iItem];
    if (item.mask & LVIF_IMAGE) item.iImage = v.entry ? v.entry->image : 0;
    if (!(item.mask & LVIF_TEXT)) return;

    wchar_t* out = g.text;
    const size_t n = std::size(g.text);
    const Column column = ColumnAt(item.iSubItem);
    switch (column) {
    case Column::Pid: swprintf_s(out, n, L"%u", v.row->pid); break;
    case Column::Cpu:
        if (v.cpu < 0) wcscpy_s(out, n, L"—");
        else swprintf_s(out, n, L"%.1f%%", v.cpu);
        break;
    case Column::Memory: FormatMemory(v.row->privateWorkingSet, out, n); break;
    case Column::Threads: swprintf_s(out, n, L"%u", v.row->threadCount); break;
    case Column::Handles: swprintf_s(out, n, L"%u", v.row->handleCount); break;
    default: item.pszText = const_cast<wchar_t*>(StringValue(v, column)); return;
    }
    item.pszText = out;
}

// Type-ahead: jump to the next process whose name starts with what was typed.
LRESULT OnFindItem(const NMLVFINDITEMW* find) {
    if (!(find->lvfi.flags & LVFI_STRING) || !find->lvfi.psz || g.view.empty()) return -1;
    const size_t len = wcslen(find->lvfi.psz);
    const int count = static_cast<int>(g.view.size());
    int i = find->iStart < 0 ? 0 : find->iStart % count;
    for (int n = 0; n < count; ++n, i = (i + 1) % count)
        if (_wcsnicmp(g.view[i].row->name, find->lvfi.psz, len) == 0) return i;
    return -1;
}

// Selected rows get the accent-tinted palette color instead of the theme's grey highlight.
LRESULT DrawListItem(NMLVCUSTOMDRAW* cd) {
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: {
        const int i = static_cast<int>(cd->nmcd.dwItemSpec);
        if (!(ListView_GetItemState(g.list, i, LVIS_SELECTED) & LVIS_SELECTED)) return CDRF_DODEFAULT;
        const bool hot = (cd->nmcd.uItemState & CDIS_HOT) != 0;
        const COLORREF fill = GetFocus() != g.list ? g.theme.selectionInactive
                              : hot                ? g.theme.selectionHot
                                                   : g.theme.selection;
        // With the selected state cleared the theme draws a plain row on top of our fill.
        cd->nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_HOT | CDIS_FOCUS);
        cd->clrTextBk = fill;
        cd->clrText = g.theme.selectionText;
        return CDRF_NEWFONT;
    }
    default: return CDRF_DODEFAULT;
    }
}

int ColumnCount() {
    return static_cast<int>(g.columns.size());
}

void UpdateSortArrows() {
    HWND header = ListView_GetHeader(g.list);
    for (int i = 0, n = Header_GetItemCount(header); i < n; ++i) {
        HDITEMW hi{};
        hi.mask = HDI_FORMAT;
        Header_GetItem(header, i, &hi);
        hi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (ColumnAt(i) == g.sortColumn) hi.fmt |= g.sortDescending ? HDF_SORTDOWN : HDF_SORTUP;
        Header_SetItem(header, i, &hi);
    }
}

void Resort() {
    UpdateSortArrows();
    const auto selected = SelectedIdentity();
    SortView();
    RestoreSelection(selected);
    RedrawVisibleRows();
    PublishWanted();
}

void OnColumnClick(int index) {
    const Column c = ColumnAt(index);
    if (c == Column::Count) return;
    if (c == g.sortColumn) {
        g.sortDescending = !g.sortDescending;
    } else {
        g.sortColumn = c;
        g.sortDescending = kColumns[Index(c)].numeric; // biggest first for numbers, A-Z for text
    }
    Resort();
}

void AppendColumn(Column column) {
    const ColumnSpec& spec = kColumns[Index(column)];
    LVCOLUMNW c{};
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT | LVCF_SUBITEM;
    c.pszText = const_cast<wchar_t*>(spec.title);
    c.cx = Scale(g.columnWidth[Index(column)]);
    c.fmt = spec.numeric ? LVCFMT_RIGHT : LVCFMT_LEFT;
    c.iSubItem = ColumnCount();
    ListView_InsertColumn(g.list, c.iSubItem, &c);
    g.columns.push_back(column);
}

// The ListView wants its sub-item indices dense (a sub-item past the column count blanks the whole
// grid), so a toggle rebuilds every column but Name, keeping the user's order and widths.
void ToggleColumn(Column column) {
    if (column == Column::Name || column == Column::Count) return;
    const int n = ColumnCount();
    std::vector<int> order(static_cast<size_t>(n));
    ListView_GetColumnOrderArray(g.list, n, order.data());
    std::vector<Column> shown; // display order
    for (int i : order) {
        const Column c = g.columns[i];
        g.columnWidth[Index(c)] = MulDiv(ListView_GetColumnWidth(g.list, i), 96, static_cast<int>(g.dpi));
        if (c != column) shown.push_back(c);
    }
    const bool show = !g.columnVisible[Index(column)];
    if (show) shown.push_back(column);
    g.columnVisible[Index(column)] = show;

    for (int i = n - 1; i > 0; --i) ListView_DeleteColumn(g.list, i);
    g.columns.resize(1);
    for (Column c : shown)
        if (c != Column::Name) AppendColumn(c);
    std::vector<int> newOrder;
    for (Column c : shown)
        newOrder.push_back(static_cast<int>(std::find(g.columns.begin(), g.columns.end(), c) - g.columns.begin()));
    ListView_SetColumnOrderArray(g.list, static_cast<int>(newOrder.size()), newOrder.data());

    if (!show && g.sortColumn == column) {
        g.sortColumn = Column::Name;
        g.sortDescending = false;
    }
    Resort(); // also republishes the wanted set: showing "Command line" adds a fact kind
    InvalidateRect(g.list, nullptr, TRUE);
}

void ShowColumnMenu() {
    HMENU menu = CreatePopupMenu();
    for (int i = 1; i < Index(Column::Count); ++i)
        AppendMenuW(menu, MF_STRING | (g.columnVisible[i] ? MF_CHECKED : 0), kCmdColumnBase + i, kColumns[i].title);
    POINT pt{};
    GetCursorPos(&pt);
    g.menuOpen = true;
    TrackPopupMenuEx(menu, TPM_RIGHTBUTTON, pt.x, pt.y, g.hwnd, nullptr); // picks arrive as WM_COMMAND
    g.menuOpen = false;
    DestroyMenu(menu);
}

// The header control is the one piece the DarkMode_Explorer theme leaves light, so draw it ourselves
// through its documented NM_CUSTOMDRAW. The header's parent is the ListView, hence the subclass.
void FillHeaderRect(HDC dc, const RECT& rc, COLORREF fill) {
    FillSolid(dc, rc, fill);
    FillSolid(dc, {rc.left, rc.bottom - 1, rc.right, rc.bottom}, g.theme.border);
}

LRESULT DrawHeader(NMCUSTOMDRAW* cd) {
    switch (cd->dwDrawStage) {
    case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
    case CDDS_POSTPAINT: {
        // The theme leaves the filler right of the last column light; paint it to match.
        HWND header = cd->hdr.hwndFrom;
        RECT client{};
        GetClientRect(header, &client);
        LONG left = client.left;
        for (int i = 0, n = Header_GetItemCount(header); i < n; ++i) {
            RECT item{};
            if (Header_GetItemRect(header, i, &item)) left = std::max(left, item.right);
        }
        if (left < client.right)
            FillHeaderRect(cd->hdc, {left, client.top, client.right, client.bottom}, g.theme.headerBackground);
        return CDRF_DODEFAULT;
    }
    case CDDS_ITEMPREPAINT: {
        HDC dc = cd->hdc;
        const RECT rc = cd->rc;

        wchar_t title[64] = {};
        HDITEMW hi{};
        hi.mask = HDI_TEXT | HDI_FORMAT;
        hi.pszText = title;
        hi.cchTextMax = static_cast<int>(std::size(title));
        Header_GetItem(cd->hdr.hwndFrom, static_cast<int>(cd->dwItemSpec), &hi);
        const bool sorted = (hi.fmt & (HDF_SORTUP | HDF_SORTDOWN)) != 0;

        const COLORREF fill = (cd->uItemState & CDIS_SELECTED) ? g.theme.headerPressed
                              : (cd->uItemState & CDIS_HOT)    ? g.theme.headerHot
                                                               : g.theme.headerBackground;
        FillHeaderRect(dc, rc, fill);
        if (sorted) FillSolid(dc, {rc.left + Scale(1), rc.bottom - Scale(2), rc.right - Scale(1), rc.bottom}, g.theme.accentText);

        HGDIOBJ oldFont = SelectObject(dc, g.font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, sorted ? g.theme.text : g.theme.textSecondary);
        RECT textRect = rc;
        textRect.left += Scale(6);
        textRect.right -= Scale(6);
        const UINT format = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX |
                            ((hi.fmt & HDF_RIGHT) ? DT_RIGHT : DT_LEFT);
        DrawTextW(dc, title, -1, &textRect, format);

        if (sorted) {
            const int cx = (rc.left + rc.right) / 2;
            const int top = rc.top + Scale(2);
            const int s = Scale(3);
            POINT up[3] = {{cx - s, top + s}, {cx + s, top + s}, {cx, top}};
            POINT down[3] = {{cx - s, top}, {cx + s, top}, {cx, top + s}};
            SetDCBrushColor(dc, g.theme.accentText);
            SetDCPenColor(dc, g.theme.accentText);
            HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
            HGDIOBJ oldPen = SelectObject(dc, GetStockObject(DC_PEN));
            Polygon(dc, (hi.fmt & HDF_SORTUP) ? up : down, 3);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
        }
        SelectObject(dc, oldFont);
        return CDRF_SKIPDEFAULT;
    }
    default: return CDRF_DODEFAULT;
    }
}

LRESULT CALLBACK ListSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_NOTIFY) {
        auto* h = reinterpret_cast<NMHDR*>(lp);
        if (h->hwndFrom == ListView_GetHeader(hwnd)) {
            if (h->code == NM_CUSTOMDRAW) return DrawHeader(reinterpret_cast<NMCUSTOMDRAW*>(lp));
            if (h->code == NM_RCLICK) {
                ShowColumnMenu();
                return TRUE;
            }
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK HeaderSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_ERASEBKGND) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillHeaderRect(reinterpret_cast<HDC>(wp), rc, g.theme.headerBackground);
        return 1;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void FocusList() {
    SetFocus(g.list);
    if (!g.view.empty() && ListView_GetSelectedCount(g.list) == 0)
        ListView_SetItemState(g.list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

// Esc clears the filter (then leaves it); Down, Enter and Tab hand the keyboard to the grid.
LRESULT CALLBACK FilterSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) {
            if (GetWindowTextLengthW(hwnd) > 0) SetWindowTextW(hwnd, L"");
            else FocusList();
            return 0;
        }
        if (wp == VK_DOWN || wp == VK_RETURN || wp == VK_TAB) {
            FocusList();
            return 0;
        }
    } else if (msg == WM_CHAR && (wp == VK_ESCAPE || wp == VK_RETURN || wp == VK_TAB)) {
        return 0; // already handled on key down; this only stops the beep
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void OnFilterChanged() {
    GetWindowTextW(g.filter, g.filterText, static_cast<int>(std::size(g.filterText)));
    ApplyView();
}

void Layout() {
    RECT rc{};
    GetClientRect(g.hwnd, &rc);
    const int pad = Scale(8);
    const int editHeight = g.fontHeight + Scale(10);
    const int bar = editHeight + 2 * pad;
    SetWindowPos(g.filter, nullptr, pad, pad, std::max(0, std::min(Scale(360), static_cast<int>(rc.right) - 2 * pad)),
                 editHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(g.list, nullptr, 0, bar, rc.right, std::max(0, static_cast<int>(rc.bottom) - bar),
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void RecreateFont() {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, g.dpi);
    HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);

    HDC dc = GetDC(g.hwnd);
    HGDIOBJ old = SelectObject(dc, font);
    TEXTMETRICW tm{};
    if (GetTextMetricsW(dc, &tm)) g.fontHeight = tm.tmHeight;
    SelectObject(dc, old);
    ReleaseDC(g.hwnd, dc);

    for (HWND child : {g.list, g.filter})
        if (child) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    if (g.filter) SendMessageW(g.filter, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(Scale(4), Scale(4)));
    if (g.font) DeleteObject(g.font);
    g.font = font;
}

void ReplaceBrush(HBRUSH& brush, COLORREF color) {
    HBRUSH old = brush;
    brush = CreateSolidBrush(color);
    if (old) DeleteObject(old);
}

void ApplyTheme() {
    g.theme = lask::ReadTheme();
    lask::SetAppDarkMode(g.theme.dark);
    lask::ApplyWindowTheme(g.hwnd, g.theme);

    // ItemsView is the theme Explorer's own list uses: dark scrollbars and hover. No visual styles at
    // all in high contrast, where the system colors must win.
    const wchar_t* listTheme = g.theme.highContrast ? L"" : (g.theme.dark ? L"DarkMode_ItemsView" : L"ItemsView");
    lask::AllowDarkModeForWindow(g.list, g.theme.dark);
    SetWindowTheme(g.list, listTheme, nullptr);
    ListView_SetBkColor(g.list, g.theme.surface);
    ListView_SetTextBkColor(g.list, g.theme.surface);
    ListView_SetTextColor(g.list, g.theme.text);

    lask::AllowDarkModeForWindow(g.filter, g.theme.dark);
    SetWindowTheme(g.filter, g.theme.dark ? L"DarkMode_CFD" : L"CFD", nullptr);

    ReplaceBrush(g.surface, g.theme.surface);
    ReplaceBrush(g.background, g.theme.background);
    SetClassLongPtrW(g.hwnd, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(g.background));

    RedrawWindow(g.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

void CreateControls(HWND parent) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    g.filter = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0,
                               0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFilterId)), instance,
                               nullptr);
    Edit_SetCueBannerTextFocused(g.filter, L"Filter: name, PID, user, description  (Ctrl+F)", TRUE);
    SetWindowSubclass(g.filter, FilterSubclassProc, 1, 0);

    g.list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL |
                                 LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS,
                             0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListId)), instance,
                             nullptr);
    ListView_SetExtendedListViewStyle(g.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP |
                                                  LVS_EX_LABELTIP);
    for (int i = 0; i < Index(Column::Count); ++i) {
        g.columnWidth[i] = kColumns[i].width;
        g.columnVisible[i] = kColumns[i].visible;
        if (g.columnVisible[i]) AppendColumn(static_cast<Column>(i));
    }
    HWND header = ListView_GetHeader(g.list);
    SetWindowLongPtrW(header, GWL_STYLE, GetWindowLongPtrW(header, GWL_STYLE) | HDS_HOTTRACK);
    SetWindowSubclass(g.list, ListSubclassProc, 1, 0);
    SetWindowSubclass(header, HeaderSubclassProc, 1, 0);
    CreateImageList();
    RecreateFont();
    ApplyTheme();
    UpdateSortArrows();
}

void OnDpiChanged(UINT dpi, const RECT& suggested) {
    const UINT old = g.dpi;
    g.dpi = dpi;
    for (int i = 0, n = ColumnCount(); i < n; ++i)
        ListView_SetColumnWidth(g.list, i, MulDiv(ListView_GetColumnWidth(g.list, i), static_cast<int>(dpi),
                                                  static_cast<int>(old)));
    RecreateFont();
    CreateImageList();
    SetWindowPos(g.hwnd, nullptr, suggested.left, suggested.top, suggested.right - suggested.left,
                 suggested.bottom - suggested.top, SWP_NOZORDER | SWP_NOACTIVATE);
    PublishWanted(); // carries the new icon size
}

// ---- actions ---------------------------------------------------------------------------------

void RestartElevated(uint32_t killPid, int64_t killCreateTime) {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t args[96] = {};
    if (killPid) swprintf_s(args, L"--kill %u --at %lld", killPid, static_cast<long long>(killCreateTime));

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = args;
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) PostMessageW(g.hwnd, WM_CLOSE, 0, 0); // hand over to the elevated instance
}

void EndSelectedTask() {
    const auto selected = SelectedIdentity();
    if (!selected) return;
    if (selected->pid == 0 || selected->pid == 4) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    const lask::Identity id = *selected;
    HWND hwnd = g.hwnd;
    std::thread([id, hwnd] {
        auto* result = new KillResult{id.pid, id.createTime, lask::TerminateProcessByIdentity(id.pid, id.createTime)};
        if (!PostMessageW(hwnd, kMsgKillDone, 0, reinterpret_cast<LPARAM>(result))) delete result;
    }).detach();
}

void OnKillDone(KillResult* result) {
    std::unique_ptr<KillResult> r(result);
    if (r->error == ERROR_SUCCESS || r->error == ERROR_NOT_FOUND) {
        g.sampler.requestNow(); // the row disappears with the next snapshot
        return;
    }
    if (r->error == ERROR_ACCESS_DENIED && !g.elevated) {
        const int choice = MessageBoxW(g.hwnd,
                                       L"Windows denied access to this process.\n\n"
                                       L"Restart Lask Manager as administrator and end it from there?",
                                       L"Lask Manager", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON1);
        if (choice == IDYES) RestartElevated(r->pid, r->createTime);
        return;
    }
    wchar_t reason[256] = {};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, r->error, 0, reason,
                   static_cast<DWORD>(std::size(reason)), nullptr);
    wchar_t message[400] = {};
    swprintf_s(message, L"Could not end process %u.\n\n%s", r->pid, reason);
    MessageBoxW(g.hwnd, message, L"Lask Manager", MB_OK | MB_ICONERROR);
}

void ShowContextMenu(LPARAM lp) {
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    if (pt.x == -1 && pt.y == -1) { // keyboard: anchor on the selected row
        const int i = ListView_GetNextItem(g.list, -1, LVNI_SELECTED);
        RECT rc{};
        if (i >= 0 && ListView_GetItemRect(g.list, i, &rc, LVIR_LABEL)) pt = {rc.left + Scale(8), rc.bottom};
        ClientToScreen(g.list, &pt);
    }
    const bool hasSelection = ListView_GetSelectedCount(g.list) > 0;

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (hasSelection ? 0 : MF_GRAYED), kCmdEndTask, L"&End task\tDel");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdRefresh, L"&Refresh now\tF5");
    AppendMenuW(menu, MF_STRING, kCmdFind, L"&Filter\tCtrl+F");
    if (!g.elevated) AppendMenuW(menu, MF_STRING, kCmdRestartAdmin, L"Restart as &administrator");

    g.menuOpen = true;
    TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, g.hwnd, nullptr);
    g.menuOpen = false;
    DestroyMenu(menu);
}

// ---- window ----------------------------------------------------------------------------------

LRESULT OnNotify(NMHDR* h) {
    if (h->idFrom != kListId) return 0;
    switch (h->code) {
    case LVN_GETDISPINFOW: OnGetDispInfo(reinterpret_cast<NMLVDISPINFOW*>(h)); return 0;
    case LVN_ODFINDITEMW: return OnFindItem(reinterpret_cast<NMLVFINDITEMW*>(h));
    case LVN_ODCACHEHINT:
    case LVN_ENDSCROLL: PublishWantedIfScrolled(); return 0;
    case LVN_COLUMNCLICK: OnColumnClick(reinterpret_cast<NMLISTVIEW*>(h)->iSubItem); return 0;
    case NM_CUSTOMDRAW: return DrawListItem(reinterpret_cast<NMLVCUSTOMDRAW*>(h));
    case LVN_KEYDOWN:
        switch (reinterpret_cast<NMLVKEYDOWN*>(h)->wVKey) {
        case VK_DELETE: EndSelectedTask(); break;
        case VK_TAB: SetFocus(g.filter); break;
        case VK_ESCAPE:
            if (g.filterText[0]) SetWindowTextW(g.filter, L""); // first Esc drops the filter
            else PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
            break;
        default: break;
        }
        return 0;
    default: return 0;
    }
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g.hwnd = hwnd;
        CreateControls(hwnd);
        return 0;
    case WM_SIZE:
        if (g.list) Layout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize = {Scale(480), Scale(320)};
        return 0;
    }
    case WM_SETFOCUS: SetFocus(g.list); return 0;
    case WM_NOTIFY: return OnNotify(reinterpret_cast<NMHDR*>(lp));
    case WM_COMMAND:
        if (LOWORD(wp) == kFilterId) {
            if (HIWORD(wp) == EN_CHANGE) OnFilterChanged();
            return 0;
        }
        switch (LOWORD(wp)) {
        case kCmdEndTask: EndSelectedTask(); break;
        case kCmdRefresh: g.sampler.requestNow(); break;
        case kCmdFind:
            SetFocus(g.filter);
            Edit_SetSel(g.filter, 0, -1);
            break;
        case kCmdRestartAdmin: RestartElevated(0, 0); break;
        default:
            if (LOWORD(wp) > kCmdColumnBase && LOWORD(wp) < kCmdColumnBase + Index(Column::Count))
                ToggleColumn(static_cast<Column>(LOWORD(wp) - kCmdColumnBase));
            break;
        }
        return 0;
    case WM_CTLCOLOREDIT:
        if (reinterpret_cast<HWND>(lp) == g.filter) {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, g.theme.text);
            SetBkColor(dc, g.theme.surface);
            return reinterpret_cast<LRESULT>(g.surface);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_CONTEXTMENU:
        if (reinterpret_cast<HWND>(wp) == g.list) ShowContextMenu(lp);
        else return DefWindowProcW(hwnd, msg, wp, lp);
        return 0;
    case kMsgSnapshot: OnSnapshot(); return 0;
    case kMsgKillDone: OnKillDone(reinterpret_cast<KillResult*>(lp)); return 0;
    case kMsgFacts: OnFacts(reinterpret_cast<lask::FactsBatch*>(lp)); return 0;
    case WM_TIMER:
        if (wp == kTimerDeferredAdopt && !IsInteracting()) {
            KillTimer(hwnd, kTimerDeferredAdopt);
            AdoptLatestSnapshot();
        }
        return 0;
    case WM_SETTINGCHANGE:
        if (lask::IsThemeSettingChange(lp) || wp == SPI_SETHIGHCONTRAST) ApplyTheme();
        if (wp == SPI_SETNONCLIENTMETRICS) {
            RecreateFont();
            Layout();
        }
        return 0;
    case WM_SYSCOLORCHANGE:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        ApplyTheme();
        return 0;
    case WM_DPICHANGED: OnDpiChanged(HIWORD(wp), *reinterpret_cast<const RECT*>(lp)); return 0;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        g.enricher.stop();
        g.sampler.stop();
        PostQuitMessage(0);
        return 0;
    default: return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// ---- startup ---------------------------------------------------------------------------------

// We must stay responsive on a machine that is already thrashing: high priority class, no power
// throttling (hybrid CPUs would park us on E-cores), normal memory priority so our pages stay in.
void BoostOwnPriority() {
    HANDLE process = GetCurrentProcess();
    SetPriorityClass(process, HIGH_PRIORITY_CLASS);

    PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    throttling.StateMask = 0; // control the setting, and set it to "off"
    SetProcessInformation(process, ProcessPowerThrottling, &throttling, sizeof throttling);

    MEMORY_PRIORITY_INFORMATION memoryPriority{MEMORY_PRIORITY_NORMAL};
    SetProcessInformation(process, ProcessMemoryPriority, &memoryPriority, sizeof memoryPriority);

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
}

struct Options {
    uint32_t killPid = 0;
    int64_t killCreateTime = 0;
};

// "--kill <pid> --at <createtime>" is what the elevated relaunch passes. Anything else (such as the
// "taskmgr.exe" argument Windows adds when we are registered through IFEO) is ignored.
Options ParseCommandLine() {
    Options o;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return o;
    for (int i = 1; i + 1 < argc; ++i) {
        if (wcscmp(argv[i], L"--kill") == 0) o.killPid = wcstoul(argv[i + 1], nullptr, 10);
        else if (wcscmp(argv[i], L"--at") == 0) o.killCreateTime = _wcstoi64(argv[i + 1], nullptr, 10);
    }
    LocalFree(argv);
    return o;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    BoostOwnPriority();
    g.elevated = lask::IsElevated();
    if (g.elevated) lask::EnableDebugPrivilege();

    const Options options = ParseCommandLine();
    if (options.killPid) lask::TerminateProcessByIdentity(options.killPid, options.killCreateTime);

    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    g.dpi = GetDpiForSystem();
    g.theme = lask::ReadTheme();
    lask::SetAppDarkMode(g.theme.dark); // before any window exists, so menus pick it up
    g.background = CreateSolidBrush(g.theme.background);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = g.background;
    wc.lpszClassName = L"LaskManager.Main";
    if (!RegisterClassExW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, g.elevated ? L"Lask Manager (Administrator)" : L"Lask Manager",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, Scale(1000), Scale(640), nullptr,
                                nullptr, instance, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, showCommand);
    UpdateWindow(hwnd); // the empty grid is on screen before the first snapshot is even requested

    g.sampler.start(hwnd, kMsgSnapshot, kDefaultIntervalMs);

    const ACCEL accelerators[] = {
        {FVIRTKEY | FCONTROL, 'F', kCmdFind},
        {FVIRTKEY, VK_F5, kCmdRefresh},
    };
    HACCEL accel = CreateAcceleratorTableW(const_cast<ACCEL*>(accelerators), static_cast<int>(std::size(accelerators)));

    MSG m{};
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (TranslateAcceleratorW(hwnd, accel, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    DestroyAcceleratorTable(accel);
    return static_cast<int>(m.wParam);
}
