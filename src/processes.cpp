#include "processes.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <numeric>

namespace lask {
namespace {

constexpr int kLookahead = 16;

struct ColumnSpec {
    const wchar_t* title;
    int width;    // at 96 dpi
    bool numeric; // right-aligned, biggest first on the first click
};

constexpr ColumnSpec kColumns[] = {
    {L"Name", 320, false}, {L"Status", 110, false}, {L"PID", 70, true},
    {L"CPU", 70, true},    {L"Memory", 100, true},  {L"Disk", 90, true},
};

constexpr const wchar_t* kGroupTitles[] = {L"Apps", L"Background processes", L"Windows processes"};

// Heat thresholds: a cell reaching threshold[i] gets heat level i + 1. CPU in percent, memory as a
// fraction of physical RAM, disk in MB/s.
constexpr double kCpuHeat[] = {0.1, 2.0, 10.0, 25.0, 50.0};
constexpr double kMemoryHeat[] = {0.002, 0.01, 0.03, 0.08, 0.15};
constexpr double kDiskHeat[] = {0.1, 1.0, 5.0, 20.0, 80.0};
static_assert(std::size(kCpuHeat) == Theme::kHeatLevels);

int Level(double value, const double (&thresholds)[Theme::kHeatLevels]) {
    int level = 0;
    while (level < Theme::kHeatLevels && value >= thresholds[level]) ++level;
    return level;
}

template <class T>
int Compare(const T& a, const T& b) {
    return a < b ? -1 : (b < a ? 1 : 0);
}

// Helpers that live in their parent's entry instead of getting one of their own.
bool IsConsoleHost(const wchar_t* name) {
    return _wcsicmp(name, L"conhost.exe") == 0 || _wcsicmp(name, L"OpenConsole.exe") == 0;
}

} // namespace

ProcessesTab::ProcessesTab(const Chrome& chrome, const FactCache& facts, const wchar_t* filter)
    : chrome_(chrome), facts_(facts), filter_(filter) {}

void ProcessesTab::create(HWND parent, int id) {
    grid_ = CreateGrid(parent, id, chrome_);
    for (int i = 0; i < static_cast<int>(Col::Count); ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT | LVCF_SUBITEM;
        c.pszText = const_cast<wchar_t*>(kColumns[i].title);
        c.cx = chrome_.scale(kColumns[i].width);
        c.fmt = kColumns[i].numeric ? LVCFMT_RIGHT : LVCFMT_LEFT;
        c.iSubItem = i;
        ListView_InsertColumn(grid_, i, &c);
    }
    SetWindowSubclass(grid_, KeySubclassProc, 2, reinterpret_cast<DWORD_PTR>(this));
    updateSortArrows();

    wchar_t dir[MAX_PATH];
    const UINT n = GetWindowsDirectoryW(dir, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        windowsDir_.assign(dir, n);
        if (windowsDir_.back() != L'\\') windowsDir_ += L'\\';
    }
}

// ---- grouping ------------------------------------------------------------------------------------

const wchar_t* ProcessesTab::friendlyName(int r) const {
    const FactEntry* f = factsAt(r);
    return f && !f->facts->description.empty() ? f->facts->description.c_str() : processAt(r).name;
}

int ProcessesTab::windowOwner(int windowIndex) const {
    auto it = pidIndex_.find(snapshot_->windows[windowIndex].pid);
    return it == pidIndex_.end() ? -1 : it->second;
}

// The design's rule: session 0, a service account, or an image under %SystemRoot%. Paths and users
// come from the fact cache; until they arrive a process is filed by its session alone.
bool ProcessesTab::isWindowsProcess(int r) const {
    if (processAt(r).sessionId == 0) return true;
    const FactEntry* entry = factsAt(r);
    if (!entry) return false;
    const ProcessFacts& f = *entry->facts;
    if (!windowsDir_.empty() && f.path.size() > windowsDir_.size() &&
        _wcsnicmp(f.path.c_str(), windowsDir_.c_str(), windowsDir_.size()) == 0)
        return true;
    return f.user == L"SYSTEM" || f.user == L"LOCAL SERVICE" || f.user == L"NETWORK SERVICE";
}

bool ProcessesTab::processHung(int r, const Entry& e) const {
    for (int k = e.firstWindow; k < e.firstWindow + e.windowCount; ++k) {
        const WindowInfo& w = snapshot_->windows[windows_[k]];
        if (w.hung && w.pid == processAt(r).pid) return true;
    }
    return false;
}

void ProcessesTab::regroup() {
    const auto& rows = snapshot_->rows;
    const int n = static_cast<int>(rows.size());

    pidIndex_.clear();
    pidIndex_.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) pidIndex_.emplace(rows[i].pid, i);
    hasWindow_.assign(static_cast<size_t>(n), 0);
    for (const WindowInfo& w : snapshot_->windows)
        if (auto it = pidIndex_.find(w.pid); it != pidIndex_.end()) hasWindow_[it->second] = 1;

    // Oldest first, so a parent has found its entry before any of its children look for it.
    order_.resize(static_cast<size_t>(n));
    std::iota(order_.begin(), order_.end(), 0);
    std::sort(order_.begin(), order_.end(), [&rows](int a, int b) {
        return rows[a].createTime != rows[b].createTime ? rows[a].createTime < rows[b].createTime
                                                        : rows[a].pid < rows[b].pid;
    });

    // A process folds into its parent's entry when it is the same program (browser helpers, a shell
    // started from a shell) or the console host of a console program. A parent created after the
    // child is a recycled PID, not a parent.
    owner_.assign(static_cast<size_t>(n), -1);
    for (int i : order_) {
        owner_[i] = i;
        auto it = pidIndex_.find(rows[i].parentPid);
        if (it == pidIndex_.end() || it->second == i) continue;
        const ProcessRow& parent = rows[it->second];
        if (parent.createTime >= rows[i].createTime || parent.sessionId != rows[i].sessionId) continue;
        const int root = owner_[it->second];
        if (_wcsicmp(rows[root].name, rows[i].name) == 0 || (!hasWindow_[i] && IsConsoleHost(rows[i].name)))
            owner_[i] = root;
    }

    entries_.clear();
    entryOf_.assign(static_cast<size_t>(n), -1);
    for (int i : order_) {
        if (owner_[i] != i) continue;
        entryOf_[i] = static_cast<int>(entries_.size());
        entries_.push_back({.root = i});
    }
    for (int i = 0; i < n; ++i) ++entries_[entryOf_[owner_[i]]].count;
    int offset = 0;
    for (Entry& e : entries_) {
        e.first = offset;
        offset += e.count;
    }
    members_.resize(static_cast<size_t>(n));
    cursor_.resize(entries_.size());
    for (size_t e = 0; e < entries_.size(); ++e) cursor_[e] = entries_[e].first;
    for (int i : order_) members_[cursor_[entryOf_[owner_[i]]]++] = i; // root first: it is the oldest

    const int windowTotal = static_cast<int>(snapshot_->windows.size());
    for (int k = 0; k < windowTotal; ++k)
        if (const int r = windowOwner(k); r >= 0) ++entries_[entryOf_[owner_[r]]].windowCount;
    offset = 0;
    for (size_t e = 0; e < entries_.size(); ++e) {
        entries_[e].firstWindow = offset;
        cursor_[e] = offset;
        offset += entries_[e].windowCount;
    }
    windows_.resize(static_cast<size_t>(offset));
    for (int k = 0; k < windowTotal; ++k)
        if (const int r = windowOwner(k); r >= 0) windows_[cursor_[entryOf_[owner_[r]]]++] = k;

    for (Entry& e : entries_) {
        bool allSuspended = true;
        for (int m = e.first; m < e.first + e.count; ++m) {
            const int r = members_[m];
            const RowRates& rate = (*rates_)[r];
            if (rate.cpu >= 0) e.cpu = std::max(e.cpu, 0.0) + rate.cpu;
            if (rate.diskBytesPerSec >= 0) e.disk = std::max(e.disk, 0.0) + rate.diskBytesPerSec;
            e.memory += rows[r].privateWorkingSet;
            allSuspended = allSuspended && rows[r].suspended;
        }
        e.suspended = allSuspended;
        for (int k = e.firstWindow; k < e.firstWindow + e.windowCount; ++k)
            e.hung = e.hung || snapshot_->windows[windows_[k]].hung;
        e.group = e.windowCount > 0 ? kApps : isWindowsProcess(e.root) ? kWindows : kBackground;
    }

    // Forget expansions of apps that have exited.
    std::erase_if(expanded_, [this](const Identity& id) {
        auto it = pidIndex_.find(id.pid);
        return it == pidIndex_.end() || processAt(it->second).createTime != id.createTime;
    });
}

bool ProcessesTab::entryMatches(int e) const {
    if (!filter_[0]) return true;
    const Entry& entry = entries_[e];
    for (int m = entry.first; m < entry.first + entry.count; ++m) {
        const FactEntry* f = factsAt(members_[m]);
        if (MatchesFilter(processAt(members_[m]), f ? f->facts.get() : nullptr, filter_)) return true;
    }
    for (int k = entry.firstWindow; k < entry.firstWindow + entry.windowCount; ++k)
        if (Contains(snapshot_->windows[windows_[k]].title, filter_)) return true;
    return false;
}

int ProcessesTab::compareEntries(const Entry& a, const Entry& b) const {
    switch (sortColumn_) {
    case Col::Name: return _wcsicmp(friendlyName(a.root), friendlyName(b.root));
    case Col::Status: return Compare(a.hung * 2 + a.suspended, b.hung * 2 + b.suspended);
    case Col::Pid: return Compare(processAt(a.root).pid, processAt(b.root).pid);
    case Col::Cpu: return Compare(a.cpu, b.cpu);
    case Col::Memory: return Compare(a.memory, b.memory);
    case Col::Disk: return Compare(a.disk, b.disk);
    default: return 0;
    }
}

int ProcessesTab::compareProcesses(int a, int b) const {
    const RowRates& ra = (*rates_)[a];
    const RowRates& rb = (*rates_)[b];
    switch (sortColumn_) {
    case Col::Name: return _wcsicmp(friendlyName(a), friendlyName(b));
    case Col::Status: return Compare(processAt(a).suspended, processAt(b).suspended);
    case Col::Pid: return Compare(processAt(a).pid, processAt(b).pid);
    case Col::Cpu: return Compare(ra.cpu, rb.cpu);
    case Col::Memory: return Compare(processAt(a).privateWorkingSet, processAt(b).privateWorkingSet);
    case Col::Disk: return Compare(ra.diskBytesPerSec, rb.diskBytesPerSec);
    default: return 0;
    }
}

void ProcessesTab::buildRows() {
    rows_.clear();
    if (!snapshot_) return;
    const bool descending = sortDescending_;
    for (uint8_t g = 0; g < kGroupCount; ++g) {
        sortedEntries_.clear();
        for (int e = 0; e < static_cast<int>(entries_.size()); ++e)
            if (entries_[e].group == g && entryMatches(e)) sortedEntries_.push_back(e);
        if (sortedEntries_.empty()) continue;
        std::sort(sortedEntries_.begin(), sortedEntries_.end(), [this, descending](int x, int y) {
            int c = compareEntries(entries_[x], entries_[y]);
            if (c == 0) c = Compare(processAt(entries_[x].root).pid, processAt(entries_[y].root).pid);
            return descending ? c > 0 : c < 0;
        });

        rows_.push_back({Kind::Group, g, -1, static_cast<int>(sortedEntries_.size())});
        for (int e : sortedEntries_) {
            const Entry& entry = entries_[e];
            rows_.push_back({Kind::Entry, g, e, entry.root});
            if (!expandable(e) || !expanded(e)) continue;
            for (int k = entry.firstWindow; k < entry.firstWindow + entry.windowCount; ++k)
                rows_.push_back({Kind::Window, g, e, windows_[k]});
            if (entry.count < 2) continue;
            sortedMembers_.assign(members_.begin() + entry.first, members_.begin() + entry.first + entry.count);
            std::sort(sortedMembers_.begin(), sortedMembers_.end(), [this, descending](int x, int y) {
                int c = compareProcesses(x, y);
                if (c == 0) c = Compare(processAt(x).pid, processAt(y).pid);
                return descending ? c > 0 : c < 0;
            });
            for (int r : sortedMembers_) rows_.push_back({Kind::Process, g, e, r});
        }
    }
}

// ---- selection -----------------------------------------------------------------------------------

std::optional<ProcessesTab::Key> ProcessesTab::selectedKey() const {
    const int i = ListView_GetNextItem(grid_, -1, LVNI_SELECTED);
    if (i < 0 || i >= static_cast<int>(rows_.size())) return std::nullopt;
    const Row& r = rows_[i];
    Key key{r.kind, r.group};
    switch (r.kind) {
    case Kind::Entry:
    case Kind::Process: key.id = identityAt(r.item); break;
    case Kind::Window: key.window = snapshot_->windows[r.item].hwnd; break;
    default: break;
    }
    return key;
}

int ProcessesTab::indexOf(const Key& key) const {
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
        const Row& r = rows_[i];
        if (r.kind != key.kind) continue;
        switch (r.kind) {
        case Kind::Group:
            if (r.group == key.group) return i;
            break;
        case Kind::Entry:
        case Kind::Process:
            if (identityAt(r.item) == key.id) return i;
            break;
        case Kind::Window:
            if (snapshot_->windows[r.item].hwnd == key.window) return i;
            break;
        }
    }
    return -1;
}

void ProcessesTab::select(int index) {
    ListView_SetItemState(grid_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    if (index < 0) return;
    ListView_SetItemState(grid_, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(grid_, index, FALSE);
}

void ProcessesTab::apply(const std::optional<Key>& keep, bool invalidateAll) {
    ListView_SetItemCountEx(grid_, static_cast<int>(rows_.size()), LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
    if (keep) {
        const int index = indexOf(*keep);
        ListView_SetItemState(grid_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        if (index >= 0) ListView_SetItemState(grid_, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    if (invalidateAll) {
        InvalidateRect(grid_, nullptr, FALSE);
    } else {
        const int top = ListView_GetTopIndex(grid_);
        ListView_RedrawItems(grid_, top, top + ListView_GetCountPerPage(grid_));
    }
}

void ProcessesTab::update(std::shared_ptr<const Snapshot> snapshot, const std::vector<RowRates>* rates) {
    const auto keep = snapshot_ ? selectedKey() : std::nullopt; // read against the old snapshot
    snapshot_ = std::move(snapshot);
    rates_ = rates;
    regroup();
    buildRows();
    // Rows can change kind under the visible range (a group gains an app), so repaint everything.
    apply(keep, true);
}

void ProcessesTab::refilter() {
    if (!snapshot_) return;
    const auto keep = selectedKey();
    buildRows();
    apply(keep, true);
}

void ProcessesTab::factsArrived() {
    if (!snapshot_) return;
    const int top = ListView_GetTopIndex(grid_);
    ListView_RedrawItems(grid_, top, top + ListView_GetCountPerPage(grid_));
}

void ProcessesTab::appendVisible(std::vector<Identity>& out) const {
    if (!snapshot_ || rows_.empty()) return;
    const int count = static_cast<int>(rows_.size());
    const int top = ListView_GetTopIndex(grid_);
    const int from = std::max(0, top - kLookahead);
    const int to = std::min(count, top + ListView_GetCountPerPage(grid_) + 1 + kLookahead);
    for (int i = from; i < to; ++i) {
        const Row& r = rows_[i];
        if (r.kind == Kind::Entry || r.kind == Kind::Process) out.push_back(identityAt(r.item));
        else if (r.kind == Kind::Window)
            if (const int owner = windowOwner(r.item); owner >= 0) out.push_back(identityAt(owner));
    }
}

std::optional<ActionTarget> ProcessesTab::selection() const {
    const int i = ListView_GetNextItem(grid_, -1, LVNI_SELECTED);
    if (i < 0 || i >= static_cast<int>(rows_.size())) return std::nullopt;
    const Row& r = rows_[i];
    ActionTarget t;
    switch (r.kind) {
    case Kind::Entry: {
        const Entry& e = entries_[r.entry];
        t.process = identityAt(e.root);
        for (int m = e.first + e.count - 1; m >= e.first; --m) t.all.push_back(identityAt(members_[m])); // youngest first
        return t;
    }
    case Kind::Process:
        t.process = identityAt(r.item);
        t.all.push_back(t.process);
        return t;
    case Kind::Window: {
        const int owner = windowOwner(r.item);
        if (owner < 0) return std::nullopt;
        t.process = identityAt(owner);
        t.all.push_back(t.process);
        t.window = snapshot_->windows[r.item].hwnd;
        return t;
    }
    default: return std::nullopt;
    }
}

// ---- expand / collapse ------------------------------------------------------------------------------

void ProcessesTab::toggle(int index) {
    if (index < 0 || index >= static_cast<int>(rows_.size())) return;
    const Row r = rows_[index];
    if (r.kind != Kind::Entry || !expandable(r.entry)) return;
    const Identity root = identityAt(r.item);
    if (!expanded_.erase(root)) expanded_.insert(root);
    buildRows();
    apply(Key{Kind::Entry, r.group, root}, true);
}

RECT ProcessesTab::chevronRect(int index) const {
    RECT icon{};
    icon.left = LVIR_ICON;
    SendMessageW(grid_, LVM_GETITEMRECT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&icon));
    return {icon.left - facts_.iconSize(), icon.top, icon.left, icon.bottom};
}

bool ProcessesTab::onChevron(int index, POINT pt) const {
    if (index < 0 || index >= static_cast<int>(rows_.size())) return false;
    const Row& r = rows_[index];
    if (r.kind != Kind::Entry || !expandable(r.entry)) return false;
    const RECT rc = chevronRect(index);
    return PtInRect(&rc, pt) != FALSE;
}

bool ProcessesTab::onKey(WPARAM key) {
    const int i = ListView_GetNextItem(grid_, -1, LVNI_SELECTED);
    if (i < 0 || i >= static_cast<int>(rows_.size())) return key == VK_LEFT || key == VK_RIGHT;
    const Row r = rows_[i];
    switch (key) {
    case VK_RIGHT:
        if (r.kind == Kind::Entry && expandable(r.entry)) {
            if (!expanded(r.entry)) toggle(i);
            else if (i + 1 < static_cast<int>(rows_.size())) select(i + 1);
        }
        return true; // never scroll sideways: the tree owns Left and Right
    case VK_LEFT:
        if (r.kind == Kind::Entry && expandable(r.entry) && expanded(r.entry)) {
            toggle(i);
        } else if (r.kind == Kind::Process || r.kind == Kind::Window) {
            for (int p = i - 1; p >= 0; --p)
                if (rows_[p].kind == Kind::Entry) {
                    select(p);
                    break;
                }
        }
        return true;
    case VK_RETURN:
        if (r.kind == Kind::Window) SwitchToWindow(snapshot_->windows[r.item].hwnd);
        else toggle(i);
        return true;
    default: return false;
    }
}

LRESULT CALLBACK ProcessesTab::KeySubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    if (msg == WM_KEYDOWN && reinterpret_cast<ProcessesTab*>(ref)->onKey(wp)) return 0;
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ---- drawing ---------------------------------------------------------------------------------------

void ProcessesTab::updateSortArrows() {
    HWND header = ListView_GetHeader(grid_);
    for (int i = 0, n = Header_GetItemCount(header); i < n; ++i) {
        HDITEMW hi{};
        hi.mask = HDI_FORMAT;
        Header_GetItem(header, i, &hi);
        hi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == static_cast<int>(sortColumn_)) hi.fmt |= sortDescending_ ? HDF_SORTDOWN : HDF_SORTUP;
        Header_SetItem(header, i, &hi);
    }
}

const wchar_t* ProcessesTab::text(const Row& row, Col col) {
    wchar_t* out = text_;
    const size_t n = std::size(text_);
    switch (row.kind) {
    case Kind::Group:
        if (col != Col::Name) return L"";
        swprintf_s(out, n, L"%s (%d)", kGroupTitles[row.group], row.item);
        return out;
    case Kind::Window: {
        const WindowInfo& w = snapshot_->windows[row.item];
        if (col == Col::Name) return w.title;
        return col == Col::Status && w.hung ? L"Not responding" : L"";
    }
    default: break;
    }

    const Entry& e = entries_[row.entry];
    const bool app = row.kind == Kind::Entry;
    const int r = row.item;
    switch (col) {
    case Col::Name:
        if (!app || e.count < 2) return friendlyName(r);
        swprintf_s(out, n, L"%s (%d)", friendlyName(r), e.count);
        return out;
    case Col::Status:
        if (app ? e.hung : processHung(r, e)) return L"Not responding";
        return (app ? e.suspended : processAt(r).suspended) ? L"Suspended" : L"";
    case Col::Pid:
        if (app && e.count > 1) return L"";
        swprintf_s(out, n, L"%u", processAt(r).pid);
        return out;
    case Col::Cpu: {
        const double cpu = app ? e.cpu : (*rates_)[r].cpu;
        if (cpu < 0) return L"";
        swprintf_s(out, n, L"%.1f%%", cpu);
        return out;
    }
    case Col::Memory: FormatMemory(app ? e.memory : processAt(r).privateWorkingSet, out, n); return out;
    case Col::Disk: {
        const double disk = app ? e.disk : (*rates_)[r].diskBytesPerSec;
        if (disk < 0) return L"";
        swprintf_s(out, n, L"%.1f MB/s", disk / (1024.0 * 1024.0));
        return out;
    }
    default: return L"";
    }
}

int ProcessesTab::heatLevel(const Row& row, Col col) const {
    if (row.kind != Kind::Entry && row.kind != Kind::Process) return 0;
    const Entry& e = entries_[row.entry];
    const bool app = row.kind == Kind::Entry;
    const int r = row.item;
    switch (col) {
    case Col::Cpu: return Level(app ? e.cpu : (*rates_)[r].cpu, kCpuHeat);
    case Col::Memory: {
        if (!snapshot_->totalPhysical) return 0;
        const uint64_t bytes = app ? e.memory : processAt(r).privateWorkingSet;
        return Level(static_cast<double>(bytes) / static_cast<double>(snapshot_->totalPhysical), kMemoryHeat);
    }
    case Col::Disk: return Level((app ? e.disk : (*rates_)[r].diskBytesPerSec) / (1024.0 * 1024.0), kDiskHeat);
    default: return 0;
    }
}

void ProcessesTab::dispInfo(NMLVDISPINFOW* info) {
    LVITEMW& item = info->item;
    if (item.iItem < 0 || item.iItem >= static_cast<int>(rows_.size())) {
        if (item.mask & LVIF_TEXT) item.pszText = const_cast<wchar_t*>(L"");
        return;
    }
    const Row& r = rows_[item.iItem];
    if (item.mask & LVIF_INDENT) item.iIndent = r.kind == Kind::Group ? 0 : r.kind == Kind::Entry ? 1 : 2;
    if (item.mask & LVIF_IMAGE) {
        int owner = r.kind == Kind::Window ? windowOwner(r.item) : r.kind == Kind::Group ? -1 : r.item;
        const FactEntry* f = owner >= 0 ? factsAt(owner) : nullptr;
        item.iImage = r.kind == Kind::Group ? I_IMAGENONE : (f ? f->image : 0);
    }
    if (item.mask & LVIF_TEXT) {
        const Col col = item.iSubItem >= 0 && item.iSubItem < static_cast<int>(Col::Count) ? static_cast<Col>(item.iSubItem)
                                                                                         : Col::Count;
        item.pszText = const_cast<wchar_t*>(text(r, col));
    }
}

void ProcessesTab::drawGroup(NMLVCUSTOMDRAW* cd, int index) {
    const Theme& theme = chrome_.theme;
    HDC dc = cd->nmcd.hdc;
    RECT rc{};
    rc.left = LVIR_BOUNDS;
    SendMessageW(grid_, LVM_GETITEMRECT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&rc));
    RECT client{};
    GetClientRect(grid_, &client);
    rc.right = std::max(rc.right, client.right);

    const bool selected = (ListView_GetItemState(grid_, index, LVIS_SELECTED) & LVIS_SELECTED) != 0;
    FillSolid(dc, rc, !selected ? theme.surface : GetFocus() == grid_ ? theme.selection : theme.selectionInactive);

    HGDIOBJ oldFont = SelectObject(dc, chrome_.bold);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, theme.text);
    RECT textRect = rc;
    textRect.left += chrome_.scale(8);
    DrawTextW(dc, text(rows_[index], Col::Name), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);
}

void ProcessesTab::drawChevron(HDC dc, int index) {
    const Row& r = rows_[index];
    if (r.kind != Kind::Entry || !expandable(r.entry)) return;
    RECT box = chevronRect(index);
    HGDIOBJ oldFont = SelectObject(dc, chrome_.smallGlyphs);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, chrome_.theme.textSecondary);
    DrawTextW(dc, expanded(r.entry) ? L"\xE70D" : L"\xE76C", 1, &box, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    SelectObject(dc, oldFont);
}

LRESULT ProcessesTab::customDraw(NMLVCUSTOMDRAW* cd) {
    const Theme& theme = chrome_.theme;
    const int i = static_cast<int>(cd->nmcd.dwItemSpec);
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
        if (i < 0 || i >= static_cast<int>(rows_.size())) return CDRF_DODEFAULT;
        if (rows_[i].kind == Kind::Group) {
            drawGroup(cd, i);
            return CDRF_SKIPDEFAULT;
        }
        PaintSelectedRow(cd, grid_, theme);
        return CDRF_NOTIFYSUBITEMDRAW | CDRF_NOTIFYPOSTPAINT | CDRF_NEWFONT;
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
        if (i < 0 || i >= static_cast<int>(rows_.size())) return CDRF_DODEFAULT;
        if (PaintSelectedRow(cd, grid_, theme)) return CDRF_NEWFONT;
        const Col col = static_cast<Col>(cd->iSubItem);
        const int level = heatLevel(rows_[i], col);
        // CLR_NONE keeps the theme's hover highlight on cells that are not shaded.
        cd->clrTextBk = level > 0 ? theme.heat[level - 1] : CLR_NONE;
        cd->clrText = theme.text;
        if (col == Col::Status) {
            const wchar_t* status = text(rows_[i], col);
            if (wcscmp(status, L"Not responding") == 0) cd->clrText = theme.warning;
        }
        return CDRF_NEWFONT;
    }
    case CDDS_ITEMPOSTPAINT:
        if (i >= 0 && i < static_cast<int>(rows_.size())) drawChevron(cd->nmcd.hdc, i);
        return CDRF_DODEFAULT;
    default: return CDRF_DODEFAULT;
    }
}

// ---- notifications -----------------------------------------------------------------------------------

LRESULT ProcessesTab::onNotify(NMHDR* h) {
    switch (h->code) {
    case LVN_GETDISPINFOW: dispInfo(reinterpret_cast<NMLVDISPINFOW*>(h)); return 0;
    case NM_CUSTOMDRAW: return customDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(h));
    case LVN_ODFINDITEMW: {
        // Type-ahead over the app rows' names.
        const auto* find = reinterpret_cast<NMLVFINDITEMW*>(h);
        if (!(find->lvfi.flags & LVFI_STRING) || !find->lvfi.psz || rows_.empty()) return -1;
        const size_t len = wcslen(find->lvfi.psz);
        const int count = static_cast<int>(rows_.size());
        int i = find->iStart < 0 ? 0 : find->iStart % count;
        for (int n = 0; n < count; ++n, i = (i + 1) % count)
            if (rows_[i].kind == Kind::Entry && _wcsnicmp(friendlyName(rows_[i].item), find->lvfi.psz, len) == 0) return i;
        return -1;
    }
    case LVN_COLUMNCLICK: {
        const int column = reinterpret_cast<NMLISTVIEW*>(h)->iSubItem;
        if (column < 0 || column >= static_cast<int>(Col::Count)) return 0;
        const auto c = static_cast<Col>(column);
        if (c == sortColumn_) {
            sortDescending_ = !sortDescending_;
        } else {
            sortColumn_ = c;
            sortDescending_ = kColumns[column].numeric;
        }
        updateSortArrows();
        refilter();
        return 0;
    }
    case NM_CLICK: {
        const auto* click = reinterpret_cast<NMITEMACTIVATE*>(h);
        if (onChevron(click->iItem, click->ptAction)) toggle(click->iItem);
        return 0;
    }
    case NM_DBLCLK: {
        const auto* click = reinterpret_cast<NMITEMACTIVATE*>(h);
        const int i = click->iItem;
        if (i < 0 || i >= static_cast<int>(rows_.size()) || onChevron(i, click->ptAction)) return 0;
        if (rows_[i].kind == Kind::Window) SwitchToWindow(snapshot_->windows[rows_[i].item].hwnd);
        else toggle(i);
        return 0;
    }
    default: return 0;
    }
}

} // namespace lask
