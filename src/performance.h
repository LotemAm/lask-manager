// The Performance tab: a list of panels on the left (name, headline number, sparkline), and the
// selected panel on the right: a heading, one or two Direct2D graphs, and a grid of numbers.
// Created the first time the tab is shown; the history it draws is kept by PerfModel from startup.
#pragma once

#include "graph.h"
#include "history.h"
#include "ui.h"

#include <windows.h>
#include <commctrl.h>

#include <string>
#include <utility>
#include <vector>

namespace lask {

class PerformanceTab {
public:
    // Both must outlive the tab.
    PerformanceTab(const Chrome& chrome, const PerfModel& model);

    void create(HWND parent, int listId);
    bool created() const { return list_ != nullptr; }
    HWND list() const { return list_; }

    void show(bool visible);
    void layout(int x, int y, int width, int height);
    void refresh();      // after every model update: the rings moved, panels may have come or gone
    void applyTheme();
    void fontsChanged(); // dpi or system font changed
    LRESULT onNotify(NMHDR* h);

private:
    static LRESULT CALLBACK DetailProc(HWND, UINT, WPARAM, LPARAM);

    const Panel* selected() const;
    int selectedIndex() const;
    void select(int index);
    void sizeRows();
    void layoutDetail();
    void updateGraphs();
    void paintDetail(HDC dc, const RECT& rc);
    void showMenu(POINT screen);
    LRESULT drawRow(NMLVCUSTOMDRAW* cd);
    void drawSparkline(HDC dc, const RECT& box, const Panel& panel) const;

    std::wstring title(const Panel& p) const;
    std::wstring device(const Panel& p) const;
    std::wstring headline(const Panel& p) const;
    std::vector<std::pair<std::wstring, std::wstring>> stats(const Panel& p) const;

    const Chrome& chrome_;
    const PerfModel& model_;
    HWND list_ = nullptr;
    HWND detail_ = nullptr;
    HIMAGELIST rowHeight_ = nullptr; // 1 pixel wide; its height sets the list's row height
    Graph graphs_[2];
    int graphCount_ = 1;
    int statsTop_ = 0;

    PanelKind selectedKind_ = PanelKind::Cpu; // the selection survives devices coming and going
    uint64_t selectedKey_ = 0;
    int samples_ = 60;    // 30 s, 1 min or 5 min at one sample a second
    bool logical_ = false; // CPU: one graph per logical processor
};

} // namespace lask
