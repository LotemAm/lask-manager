// Lask Manager - milestone 1: open instantly, list processes, end the selected one.
//
// Thread model (see docs/design.md): the UI thread owns the window and only ever reads the newest
// snapshot; the sampler thread does the one kernel query per tick; kills run on a throwaway worker.

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
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr UINT kMsgSnapshot = WM_APP + 1;
constexpr UINT kMsgKillDone = WM_APP + 2;
constexpr UINT_PTR kTimerDeferredAdopt = 1;
constexpr int kListId = 100;
constexpr unsigned kDefaultIntervalMs = 1000;

enum Command : int { kCmdEndTask = 1001, kCmdRefresh, kCmdRestartAdmin };

enum class Column : int { Name, Pid, Cpu, Memory, Threads, Handles, Count };

struct ColumnSpec {
    const wchar_t* title;
    int width; // at 96 dpi
    bool numeric;
};

constexpr ColumnSpec kColumns[] = {
    {L"Name", 280, false}, {L"PID", 70, true},     {L"CPU", 70, true},
    {L"Memory", 110, true}, {L"Threads", 70, true}, {L"Handles", 80, true},
};
static_assert(std::size(kColumns) == static_cast<size_t>(Column::Count));

struct ViewRow {
    const lask::ProcessRow* row; // points into App::current, rebuilt whenever it changes
    double cpu;                  // percent of all logical CPUs; < 0 until two snapshots exist
};

struct Identity {
    uint32_t pid;
    int64_t createTime;
};

struct KillResult {
    uint32_t pid;
    int64_t createTime;
    DWORD error;
};

struct App {
    HWND hwnd = nullptr;
    HWND list = nullptr;
    HFONT font = nullptr;
    HBRUSH background = nullptr;
    UINT dpi = 96;
    bool elevated = false;
    lask::Theme theme;
    lask::Sampler sampler;
    std::shared_ptr<const lask::Snapshot> current;
    std::shared_ptr<const lask::Snapshot> previous;
    std::vector<ViewRow> view;
    Column sortColumn = Column::Cpu;
    bool sortDescending = true;
    bool menuOpen = false;
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

// ---- view: the sorted, rate-annotated list the grid shows ------------------------------------

void SortView() {
    const Column column = g.sortColumn;
    const bool descending = g.sortDescending;
    std::sort(g.view.begin(), g.view.end(), [column, descending](const ViewRow& a, const ViewRow& b) {
        int c = 0;
        switch (column) {
        case Column::Name: c = _wcsicmp(a.row->name, b.row->name); break;
        case Column::Pid: c = Compare(a.row->pid, b.row->pid); break;
        case Column::Cpu: c = Compare(a.cpu, b.cpu); break;
        case Column::Memory: c = Compare(a.row->privateWorkingSet, b.row->privateWorkingSet); break;
        case Column::Threads: c = Compare(a.row->threadCount, b.row->threadCount); break;
        case Column::Handles: c = Compare(a.row->handleCount, b.row->handleCount); break;
        default: break;
        }
        if (c == 0) c = Compare(a.row->pid, b.row->pid); // deterministic order for ties
        return descending ? c > 0 : c < 0;
    });
}

void RebuildView() {
    g.view.clear();
    if (!g.current) return;
    const lask::Snapshot& cur = *g.current;

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
        g.view.push_back({&r, cpu});
    }
    SortView();
}

std::optional<Identity> SelectedIdentity() {
    const int i = ListView_GetNextItem(g.list, -1, LVNI_SELECTED);
    if (i < 0 || i >= static_cast<int>(g.view.size())) return std::nullopt;
    return Identity{g.view[i].row->pid, g.view[i].row->createTime};
}

int IndexOf(const Identity& id) {
    for (size_t i = 0; i < g.view.size(); ++i)
        if (g.view[i].row->pid == id.pid && g.view[i].row->createTime == id.createTime) return static_cast<int>(i);
    return -1;
}

void RestoreSelection(const std::optional<Identity>& selected) {
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

// Only the visible rows are invalidated; the control asks for their text through LVN_GETDISPINFO.
void ApplyView() {
    const auto selected = SelectedIdentity();
    RebuildView();
    ListView_SetItemCountEx(g.list, static_cast<int>(g.view.size()), LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
    RestoreSelection(selected);
    RedrawVisibleRows();
}

bool IsInteracting() {
    return g.menuOpen || (GetKeyState(VK_LBUTTON) & 0x8000) || (GetKeyState(VK_RBUTTON) & 0x8000);
}

void AdoptLatestSnapshot() {
    auto latest = g.sampler.latest();
    if (!latest || latest == g.current) return;
    g.previous = std::move(g.current);
    g.current = std::move(latest);
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

// ---- grid -------------------------------------------------------------------------------------

void FormatMemory(uint64_t bytes, wchar_t* out, size_t count) {
    swprintf_s(out, count, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

void OnGetDispInfo(NMLVDISPINFOW* info) {
    LVITEMW& item = info->item;
    if (!(item.mask & LVIF_TEXT)) return;
    if (item.iItem < 0 || item.iItem >= static_cast<int>(g.view.size())) {
        item.pszText = const_cast<wchar_t*>(L"");
        return;
    }
    const ViewRow& v = g.view[item.iItem];
    wchar_t* out = g.text;
    const size_t n = std::size(g.text);
    switch (static_cast<Column>(item.iSubItem)) {
    case Column::Name: item.pszText = const_cast<wchar_t*>(v.row->name); return;
    case Column::Pid: swprintf_s(out, n, L"%u", v.row->pid); break;
    case Column::Cpu:
        if (v.cpu < 0) wcscpy_s(out, n, L"—");
        else swprintf_s(out, n, L"%.1f%%", v.cpu);
        break;
    case Column::Memory: FormatMemory(v.row->privateWorkingSet, out, n); break;
    case Column::Threads: swprintf_s(out, n, L"%u", v.row->threadCount); break;
    case Column::Handles: swprintf_s(out, n, L"%u", v.row->handleCount); break;
    default: out[0] = L'\0'; break;
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

void UpdateSortArrows() {
    HWND header = ListView_GetHeader(g.list);
    for (int i = 0; i < static_cast<int>(Column::Count); ++i) {
        HDITEMW hi{};
        hi.mask = HDI_FORMAT;
        Header_GetItem(header, i, &hi);
        hi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == static_cast<int>(g.sortColumn)) hi.fmt |= g.sortDescending ? HDF_SORTDOWN : HDF_SORTUP;
        Header_SetItem(header, i, &hi);
    }
}

void OnColumnClick(int column) {
    if (column < 0 || column >= static_cast<int>(Column::Count)) return;
    const auto c = static_cast<Column>(column);
    if (c == g.sortColumn) {
        g.sortDescending = !g.sortDescending;
    } else {
        g.sortColumn = c;
        g.sortDescending = kColumns[column].numeric; // biggest first for numbers, A-Z for names
    }
    UpdateSortArrows();
    const auto selected = SelectedIdentity();
    SortView();
    RestoreSelection(selected);
    RedrawVisibleRows();
}

// The header control is the one piece the DarkMode_Explorer theme leaves light, so draw it ourselves
// through its documented NM_CUSTOMDRAW. The header's parent is the ListView, hence the subclass.
void FillHeaderRect(HDC dc, const RECT& rc) {
    HBRUSH fill = CreateSolidBrush(g.theme.headerBackground);
    FillRect(dc, &rc, fill);
    DeleteObject(fill);
    RECT bottom = rc;
    bottom.top = rc.bottom - 1;
    HBRUSH line = CreateSolidBrush(g.theme.border);
    FillRect(dc, &bottom, line);
    DeleteObject(line);
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
        if (left < client.right) FillHeaderRect(cd->hdc, {left, client.top, client.right, client.bottom});
        return CDRF_DODEFAULT;
    }
    case CDDS_ITEMPREPAINT: {
        HDC dc = cd->hdc;
        const RECT rc = cd->rc;
        FillHeaderRect(dc, rc);

        wchar_t title[64] = {};
        HDITEMW hi{};
        hi.mask = HDI_TEXT | HDI_FORMAT;
        hi.pszText = title;
        hi.cchTextMax = static_cast<int>(std::size(title));
        Header_GetItem(cd->hdr.hwndFrom, static_cast<int>(cd->dwItemSpec), &hi);

        HGDIOBJ oldFont = SelectObject(dc, g.font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, g.theme.textSecondary);
        RECT textRect = rc;
        textRect.left += Scale(6);
        textRect.right -= Scale(6);
        const UINT format = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX |
                            ((hi.fmt & HDF_RIGHT) ? DT_RIGHT : DT_LEFT);
        DrawTextW(dc, title, -1, &textRect, format);

        if (hi.fmt & (HDF_SORTUP | HDF_SORTDOWN)) {
            const int cx = (rc.left + rc.right) / 2;
            const int top = rc.top + Scale(2);
            const int s = Scale(3);
            POINT up[3] = {{cx - s, top + s}, {cx + s, top + s}, {cx, top}};
            POINT down[3] = {{cx - s, top}, {cx + s, top}, {cx, top + s}};
            HBRUSH arrowBrush = CreateSolidBrush(g.theme.textSecondary);
            HPEN arrowPen = CreatePen(PS_SOLID, 1, g.theme.textSecondary);
            HGDIOBJ oldBrush = SelectObject(dc, arrowBrush);
            HGDIOBJ oldPen = SelectObject(dc, arrowPen);
            Polygon(dc, (hi.fmt & HDF_SORTUP) ? up : down, 3);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(arrowBrush);
            DeleteObject(arrowPen);
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
        if (h->code == NM_CUSTOMDRAW && h->hwndFrom == ListView_GetHeader(hwnd))
            return DrawHeader(reinterpret_cast<NMCUSTOMDRAW*>(lp));
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK HeaderSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_ERASEBKGND) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillHeaderRect(reinterpret_cast<HDC>(wp), rc);
        return 1;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void RecreateFont() {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, g.dpi);
    HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
    if (g.list) SendMessageW(g.list, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    if (g.font) DeleteObject(g.font);
    g.font = font;
}

void ApplyTheme() {
    g.theme = lask::ReadTheme();
    lask::SetAppDarkMode(g.theme.dark);
    lask::ApplyWindowTheme(g.hwnd, g.theme);

    // ItemsView is the theme Explorer's own list uses: no column separator lines, dark scrollbars.
    lask::AllowDarkModeForWindow(g.list, g.theme.dark);
    SetWindowTheme(g.list, g.theme.dark ? L"DarkMode_ItemsView" : L"ItemsView", nullptr);
    ListView_SetBkColor(g.list, g.theme.surface);
    ListView_SetTextBkColor(g.list, g.theme.surface);
    ListView_SetTextColor(g.list, g.theme.text);

    HBRUSH old = g.background;
    g.background = CreateSolidBrush(g.theme.background);
    SetClassLongPtrW(g.hwnd, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(g.background));
    if (old) DeleteObject(old);

    RedrawWindow(g.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

void CreateList(HWND parent) {
    g.list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL |
                                 LVS_SHOWSELALWAYS,
                             0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListId)),
                             GetModuleHandleW(nullptr), nullptr);
    ListView_SetExtendedListViewStyle(g.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP |
                                                  LVS_EX_LABELTIP);
    for (int i = 0; i < static_cast<int>(Column::Count); ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.pszText = const_cast<wchar_t*>(kColumns[i].title);
        c.cx = Scale(kColumns[i].width);
        c.fmt = kColumns[i].numeric ? LVCFMT_RIGHT : LVCFMT_LEFT;
        ListView_InsertColumn(g.list, i, &c);
    }
    SetWindowSubclass(g.list, ListSubclassProc, 1, 0);
    SetWindowSubclass(ListView_GetHeader(g.list), HeaderSubclassProc, 1, 0);
    RecreateFont();
    ApplyTheme();
    UpdateSortArrows();
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
    const Identity id = *selected;
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
    case LVN_COLUMNCLICK: OnColumnClick(reinterpret_cast<NMLISTVIEW*>(h)->iSubItem); return 0;
    case LVN_KEYDOWN:
        switch (reinterpret_cast<NMLVKEYDOWN*>(h)->wVKey) {
        case VK_DELETE: EndSelectedTask(); break;
        case VK_F5: g.sampler.requestNow(); break;
        case VK_ESCAPE: PostMessageW(g.hwnd, WM_CLOSE, 0, 0); break;
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
        CreateList(hwnd);
        return 0;
    case WM_SIZE:
        if (g.list) SetWindowPos(g.list, nullptr, 0, 0, LOWORD(lp), HIWORD(lp), SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize = {Scale(480), Scale(320)};
        return 0;
    }
    case WM_SETFOCUS: SetFocus(g.list); return 0;
    case WM_NOTIFY: return OnNotify(reinterpret_cast<NMHDR*>(lp));
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kCmdEndTask: EndSelectedTask(); break;
        case kCmdRefresh: g.sampler.requestNow(); break;
        case kCmdRestartAdmin: RestartElevated(0, 0); break;
        default: break;
        }
        return 0;
    case WM_CONTEXTMENU:
        if (reinterpret_cast<HWND>(wp) == g.list) ShowContextMenu(lp);
        return 0;
    case kMsgSnapshot: OnSnapshot(); return 0;
    case kMsgKillDone: OnKillDone(reinterpret_cast<KillResult*>(lp)); return 0;
    case WM_TIMER:
        if (wp == kTimerDeferredAdopt && !IsInteracting()) {
            KillTimer(hwnd, kTimerDeferredAdopt);
            AdoptLatestSnapshot();
        }
        return 0;
    case WM_SETTINGCHANGE:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        ApplyTheme();
        return 0;
    case WM_DPICHANGED: {
        g.dpi = HIWORD(wp);
        const RECT* rc = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        RecreateFont();
        return 0;
    }
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
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

    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_LISTVIEW_CLASSES};
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
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, Scale(920), Scale(600), nullptr,
                                nullptr, instance, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, showCommand);
    UpdateWindow(hwnd); // the empty grid is on screen before the first snapshot is even requested

    g.sampler.start(hwnd, kMsgSnapshot, kDefaultIntervalMs);

    MSG m{};
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return static_cast<int>(m.wParam);
}
