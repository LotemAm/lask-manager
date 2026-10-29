// The one hand-drawn control: a child window that draws a ring buffer as a filled line graph with
// Direct2D (a polyline and its fill, a faint grid, four corner labels), or a grid of small graphs,
// one per logical processor. d2d1.dll and dwrite.dll are loaded the first time a graph is created,
// so they cost nothing at startup.
#pragma once

#include "history.h"
#include "ui.h"

#include <windows.h>

#include <string>
#include <vector>

struct ID2D1HwndRenderTarget;
struct IDWriteTextFormat;

namespace lask {

struct GraphData {
    const Ring* primary = nullptr;          // solid line over a translucent fill
    const Ring* secondary = nullptr;        // dashed line, no fill
    const std::vector<Ring>* grid = nullptr; // set instead: one small graph per ring, no labels
    float maximum = 100.0f;                 // value at the top edge
    int samples = 60;                       // how many of the newest samples span the width
    std::wstring topLeft, topRight, bottomLeft, bottomRight;
};

// Software rendering by default; hardware loads the GPU driver (~20 MB) for smoother drawing.
// Existing graphs pick the change up after reset().
void SetHardwareGraphs(bool hardware);

class Graph {
public:
    Graph() = default;
    ~Graph();
    Graph(const Graph&) = delete;
    Graph& operator=(const Graph&) = delete;

    // `chrome` must outlive the window.
    HWND create(HWND parent, const Chrome& chrome);
    HWND hwnd() const { return hwnd_; }
    void set(GraphData data); // invalidates; the window repaints from the rings on WM_PAINT
    void themeChanged();      // drops the text format so the next paint picks up the new dpi
    void reset();             // drops the render target too: after SetHardwareGraphs

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void paint();
    bool ensureTarget();
    void releaseTarget();

    HWND hwnd_ = nullptr;
    const Chrome* chrome_ = nullptr;
    GraphData data_;
    ID2D1HwndRenderTarget* target_ = nullptr;
    IDWriteTextFormat* labels_ = nullptr;
};

} // namespace lask
