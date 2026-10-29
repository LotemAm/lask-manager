// Windows look: light/dark from the user's setting, the user's accent color, dark title bar and menus.
#pragma once

#include <windows.h>

namespace lask {

struct Theme {
    bool dark = false;
    COLORREF accent = RGB(0, 120, 212);
    COLORREF background = 0;    // top-level window client area
    COLORREF surface = 0;       // list / card background
    COLORREF text = 0;
    COLORREF textSecondary = 0;
    COLORREF border = 0;
    COLORREF selection = 0;     // accent-tinted row background
    COLORREF headerBackground = 0;
};

// Reads AppsUseLightTheme and the DWM accent color from the registry (falls back to DWM APIs).
Theme ReadTheme();

// Dark/light for menus and other uxtheme-drawn parts. Call before creating any window, and again on
// theme change. Uses the uxtheme ordinals every large Win32 app ships; no-op before Windows 10 1809.
void SetAppDarkMode(bool dark);

// Must be called on a control before SetWindowTheme(..., L"DarkMode_Explorer", ...) has any effect.
void AllowDarkModeForWindow(HWND hwnd, bool dark);

// Dark title bar (Windows 10 1809+) and Mica backdrop (Windows 11; harmless elsewhere).
void ApplyWindowTheme(HWND topLevel, const Theme& theme);

} // namespace lask
