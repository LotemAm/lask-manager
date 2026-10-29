#include "performance.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <iterator>

namespace lask {
namespace {

constexpr wchar_t kDetailClass[] = L"LaskManager.PerformanceDetail";
constexpr int kListWidth = 250;   // at 96 dpi
constexpr int kRowHeight = 68;
constexpr int kSparkSamples = 60; // the list's sparklines always show the last minute
constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;

enum MenuId : UINT { kMenu30s = 1, kMenu60s, kMenu300s, kMenuOverall, kMenuLogical };

constexpr const wchar_t* kEngineNames[kGpuEngineTypes] = {
    L"Other", L"3D", L"Video Decode", L"Video Encode", L"Video Processing",
    L"Scene Assembly", L"Copy", L"Overlay", L"Crypto", L"Video Codec",
};

template <class... Args>
std::wstring Format(const wchar_t* format, Args... args) {
    wchar_t buffer[160];
    swprintf_s(buffer, format, args...);
    return buffer;
}

std::wstring Bytes(double bytes) {
    if (bytes >= kGiB) return Format(L"%.1f GB", bytes / kGiB);
    if (bytes >= 1024.0 * 1024.0) return Format(L"%.0f MB", bytes / (1024.0 * 1024.0));
    return Format(L"%.0f KB", bytes / 1024.0);
}

std::wstring Rate(double bytesPerSecond) {
    return Bytes(bytesPerSecond) + L"/s";
}

std::wstring Bits(double bitsPerSecond) {
    if (bitsPerSecond >= 1e9) return Format(L"%.1f Gbps", bitsPerSecond / 1e9);
    if (bitsPerSecond >= 1e6) return Format(L"%.1f Mbps", bitsPerSecond / 1e6);
    return Format(L"%.0f Kbps", bitsPerSecond / 1e3);
}

std::wstring Duration(uint64_t ms) {
    const uint64_t s = ms / 1000;
    return Format(L"%llu:%02llu:%02llu:%02llu", s / 86400, s / 3600 % 24, s / 60 % 60, s % 60);
}

std::wstring Percent(double value) {
    return Format(L"%.0f%%", value);
}

// The smallest 1, 2 or 5 times a power of ten at or above `value`: a graph's top edge.
double NiceCeiling(double value, double floor) {
    value = std::max(value, floor);
    const double power = std::pow(10.0, std::floor(std::log10(value)));
    for (double m : {1.0, 2.0, 5.0, 10.0})
        if (m * power >= value) return m * power;
    return 10.0 * power;
}

std::wstring RangeLabel(int seconds) {
    return seconds <= 30 ? L"30 seconds" : seconds <= 60 ? L"60 seconds" : L"5 minutes";
}

} // namespace

PerformanceTab::PerformanceTab(const Chrome& chrome, const PerfModel& model) : chrome_(chrome), model_(model) {}

void PerformanceTab::create(HWND parent, int listId) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    list_ = CreateWindowExW(0, WC_LISTVIEWW, L"",
                            WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL |
                                LVS_SHOWSELALWAYS | LVS_OWNERDATA | LVS_SHAREIMAGELISTS,
                            0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(listId)), instance, nullptr);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW column{};
    column.mask = LVCF_WIDTH;
    column.cx = chrome_.scale(kListWidth);
    ListView_InsertColumn(list_, 0, &column);
    SendMessageW(list_, WM_SETFONT, reinterpret_cast<WPARAM>(chrome_.font), FALSE);
    sizeRows();

    static const ATOM registered = [instance] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = DetailProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kDetailClass;
        return RegisterClassExW(&wc);
    }();
    if (registered)
        detail_ = CreateWindowExW(0, kDetailClass, L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, parent, nullptr, instance, this);
    graphs_[0].create(detail_, chrome_);
    graphs_[1].create(detail_, chrome_);
    applyTheme();
    refresh();
}

void PerformanceTab::sizeRows() {
    HIMAGELIST rows = ImageList_Create(1, chrome_.scale(kRowHeight), ILC_COLOR32, 1, 0);
    ListView_SetImageList(list_, rows, LVSIL_SMALL);
    if (rowHeight_) ImageList_Destroy(rowHeight_);
    rowHeight_ = rows;
}

void PerformanceTab::show(bool visible) {
    ShowWindow(list_, visible ? SW_SHOW : SW_HIDE);
    if (detail_) ShowWindow(detail_, visible ? SW_SHOW : SW_HIDE);
}

void PerformanceTab::layout(int x, int y, int width, int height) {
    const int listWidth = std::min(chrome_.scale(kListWidth), width / 2);
    SetWindowPos(list_, nullptr, x, y, listWidth, height, SWP_NOZORDER | SWP_NOACTIVATE);
    // Leave room for the vertical scroll bar, or a horizontal one appears with it.
    ListView_SetColumnWidth(list_, 0, listWidth - GetSystemMetricsForDpi(SM_CXVSCROLL, chrome_.dpi));
    if (detail_) SetWindowPos(detail_, nullptr, x + listWidth, y, std::max(0, width - listWidth), height, SWP_NOZORDER | SWP_NOACTIVATE);
}

void PerformanceTab::layoutDetail() {
    if (!detail_) return;
    RECT rc{};
    GetClientRect(detail_, &rc);
    const int pad = chrome_.scale(16);
    const int top = pad + chrome_.scale(44);
    const int statsHeight = chrome_.scale(150);
    const int bottom = std::max(top, static_cast<int>(rc.bottom) - pad - statsHeight);
    const int width = std::max(0, static_cast<int>(rc.right) - 2 * pad);
    const int gap = chrome_.scale(12);
    if (graphCount_ == 2) {
        const int h = std::max(0, (bottom - top - gap) / 2);
        MoveWindow(graphs_[0].hwnd(), pad, top, width, h, TRUE);
        MoveWindow(graphs_[1].hwnd(), pad, top + h + gap, width, h, TRUE);
    } else {
        MoveWindow(graphs_[0].hwnd(), pad, top, width, bottom - top, TRUE);
    }
    ShowWindow(graphs_[1].hwnd(), graphCount_ == 2 ? SW_SHOW : SW_HIDE);
    statsTop_ = bottom + chrome_.scale(12);
    InvalidateRect(detail_, nullptr, FALSE);
}

// ---- selection ---------------------------------------------------------------------------------

int PerformanceTab::selectedIndex() const {
    const auto& panels = model_.panels();
    for (size_t i = 0; i < panels.size(); ++i)
        if (panels[i]->kind == selectedKind_ && panels[i]->key == selectedKey_) return static_cast<int>(i);
    return panels.empty() ? -1 : 0; // the device went away: back to CPU
}

const Panel* PerformanceTab::selected() const {
    const int i = selectedIndex();
    return i < 0 ? nullptr : model_.panels()[i].get();
}

void PerformanceTab::select(int index) {
    const auto& panels = model_.panels();
    if (index < 0 || index >= static_cast<int>(panels.size())) return;
    if (panels[index]->kind == selectedKind_ && panels[index]->key == selectedKey_) return;
    selectedKind_ = panels[index]->kind;
    selectedKey_ = panels[index]->key;
    updateGraphs();
    InvalidateRect(list_, nullptr, FALSE);
    InvalidateRect(detail_, nullptr, FALSE);
}

// ---- per tick -------------------------------------------------------------------------------------

void PerformanceTab::refresh() {
    if (!list_) return;
    const int count = static_cast<int>(model_.panels().size());
    ListView_SetItemCountEx(list_, count, LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
    const int index = selectedIndex();
    if (index >= 0 && ListView_GetNextItem(list_, -1, LVNI_SELECTED) != index)
        ListView_SetItemState(list_, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    // Always: the graphs point into the panels, which may just have been replaced.
    updateGraphs();
    if (!IsWindowVisible(list_)) return;
    InvalidateRect(list_, nullptr, FALSE);
    InvalidateRect(detail_, nullptr, FALSE);
}

int PerformanceTab::samples() const {
    return std::clamp(static_cast<int>(seconds_ * 1000 / std::max(intervalMs_, 1u)), 2, Ring::kCapacity);
}

void PerformanceTab::setView(int seconds, bool logical) {
    seconds_ = seconds <= 30 ? 30 : seconds <= 60 ? 60 : 300;
    logical_ = logical;
    if (list_) updateGraphs();
}

// Paused (0) keeps the last interval, so the graphs keep their scale.
void PerformanceTab::setInterval(unsigned ms) {
    if (ms) intervalMs_ = ms;
    if (list_) updateGraphs();
}

void PerformanceTab::graphsReset() {
    for (Graph& g : graphs_) g.reset();
}

static_assert(static_cast<int>(PanelKind::Gpu) + 1 == Theme::kGraphKinds, "one graph palette per PanelKind");

void PerformanceTab::updateGraphs() {
    const Panel* p = selected();
    const Machine* m = model_.machine();
    GraphData a, b;
    const int samples_ = samples();
    a.samples = b.samples = samples_;
    if (p) a.palette = b.palette = static_cast<int>(p->kind);
    a.bottomLeft = b.bottomLeft = RangeLabel(seconds_);
    a.bottomRight = b.bottomRight = L"0";
    int count = 1;
    if (p && m) {
        switch (p->kind) {
        case PanelKind::Cpu:
            if (logical_ && !p->cores.empty()) {
                a.grid = &p->cores;
            } else {
                a.primary = &p->a;
                a.topLeft = L"% Utilization";
                a.topRight = L"100%";
            }
            break;
        case PanelKind::Memory:
            a.primary = &p->a;
            a.maximum = static_cast<float>(m->totalPhysical);
            a.topLeft = L"Memory usage";
            a.topRight = Bytes(static_cast<double>(m->totalPhysical));
            break;
        case PanelKind::Disk: {
            a.primary = &p->a;
            a.topLeft = L"Active time";
            a.topRight = L"100%";
            const double top = NiceCeiling(std::max(p->b.max(samples_), p->c.max(samples_)), 1024.0 * 1024.0);
            b.primary = &p->b;   // read
            b.secondary = &p->c; // write, dashed
            b.maximum = static_cast<float>(top);
            b.topLeft = L"Disk transfer rate (read, write dashed)";
            b.topRight = Rate(top);
            count = 2;
            break;
        }
        case PanelKind::Network: {
            const double top = NiceCeiling(std::max(p->a.max(samples_), p->b.max(samples_)), 100e3);
            a.primary = &p->b;   // receive
            a.secondary = &p->a; // send, dashed
            a.maximum = static_cast<float>(top);
            a.topLeft = L"Throughput (receive, send dashed)";
            a.topRight = Bits(top);
            break;
        }
        case PanelKind::Gpu: {
            a.primary = &p->a;
            a.topLeft = L"Utilization (busiest engine)";
            a.topRight = L"100%";
            const uint64_t limit = p->device < m->gpus.size() ? m->gpus[p->device].dedicatedLimit : 0;
            if (limit) {
                b.primary = &p->b;
                b.maximum = static_cast<float>(limit);
                b.topLeft = L"Dedicated GPU memory usage";
                b.topRight = Bytes(static_cast<double>(limit));
                count = 2;
            }
            break;
        }
        }
    }
    graphs_[0].set(std::move(a));
    graphs_[1].set(count == 2 ? std::move(b) : GraphData{});
    if (count != graphCount_) {
        graphCount_ = count;
        layoutDetail();
    }
}

// ---- text ----------------------------------------------------------------------------------------

std::wstring PerformanceTab::title(const Panel& p) const {
    const Machine* m = model_.machine();
    switch (p.kind) {
    case PanelKind::Cpu: return L"CPU";
    case PanelKind::Memory: return L"Memory";
    case PanelKind::Disk: {
        std::wstring t = Format(L"Disk %d", p.ordinal);
        if (m && p.device < m->disks.size() && !m->disks[p.device].volumes.empty()) t += L" (" + m->disks[p.device].volumes + L")";
        return t;
    }
    case PanelKind::Network: return m && p.device < m->networks.size() ? m->networks[p.device].alias : L"Network";
    case PanelKind::Gpu: return Format(L"GPU %d", p.ordinal);
    }
    return {};
}

std::wstring PerformanceTab::device(const Panel& p) const {
    const Machine* m = model_.machine();
    if (!m) return {};
    switch (p.kind) {
    case PanelKind::Cpu: return m->cpuName;
    case PanelKind::Memory: return Bytes(static_cast<double>(m->totalPhysical));
    case PanelKind::Disk: return p.device < m->disks.size() ? m->disks[p.device].model : L"";
    case PanelKind::Network: return p.device < m->networks.size() ? m->networks[p.device].description : L"";
    case PanelKind::Gpu: return p.device < m->gpus.size() ? m->gpus[p.device].name : L"";
    }
    return {};
}

std::wstring PerformanceTab::headline(const Panel& p) const {
    const Panel::Latest& l = p.latest;
    if (!l.valid) return L"—";
    const Machine* m = model_.machine();
    switch (p.kind) {
    case PanelKind::Cpu: return l.mhz ? Format(L"%.0f%%  %.2f GHz", l.cpu, l.mhz / 1000.0) : Percent(l.cpu);
    case PanelKind::Memory: {
        const double total = m ? static_cast<double>(m->totalPhysical) : 0.0;
        return Format(L"%.1f/%.1f GB (%.0f%%)", static_cast<double>(l.inUse) / kGiB, total / kGiB,
                      total > 0 ? 100.0 * static_cast<double>(l.inUse) / total : 0.0);
    }
    case PanelKind::Disk: return Percent(l.active);
    case PanelKind::Network: return L"S: " + Bits(l.sendBits) + L"  R: " + Bits(l.receiveBits);
    case PanelKind::Gpu: return Percent(l.gpu);
    }
    return {};
}

std::vector<std::pair<std::wstring, std::wstring>> PerformanceTab::stats(const Panel& p) const {
    const Panel::Latest& l = p.latest;
    const Machine* m = model_.machine();
    const std::wstring none = L"—";
    auto value = [&](const std::wstring& v) { return l.valid ? v : none; };
    std::vector<std::pair<std::wstring, std::wstring>> out;
    if (!m) return out;
    switch (p.kind) {
    case PanelKind::Cpu:
        out = {{L"Utilization", value(Percent(l.cpu))},
               {L"Speed", l.mhz ? Format(L"%.2f GHz", l.mhz / 1000.0) : none},
               {L"Processes", Format(L"%zu", l.processes)},
               {L"Threads", Format(L"%llu", l.threads)},
               {L"Handles", Format(L"%llu", l.handles)},
               {L"Up time", Duration(l.uptimeMs)},
               {L"Base speed", m->baseMhz ? Format(L"%.2f GHz", m->baseMhz / 1000.0) : none},
               {L"Sockets", Format(L"%u", m->sockets)},
               {L"Cores", Format(L"%u", m->cores)},
               {L"Logical processors", Format(L"%u", m->logical)}};
        break;
    case PanelKind::Memory:
        out = {{L"In use", Bytes(static_cast<double>(l.inUse))},
               {L"Available", Bytes(static_cast<double>(l.available))},
               {L"Committed", Bytes(static_cast<double>(l.committed)) + L" / " + Bytes(static_cast<double>(l.commitLimit))},
               {L"Cached", l.hasDetails ? Bytes(static_cast<double>(l.cached)) : none},
               {L"Paged pool", l.hasDetails ? Bytes(static_cast<double>(l.pagedPool)) : none},
               {L"Non-paged pool", l.hasDetails ? Bytes(static_cast<double>(l.nonPagedPool)) : none},
               {L"Installed", Bytes(static_cast<double>(m->totalPhysical))}};
        break;
    case PanelKind::Disk: {
        const Machine::Disk* d = p.device < m->disks.size() ? &m->disks[p.device] : nullptr;
        out = {{L"Active time", value(Percent(l.active))},
               {L"Average response time", value(Format(L"%.1f ms", l.responseMs))},
               {L"Read speed", value(Rate(l.readBps))},
               {L"Write speed", value(Rate(l.writeBps))},
               {L"Capacity", d && d->size ? Bytes(static_cast<double>(d->size)) : none},
               {L"Type", d ? d->kind : none},
               {L"Volumes", d && !d->volumes.empty() ? d->volumes : none}};
        break;
    }
    case PanelKind::Network: {
        const Machine::Network* n = p.device < m->networks.size() ? &m->networks[p.device] : nullptr;
        out = {{L"Send", value(Bits(l.sendBits))},
               {L"Receive", value(Bits(l.receiveBits))},
               {L"Connection type", n ? (n->wireless ? L"Wi-Fi" : L"Ethernet") : none},
               {L"Link speed", n && n->linkBitsPerSec ? Bits(static_cast<double>(n->linkBitsPerSec)) : none},
               {L"Adapter", n ? n->description : none}};
        break;
    }
    case PanelKind::Gpu: {
        const Machine::Gpu* g = p.device < m->gpus.size() ? &m->gpus[p.device] : nullptr;
        out.push_back({L"Utilization", value(Percent(l.gpu))});
        if (l.valid)
            for (int t = 0; t < kGpuEngineTypes; ++t)
                if (l.engine[t] >= 0) out.push_back({kEngineNames[t], Percent(l.engine[t])});
        if (g) {
            out.push_back({L"Dedicated memory", Bytes(static_cast<double>(l.dedicated)) + L" / " +
                                                    Bytes(static_cast<double>(g->dedicatedLimit))});
            out.push_back({L"Shared memory", Bytes(static_cast<double>(l.shared)) + L" / " + Bytes(static_cast<double>(g->sharedLimit))});
        }
        break;
    }
    }
    return out;
}

// ---- drawing -------------------------------------------------------------------------------------

void PerformanceTab::drawSparkline(HDC dc, const RECT& box, const Panel& p) const {
    const Theme& theme = chrome_.theme;
    const Theme::GraphColors& colors = theme.graph[static_cast<int>(p.kind)];
    FillSolid(dc, box, theme.surface);
    const Machine* m = model_.machine();
    const Ring& ring = p.kind == PanelKind::Network ? p.b : p.a;
    double maximum = 100.0;
    if (p.kind == PanelKind::Memory) maximum = m ? static_cast<double>(m->totalPhysical) : 0.0;
    else if (p.kind == PanelKind::Network) maximum = NiceCeiling(std::max(p.a.max(kSparkSamples), p.b.max(kSparkSamples)), 100e3);

    const int n = std::min(ring.size(), kSparkSamples);
    if (n >= 2 && maximum > 0) {
        POINT points[kSparkSamples + 2];
        const double width = static_cast<double>(box.right - box.left - 1);
        const double height = static_cast<double>(box.bottom - box.top - 1);
        for (int age = 0; age < n; ++age) {
            const double v = std::clamp(static_cast<double>(ring.ago(age)), 0.0, maximum);
            points[n - 1 - age] = {box.right - 1 - static_cast<LONG>(age * width / (kSparkSamples - 1)),
                                   box.bottom - 1 - static_cast<LONG>(v / maximum * height)};
        }
        points[n] = {box.right - 1, box.bottom - 1};
        points[n + 1] = {points[0].x, box.bottom - 1};
        SetDCBrushColor(dc, colors.fill);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, points, n + 2);
        SetDCPenColor(dc, colors.line);
        SelectObject(dc, GetStockObject(DC_PEN));
        Polyline(dc, points, n);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
    }
    SetDCBrushColor(dc, colors.line);
    FrameRect(dc, &box, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

// The panel list: drawn entirely by hand, like the navigation, with a sparkline per row.
LRESULT PerformanceTab::drawRow(NMLVCUSTOMDRAW* cd) {
    const Theme& theme = chrome_.theme;
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
    case CDDS_POSTPAINT: FillBelowRows(list_, cd->nmcd.hdc, theme.background); return CDRF_DODEFAULT;
    case CDDS_ITEMPREPAINT: {
        const int i = static_cast<int>(cd->nmcd.dwItemSpec);
        const auto& panels = model_.panels();
        if (i < 0 || i >= static_cast<int>(panels.size())) return CDRF_SKIPDEFAULT;
        const Panel& p = *panels[i];
        HDC dc = cd->nmcd.hdc;
        RECT rc{};
        rc.left = LVIR_BOUNDS;
        SendMessageW(list_, LVM_GETITEMRECT, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&rc));
        RECT client{};
        GetClientRect(list_, &client);
        rc.left = client.left;
        rc.right = client.right;
        FillSolid(dc, rc, theme.background);

        const bool current = i == selectedIndex();
        const bool hot = (cd->nmcd.uItemState & CDIS_HOT) != 0;
        const RECT pill = {rc.left + chrome_.scale(4), rc.top + chrome_.scale(2), rc.right - chrome_.scale(4),
                           rc.bottom - chrome_.scale(2)};
        if (current || hot) RoundFill(dc, pill, chrome_.scale(8), current ? theme.selectionInactive : theme.headerHot);
        if (current) {
            const int mid = (pill.top + pill.bottom) / 2;
            RoundFill(dc, {pill.left, mid - chrome_.scale(8), pill.left + chrome_.scale(3), mid + chrome_.scale(8)},
                      chrome_.scale(3), theme.accentText);
        }

        const RECT spark = {pill.left + chrome_.scale(12), pill.top + chrome_.scale(8), pill.left + chrome_.scale(76),
                            pill.bottom - chrome_.scale(8)};
        drawSparkline(dc, spark, p);

        // Title, then one or two lines of detail, centered as a block.
        // Devices get a line saying what they are (disk type, adapter or GPU name) above the number.
        std::wstring lines[3] = {title(p), headline(p), L""};
        const Machine* m = model_.machine();
        if (p.kind == PanelKind::Disk && m && p.device < m->disks.size()) lines[1] = m->disks[p.device].kind;
        else if (p.kind == PanelKind::Network || p.kind == PanelKind::Gpu) lines[1] = device(p);
        if (lines[1] != headline(p)) lines[2] = headline(p);
        const int lineCount = lines[2].empty() ? 2 : 3;
        const int lineHeight = chrome_.fontHeight + chrome_.scale(2);
        int y = (pill.top + pill.bottom - lineCount * lineHeight) / 2;
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ oldFont = SelectObject(dc, chrome_.bold);
        for (int line = 0; line < lineCount; ++line) {
            if (line == 1) SelectObject(dc, chrome_.font);
            SetTextColor(dc, line == 0 ? theme.text : theme.textSecondary);
            RECT text = {spark.right + chrome_.scale(12), y, pill.right - chrome_.scale(6), y + lineHeight};
            DrawTextW(dc, lines[line].c_str(), -1, &text, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            y += lineHeight;
        }
        SelectObject(dc, oldFont);
        return CDRF_SKIPDEFAULT;
    }
    default: return CDRF_DODEFAULT;
    }
}

void PerformanceTab::paintDetail(HDC dc, const RECT& rc) {
    const Theme& theme = chrome_.theme;
    FillSolid(dc, rc, theme.surface);
    const Panel* p = selected();
    if (!p) return;

    const int pad = chrome_.scale(16);
    const int titleBottom = pad + chrome_.scale(36);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, chrome_.title);
    SetTextColor(dc, theme.text);
    const std::wstring heading = title(*p);
    RECT left = {pad, pad, rc.right / 2, titleBottom};
    DrawTextW(dc, heading.c_str(), -1, &left, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, chrome_.font);
    SetTextColor(dc, theme.textSecondary);
    const std::wstring name = device(*p);
    RECT right = {rc.right / 2, pad, rc.right - pad, titleBottom};
    DrawTextW(dc, name.c_str(), -1, &right, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX | DT_END_ELLIPSIS);

    // The numbers: label over value, in columns that wrap.
    const int cellWidth = chrome_.scale(160);
    const int cellHeight = chrome_.fontHeight * 2 + chrome_.scale(12);
    int x = pad, y = statsTop_;
    for (const auto& [label, value] : stats(*p)) {
        if (x + cellWidth > rc.right - pad && x > pad) {
            x = pad;
            y += cellHeight;
        }
        RECT labelRect = {x, y, x + cellWidth - chrome_.scale(8), y + chrome_.fontHeight};
        SelectObject(dc, chrome_.font);
        SetTextColor(dc, theme.textSecondary);
        DrawTextW(dc, label.c_str(), -1, &labelRect, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        RECT valueRect = {x, y + chrome_.fontHeight + chrome_.scale(2), x + cellWidth - chrome_.scale(8), y + cellHeight};
        SelectObject(dc, chrome_.bold);
        SetTextColor(dc, theme.text);
        DrawTextW(dc, value.c_str(), -1, &valueRect, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        x += cellWidth;
    }
    SelectObject(dc, oldFont);
}

void PerformanceTab::showMenu(POINT screen) {
    const Panel* p = selected();
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenu30s, L"30 seconds");
    AppendMenuW(menu, MF_STRING, kMenu60s, L"1 minute");
    AppendMenuW(menu, MF_STRING, kMenu300s, L"5 minutes");
    CheckMenuRadioItem(menu, kMenu30s, kMenu300s, seconds_ <= 30 ? kMenu30s : seconds_ <= 60 ? kMenu60s : kMenu300s, MF_BYCOMMAND);
    if (p && p->kind == PanelKind::Cpu) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuOverall, L"Overall utilization");
        AppendMenuW(menu, MF_STRING, kMenuLogical, L"Logical processors");
        CheckMenuRadioItem(menu, kMenuOverall, kMenuLogical, logical_ ? kMenuLogical : kMenuOverall, MF_BYCOMMAND);
    }
    const UINT cmd = static_cast<UINT>(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, detail_, nullptr));
    DestroyMenu(menu);
    switch (cmd) {
    case kMenu30s: seconds_ = 30; break;
    case kMenu60s: seconds_ = 60; break;
    case kMenu300s: seconds_ = 300; break;
    case kMenuOverall: logical_ = false; break;
    case kMenuLogical: logical_ = true; break;
    default: return;
    }
    updateGraphs();
}

// ---- theme, fonts, notifications -------------------------------------------------------------------

void PerformanceTab::applyTheme() {
    if (!list_) return;
    ApplyGridTheme(list_, chrome_.theme);
    ListView_SetBkColor(list_, chrome_.theme.background);
    for (Graph& g : graphs_) g.themeChanged();
    InvalidateRect(list_, nullptr, TRUE);
    if (detail_) InvalidateRect(detail_, nullptr, FALSE);
}

void PerformanceTab::fontsChanged() {
    if (!list_) return;
    SendMessageW(list_, WM_SETFONT, reinterpret_cast<WPARAM>(chrome_.font), FALSE);
    sizeRows();
    for (Graph& g : graphs_) g.themeChanged();
    layoutDetail();
}

LRESULT PerformanceTab::onNotify(NMHDR* h) {
    switch (h->code) {
    case NM_CUSTOMDRAW: return drawRow(reinterpret_cast<NMLVCUSTOMDRAW*>(h));
    case LVN_GETDISPINFOW: {
        // For screen readers; the rows are drawn by hand.
        static std::wstring text;
        LVITEMW& item = reinterpret_cast<NMLVDISPINFOW*>(h)->item;
        if (!(item.mask & LVIF_TEXT)) return 0;
        const auto& panels = model_.panels();
        text = item.iItem >= 0 && item.iItem < static_cast<int>(panels.size()) ? title(*panels[item.iItem]) : L"";
        item.pszText = const_cast<wchar_t*>(text.c_str());
        return 0;
    }
    case LVN_ITEMCHANGED: {
        const auto* change = reinterpret_cast<NMLISTVIEW*>(h);
        if ((change->uChanged & LVIF_STATE) && (change->uNewState & LVIS_SELECTED)) select(change->iItem);
        return 0;
    }
    default: return 0;
    }
}

LRESULT CALLBACK PerformanceTab::DetailProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<PerformanceTab*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        break;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
        if (self) self->layoutDetail();
        return 0;
    case WM_PAINT:
        if (self) {
            // Double-buffered: it repaints every second.
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bitmap = CreateCompatibleBitmap(dc, std::max<LONG>(rc.right, 1), std::max<LONG>(rc.bottom, 1));
            HGDIOBJ old = SelectObject(mem, bitmap);
            self->paintDetail(mem, rc);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bitmap);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }
        break;
    case WM_CONTEXTMENU:
        if (self) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (pt.x == -1 && pt.y == -1) {
                pt = {self->chrome_.scale(40), self->chrome_.scale(60)};
                ClientToScreen(hwnd, &pt);
            }
            self->showMenu(pt);
            return 0;
        }
        break;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace lask
