// Lask Manager - the window: left navigation, the Processes and Details tabs, the filter box, and
// the actions (end task, end tree, open file location).
//
// Thread model (see docs/design.md): the UI thread owns the window and only ever reads the newest
// snapshot; the sampler thread does the one kernel query per tick; the enrichment thread fills in
// per-process facts for the rows on screen; kills and shell calls run on throwaway workers.

#include "facts.h"
#include "history.h"
#include "performance.h"
#include "processes.h"
#include "resource.h"
#include "services.h"
#include "settings.h"
#include "shell.h"
#include "servicestab.h"
#include "snapshot.h"
#include "theme.h"
#include "ui.h"

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr UINT kMsgSnapshot = WM_APP + 1;
constexpr UINT kMsgKillDone = WM_APP + 2;
constexpr UINT kMsgFacts = WM_APP + 3;
constexpr UINT kMsgServiceConfigs = WM_APP + 4;
constexpr UINT kMsgServiceDone = WM_APP + 5;
constexpr UINT kMsgTray = WM_APP + 6;
constexpr int kHotkeyShow = 1;            // Win+Shift+Esc in tray mode (Explorer owns Ctrl+Shift+Esc)
constexpr int kOptionsId = 106;           // the gear button
constexpr int kNavToggleId = 107;         // the hamburger button that collapses the nav to icons
constexpr UINT kPriorityClasses[] = {REALTIME_PRIORITY_CLASS, HIGH_PRIORITY_CLASS, ABOVE_NORMAL_PRIORITY_CLASS,
                                     NORMAL_PRIORITY_CLASS, BELOW_NORMAL_PRIORITY_CLASS, IDLE_PRIORITY_CLASS};
constexpr const wchar_t* kPriorityNames[] = {L"&Realtime", L"&High", L"&Above normal", L"&Normal", L"&Below normal", L"&Low"};
constexpr int kPriorityBase[] = {24, 13, 10, 8, 6, 4}; // the base priority each class gives a process
constexpr UINT_PTR kTimerDeferredAdopt = 1;
constexpr UINT_PTR kTimerNavAnimation = 2;
constexpr int kListId = 100; // the Details grid
constexpr int kFilterId = 101;
constexpr int kProcessesId = 102;
constexpr int kNavId = 103;
constexpr int kPerformanceId = 104;
constexpr int kServicesId = 105;
constexpr unsigned kDefaultIntervalMs = 1000;
constexpr int kLookahead = 16;      // rows above and below the visible page whose facts are fetched too
constexpr uint32_t kSweepEvery = 16; // ticks between sweeps of the UI-side fact cache
constexpr int kNavWidth = 172;       // at 96 dpi
constexpr int kNavCollapsedWidth = 46; // icons only
constexpr int kNavGlyphCenter = 27;    // x of the nav glyphs' centre; the hamburger centres on it too
constexpr double kNavAnimationMs = 167;

enum Command : int {
    kCmdEndTask = 1001,
    kCmdEndTree,
    kCmdOpenLocation,
    kCmdGoToDetails,
    kCmdSwitchTo,
    kCmdRefresh,
    kCmdRestartAdmin,
    kCmdFind,
    kCmdNextTab,
    kCmdTabProcesses,
    kCmdTabPerformance,
    kCmdTabDetails,
    kCmdTabServices,
    kCmdServiceStart,
    kCmdServiceStop,
    kCmdServiceRestart,
    kCmdOpenServices,
    kCmdOptions,
    kCmdOnTop,
    kCmdSpeedHigh,
    kCmdSpeedNormal,
    kCmdSpeedLow,
    kCmdSpeedPaused,
    kCmdHardwareGraphs,
    kCmdTray,
    kCmdAccentHeat,
    kCmdThemeSystem, // in lask::ThemeMode order
    kCmdThemeLight,
    kCmdThemeDark,
    kCmdReplaceTaskManager,
    kCmdShow,
    kCmdExit,
    kCmdPriorityBase = 1080, // + index into kPriorityClasses
    kCmdColumnBase = 1100
};

enum class Tab : int { Processes, Performance, Details, Services, Count }; // navigation order

struct NavItem {
    const wchar_t* glyph; // Segoe Fluent Icons / MDL2 code point
    const wchar_t* label;
};
constexpr NavItem kNav[] = {{L"\xE71D", L"Processes"}, {L"\xE9D9", L"Performance"}, {L"\xE8FD", L"Details"}, {L"\xEA86", L"Services"}};
static_assert(std::size(kNav) == static_cast<size_t>(Tab::Count));

// Every column the Details grid can show. App::columns maps the ListView's sub-item indices to these;
// Name is always sub-item 0, the item column, and cannot be hidden.
enum class Column : int {
    Name,
    Pid,
    Status,
    User,
    Cpu,
    Memory,
    Disk,
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
    {L"Disk", 90, true, false},           {L"Description", 240, false, true},
    {L"Command line", 360, false, false}, {L"Image path", 300, false, false},
    {L"Elevated", 72, false, false},      {L"Architecture", 92, false, false},
    {L"Threads", 70, true, false},        {L"Handles", 76, true, false},
    {L"Priority", 92, false, false},
};
static_assert(std::size(kColumns) == static_cast<size_t>(Column::Count));

constexpr int Index(Column c) {
    return static_cast<int>(c);
}

struct ViewRow {
    const lask::ProcessRow* row;   // points into App::current, rebuilt whenever it changes
    const lask::RowRates* rate;    // points into App::rates, same lifetime
    const lask::FactEntry* entry;  // null until the enrichment thread has reported on this process
};

struct KillResult {
    uint32_t pid;
    int64_t createTime;
    DWORD error;
    bool priority = false; // a Set priority result, not a kill
};

struct App {
    HWND hwnd = nullptr;
    HWND list = nullptr; // the Details grid; created the first time the tab is shown
    HWND filter = nullptr;
    HWND nav = nullptr;
    HWND options = nullptr; // the gear button
    HWND navToggle = nullptr; // the hamburger button
    HWND hotButton = nullptr; // the gear or hamburger while the mouse is over it
    bool navAnimating = false;
    double navAnimationFrom = 0; // nav width at 96 dpi when the running animation started
    std::chrono::steady_clock::time_point navAnimationStart;
    HIMAGELIST navRows = nullptr; // a 1-pixel-wide image list whose height sets the nav row height
    HBRUSH background = nullptr;
    HBRUSH surface = nullptr;
    bool elevated = false;
    lask::Chrome chrome;
    lask::FactCache facts;
    lask::Sampler sampler;
    lask::Enricher enricher;
    lask::ProcessesTab processes{chrome, facts, filterText};
    lask::PerfModel perf;                            // fed every tick, from startup
    lask::PerformanceTab performance{chrome, perf}; // its windows are created on first show
    lask::ServicesTab services{chrome, filterText};  // likewise
    Tab tab = Tab::Processes;
    std::shared_ptr<const lask::Snapshot> current;
    std::shared_ptr<const lask::Snapshot> previous;
    std::vector<lask::RowRates> rates; // aligned with current->rows
    std::vector<ViewRow> view;         // the Details grid's rows
    uint32_t tick = 0;
    int wantedTop = -1;
    Column sortColumn = Column::Cpu;
    bool sortDescending = true;
    std::vector<Column> columns; // by ListView sub-item index; display order is the ListView's own
    bool columnVisible[Index(Column::Count)] = {};
    int columnWidth[Index(Column::Count)] = {}; // at 96 dpi, remembered while a column is hidden
    bool menuOpen = false;
    lask::Settings settings;
    lask::TrayIcon tray;
    HICON smallIcon = nullptr; // title bar, taskbar fallback and tray
    UINT taskbarCreated = 0; // Explorer restarted: the tray icon has to be added again
    UINT activateMessage = 0; // a second launch asks the running instance to show itself
    bool exiting = false;     // WM_CLOSE really closes (tray mode otherwise only hides)
    bool hidden = false;      // in the notification area; sampling is paused
    bool bench = false;       // --bench: print startup times and exit
    double benchPainted = 0;
    wchar_t filterText[128] = {};
    wchar_t text[160] = {}; // LVN_GETDISPINFO scratch; the control copies it before the next request
};

App g;

int Scale(int px) {
    return g.chrome.scale(px);
}

template <class T>
int Compare(const T& a, const T& b) {
    return a < b ? -1 : (b < a ? 1 : 0);
}

HWND ActiveGrid() {
    switch (g.tab) {
    case Tab::Processes: return g.processes.grid();
    case Tab::Performance: return g.performance.list();
    case Tab::Services: return g.services.grid();
    default: return g.list;
    }
}

// ---- Details: cell values ----------------------------------------------------------------------

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
    case Column::Cpu: return Compare(a.rate->cpu, b.rate->cpu);
    case Column::Memory: return Compare(a.row->privateWorkingSet, b.row->privateWorkingSet);
    case Column::Disk: return Compare(a.rate->diskBytesPerSec, b.rate->diskBytesPerSec);
    case Column::Threads: return Compare(a.row->threadCount, b.row->threadCount);
    case Column::Handles: return Compare(a.row->handleCount, b.row->handleCount);
    case Column::Priority: return Compare(a.row->basePriority, b.row->basePriority);
    default: return _wcsicmp(StringValue(a, c), StringValue(b, c));
    }
}

// ---- Details: the sorted, filtered list the grid shows -------------------------------------------

void SortView() {
    const Column column = g.sortColumn;
    const bool descending = g.sortDescending;
    std::sort(g.view.begin(), g.view.end(), [column, descending](const ViewRow& a, const ViewRow& b) {
        int c = CompareRows(a, b, column);
        if (c == 0) c = Compare(a.row->pid, b.row->pid); // deterministic order for ties
        return descending ? c > 0 : c < 0;
    });
}

void RebuildView() {
    g.view.clear();
    if (!g.current) return;
    const auto& rows = g.current->rows;
    g.view.reserve(rows.size());
    for (size_t i = 0; i < rows.size(); ++i) {
        const ViewRow v{&rows[i], &g.rates[i], g.facts.find({rows[i].pid, rows[i].createTime})};
        if (lask::MatchesFilter(*v.row, FactsOf(v), g.filterText)) g.view.push_back(v);
    }
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

void RedrawVisibleRows(HWND grid) {
    if (!grid) return;
    const int top = ListView_GetTopIndex(grid);
    ListView_RedrawItems(grid, top, top + ListView_GetCountPerPage(grid));
}

// Only the visible rows are invalidated; the control asks for their text through LVN_GETDISPINFO.
void ApplyDetails() {
    if (!g.list) return; // never shown yet: nothing to keep up to date
    const auto selected = SelectedIdentity();
    RebuildView();
    ListView_SetItemCountEx(g.list, static_cast<int>(g.view.size()), LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
    RestoreSelection(selected);
    RedrawVisibleRows(g.list);
}

void AppendDetailsVisible(std::vector<lask::Identity>& ids) {
    if (g.view.empty()) return;
    const int count = static_cast<int>(g.view.size());
    const int top = std::clamp(ListView_GetTopIndex(g.list), 0, count - 1);
    const int end = std::min(count, top + ListView_GetCountPerPage(g.list) + 1);
    auto add = [&ids](int i) { ids.push_back({g.view[i].row->pid, g.view[i].row->createTime}); };
    for (int i = top; i < end; ++i) add(i);
    for (int i = end; i < std::min(count, end + kLookahead); ++i) add(i);
    for (int i = top - 1; i >= std::max(0, top - kLookahead); --i) add(i);
}

// Tells the enrichment thread what is on screen (the visible page first, then a few rows of
// lookahead) and which processes it has never reported on, whose paths the grouping needs.
void PublishWanted() {
    if (!g.enricher.running() || !g.current) return;
    std::vector<lask::Identity> ids;
    if (g.tab == Tab::Processes) g.processes.appendVisible(ids);
    else if (g.tab == Tab::Details) AppendDetailsVisible(ids);
    g.wantedTop = ListView_GetTopIndex(ActiveGrid());

    std::vector<lask::Identity> others;
    for (const auto& r : g.current->rows)
        if (!g.facts.find({r.pid, r.createTime})) others.push_back({r.pid, r.createTime});

    const uint32_t kinds = lask::kFactBasic | (g.columnVisible[Index(Column::CommandLine)] ? lask::kFactCommandLine : 0u);
    g.enricher.want(std::move(ids), std::move(others), kinds, g.facts.iconSize());
}

void PublishWantedIfScrolled() {
    if (ListView_GetTopIndex(ActiveGrid()) != g.wantedTop) PublishWanted();
}

bool IsInteracting() {
    return g.menuOpen || (GetKeyState(VK_LBUTTON) & 0x8000) || (GetKeyState(VK_RBUTTON) & 0x8000);
}

void OnBenchRows();

void AdoptLatestSnapshot() {
    auto latest = g.sampler.latest();
    if (!latest || latest == g.current) return;
    g.previous = std::move(g.current);
    g.current = std::move(latest);
    lask::ComputeRates(*g.current, g.previous.get(), g.rates);
    if (++g.tick % kSweepEvery == 0) g.facts.sweep(*g.current); // before the views take new pointers
    // Enrichment starts with the second tick, so icons and descriptions never delay the list.
    if (g.previous && !g.enricher.running()) g.enricher.start(g.hwnd, kMsgFacts);
    ApplyDetails();
    g.processes.update(g.current, &g.rates);
    g.perf.update(*g.current, g.previous.get());
    g.performance.refresh();
    g.services.update(g.current->services);
    PublishWanted();
    if (g.bench && !g.current->rows.empty()) OnBenchRows();
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

void OnFacts(lask::FactsBatch* batch) {
    g.facts.apply(batch);
    for (ViewRow& v : g.view)
        if (!v.entry) v.entry = g.facts.find({v.row->pid, v.row->createTime});
    RedrawVisibleRows(g.list);
    g.processes.factsArrived();
}

// ---- Details: grid -------------------------------------------------------------------------------

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
        if (v.rate->cpu < 0) wcscpy_s(out, n, L"—");
        else swprintf_s(out, n, L"%.1f%%", v.rate->cpu);
        break;
    case Column::Memory: lask::FormatMemory(v.row->privateWorkingSet, out, n); break;
    case Column::Disk:
        if (v.rate->diskBytesPerSec < 0) out[0] = L'\0';
        else swprintf_s(out, n, L"%.1f MB/s", v.rate->diskBytesPerSec / (1024.0 * 1024.0));
        break;
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

LRESULT DrawDetailsItem(NMLVCUSTOMDRAW* cd) {
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: return lask::PaintSelectedRow(cd, g.list, g.chrome.theme) ? CDRF_NEWFONT : CDRF_DODEFAULT;
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
    RedrawVisibleRows(g.list);
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
        g.columnWidth[Index(c)] = MulDiv(ListView_GetColumnWidth(g.list, i), 96, static_cast<int>(g.chrome.dpi));
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

void SelectInDetails(const lask::Identity& id) {
    int index = IndexOf(id);
    if (index < 0 && g.filterText[0]) {
        SetWindowTextW(g.filter, L""); // the filter hid it; EN_CHANGE rebuilds the view
        index = IndexOf(id);
    }
    ListView_SetItemState(g.list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    if (index < 0) return;
    ListView_SetItemState(g.list, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(g.list, index, FALSE);
}

// ---- navigation and tabs -------------------------------------------------------------------------

void EnsureDetails();
void EnsurePerformance();
void EnsureServices();

void ShowTab(Tab tab) {
    if (tab == Tab::Details) EnsureDetails();
    if (tab == Tab::Performance) EnsurePerformance();
    if (tab == Tab::Services) EnsureServices();
    const bool focusGrid = GetFocus() != g.nav;
    const bool switched = tab != g.tab;
    g.tab = tab;
    ShowWindow(g.processes.grid(), tab == Tab::Processes ? SW_SHOW : SW_HIDE);
    if (g.list) ShowWindow(g.list, tab == Tab::Details ? SW_SHOW : SW_HIDE);
    if (g.performance.created()) g.performance.show(tab == Tab::Performance);
    if (g.services.created()) ShowWindow(g.services.grid(), tab == Tab::Services ? SW_SHOW : SW_HIDE);
    ShowWindow(g.filter, tab == Tab::Performance ? SW_HIDE : SW_SHOW); // nothing to filter there
    // Window titles, the hung check, clock speed and pool sizes only cost anything while someone is
    // looking at them.
    g.sampler.setWantWindows(tab == Tab::Processes);
    g.sampler.setWantDetails(tab == Tab::Performance);
    g.sampler.setWantServices(tab == Tab::Services);
    if (switched && tab == Tab::Services) g.sampler.refreshServices();
    if (switched && tab != Tab::Details) g.sampler.requestNow(); // titles or details right away
    ListView_SetItemState(g.nav, static_cast<int>(tab), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    InvalidateRect(g.nav, nullptr, FALSE);
    if (focusGrid) SetFocus(ActiveGrid());
    PublishWanted();
}

void OnNavChanged(const NMLISTVIEW* change) {
    if (!(change->uChanged & LVIF_STATE) || !(change->uNewState & LVIS_SELECTED)) return;
    if (change->iItem >= 0 && change->iItem < static_cast<int>(Tab::Count) && change->iItem != static_cast<int>(g.tab))
        ShowTab(static_cast<Tab>(change->iItem));
}

// Windows 11 navigation look: a rounded pill behind the current and hovered item, an accent bar on
// the current one, a Fluent glyph and the label (no label while collapsed; the info tip names it).
LRESULT DrawNav(NMLVCUSTOMDRAW* cd) {
    const lask::Theme& theme = g.chrome.theme;
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
    case CDDS_POSTPAINT: lask::FillBelowRows(g.nav, cd->nmcd.hdc, theme.background); return CDRF_DODEFAULT;
    case CDDS_ITEMPREPAINT: {
        const int i = static_cast<int>(cd->nmcd.dwItemSpec);
        if (i < 0 || i >= static_cast<int>(Tab::Count)) return CDRF_SKIPDEFAULT;
        HDC dc = cd->nmcd.hdc;
        RECT rc{};
        rc.left = LVIR_BOUNDS;
        SendMessageW(g.nav, LVM_GETITEMRECT, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&rc));
        RECT client{};
        GetClientRect(g.nav, &client);
        rc.left = client.left;
        rc.right = client.right;
        lask::FillSolid(dc, rc, theme.background);

        const bool current = i == static_cast<int>(g.tab);
        const bool hot = (cd->nmcd.uItemState & CDIS_HOT) != 0;
        RECT pill = {rc.left + Scale(4), rc.top + Scale(2), rc.right - Scale(4), rc.bottom - Scale(2)};
        if (current || hot) lask::RoundFill(dc, pill, Scale(8), current ? theme.selectionInactive : theme.headerHot);
        if (current) {
            const int mid = (pill.top + pill.bottom) / 2;
            lask::RoundFill(dc, {pill.left, mid - Scale(8), pill.left + Scale(3), mid + Scale(8)}, Scale(3), theme.accentText);
        }

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, theme.text);
        HGDIOBJ oldFont = SelectObject(dc, g.chrome.glyphs);
        RECT glyph = {pill.left + Scale(10), pill.top, pill.left + Scale(36), pill.bottom};
        DrawTextW(dc, kNav[i].glyph, -1, &glyph, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        // Clipped, not ellipsized: while the nav animates the label slides under its edge.
        RECT label = {pill.left + Scale(46), pill.top, pill.right, pill.bottom};
        if (label.right > label.left) {
            SelectObject(dc, g.chrome.font);
            DrawTextW(dc, kNav[i].label, -1, &label, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        }
        SelectObject(dc, oldFont);
        return CDRF_SKIPDEFAULT;
    }
    default: return CDRF_DODEFAULT;
    }
}

void SizeNavRows() {
    HIMAGELIST rows = ImageList_Create(1, Scale(40), ILC_COLOR32, 1, 0);
    ListView_SetImageList(g.nav, rows, LVSIL_SMALL);
    if (g.navRows) ImageList_Destroy(g.navRows);
    g.navRows = rows;
}

void CreateNav(HWND parent) {
    g.nav = CreateWindowExW(0, WC_LISTVIEWW, L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL |
                                LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS,
                            0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kNavId)),
                            GetModuleHandleW(nullptr), nullptr);
    ListView_SetExtendedListViewStyle(g.nav, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
    LVCOLUMNW c{};
    c.mask = LVCF_WIDTH;
    c.cx = Scale(kNavWidth);
    ListView_InsertColumn(g.nav, 0, &c);
    for (int i = 0; i < static_cast<int>(Tab::Count); ++i) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText = const_cast<wchar_t*>(kNav[i].label); // for screen readers; drawing is custom
        ListView_InsertItem(g.nav, &item);
    }
    SizeNavRows();
}

// Collapsed, the labels only show as tips; expanded, they are on screen already.
void OnNavInfoTip(NMLVGETINFOTIPW* tip) {
    if (!tip->pszText || tip->cchTextMax <= 0) return;
    if (g.settings.navCollapsed && tip->iItem >= 0 && tip->iItem < static_cast<int>(Tab::Count))
        wcsncpy_s(tip->pszText, tip->cchTextMax, kNav[tip->iItem].label, _TRUNCATE);
    else
        tip->pszText[0] = L'\0';
}

// ---- layout, fonts, theme --------------------------------------------------------------------------

// The nav's width at 96 dpi: the collapsed or expanded width, or, while it animates, an ease-out
// cubic between where the animation started and that target.
double NavUnits() {
    const double target = g.settings.navCollapsed ? kNavCollapsedWidth : kNavWidth;
    if (!g.navAnimating) return target;
    const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - g.navAnimationStart;
    const double t = elapsed.count() / kNavAnimationMs;
    if (t >= 1) return target;
    const double eased = 1 - (1 - t) * (1 - t) * (1 - t);
    return g.navAnimationFrom + (target - g.navAnimationFrom) * eased;
}

void Layout() {
    RECT rc{};
    GetClientRect(g.hwnd, &rc);
    const int pad = Scale(8);
    const int nav = static_cast<int>(std::lround(NavUnits() * g.chrome.dpi / 96.0)) + Scale(8);
    const int editHeight = g.chrome.fontHeight + Scale(10);
    const int bar = editHeight + 2 * pad;
    const int width = std::max(0, static_cast<int>(rc.right) - nav);
    // The hamburger stays put at the left, centred over the nav glyphs, as in Task Manager.
    const int toggleWidth = Scale(32);
    SetWindowPos(g.navToggle, nullptr, Scale(kNavGlyphCenter) - toggleWidth / 2, pad, toggleWidth, editHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(g.nav, nullptr, 0, bar, nav, std::max(0, static_cast<int>(rc.bottom) - bar), SWP_NOZORDER | SWP_NOACTIVATE);
    ListView_SetColumnWidth(g.nav, 0, nav); // no column edge, so no separator line inside the nav
    SetWindowPos(g.filter, nullptr, nav + pad, pad, std::max(0, std::min(Scale(360), width - 2 * pad)), editHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(g.options, nullptr, static_cast<int>(rc.right) - pad - editHeight, pad, editHeight, editHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    for (HWND grid : {g.list, g.processes.grid(), g.services.grid()})
        if (grid)
            SetWindowPos(grid, nullptr, nav, bar, width, std::max(0, static_cast<int>(rc.bottom) - bar),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    if (g.performance.created()) g.performance.layout(nav, bar, width, std::max(0, static_cast<int>(rc.bottom) - bar));
}

void SetControlFonts() {
    for (HWND child : {g.list, g.processes.grid(), g.services.grid(), g.filter, g.nav})
        if (child) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(g.chrome.font), TRUE);
    if (g.filter) SendMessageW(g.filter, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(Scale(4), Scale(4)));
}

void RecreateFonts() {
    g.chrome.recreateFonts(g.hwnd);
    SetControlFonts();
    g.performance.fontsChanged();
}

void ReplaceBrush(HBRUSH& brush, COLORREF color) {
    HBRUSH old = brush;
    brush = CreateSolidBrush(color);
    if (old) DeleteObject(old);
}

lask::ThemeMode ThemeModeSetting() {
    const int mode = g.settings.theme;
    return mode >= 0 && mode <= static_cast<int>(lask::ThemeMode::Dark) ? static_cast<lask::ThemeMode>(mode)
                                                                         : lask::ThemeMode::System;
}

void ApplyTheme() {
    lask::Theme& theme = g.chrome.theme;
    theme = lask::ReadTheme(ThemeModeSetting(), g.settings.accentHeat);
    lask::SetAppDarkMode(theme.dark);
    lask::ApplyWindowTheme(g.hwnd, theme);

    if (g.list) lask::ApplyGridTheme(g.list, theme);
    if (g.services.created()) lask::ApplyGridTheme(g.services.grid(), theme);
    lask::ApplyGridTheme(g.processes.grid(), theme);
    lask::ApplyGridTheme(g.nav, theme);
    g.performance.applyTheme();
    ListView_SetBkColor(g.nav, theme.background);

    lask::AllowDarkModeForWindow(g.filter, theme.dark);
    SetWindowTheme(g.filter, theme.dark ? L"DarkMode_CFD" : L"CFD", nullptr);

    ReplaceBrush(g.surface, theme.surface);
    ReplaceBrush(g.background, theme.background);
    SetClassLongPtrW(g.hwnd, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(g.background));

    RedrawWindow(g.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

void SetGridImages() {
    if (g.list) ListView_SetImageList(g.list, g.facts.images(), LVSIL_SMALL);
    ListView_SetImageList(g.processes.grid(), g.facts.images(), LVSIL_SMALL);
}

void CreateControls(HWND parent) {
    g.chrome.recreateFonts(parent);
    g.facts.createImages(g.chrome.dpi);

    g.navToggle = CreateWindowExW(0, WC_BUTTONW, L"Navigation", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0,
                                  0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kNavToggleId)),
                                  GetModuleHandleW(nullptr), nullptr);
    g.filter =CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0,
                               0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFilterId)),
                               GetModuleHandleW(nullptr), nullptr);
    Edit_SetCueBannerTextFocused(g.filter, L"Filter: name, PID, user, description  (Ctrl+F)", TRUE);
    g.options = CreateWindowExW(0, WC_BUTTONW, L"Options", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0,
                                parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOptionsId)), GetModuleHandleW(nullptr),
                                nullptr);

    // Details columns: the defaults, then whatever the settings file remembers.
    const lask::Settings& s = g.settings;
    for (int i = 0; i < Index(Column::Count); ++i) {
        g.columnWidth[i] = kColumns[i].width;
        g.columnVisible[i] = kColumns[i].visible;
        if (s.detailsWidths.size() == static_cast<size_t>(Column::Count) && s.detailsWidths[i] > 0)
            g.columnWidth[i] = s.detailsWidths[i];
    }
    if (!s.detailsColumns.empty()) {
        for (bool& v : g.columnVisible) v = false;
        g.columnVisible[Index(Column::Name)] = true;
        for (int c : s.detailsColumns)
            if (c > 0 && c < Index(Column::Count)) g.columnVisible[c] = true;
    }
    if (s.detailsSort >= 0 && s.detailsSort < Index(Column::Count)) {
        g.sortColumn = static_cast<Column>(s.detailsSort);
        g.sortDescending = s.detailsDescending;
    }
    g.processes.create(parent, kProcessesId);
    lask::SetGridLayout(g.processes.grid(), s.processesWidths, s.processesOrder, g.chrome.dpi);
    CreateNav(parent);
    SetGridImages();
    SetControlFonts();
    ApplyTheme();
}

// Every ListView costs several milliseconds to create, and the app opens on the Processes tab, so
// the Details grid waits until it is first shown.
void EnsureDetails() {
    if (g.list) return;
    g.list = lask::CreateGrid(g.hwnd, kListId, g.chrome);
    // In the remembered display order (insertion order is display order), then any visible column
    // the settings did not list.
    AppendColumn(Column::Name);
    for (int c : g.settings.detailsColumns)
        if (c > 0 && c < Index(Column::Count) && g.columnVisible[c] &&
            std::find(g.columns.begin(), g.columns.end(), static_cast<Column>(c)) == g.columns.end())
            AppendColumn(static_cast<Column>(c));
    for (int i = 1; i < Index(Column::Count); ++i)
        if (g.columnVisible[i] && std::find(g.columns.begin(), g.columns.end(), static_cast<Column>(i)) == g.columns.end())
            AppendColumn(static_cast<Column>(i));
    lask::ApplyGridTheme(g.list, g.chrome.theme);
    SetGridImages();
    UpdateSortArrows();
    Layout();
    ApplyDetails();
}

// Direct2D and DirectWrite load here, on first visit, not at startup.
void EnsurePerformance() {
    if (g.performance.created()) return;
    g.performance.create(g.hwnd, kPerformanceId);
    Layout();
}

// The service list arrives with the next snapshot; the sampler is told to read it right away.
void EnsureServices() {
    if (g.services.created()) return;
    g.services.create(g.hwnd, kServicesId, kMsgServiceConfigs);
    lask::SetGridLayout(g.services.grid(), g.settings.servicesWidths, g.settings.servicesOrder, g.chrome.dpi);
    Layout();
    if (g.current) g.services.update(g.current->services);
}

void RescaleColumns(HWND grid, UINT from, UINT to) {
    if (!grid) return;
    const int n = Header_GetItemCount(ListView_GetHeader(grid));
    for (int i = 0; i < n; ++i)
        ListView_SetColumnWidth(grid, i, MulDiv(ListView_GetColumnWidth(grid, i), static_cast<int>(to), static_cast<int>(from)));
}

void OnDpiChanged(UINT dpi, const RECT& suggested) {
    const UINT old = g.chrome.dpi;
    g.chrome.dpi = dpi;
    RescaleColumns(g.list, old, dpi);
    RescaleColumns(g.processes.grid(), old, dpi);
    RescaleColumns(g.services.grid(), old, dpi);
    RecreateFonts();
    SizeNavRows();
    if (g.facts.createImages(dpi)) SetGridImages();
    SetWindowPos(g.hwnd, nullptr, suggested.left, suggested.top, suggested.right - suggested.left,
                 suggested.bottom - suggested.top, SWP_NOZORDER | SWP_NOACTIVATE);
    PublishWanted(); // carries the new icon size
}

// ---- actions ---------------------------------------------------------------------------------

void RestartElevatedWith(const wchar_t* args) {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = args;
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) {
        g.exiting = true; // hand over to the elevated instance, even in tray mode
        PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
    }
}

void RestartElevated(uint32_t killPid, int64_t killCreateTime) {
    wchar_t args[96] = {};
    if (killPid) swprintf_s(args, L"--kill %u --at %lld", killPid, static_cast<long long>(killCreateTime));
    RestartElevatedWith(args);
}

// ---- services ------------------------------------------------------------------------------------

struct ServiceResult {
    std::wstring name;
    lask::ServiceAction action;
    DWORD error;
};

const wchar_t* Verb(lask::ServiceAction action) {
    switch (action) {
    case lask::ServiceAction::Start: return L"start";
    case lask::ServiceAction::Stop: return L"stop";
    default: return L"restart";
    }
}

// StartService and a restart's wait for "stopped" can take seconds; never on this thread.
void RunServiceActionAsync(std::wstring name, lask::ServiceAction action) {
    HWND hwnd = g.hwnd;
    std::thread([name = std::move(name), action, hwnd] {
        auto* result = new ServiceResult{name, action, lask::RunServiceAction(name, action)};
        if (!PostMessageW(hwnd, kMsgServiceDone, 0, reinterpret_cast<LPARAM>(result))) delete result;
    }).detach();
    g.sampler.refreshServices(); // show "Stopping" / "Starting" at once
    g.sampler.requestNow();
}

void OnServiceDone(ServiceResult* result) {
    std::unique_ptr<ServiceResult> r(result);
    g.sampler.refreshServices();
    g.sampler.requestNow();
    if (r->error == ERROR_SUCCESS) return;
    wchar_t message[600] = {};
    if (r->error == ERROR_ACCESS_DENIED && !g.elevated) {
        swprintf_s(message, L"Windows needs administrator rights to %s the %s service.\n\n"
                            L"Restart Lask Manager as administrator and %s it from there?",
                   Verb(r->action), r->name.c_str(), Verb(r->action));
        if (MessageBoxW(g.hwnd, message, L"Lask Manager", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON1) == IDYES) {
            const std::wstring args = std::wstring(L"--service ") + Verb(r->action) + L" \"" + r->name + L"\"";
            RestartElevatedWith(args.c_str());
        }
        return;
    }
    wchar_t reason[256] = {};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, r->error, 0, reason,
                   static_cast<DWORD>(std::size(reason)), nullptr);
    swprintf_s(message, L"Could not %s the %s service.\n\n%s", Verb(r->action), r->name.c_str(), reason);
    MessageBoxW(g.hwnd, message, L"Lask Manager", MB_OK | MB_ICONERROR);
}

void OnServiceCommand(lask::ServiceAction action) {
    if (const auto s = g.services.selection()) RunServiceActionAsync(s->name, action);
}

void OpenServicesConsole() {
    std::thread([] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ShellExecuteW(nullptr, L"open", L"services.msc", nullptr, nullptr, SW_SHOWNORMAL);
        if (SUCCEEDED(com)) CoUninitialize();
    }).detach();
}

// The process a service runs in, as an identity from the current snapshot.
std::optional<lask::Identity> ServiceProcess() {
    const auto s = g.services.selection();
    if (!s || !s->pid || !g.current) return std::nullopt;
    for (const auto& r : g.current->rows)
        if (r.pid == s->pid) return lask::Identity{r.pid, r.createTime};
    return std::nullopt;
}

std::optional<lask::ActionTarget> SelectedTarget() {
    if (g.tab == Tab::Processes) return g.processes.selection();
    if (g.tab == Tab::Performance || g.tab == Tab::Services) return std::nullopt; // no kills from there
    const auto id = SelectedIdentity();
    if (!id) return std::nullopt;
    return lask::ActionTarget{*id, {*id}, nullptr};
}

bool IsCritical(uint32_t pid) {
    return pid == 0 || pid == 4; // Idle and System cannot be ended; don't try
}

void PostKillResult(HWND hwnd, uint32_t pid, int64_t createTime, DWORD error) {
    auto* result = new KillResult{pid, createTime, error};
    if (!PostMessageW(hwnd, kMsgKillDone, 0, reinterpret_cast<LPARAM>(result))) delete result;
}

// End task: every process the selected row stands for (an app row ends all of its processes).
void EndSelectedTask() {
    auto target = SelectedTarget();
    if (!target) return;
    std::erase_if(target->all, [](const lask::Identity& id) { return IsCritical(id.pid); });
    if (target->all.empty()) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    HWND hwnd = g.hwnd;
    std::thread([ids = std::move(target->all), hwnd] {
        for (const lask::Identity& id : ids) {
            const DWORD error = lask::TerminateProcessByIdentity(id.pid, id.createTime);
            if (error != ERROR_SUCCESS && error != ERROR_NOT_FOUND) return PostKillResult(hwnd, id.pid, id.createTime, error);
        }
        PostKillResult(hwnd, ids.front().pid, ids.front().createTime, ERROR_SUCCESS);
    }).detach();
}

void EndSelectedTree() {
    const auto target = SelectedTarget();
    if (!target || !g.current) return;
    if (IsCritical(target->process.pid)) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    HWND hwnd = g.hwnd;
    std::thread([snapshot = g.current, root = target->process, hwnd] {
        lask::Identity failed = root;
        const DWORD error = lask::TerminateTree(*snapshot, root, failed);
        PostKillResult(hwnd, failed.pid, failed.createTime, error);
    }).detach();
}

// Explorer can take its time (network drives, shell extensions), so this runs on its own thread.
void OpenFileLocation() {
    const auto target = SelectedTarget();
    if (!target) return;
    const lask::FactEntry* entry = g.facts.find(target->process);
    std::wstring path = entry ? entry->facts->path : std::wstring();
    std::thread([path = std::move(path), id = target->process]() mutable {
        if (path.empty()) {
            if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id.pid)) {
                wchar_t buffer[MAX_PATH * 4];
                DWORD size = static_cast<DWORD>(std::size(buffer));
                if (QueryFullProcessImageNameW(h, 0, buffer, &size)) path.assign(buffer, size);
                CloseHandle(h);
            }
        }
        if (path.empty() || path.starts_with(L"\\\\?\\GLOBALROOT")) {
            MessageBeep(MB_ICONWARNING);
            return;
        }
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const std::wstring args = L"/select,\"" + path + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        if (SUCCEEDED(com)) CoUninitialize();
    }).detach();
}

void GoToDetails() {
    std::optional<lask::Identity> id;
    if (g.tab == Tab::Services) id = ServiceProcess();
    else if (const auto target = SelectedTarget()) id = target->process;
    if (!id) return;
    ShowTab(Tab::Details);
    SelectInDetails(*id);
}

void OnKillDone(KillResult* result) {
    std::unique_ptr<KillResult> r(result);
    if (r->priority) {
        if (r->error == ERROR_SUCCESS || r->error == ERROR_NOT_FOUND) {
            g.sampler.requestNow();
        } else if (r->error == ERROR_ACCESS_DENIED && !g.elevated) {
            if (MessageBoxW(g.hwnd, L"Windows denied access to this process.\n\nRestart Lask Manager as administrator?",
                            L"Lask Manager", MB_YESNO | MB_ICONWARNING) == IDYES)
                RestartElevated(0, 0);
        } else {
            wchar_t reason[256] = {}, message[400] = {};
            FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, r->error, 0, reason,
                           static_cast<DWORD>(std::size(reason)), nullptr);
            swprintf_s(message, L"Could not change the priority of process %u.\n\n%s", r->pid, reason);
            MessageBoxW(g.hwnd, message, L"Lask Manager", MB_OK | MB_ICONERROR);
        }
        return;
    }
    if (r->error == ERROR_SUCCESS || r->error == ERROR_NOT_FOUND) {
        g.sampler.requestNow(); // the rows disappear with the next snapshot
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

POINT MenuPoint(HWND grid, LPARAM lp) {
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    if (pt.x == -1 && pt.y == -1) { // keyboard: anchor on the selected row
        const int i = ListView_GetNextItem(grid, -1, LVNI_SELECTED);
        RECT rc{};
        if (i >= 0 && ListView_GetItemRect(grid, i, &rc, LVIR_LABEL)) pt = {rc.left + Scale(8), rc.bottom};
        ClientToScreen(grid, &pt);
    }
    return pt;
}

// Start / Stop / Restart carry the UAC shield when we are not elevated: most services need it.
void ShowServiceMenu(LPARAM lp) {
    const POINT pt = MenuPoint(g.services.grid(), lp);
    const auto s = g.services.selection();
    const bool stopped = s && s->state == SERVICE_STOPPED;
    const bool stoppable = s && s->state == SERVICE_RUNNING && (s->controlsAccepted & SERVICE_ACCEPT_STOP);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (stopped ? 0 : MF_GRAYED), kCmdServiceStart, L"&Start");
    AppendMenuW(menu, MF_STRING | (stoppable ? 0 : MF_GRAYED), kCmdServiceStop, L"S&top");
    AppendMenuW(menu, MF_STRING | (stoppable ? 0 : MF_GRAYED), kCmdServiceRestart, L"&Restart");
    HBITMAP shield = g.elevated ? nullptr : lask::CreateShieldBitmap(GetSystemMetricsForDpi(SM_CXSMICON, g.chrome.dpi));
    if (shield) {
        MENUITEMINFOW mii{};
        mii.cbSize = sizeof mii;
        mii.fMask = MIIM_BITMAP;
        mii.hbmpItem = shield;
        for (UINT id : {kCmdServiceStart, kCmdServiceStop, kCmdServiceRestart}) SetMenuItemInfoW(menu, id, FALSE, &mii);
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (s && s->pid ? 0 : MF_GRAYED), kCmdGoToDetails, L"&Go to details");
    AppendMenuW(menu, MF_STRING, kCmdOpenServices, L"&Open Services");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdRefresh, L"Re&fresh now\tF5");
    AppendMenuW(menu, MF_STRING, kCmdFind, L"F&ilter\tCtrl+F");
    if (!g.elevated) AppendMenuW(menu, MF_STRING, kCmdRestartAdmin, L"Restart as &administrator");

    g.menuOpen = true;
    TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, g.hwnd, nullptr);
    g.menuOpen = false;
    DestroyMenu(menu);
    if (shield) DeleteObject(shield);
}

void ShowContextMenu(HWND grid, LPARAM lp) {
    const POINT pt = MenuPoint(grid, lp);
    const auto target = SelectedTarget();
    const UINT enabled = target ? 0 : MF_GRAYED;

    HMENU menu = CreatePopupMenu();
    if (target && target->window) {
        AppendMenuW(menu, MF_STRING, kCmdSwitchTo, L"&Switch to");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING | enabled, kCmdEndTask, L"&End task\tDel");
    AppendMenuW(menu, MF_STRING | enabled, kCmdEndTree, L"End process &tree");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    HMENU priority = CreatePopupMenu();
    int current = -1;
    if (target && g.current)
        for (const auto& r : g.current->rows)
            if (r.pid == target->process.pid && r.createTime == target->process.createTime)
                for (int i = 0; i < static_cast<int>(std::size(kPriorityBase)); ++i)
                    if (kPriorityBase[i] == r.basePriority) current = i;
    for (int i = 0; i < static_cast<int>(std::size(kPriorityClasses)); ++i)
        AppendMenuW(priority, MF_STRING | (i == current ? MF_CHECKED : 0), kCmdPriorityBase + i, kPriorityNames[i]);
    AppendMenuW(menu, MF_POPUP | enabled, reinterpret_cast<UINT_PTR>(priority), L"Set &priority");
    AppendMenuW(menu, MF_STRING | enabled, kCmdOpenLocation, L"&Open file location");
    if (g.tab == Tab::Processes) AppendMenuW(menu, MF_STRING | enabled, kCmdGoToDetails, L"&Go to details");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdRefresh, L"&Refresh now\tF5");
    AppendMenuW(menu, MF_STRING, kCmdFind, L"&Filter\tCtrl+F");
    if (!g.elevated) AppendMenuW(menu, MF_STRING, kCmdRestartAdmin, L"Restart as &administrator");

    g.menuOpen = true;
    TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, g.hwnd, nullptr);
    g.menuOpen = false;
    DestroyMenu(menu);
}

// ---- keyboard, filter ----------------------------------------------------------------------------

void FocusGrid() {
    HWND grid = ActiveGrid();
    SetFocus(grid);
    if (ListView_GetItemCount(grid) > 0 && ListView_GetSelectedCount(grid) == 0)
        ListView_SetItemState(grid, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

// Esc clears the filter (then leaves it); Down, Enter and Tab hand the keyboard to the grid.
LRESULT CALLBACK FilterSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) {
            if (GetWindowTextLengthW(hwnd) > 0) SetWindowTextW(hwnd, L"");
            else FocusGrid();
            return 0;
        }
        if (wp == VK_DOWN || wp == VK_RETURN || wp == VK_TAB) {
            FocusGrid();
            return 0;
        }
    } else if (msg == WM_CHAR && (wp == VK_ESCAPE || wp == VK_RETURN || wp == VK_TAB)) {
        return 0; // already handled on key down; this only stops the beep
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// The gear and hamburger: owner-drawn buttons get no hover state of their own, so track it here.
LRESULT CALLBACK BarButtonSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_MOUSEMOVE && g.hotButton != hwnd) {
        g.hotButton = hwnd;
        TRACKMOUSEEVENT track{sizeof track, TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&track);
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (msg == WM_MOUSELEAVE && g.hotButton == hwnd) {
        g.hotButton = nullptr;
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void OnFilterChanged() {
    GetWindowTextW(g.filter, g.filterText, static_cast<int>(std::size(g.filterText)));
    ApplyDetails();
    g.processes.refilter();
    g.services.refilter();
    // Show the matches from the top, or the selected row if it survived the filter.
    for (HWND grid : {g.list, g.processes.grid(), g.services.grid()}) {
        if (!grid) continue;
        const int selected = ListView_GetNextItem(grid, -1, LVNI_SELECTED);
        if (ListView_GetItemCount(grid) > 0) ListView_EnsureVisible(grid, selected >= 0 ? selected : 0, FALSE);
    }
    PublishWanted();
}

// Keys both grids share. Left, Right and Enter on the Processes tab are the tree's.
void OnGridKey(WORD key) {
    switch (key) {
    case VK_DELETE: EndSelectedTask(); break;
    case VK_TAB: SetFocus(g.filter); break;
    case VK_ESCAPE:
        if (g.filterText[0]) SetWindowTextW(g.filter, L""); // first Esc drops the filter
        else PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
        break;
    default: break;
    }
}

// ---- window ----------------------------------------------------------------------------------

LRESULT OnNotify(NMHDR* h) {
    if (h->idFrom == kNavId) {
        if (h->code == NM_CUSTOMDRAW) return DrawNav(reinterpret_cast<NMLVCUSTOMDRAW*>(h));
        if (h->code == LVN_ITEMCHANGED) OnNavChanged(reinterpret_cast<NMLISTVIEW*>(h));
        if (h->code == LVN_GETINFOTIPW) OnNavInfoTip(reinterpret_cast<NMLVGETINFOTIPW*>(h));
        return 0;
    }
    if (h->idFrom == kServicesId) {
        if (h->code == LVN_KEYDOWN) {
            OnGridKey(reinterpret_cast<NMLVKEYDOWN*>(h)->wVKey); // Delete does nothing here: no target
            return 0;
        }
        return g.services.onNotify(h);
    }
    if (h->idFrom == kPerformanceId) {
        if (h->code == LVN_KEYDOWN && reinterpret_cast<NMLVKEYDOWN*>(h)->wVKey == VK_ESCAPE)
            PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
        return g.performance.onNotify(h);
    }
    if (h->idFrom == kProcessesId) {
        switch (h->code) {
        case LVN_KEYDOWN: OnGridKey(reinterpret_cast<NMLVKEYDOWN*>(h)->wVKey); return 0;
        case LVN_ODCACHEHINT:
        case LVN_ENDSCROLL: PublishWantedIfScrolled(); return 0;
        default: return g.processes.onNotify(h);
        }
    }
    if (h->idFrom != kListId) return 0;
    switch (h->code) {
    case LVN_GETDISPINFOW: OnGetDispInfo(reinterpret_cast<NMLVDISPINFOW*>(h)); return 0;
    case LVN_ODFINDITEMW: return OnFindItem(reinterpret_cast<NMLVFINDITEMW*>(h));
    case LVN_ODCACHEHINT:
    case LVN_ENDSCROLL: PublishWantedIfScrolled(); return 0;
    case LVN_COLUMNCLICK: OnColumnClick(reinterpret_cast<NMLISTVIEW*>(h)->iSubItem); return 0;
    case NM_CUSTOMDRAW: return DrawDetailsItem(reinterpret_cast<NMLVCUSTOMDRAW*>(h));
    case LVN_KEYDOWN: OnGridKey(reinterpret_cast<NMLVKEYDOWN*>(h)->wVKey); return 0;
    default: return 0;
    }
}

// ---- settings, options, tray ---------------------------------------------------------------------

void CaptureSettings() {
    lask::Settings& s = g.settings;
    s.tab = static_cast<int>(g.tab);
    WINDOWPLACEMENT wp{};
    wp.length = sizeof wp;
    if (!g.hidden && GetWindowPlacement(g.hwnd, &wp)) {
        s.window = wp.rcNormalPosition;
        s.hasWindow = true;
        s.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
    }
    s.graphSeconds = g.performance.seconds();
    s.logicalProcessors = g.performance.logical();
    s.detailsSort = Index(g.sortColumn);
    s.detailsDescending = g.sortDescending;
    if (g.list) {
        s.detailsColumns.clear();
        for (int i : lask::GridOrder(g.list)) s.detailsColumns.push_back(Index(g.columns[i]));
        for (size_t i = 0; i < g.columns.size(); ++i)
            g.columnWidth[Index(g.columns[i])] = MulDiv(ListView_GetColumnWidth(g.list, static_cast<int>(i)), 96,
                                                        static_cast<int>(g.chrome.dpi));
        s.detailsWidths.assign(std::begin(g.columnWidth), std::end(g.columnWidth));
    }
    s.processesWidths = lask::GridWidths(g.processes.grid(), g.chrome.dpi);
    s.processesOrder = lask::GridOrder(g.processes.grid());
    if (g.services.created()) {
        s.servicesWidths = lask::GridWidths(g.services.grid(), g.chrome.dpi);
        s.servicesOrder = lask::GridOrder(g.services.grid());
    }
}

// Option changes are saved at once, off this thread; the exit save is the synchronous one.
void SaveSettingsSoon() {
    CaptureSettings();
    std::thread([settings = g.settings] { lask::SaveSettings(settings); }).detach();
}

void UpdateTitle() {
    std::wstring title = g.elevated ? L"Lask Manager (Administrator)" : L"Lask Manager";
    if (g.settings.refreshMs == 0) title += L" — updates paused (F5 refreshes)";
    SetWindowTextW(g.hwnd, title.c_str());
}

// Hidden in the tray means paused: nobody is looking, so nothing is sampled.
void ApplyRefresh() {
    const unsigned ms = g.settings.refreshMs;
    g.sampler.setInterval(g.hidden || ms == 0 ? INFINITE : ms);
    g.performance.setInterval(ms);
    if (!g.hidden) g.sampler.requestNow(); // the new interval applies from the next wait
    UpdateTitle();
}

void ApplyOnTop() {
    SetWindowPos(g.hwnd, g.settings.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void ApplyTray() {
    if (g.settings.tray) {
        g.tray.show(g.hwnd, kMsgTray, g.smallIcon, L"Lask Manager");
        RegisterHotKey(g.hwnd, kHotkeyShow, MOD_WIN | MOD_SHIFT | MOD_NOREPEAT, VK_ESCAPE); // fails soft
    } else {
        g.tray.hide();
        UnregisterHotKey(g.hwnd, kHotkeyShow);
    }
}

void HideToTray() {
    ShowWindow(g.hwnd, SW_HIDE);
    g.hidden = true;
    ApplyRefresh();
}

void ShowFromTray() {
    const bool wasHidden = g.hidden;
    g.hidden = false;
    ShowWindow(g.hwnd, IsIconic(g.hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g.hwnd);
    if (wasHidden) ApplyRefresh(); // and a snapshot right away
}

void ShowTrayMenu(POINT pt) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kCmdShow, L"&Open Lask Manager");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdExit, L"E&xit");
    SetMenuDefaultItem(menu, kCmdShow, FALSE);
    SetForegroundWindow(g.hwnd); // or the menu does not close when clicking elsewhere
    TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, g.hwnd, nullptr);
    PostMessageW(g.hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void OnTrayEvent(WPARAM wp, LPARAM lp) {
    switch (LOWORD(lp)) {
    case NIN_SELECT:
    case NIN_KEYSELECT:
        if (g.hidden || GetForegroundWindow() != g.hwnd) ShowFromTray();
        else HideToTray();
        break;
    case WM_CONTEXTMENU: ShowTrayMenu({GET_X_LPARAM(wp), GET_Y_LPARAM(wp)}); break;
    default: break;
    }
}

void ToggleReplaceTaskManager() {
    const bool on = !lask::IsReplacingTaskManager();
    if (on && MessageBoxW(g.hwnd,
                          L"Windows will start Lask Manager instead of Task Manager: for Ctrl+Shift+Esc, from the "
                          L"Ctrl+Alt+Del screen and from the taskbar.\n\n"
                          L"If you move or delete lask.exe, turn this off first, or Task Manager will not open.\n\n"
                          L"Continue?",
                          L"Lask Manager", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
    if (!g.elevated) { // the IFEO key is under HKLM: the elevated instance makes the change
        RestartElevatedWith(on ? L"--replace-taskmgr on" : L"--replace-taskmgr off");
        return;
    }
    if (const DWORD error = lask::SetReplaceTaskManager(on)) {
        wchar_t reason[256] = {};
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0, reason,
                       static_cast<DWORD>(std::size(reason)), nullptr);
        MessageBoxW(g.hwnd, reason, L"Lask Manager", MB_OK | MB_ICONERROR);
    }
}

void ShowOptionsMenu() {
    RECT rc{};
    GetWindowRect(g.options, &rc);
    const lask::Settings& s = g.settings;
    HMENU speed = CreatePopupMenu();
    AppendMenuW(speed, MF_STRING, kCmdSpeedHigh, L"&High (0.5 s)");
    AppendMenuW(speed, MF_STRING, kCmdSpeedNormal, L"&Normal (1 s)");
    AppendMenuW(speed, MF_STRING, kCmdSpeedLow, L"&Low (2 s)");
    AppendMenuW(speed, MF_STRING, kCmdSpeedPaused, L"&Paused");
    const UINT checked = s.refreshMs == 0 ? kCmdSpeedPaused : s.refreshMs <= 500 ? kCmdSpeedHigh
                       : s.refreshMs >= 2000 ? kCmdSpeedLow : kCmdSpeedNormal;
    CheckMenuRadioItem(speed, kCmdSpeedHigh, kCmdSpeedPaused, checked, MF_BYCOMMAND);

    HMENU look = CreatePopupMenu();
    AppendMenuW(look, MF_STRING, kCmdThemeSystem, L"Use &Windows setting");
    AppendMenuW(look, MF_STRING, kCmdThemeLight, L"&Light");
    AppendMenuW(look, MF_STRING, kCmdThemeDark, L"&Dark");
    CheckMenuRadioItem(look, kCmdThemeSystem, kCmdThemeDark, kCmdThemeSystem + static_cast<UINT>(ThemeModeSetting()),
                       MF_BYCOMMAND);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (s.alwaysOnTop ? MF_CHECKED : 0), kCmdOnTop, L"Always on &top");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(speed), L"&Update speed");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(look), L"Th&eme");
    AppendMenuW(menu, MF_STRING | (s.hardwareGraphs ? MF_CHECKED : 0), kCmdHardwareGraphs,
                L"&Hardware-accelerated graphs (uses more memory)");
    AppendMenuW(menu, MF_STRING | (s.tray ? MF_CHECKED : 0), kCmdTray,
                L"&Keep running in the notification area (Win+Shift+Esc)");
    AppendMenuW(menu, MF_STRING | (s.accentHeat ? MF_CHECKED : 0), kCmdAccentHeat,
                L"Show high usage in the &accent color");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (lask::IsReplacingTaskManager() ? MF_CHECKED : 0), kCmdReplaceTaskManager,
                L"&Replace Task Manager");
    HBITMAP shield = g.elevated ? nullptr : lask::CreateShieldBitmap(GetSystemMetricsForDpi(SM_CXSMICON, g.chrome.dpi));
    if (shield) {
        MENUITEMINFOW mii{};
        mii.cbSize = sizeof mii;
        mii.fMask = MIIM_BITMAP;
        mii.hbmpItem = shield;
        SetMenuItemInfoW(menu, kCmdReplaceTaskManager, FALSE, &mii);
        AppendMenuW(menu, MF_STRING, kCmdRestartAdmin, L"Restart as &administrator");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdExit, L"E&xit");

    g.menuOpen = true;
    TrackPopupMenuEx(menu, TPM_RIGHTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, rc.right, rc.bottom, g.hwnd, nullptr);
    g.menuOpen = false;
    DestroyMenu(menu);
    if (shield) DeleteObject(shield);
}

void OnOptionCommand(WORD id) {
    lask::Settings& s = g.settings;
    switch (id) {
    case kCmdOnTop:
        s.alwaysOnTop = !s.alwaysOnTop;
        ApplyOnTop();
        break;
    case kCmdSpeedHigh: s.refreshMs = 500; ApplyRefresh(); break;
    case kCmdSpeedNormal: s.refreshMs = 1000; ApplyRefresh(); break;
    case kCmdSpeedLow: s.refreshMs = 2000; ApplyRefresh(); break;
    case kCmdSpeedPaused: s.refreshMs = 0; ApplyRefresh(); break;
    case kCmdHardwareGraphs:
        s.hardwareGraphs = !s.hardwareGraphs;
        lask::SetHardwareGraphs(s.hardwareGraphs);
        g.performance.graphsReset();
        break;
    case kCmdTray:
        s.tray = !s.tray;
        lask::SetStartWithWindows(s.tray); // resident only makes sense if it is already running
        ApplyTray();
        break;
    case kCmdAccentHeat:
        s.accentHeat = !s.accentHeat;
        ApplyTheme();
        break;
    case kCmdThemeSystem:
    case kCmdThemeLight:
    case kCmdThemeDark:
        s.theme = id - kCmdThemeSystem;
        ApplyTheme();
        break;
    case kCmdReplaceTaskManager: ToggleReplaceTaskManager(); return;
    default: return;
    }
    SaveSettingsSoon();
}

// Slides the nav to its other width; a toggle mid-animation turns around from where it is. With
// "Animation effects" off in Windows it jumps.
void ToggleNav() {
    const double from = NavUnits();
    g.settings.navCollapsed = !g.settings.navCollapsed;
    BOOL animate = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0);
    g.navAnimating = animate != FALSE;
    if (g.navAnimating) {
        g.navAnimationFrom = from;
        g.navAnimationStart = std::chrono::steady_clock::now();
        SetTimer(g.hwnd, kTimerNavAnimation, USER_TIMER_MINIMUM, nullptr);
    }
    Layout();
    InvalidateRect(g.nav, nullptr, FALSE);
    if (GetFocus() == g.navToggle) SetFocus(ActiveGrid()); // keys keep going to the grid, as before the click
    SaveSettingsSoon();
}

void OnNavAnimationFrame() {
    if (NavUnits() == (g.settings.navCollapsed ? kNavCollapsedWidth : kNavWidth)) {
        g.navAnimating = false;
        KillTimer(g.hwnd, kTimerNavAnimation);
    }
    Layout();
    InvalidateRect(g.nav, nullptr, FALSE); // every label's clip moves
}

void SetSelectedPriority(int index) {
    const auto target = SelectedTarget();
    if (!target || index < 0 || index >= static_cast<int>(std::size(kPriorityClasses))) return;
    if (kPriorityClasses[index] == REALTIME_PRIORITY_CLASS &&
        MessageBoxW(g.hwnd, L"Realtime priority can make the whole system unresponsive.\n\nSet it anyway?",
                    L"Lask Manager", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;
    HWND hwnd = g.hwnd;
    std::thread([id = target->process, priorityClass = kPriorityClasses[index], hwnd] {
        auto* result = new KillResult{id.pid, id.createTime, lask::SetProcessPriorityByIdentity(id.pid, id.createTime, priorityClass), true};
        if (!PostMessageW(hwnd, kMsgKillDone, 0, reinterpret_cast<LPARAM>(result))) delete result;
    }).detach();
}

// --bench: the startup numbers the design asks to watch, on stdout (works with redirection),
// then exit. Times are from process creation.
double MsSinceProcessStart() {
    FILETIME created{}, exited{}, kernel{}, user{}, now{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    GetSystemTimePreciseAsFileTime(&now);
    const auto ticks = [](const FILETIME& f) { return (static_cast<int64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return static_cast<double>(ticks(now) - ticks(created)) / 10'000.0;
}

void OnBenchRows() {
    g.bench = false;
    char line[160];
    const int n = sprintf_s(line, "first-paint-ms=%.1f first-rows-ms=%.1f processes=%zu\r\n", g.benchPainted,
                            MsSinceProcessStart(), g.current->rows.size());
    AttachConsole(ATTACH_PARENT_PROCESS);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    if (out && out != INVALID_HANDLE_VALUE && n > 0) WriteFile(out, line, static_cast<DWORD>(n), &written, nullptr);
    g.exiting = true;
    PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
}

void OnCommand(WORD id) {
    if (id >= kCmdPriorityBase && id < kCmdPriorityBase + std::size(kPriorityClasses)) {
        SetSelectedPriority(id - kCmdPriorityBase);
        return;
    }
    switch (id) {
    case kCmdEndTask: EndSelectedTask(); break;
    case kCmdEndTree: EndSelectedTree(); break;
    case kCmdOpenLocation: OpenFileLocation(); break;
    case kCmdGoToDetails: GoToDetails(); break;
    case kCmdSwitchTo:
        if (const auto target = SelectedTarget(); target && target->window) lask::SwitchToWindow(target->window);
        break;
    case kCmdRefresh: g.sampler.requestNow(); break;
    case kCmdFind:
        SetFocus(g.filter);
        Edit_SetSel(g.filter, 0, -1);
        break;
    case kCmdNextTab:
        ShowTab(static_cast<Tab>((static_cast<int>(g.tab) + 1) % static_cast<int>(Tab::Count)));
        break;
    case kCmdTabProcesses: ShowTab(Tab::Processes); break;
    case kCmdTabPerformance: ShowTab(Tab::Performance); break;
    case kCmdTabDetails: ShowTab(Tab::Details); break;
    case kCmdTabServices: ShowTab(Tab::Services); break;
    case kCmdServiceStart: OnServiceCommand(lask::ServiceAction::Start); break;
    case kCmdServiceStop: OnServiceCommand(lask::ServiceAction::Stop); break;
    case kCmdServiceRestart: OnServiceCommand(lask::ServiceAction::Restart); break;
    case kCmdOpenServices: OpenServicesConsole(); break;
    case kCmdOptions: ShowOptionsMenu(); break;
    case kCmdShow: ShowFromTray(); break;
    case kCmdExit:
        g.exiting = true;
        DestroyWindow(g.hwnd);
        break;
    case kCmdOnTop:
    case kCmdSpeedHigh:
    case kCmdSpeedNormal:
    case kCmdSpeedLow:
    case kCmdSpeedPaused:
    case kCmdHardwareGraphs:
    case kCmdTray:
    case kCmdAccentHeat:
    case kCmdThemeSystem:
    case kCmdThemeLight:
    case kCmdThemeDark:
    case kCmdReplaceTaskManager: OnOptionCommand(id); break;
    case kCmdRestartAdmin: RestartElevated(0, 0); break;
    default:
        if (id > kCmdColumnBase && id < kCmdColumnBase + Index(Column::Count))
            ToggleColumn(static_cast<Column>(id - kCmdColumnBase));
        break;
    }
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g.hwnd = hwnd;
        // The first snapshot is taken while the window is built; its message is only handled once
        // the message loop runs, after the empty grid has been painted.
        g.sampler.start(hwnd, kMsgSnapshot, g.settings.refreshMs ? g.settings.refreshMs : INFINITE);
        CreateControls(hwnd);
        SetWindowSubclass(g.filter, FilterSubclassProc, 1, 0);
        SetWindowSubclass(g.options, BarButtonSubclassProc, 1, 0);
        SetWindowSubclass(g.navToggle, BarButtonSubclassProc, 1, 0);
        ShowTab(g.settings.tab >= 0 && g.settings.tab < static_cast<int>(Tab::Count) ? static_cast<Tab>(g.settings.tab)
                                                                                     : Tab::Processes);
        return 0;
    case WM_SIZE:
        if (g.nav) Layout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize = {Scale(640), Scale(360)};
        return 0;
    }
    case WM_SETFOCUS: SetFocus(ActiveGrid()); return 0;
    case WM_NOTIFY: return OnNotify(reinterpret_cast<NMHDR*>(lp));
    case lask::kMsgHeaderMenu:
        if (reinterpret_cast<HWND>(wp) == g.list) ShowColumnMenu();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == kFilterId) {
            if (HIWORD(wp) == EN_CHANGE) OnFilterChanged();
            return 0;
        }
        if (LOWORD(wp) == kOptionsId) {
            ShowOptionsMenu();
            return 0;
        }
        if (LOWORD(wp) == kNavToggleId) {
            ToggleNav();
            return 0;
        }
        OnCommand(LOWORD(wp));
        return 0;
    case WM_DRAWITEM: {
        // The gear and hamburger buttons: a Fluent glyph on the bar's own background, a soft fill on
        // hover / press only (a focus fill would stay lit after every click).
        const auto* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if (di->CtlID != kOptionsId && di->CtlID != kNavToggleId) return FALSE;
        const lask::Theme& theme = g.chrome.theme;
        lask::FillSolid(di->hDC, di->rcItem, theme.background);
        if ((di->itemState & ODS_SELECTED) || di->hwndItem == g.hotButton)
            lask::RoundFill(di->hDC, di->rcItem, Scale(4), (di->itemState & ODS_SELECTED) ? theme.headerPressed : theme.headerHot);
        SetBkMode(di->hDC, TRANSPARENT);
        SetTextColor(di->hDC, theme.text);
        const bool gear = di->CtlID == kOptionsId;
        HGDIOBJ old = SelectObject(di->hDC, gear ? g.chrome.glyphs : g.chrome.menuGlyph);
        RECT rc = di->rcItem;
        DrawTextW(di->hDC, gear ? L"\xE713" : L"\xE700", 1, &rc, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
        SelectObject(di->hDC, old);
        return TRUE;
    }
    case WM_HOTKEY:
        if (wp == kHotkeyShow) {
            if (g.hidden || GetForegroundWindow() != hwnd) ShowFromTray();
            else HideToTray();
        }
        return 0;
    case kMsgTray: OnTrayEvent(wp, lp); return 0;
    case WM_CTLCOLOREDIT:
        if (reinterpret_cast<HWND>(lp) == g.filter) {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, g.chrome.theme.text);
            SetBkColor(dc, g.chrome.theme.surface);
            return reinterpret_cast<LRESULT>(g.surface);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_CONTEXTMENU:
        if (g.tab == Tab::Services && reinterpret_cast<HWND>(wp) == ActiveGrid()) ShowServiceMenu(lp);
        else if (g.tab != Tab::Performance && reinterpret_cast<HWND>(wp) == ActiveGrid()) ShowContextMenu(ActiveGrid(), lp);
        else return DefWindowProcW(hwnd, msg, wp, lp);
        return 0;
    case kMsgSnapshot: OnSnapshot(); return 0;
    case kMsgKillDone: OnKillDone(reinterpret_cast<KillResult*>(lp)); return 0;
    case kMsgFacts: OnFacts(reinterpret_cast<lask::FactsBatch*>(lp)); return 0;
    case kMsgServiceConfigs: g.services.onConfigs(reinterpret_cast<lask::ServiceConfigBatch*>(lp)); return 0;
    case kMsgServiceDone: OnServiceDone(reinterpret_cast<ServiceResult*>(lp)); return 0;
    case WM_TIMER:
        if (wp == kTimerNavAnimation) OnNavAnimationFrame();
        if (wp == kTimerDeferredAdopt && !IsInteracting()) {
            KillTimer(hwnd, kTimerDeferredAdopt);
            AdoptLatestSnapshot();
        }
        return 0;
    case WM_SETTINGCHANGE:
        if (lask::IsThemeSettingChange(lp) || wp == SPI_SETHIGHCONTRAST) ApplyTheme();
        if (wp == SPI_SETNONCLIENTMETRICS) {
            RecreateFonts();
            Layout();
        }
        return 0;
    case WM_SYSCOLORCHANGE:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        ApplyTheme();
        return 0;
    case WM_DPICHANGED: OnDpiChanged(HIWORD(wp), *reinterpret_cast<const RECT*>(lp)); return 0;
    case WM_CLOSE:
        if (g.settings.tray && !g.exiting) HideToTray(); // Esc and the close button keep it warm
        else DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        CaptureSettings();
        lask::SaveSettings(g.settings); // the window is gone; nobody waits on this
        g.tray.hide();
        g.enricher.stop();
        g.sampler.stop();
        PostQuitMessage(0);
        return 0;
    default:
        if (msg == g.taskbarCreated && g.settings.tray) {
            ApplyTray(); // Explorer restarted and lost our icon
            return 0;
        }
        if (msg == g.activateMessage) { // a second launch: show this instance instead
            ShowFromTray();
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
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
    // Our own page-ins should win against a disk-bound hog. Needs SeIncreaseBasePriorityPrivilege
    // (elevated); silently stays normal otherwise.
    lask::RaiseOwnIoPriority();
}

struct Options {
    uint32_t killPid = 0;
    int64_t killCreateTime = 0;
    std::optional<lask::ServiceAction> serviceAction; // --service <start|stop|restart> <name>
    std::wstring serviceName;
    std::optional<bool> replaceTaskManager; // --replace-taskmgr on|off (the elevated relaunch)
    bool tray = false;                      // --tray: started with Windows, stay hidden
    bool bench = false;                     // --bench
};

// "--kill <pid> --at <createtime>" is what the elevated relaunch passes. Anything else (such as the
// "taskmgr.exe" argument Windows adds when we are registered through IFEO) is ignored.
Options ParseCommandLine() {
    Options o;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return o;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--tray") == 0) o.tray = true;
        else if (wcscmp(argv[i], L"--bench") == 0) o.bench = true;
        if (i + 1 >= argc) break;
        if (wcscmp(argv[i], L"--replace-taskmgr") == 0) o.replaceTaskManager = wcscmp(argv[i + 1], L"on") == 0;
        else if (wcscmp(argv[i], L"--kill") == 0) o.killPid = wcstoul(argv[i + 1], nullptr, 10);
        else if (wcscmp(argv[i], L"--at") == 0) o.killCreateTime = _wcstoi64(argv[i + 1], nullptr, 10);
        else if (wcscmp(argv[i], L"--service") == 0 && i + 2 < argc) {
            if (wcscmp(argv[i + 1], L"start") == 0) o.serviceAction = lask::ServiceAction::Start;
            else if (wcscmp(argv[i + 1], L"stop") == 0) o.serviceAction = lask::ServiceAction::Stop;
            else if (wcscmp(argv[i + 1], L"restart") == 0) o.serviceAction = lask::ServiceAction::Restart;
            o.serviceName = argv[i + 2];
        }
    }
    LocalFree(argv);
    return o;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    g.elevated = lask::IsElevated();
    if (g.elevated) {
        lask::EnableDebugPrivilege();
        lask::EnablePrivilege(SE_INC_BASE_PRIORITY_NAME); // Set priority: High / Realtime; our I/O priority
    }
    BoostOwnPriority();

    const Options options = ParseCommandLine();
    if (options.killPid) lask::TerminateProcessByIdentity(options.killPid, options.killCreateTime);
    if (options.replaceTaskManager && g.elevated) lask::SetReplaceTaskManager(*options.replaceTaskManager);

    // One instance: a second launch (Ctrl+Shift+Esc through IFEO, the Start menu) shows the one that
    // is already running, which is instant in tray mode. Elevated hand-overs and --bench always run.
    g.activateMessage = RegisterWindowMessageW(L"LaskManager.Activate");
    const bool handover = options.killPid || options.serviceAction || options.replaceTaskManager || options.bench;
    if (!handover)
        if (HWND running = FindWindowW(L"LaskManager.Main", nullptr)) {
            if (!options.tray) {
                DWORD pid = 0;
                GetWindowThreadProcessId(running, &pid);
                AllowSetForegroundWindow(pid);
                PostMessageW(running, g.activateMessage, 0, 0);
            }
            return 0;
        }
    g.bench = options.bench;
    lask::LoadSettings(g.settings);
    if (options.bench) g.settings.tray = false; // never hide, never stay
    lask::SetHardwareGraphs(g.settings.hardwareGraphs);
    g.performance.setView(g.settings.graphSeconds, g.settings.logicalProcessors);
    g.performance.setInterval(g.settings.refreshMs);

    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    g.chrome.dpi = GetDpiForSystem();
    g.chrome.theme = lask::ReadTheme(ThemeModeSetting(), g.settings.accentHeat);
    lask::SetAppDarkMode(g.chrome.theme.dark); // before any window exists, so menus pick it up
    g.background = CreateSolidBrush(g.chrome.theme.background);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // LoadIconMetric picks the size for the current DPI; the .ico has a hand-drawn 16 px image.
    LoadIconMetric(instance, MAKEINTRESOURCEW(IDI_LASK), LIM_LARGE, &wc.hIcon);
    LoadIconMetric(instance, MAKEINTRESOURCEW(IDI_LASK), LIM_SMALL, &g.smallIcon);
    wc.hIconSm = g.smallIcon;
    wc.hbrBackground = g.background;
    wc.lpszClassName = L"LaskManager.Main";
    if (!RegisterClassExW(&wc)) return 1;

    // The Processes tab is the first one shown; it wants window titles from the very first tick.
    g.sampler.setWantWindows(true);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, g.elevated ? L"Lask Manager (Administrator)" : L"Lask Manager",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, Scale(1120),
                                Scale(680), nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;
    // A medium-integrity second launch must be able to wake an elevated instance.
    ChangeWindowMessageFilterEx(hwnd, g.activateMessage, MSGFLT_ALLOW, nullptr);
    g.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    // Where it was last time, if that is still on a monitor.
    const lask::Settings& s = g.settings;
    if (s.hasWindow && MonitorFromRect(&s.window, MONITOR_DEFAULTTONULL)) {
        WINDOWPLACEMENT wp{};
        wp.length = sizeof wp;
        wp.showCmd = SW_HIDE;
        wp.rcNormalPosition = s.window;
        SetWindowPlacement(hwnd, &wp);
    }
    if (s.alwaysOnTop) ApplyOnTop();
    ApplyTray();
    UpdateTitle();
    if (options.tray && s.tray) {
        HideToTray(); // started with Windows: resident, invisible, not sampling
    } else {
        ShowWindow(hwnd, s.hasWindow && s.maximized ? SW_SHOWMAXIMIZED : showCommand);
        UpdateWindow(hwnd); // the empty grid is on screen before the first snapshot is handled
        if (g.bench) g.benchPainted = MsSinceProcessStart();
    }
    // The elevated relaunch after "access denied" on a service: do what was asked, on a worker,
    // with the Services tab showing so the result is visible.
    if (options.serviceAction && !options.serviceName.empty()) {
        ShowTab(Tab::Services);
        RunServiceActionAsync(options.serviceName, *options.serviceAction);
    }

    const ACCEL accelerators[] = {
        {FVIRTKEY | FCONTROL, 'F', kCmdFind},
        {FVIRTKEY, VK_F5, kCmdRefresh},
        {FVIRTKEY | FCONTROL, VK_TAB, kCmdNextTab},
        {FVIRTKEY | FCONTROL | FSHIFT, VK_TAB, kCmdNextTab},
        {FVIRTKEY | FCONTROL, '1', kCmdTabProcesses},
        {FVIRTKEY | FCONTROL, '2', kCmdTabPerformance},
        {FVIRTKEY | FCONTROL, '3', kCmdTabDetails},
        {FVIRTKEY | FCONTROL, '4', kCmdTabServices},
        {FVIRTKEY | FCONTROL, VK_OEM_COMMA, kCmdOptions},
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
