// What every grid shares: fonts and palette, the dark header, accent selection, the filter match.
// main.cpp owns the one Chrome; tabs keep a const reference to it.
#pragma once

#include "facts.h"
#include "snapshot.h"
#include "theme.h"

#include <windows.h>
#include <commctrl.h>

#include <vector>

namespace lask {

// Sent to a grid's parent when its header is right-clicked; wParam is the grid.
constexpr UINT kMsgHeaderMenu = WM_APP + 20;

struct Chrome {
    Theme theme;
    HFONT font = nullptr;       // the system message font
    HFONT bold = nullptr;       // same, semibold: group headers
    HFONT title = nullptr;      // 20 px semibold: the Performance tab's headings
    HFONT glyphs = nullptr;     // Segoe Fluent Icons (Segoe MDL2 Assets on Windows 10), navigation size
    HFONT smallGlyphs = nullptr; // the same face, chevron size
    int fontHeight = 16;
    UINT dpi = 96;

    int scale(int px) const { return MulDiv(px, static_cast<int>(dpi), 96); }
    // Recreates every font for `dpi`; `measure` is any window, for the text metrics.
    void recreateFonts(HWND measure);
};

// What an action applies to: one process, or every process an app row stands for.
struct ActionTarget {
    Identity process;           // the row's own process (an app row's root process)
    std::vector<Identity> all;  // every process to end for "End task"
    HWND window = nullptr;      // set for a window row: "Switch to"
};

void FillSolid(HDC dc, const RECT& rc, COLORREF color);
void RoundFill(HDC dc, const RECT& rc, int radius, COLORREF color);

// A report ListView in owner-data mode with the header drawn from `chrome` (the dark-mode themes
// leave it light). `chrome` must outlive the window.
HWND CreateGrid(HWND parent, int id, const Chrome& chrome);
void ApplyGridTheme(HWND grid, const Theme& theme);

// Accent selection for a row (CDDS_ITEMPREPAINT, and again at each sub-item stage if sub-items
// are custom drawn). Returns false, touching nothing, if the row is not selected.
bool PaintSelectedRow(NMLVCUSTOMDRAW* cd, HWND grid, const Theme& theme);

// Case-insensitive substring; false for an empty haystack.
bool Contains(const wchar_t* haystack, const wchar_t* needle);
// The filter box's rule: name, PID, and any fact already fetched.
bool MatchesFilter(const ProcessRow& row, const ProcessFacts* facts, const wchar_t* needle);

void FormatMemory(uint64_t bytes, wchar_t* out, size_t count);

// The UAC shield as a 32-bit menu bitmap (MENUITEMINFO::hbmpItem); the caller deletes it.
HBITMAP CreateShieldBitmap(int size);

// Brings another process's window forward. Neither call waits on the target's message loop.
void SwitchToWindow(HWND window);

} // namespace lask
