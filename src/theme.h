// Windows look: light/dark from the user's setting, the user's accent color, dark title bar and menus.
#pragma once

#include <windows.h>

namespace lask {

// Color roles, filled once per theme change. Nothing outside theme.cpp holds a raw COLORREF.
struct Theme {
    bool dark = false;
    bool highContrast = false;
    COLORREF accent = RGB(0, 120, 212);
    // Fluent's SystemAccentColorLight1..3 / Dark1..3, derived from the accent in HSL.
    COLORREF accentLight[3] = {};
    COLORREF accentDark[3] = {};
    COLORREF accentText = 0;        // accent-colored text that reads on `surface`
    COLORREF onAccent = 0;          // text drawn on an `accent` fill
    COLORREF background = 0;        // top-level window client area
    COLORREF surface = 0;           // list / card background
    COLORREF text = 0;
    COLORREF textSecondary = 0;
    COLORREF border = 0;
    COLORREF selection = 0;         // accent-tinted row background while the list has focus
    COLORREF selectionHot = 0;      // the same row under the mouse
    COLORREF selectionInactive = 0; // selected row while focus is elsewhere
    COLORREF selectionText = 0;
    COLORREF headerBackground = 0;
    COLORREF headerHot = 0;
    COLORREF headerPressed = 0;
    COLORREF graphLine = 0;
    COLORREF graphFill = 0;
    COLORREF warning = 0;
    // Heat-map cell backgrounds on the Processes tab, from barely busy (0) to very busy (last).
    // All equal to `surface` in high contrast, where shading would fight the user's scheme.
    static constexpr int kHeatLevels = 5;
    COLORREF heat[kHeatLevels] = {};
};

// Reads AppsUseLightTheme and the DWM accent color from the registry (falls back to DWM APIs).
// High contrast wins over both and maps every role to a system color.
Theme ReadTheme();

// Dark/light for menus and other uxtheme-drawn parts. Call before creating any window, and again on
// theme change. Uses the uxtheme ordinals every large Win32 app ships; no-op before Windows 10 1809.
void SetAppDarkMode(bool dark);

// Must be called on a control before SetWindowTheme(..., L"DarkMode_Explorer", ...) has any effect.
void AllowDarkModeForWindow(HWND hwnd, bool dark);

// Dark title bar (Windows 10 1809+) and Mica backdrop (Windows 11; harmless elsewhere).
void ApplyWindowTheme(HWND topLevel, const Theme& theme);

// True for the WM_SETTINGCHANGE that Windows broadcasts when light/dark or the accent changes.
bool IsThemeSettingChange(LPARAM lp);

} // namespace lask
