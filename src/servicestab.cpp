#include "servicestab.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <iterator>

namespace lask {
namespace {

struct ColumnSpec {
    const wchar_t* title;
    int width; // at 96 dpi
    bool numeric;
};

constexpr ColumnSpec kColumns[] = {
    {L"Name", 200, false},    {L"PID", 64, true},           {L"Description", 280, false}, {L"Status", 110, false},
    {L"Startup type", 170, false}, {L"Group", 140, false}, {L"Details", 420, false},
};

constexpr wchar_t kStatusIndent[] = L"     "; // room for the dot drawn in front of the status text

template <class T>
int Compare(const T& a, const T& b) {
    return a < b ? -1 : (b < a ? 1 : 0);
}

// Pending states sort between running and stopped.
int StateRank(DWORD state) {
    switch (state) {
    case SERVICE_RUNNING: return 0;
    case SERVICE_STOPPED: return 2;
    default: return 1;
    }
}

} // namespace

ServicesTab::ServicesTab(const Chrome& chrome, const wchar_t* filter) : chrome_(chrome), filter_(filter) {}

void ServicesTab::create(HWND parent, int id, UINT configMessage) {
    parent_ = parent;
    configMessage_ = configMessage;
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
    ApplyGridTheme(grid_, chrome_.theme);
    updateSortArrows();
}

void ServicesTab::updateSortArrows() {
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

void ServicesTab::update(std::shared_ptr<const ServiceList> services) {
    if (!grid_ || !services || services == services_) return; // the same list until the next refresh
    services_ = std::move(services);
    rebuild();
    requestConfigs();
}

void ServicesTab::refilter() {
    if (grid_ && services_) rebuild();
}

// Asks a worker for the configuration of every service not seen before: the first time, all of
// them (a few hundred, well under a second); later, only services that were installed meanwhile.
void ServicesTab::requestConfigs() {
    std::vector<std::wstring> names;
    for (const ServiceInfo& s : *services_)
        if (!configs_.contains(s.name) && pending_.insert(s.name).second) names.push_back(s.name);
    if (!names.empty()) LoadServiceConfigs(parent_, configMessage_, std::move(names));
}

void ServicesTab::onConfigs(ServiceConfigBatch* batch) {
    std::unique_ptr<ServiceConfigBatch> owned(batch);
    for (auto& [name, config] : *owned) {
        pending_.erase(name);
        configs_.insert_or_assign(name, std::move(config));
    }
    if (!grid_ || !services_) return;
    // Pointers into configs_ stay valid (unordered_map never moves elements); attach the new ones.
    for (Row& r : rows_)
        if (!r.config)
            if (auto it = configs_.find(r.info->name); it != configs_.end()) r.config = &it->second;
    const int top = ListView_GetTopIndex(grid_);
    ListView_RedrawItems(grid_, top, top + ListView_GetCountPerPage(grid_));
}

std::optional<ServiceInfo> ServicesTab::selection() const {
    const int i = grid_ ? ListView_GetNextItem(grid_, -1, LVNI_SELECTED) : -1;
    if (i < 0 || i >= static_cast<int>(rows_.size())) return std::nullopt;
    return *rows_[i].info;
}

void ServicesTab::rebuild() {
    const auto keep = selection();
    rows_.clear();
    rows_.reserve(services_->size());
    for (const ServiceInfo& s : *services_) {
        auto it = configs_.find(s.name);
        const ServiceConfig* config = it == configs_.end() ? nullptr : &it->second;
        if (filter_[0]) {
            wchar_t pid[16];
            swprintf_s(pid, L"%u", s.pid);
            const bool match = Contains(s.name.c_str(), filter_) || Contains(s.displayName.c_str(), filter_) ||
                               (s.pid && Contains(pid, filter_)) ||
                               (config && (Contains(config->description.c_str(), filter_) ||
                                           Contains(config->group.c_str(), filter_)));
            if (!match) continue;
        }
        rows_.push_back({&s, config});
    }

    const Col column = sortColumn_;
    const bool descending = sortDescending_;
    auto configText = [](const Row& r, std::wstring ServiceConfig::*field) {
        return r.config ? (r.config->*field).c_str() : L"";
    };
    std::sort(rows_.begin(), rows_.end(), [&](const Row& a, const Row& b) {
        int c = 0;
        switch (column) {
        case Col::Pid: c = Compare(a.info->pid, b.info->pid); break;
        case Col::Description: c = _wcsicmp(a.info->displayName.c_str(), b.info->displayName.c_str()); break;
        case Col::Status: c = Compare(StateRank(a.info->state), StateRank(b.info->state)); break;
        case Col::Startup: c = _wcsicmp(configText(a, &ServiceConfig::startup), configText(b, &ServiceConfig::startup)); break;
        case Col::Group: c = _wcsicmp(configText(a, &ServiceConfig::group), configText(b, &ServiceConfig::group)); break;
        case Col::Details: c = _wcsicmp(configText(a, &ServiceConfig::description), configText(b, &ServiceConfig::description)); break;
        default: break;
        }
        if (c == 0) c = _wcsicmp(a.info->name.c_str(), b.info->name.c_str());
        return descending ? c > 0 : c < 0;
    });

    ListView_SetItemCountEx(grid_, static_cast<int>(rows_.size()), LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
    ListView_SetItemState(grid_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    if (keep)
        for (int i = 0; i < static_cast<int>(rows_.size()); ++i)
            if (rows_[i].info->name == keep->name) {
                ListView_SetItemState(grid_, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                break;
            }
    InvalidateRect(grid_, nullptr, FALSE);
}

const wchar_t* ServicesTab::text(const Row& r, Col col) {
    switch (col) {
    case Col::Name: return r.info->name.c_str();
    case Col::Pid:
        if (!r.info->pid) return L"";
        swprintf_s(text_, L"%u", r.info->pid);
        return text_;
    case Col::Description: return r.info->displayName.c_str();
    case Col::Status:
        swprintf_s(text_, L"%s%s", kStatusIndent, ServiceStateName(r.info->state));
        return text_;
    case Col::Startup: return r.config ? r.config->startup.c_str() : L"";
    case Col::Group: return r.config ? r.config->group.c_str() : L"";
    case Col::Details: return r.config ? r.config->description.c_str() : L"";
    default: return L"";
    }
}

LRESULT ServicesTab::customDraw(NMLVCUSTOMDRAW* cd) {
    const Theme& theme = chrome_.theme;
    const int i = static_cast<int>(cd->nmcd.dwItemSpec);
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
        PaintSelectedRow(cd, grid_, theme);
        return CDRF_NEWFONT | CDRF_NOTIFYPOSTPAINT;
    case CDDS_ITEMPOSTPAINT: {
        // The status dot, in front of the status text: green running, grey stopped, amber in between.
        if (i < 0 || i >= static_cast<int>(rows_.size())) return CDRF_DODEFAULT;
        RECT cell{};
        if (!ListView_GetSubItemRect(grid_, i, static_cast<int>(Col::Status), LVIR_BOUNDS, &cell)) return CDRF_DODEFAULT;
        const DWORD state = rows_[i].info->state;
        const COLORREF color = state == SERVICE_RUNNING ? theme.good : state == SERVICE_STOPPED ? theme.textSecondary : theme.warning;
        const int d = chrome_.scale(8);
        const int x = cell.left + chrome_.scale(8);
        const int y = (cell.top + cell.bottom - d) / 2;
        HDC dc = cd->nmcd.hdc;
        SetDCBrushColor(dc, color);
        SetDCPenColor(dc, color);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(DC_PEN));
        Ellipse(dc, x, y, x + d, y + d);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        return CDRF_DODEFAULT;
    }
    default: return CDRF_DODEFAULT;
    }
}

LRESULT ServicesTab::onNotify(NMHDR* h) {
    switch (h->code) {
    case LVN_GETDISPINFOW: {
        LVITEMW& item = reinterpret_cast<NMLVDISPINFOW*>(h)->item;
        if (!(item.mask & LVIF_TEXT)) return 0;
        if (item.iItem < 0 || item.iItem >= static_cast<int>(rows_.size()) || item.iSubItem < 0 ||
            item.iSubItem >= static_cast<int>(Col::Count)) {
            item.pszText = const_cast<wchar_t*>(L"");
            return 0;
        }
        item.pszText = const_cast<wchar_t*>(text(rows_[item.iItem], static_cast<Col>(item.iSubItem)));
        return 0;
    }
    case NM_CUSTOMDRAW: return customDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(h));
    case LVN_ODFINDITEMW: {
        const auto* find = reinterpret_cast<NMLVFINDITEMW*>(h);
        if (!(find->lvfi.flags & LVFI_STRING) || !find->lvfi.psz || rows_.empty()) return -1;
        const size_t len = wcslen(find->lvfi.psz);
        const int count = static_cast<int>(rows_.size());
        int i = find->iStart < 0 ? 0 : find->iStart % count;
        for (int n = 0; n < count; ++n, i = (i + 1) % count)
            if (_wcsnicmp(rows_[i].info->name.c_str(), find->lvfi.psz, len) == 0) return i;
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
        if (services_) rebuild();
        return 0;
    }
    default: return 0;
    }
}

} // namespace lask
