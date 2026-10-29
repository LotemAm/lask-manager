#include "graph.h"

#include <d2d1.h>
#include <dwrite.h>

#include <algorithm>
#include <cmath>

namespace lask {
namespace {

constexpr wchar_t kClass[] = L"LaskManager.Graph";
constexpr float kFillAlpha = 0.18f; // the area under the line: the line color, mostly transparent
bool hardwareGraphs = false;        // UI thread only, like every Graph

struct Factories {
    ID2D1Factory* d2d = nullptr;
    IDWriteFactory* dwrite = nullptr;
};

// Loaded on first use, not linked: Direct2D and DirectWrite pull in a dozen DLLs, and only the
// Performance tab needs them.
const Factories& Get() {
    static const Factories factories = [] {
        Factories f;
        using D2DCreate = HRESULT(WINAPI*)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS*, void**);
        using DWriteCreate = HRESULT(WINAPI*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);
        if (HMODULE d2d = LoadLibraryExW(L"d2d1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
            if (auto create = reinterpret_cast<D2DCreate>(GetProcAddress(d2d, "D2D1CreateFactory")))
                create(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, reinterpret_cast<void**>(&f.d2d));
        if (HMODULE dw = LoadLibraryExW(L"dwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
            if (auto create = reinterpret_cast<DWriteCreate>(GetProcAddress(dw, "DWriteCreateFactory")))
                create(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&f.dwrite));
        return f;
    }();
    return factories;
}

D2D1_COLOR_F Color(COLORREF c, float alpha = 1.0f) {
    return {GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, alpha};
}

template <class T>
void Release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

struct Brushes {
    ID2D1SolidColorBrush* line = nullptr;
    ID2D1SolidColorBrush* fill = nullptr;
    ID2D1SolidColorBrush* grid = nullptr;
    ID2D1SolidColorBrush* text = nullptr;
    ID2D1StrokeStyle* dashed = nullptr;

    Brushes(ID2D1RenderTarget* t, const Theme& theme, const Theme::GraphColors& colors) {
        t->CreateSolidColorBrush(Color(colors.line), &line);
        t->CreateSolidColorBrush(Color(colors.line, kFillAlpha), &fill);
        t->CreateSolidColorBrush(Color(colors.grid), &grid);
        t->CreateSolidColorBrush(Color(theme.textSecondary), &text);
        D2D1_STROKE_STYLE_PROPERTIES dash = {D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                             D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_DASH, 0.0f};
        Get().d2d->CreateStrokeStyle(dash, nullptr, 0, &dashed);
    }
    ~Brushes() {
        Release(line);
        Release(fill);
        Release(grid);
        Release(text);
        Release(dashed);
    }
    bool ok() const { return line && fill && grid && text && dashed; }
};

// One series as a path: the newest sample at the right edge, `samples` of them across the width.
ID2D1PathGeometry* SeriesPath(const Ring& ring, const D2D1_RECT_F& r, float maximum, int samples, bool closed) {
    const int n = std::min(ring.size(), samples);
    if (n < 2 || maximum <= 0) return nullptr;
    ID2D1PathGeometry* path = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(Get().d2d->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) {
        Release(path);
        return nullptr;
    }
    const float step = (r.right - r.left) / static_cast<float>(samples - 1);
    const float height = r.bottom - r.top;
    auto point = [&](int age) {
        const float v = std::clamp(ring.ago(age), 0.0f, maximum);
        return D2D1::Point2F(r.right - static_cast<float>(age) * step, r.bottom - v / maximum * height);
    };
    const D2D1_POINT_2F oldest = point(n - 1);
    sink->BeginFigure(closed ? D2D1::Point2F(oldest.x, r.bottom) : oldest,
                      closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    for (int age = closed ? n - 1 : n - 2; age >= 0; --age) sink->AddLine(point(age));
    if (closed) sink->AddLine(D2D1::Point2F(r.right, r.bottom));
    sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    sink->Close();
    sink->Release();
    return path;
}

void DrawPlot(ID2D1RenderTarget* t, const Brushes& b, const D2D1_RECT_F& r, const Ring* primary,
              const Ring* secondary, float maximum, int samples, int columns) {
    const float w = r.right - r.left, h = r.bottom - r.top;
    for (int k = 1; k < 5; ++k) { // a faint grid: 5 rows, `columns` columns
        const float y = std::floor(r.top + h * static_cast<float>(k) / 5.0f) + 0.5f;
        t->DrawLine({r.left, y}, {r.right, y}, b.grid, 1.0f);
    }
    for (int k = 1; k < columns; ++k) {
        const float x = std::floor(r.left + w * static_cast<float>(k) / static_cast<float>(columns)) + 0.5f;
        t->DrawLine({x, r.top}, {x, r.bottom}, b.grid, 1.0f);
    }
    if (primary) {
        if (ID2D1PathGeometry* area = SeriesPath(*primary, r, maximum, samples, true)) {
            t->FillGeometry(area, b.fill);
            area->Release();
        }
        if (ID2D1PathGeometry* line = SeriesPath(*primary, r, maximum, samples, false)) {
            t->DrawGeometry(line, b.line, 1.5f);
            line->Release();
        }
    }
    if (secondary)
        if (ID2D1PathGeometry* line = SeriesPath(*secondary, r, maximum, samples, false)) {
            t->DrawGeometry(line, b.line, 1.5f, b.dashed);
            line->Release();
        }
    t->DrawRectangle({r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f}, b.line, 1.0f);
}

} // namespace

Graph::~Graph() {
    releaseTarget();
    Release(labels_);
}

HWND Graph::create(HWND parent, const Chrome& chrome) {
    chrome_ = &chrome;
    static const ATOM registered = [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        return RegisterClassExW(&wc);
    }();
    if (!registered) return nullptr;
    hwnd_ = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent, nullptr, GetModuleHandleW(nullptr), this);
    return hwnd_;
}

void Graph::set(GraphData data) {
    data_ = std::move(data);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void Graph::themeChanged() {
    Release(labels_);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void SetHardwareGraphs(bool hardware) {
    hardwareGraphs = hardware;
}

void Graph::reset() {
    releaseTarget();
    themeChanged();
}

void Graph::releaseTarget() {
    Release(target_);
}

bool Graph::ensureTarget() {
    const Factories& f = Get();
    if (!f.d2d) return false;
    if (!labels_ && f.dwrite)
        f.dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL,
                                   DWRITE_FONT_STRETCH_NORMAL, static_cast<float>(chrome_->scale(12)), L"", &labels_);
    if (target_) return true;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    // 96 dpi on purpose: one unit is one pixel, and the window is already sized for the real dpi.
    // Software on purpose too: a hardware target loads the GPU vendor's user-mode driver (tens of MB
    // of working set), and a few hundred anti-aliased lines a second are nothing for the CPU.
    // The "hardware-accelerated graphs" setting switches to DEFAULT, which uses the GPU and still
    // falls back to WARP when there is none.
    const auto type = hardwareGraphs ? D2D1_RENDER_TARGET_TYPE_DEFAULT : D2D1_RENDER_TARGET_TYPE_SOFTWARE;
    const auto props = D2D1::RenderTargetProperties(type, D2D1::PixelFormat(), 96.0f, 96.0f);
    const auto hwndProps = D2D1::HwndRenderTargetProperties(
        hwnd_, D2D1::SizeU(static_cast<UINT32>(rc.right), static_cast<UINT32>(rc.bottom)));
    return SUCCEEDED(f.d2d->CreateHwndRenderTarget(props, hwndProps, &target_));
}

void Graph::paint() {
    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const Theme& theme = chrome_->theme;
    if (!ensureTarget()) {
        FillSolid(ps.hdc, rc, theme.surface); // no Direct2D at all: an empty panel, not a crash
        EndPaint(hwnd_, &ps);
        return;
    }

    target_->BeginDraw();
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    target_->Clear(Color(theme.surface));
    {
        Brushes b(target_, theme, theme.graph[std::clamp(data_.palette, 0, Theme::kGraphKinds - 1)]);
        const float w = static_cast<float>(rc.right), h = static_cast<float>(rc.bottom);
        if (b.ok() && data_.grid && !data_.grid->empty()) {
            // One small graph per logical processor, laid out close to square.
            const int n = static_cast<int>(data_.grid->size());
            const int columns = std::max(1, static_cast<int>(std::ceil(std::sqrt(n * w / std::max(h, 1.0f)))));
            const int rows = (n + columns - 1) / columns;
            const float gap = static_cast<float>(chrome_->scale(4));
            const float cw = (w - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
            const float ch = (h - gap * static_cast<float>(rows - 1)) / static_cast<float>(rows);
            for (int i = 0; i < n; ++i) {
                const float x = static_cast<float>(i % columns) * (cw + gap);
                const float y = static_cast<float>(i / columns) * (ch + gap);
                DrawPlot(target_, b, {std::floor(x), std::floor(y), std::floor(x + cw), std::floor(y + ch)},
                         &(*data_.grid)[i], nullptr, 100.0f, data_.samples, 2);
            }
        } else if (b.ok()) {
            const float line = labels_ ? static_cast<float>(chrome_->scale(18)) : 0.0f;
            const D2D1_RECT_F plot = {0.0f, line, w, h - line};
            if (labels_) {
                auto text = [&](const std::wstring& s, const D2D1_RECT_F& r, DWRITE_TEXT_ALIGNMENT align) {
                    if (s.empty()) return;
                    labels_->SetTextAlignment(align);
                    target_->DrawText(s.c_str(), static_cast<UINT32>(s.size()), labels_, r, b.text);
                };
                text(data_.topLeft, {0, 0, w, line}, DWRITE_TEXT_ALIGNMENT_LEADING);
                text(data_.topRight, {0, 0, w, line}, DWRITE_TEXT_ALIGNMENT_TRAILING);
                text(data_.bottomLeft, {0, h - line + 2, w, h}, DWRITE_TEXT_ALIGNMENT_LEADING);
                text(data_.bottomRight, {0, h - line + 2, w, h}, DWRITE_TEXT_ALIGNMENT_TRAILING);
            }
            DrawPlot(target_, b, plot, data_.primary, data_.secondary, data_.maximum, data_.samples, 10);
        }
    }
    if (target_->EndDraw() == D2DERR_RECREATE_TARGET) releaseTarget(); // device lost: next paint rebuilds it
    EndPaint(hwnd_, &ps);
}

LRESULT CALLBACK Graph::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<Graph*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:
        if (self) {
            self->paint();
            return 0;
        }
        break;
    case WM_SIZE:
        if (self && self->target_) self->target_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_DESTROY:
        if (self) {
            self->releaseTarget();
            self->hwnd_ = nullptr;
        }
        break;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace lask
