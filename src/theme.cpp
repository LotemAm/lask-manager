#include "theme.h"

#include <dwmapi.h>
#include <winternl.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

extern "C" NTSTATUS NTAPI RtlGetVersion(PRTL_OSVERSIONINFOW versionInformation);

namespace lask {
namespace {

bool ReadDword(HKEY root, const wchar_t* key, const wchar_t* value, DWORD& out) {
    DWORD size = sizeof out;
    DWORD type = 0;
    return RegGetValueW(root, key, value, RRF_RT_REG_DWORD, &type, &out, &size) == ERROR_SUCCESS;
}

COLORREF Mix(COLORREF a, COLORREF b, int percentB) {
    auto ch = [percentB](int x, int y) { return (x * (100 - percentB) + y * percentB) / 100; };
    return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
}

struct Hsl {
    double h, s, l; // h in [0, 6), s and l in [0, 1]
};

Hsl ToHsl(COLORREF c) {
    const double r = GetRValue(c) / 255.0, g = GetGValue(c) / 255.0, b = GetBValue(c) / 255.0;
    const double hi = std::max({r, g, b}), lo = std::min({r, g, b});
    const double l = (hi + lo) / 2;
    const double d = hi - lo;
    if (d <= 0) return {0, 0, l};
    const double s = d / (1 - std::abs(2 * l - 1));
    double h = hi == r ? std::fmod((g - b) / d + 6, 6) : hi == g ? (b - r) / d + 2 : (r - g) / d + 4;
    return {h, s, l};
}

COLORREF FromHsl(Hsl v) {
    const double c = (1 - std::abs(2 * v.l - 1)) * v.s;
    const double x = c * (1 - std::abs(std::fmod(v.h, 2) - 1));
    const double m = v.l - c / 2;
    double r = 0, g = 0, b = 0;
    switch (static_cast<int>(v.h)) {
    case 0: r = c, g = x; break;
    case 1: r = x, g = c; break;
    case 2: g = c, b = x; break;
    case 3: g = x, b = c; break;
    case 4: r = x, b = c; break;
    default: r = c, b = x; break;
    }
    auto byte = [m](double ch) { return static_cast<BYTE>(std::clamp((ch + m) * 255.0 + 0.5, 0.0, 255.0)); };
    return RGB(byte(r), byte(g), byte(b));
}

// Fluent's accent ramp steps lightness in fixed amounts; these land close to the AccentPalette
// Windows itself writes, without a second registry read.
COLORREF ShiftLightness(COLORREF c, double delta) {
    Hsl v = ToHsl(c);
    v.l = std::clamp(v.l + delta, 0.0, 1.0);
    return FromHsl(v);
}

double Luminance(COLORREF c) {
    return (0.2126 * GetRValue(c) + 0.7152 * GetGValue(c) + 0.0722 * GetBValue(c)) / 255.0;
}

bool IsHighContrast() {
    HIGHCONTRASTW hc{sizeof hc};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

// High contrast: every role is a system color, so the user's chosen scheme is honored exactly.
void FillHighContrast(Theme& t) {
    t.highContrast = true;
    t.dark = false; // the dark-mode uxtheme classes would override the high-contrast scheme
    t.accent = GetSysColor(COLOR_HIGHLIGHT);
    for (auto& c : t.accentLight) c = t.accent;
    for (auto& c : t.accentDark) c = t.accent;
    t.accentText = GetSysColor(COLOR_HOTLIGHT);
    t.onAccent = GetSysColor(COLOR_HIGHLIGHTTEXT);
    t.background = t.surface = GetSysColor(COLOR_WINDOW);
    t.text = t.textSecondary = GetSysColor(COLOR_WINDOWTEXT);
    t.border = GetSysColor(COLOR_WINDOWTEXT);
    t.selection = t.selectionHot = t.selectionInactive = GetSysColor(COLOR_HIGHLIGHT);
    t.selectionText = GetSysColor(COLOR_HIGHLIGHTTEXT);
    t.headerBackground = GetSysColor(COLOR_BTNFACE);
    t.headerHot = t.headerPressed = GetSysColor(COLOR_BTNFACE);
    t.graphLine = GetSysColor(COLOR_HIGHLIGHT);
    t.graphFill = GetSysColor(COLOR_WINDOW);
    t.warning = GetSysColor(COLOR_HOTLIGHT);
}

// uxtheme exports these by ordinal only (Windows 10 1809+). Ordinal 135 changed meaning in 1903:
// AllowDarkModeForApp(BOOL) before, SetPreferredAppMode(enum) after.
enum class AppMode : int { Default = 0, AllowDark = 1, ForceDark = 2, ForceLight = 3 };
using SetPreferredAppModeFn = AppMode(WINAPI*)(AppMode);
using AllowDarkModeForAppFn = BOOL(WINAPI*)(BOOL);
using AllowDarkModeForWindowFn = BOOL(WINAPI*)(HWND, BOOL);
using FlushMenuThemesFn = void(WINAPI*)();
using RefreshImmersiveColorPolicyStateFn = void(WINAPI*)();

struct DarkModeApi {
    bool available = false;
    SetPreferredAppModeFn setPreferredAppMode = nullptr;
    AllowDarkModeForAppFn allowDarkModeForApp = nullptr;
    AllowDarkModeForWindowFn allowDarkModeForWindow = nullptr;
    FlushMenuThemesFn flushMenuThemes = nullptr;
    RefreshImmersiveColorPolicyStateFn refreshImmersiveColorPolicyState = nullptr;
};

const DarkModeApi& Api() {
    static const DarkModeApi api = [] {
        DarkModeApi a;
        RTL_OSVERSIONINFOW v{};
        v.dwOSVersionInfoSize = sizeof v;
        RtlGetVersion(&v);
        if (v.dwMajorVersion < 10 || (v.dwMajorVersion == 10 && v.dwBuildNumber < 17763)) return a;

        HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!ux) return a;
        a.available = true;
        auto ordinal = [ux](WORD n) { return GetProcAddress(ux, reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(n))); };
        if (v.dwBuildNumber >= 18362)
            a.setPreferredAppMode = reinterpret_cast<SetPreferredAppModeFn>(ordinal(135));
        else
            a.allowDarkModeForApp = reinterpret_cast<AllowDarkModeForAppFn>(ordinal(135));
        a.allowDarkModeForWindow = reinterpret_cast<AllowDarkModeForWindowFn>(ordinal(133));
        a.flushMenuThemes = reinterpret_cast<FlushMenuThemesFn>(ordinal(136));
        a.refreshImmersiveColorPolicyState = reinterpret_cast<RefreshImmersiveColorPolicyStateFn>(ordinal(104));
        return a;
    }();
    return api;
}

} // namespace

Theme ReadTheme() {
    Theme t;
    if (IsHighContrast()) {
        FillHighContrast(t);
        return t;
    }

    DWORD light = 1;
    if (ReadDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                  L"AppsUseLightTheme", light))
        t.dark = light == 0;

    // DWM\AccentColor is stored as 0xAABBGGRR, which is exactly a COLORREF once the alpha is masked off.
    DWORD accent = 0;
    if (ReadDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", accent)) {
        t.accent = static_cast<COLORREF>(accent & 0x00FFFFFFu);
    } else {
        DWORD argb = 0;
        BOOL opaque = FALSE;
        if (SUCCEEDED(DwmGetColorizationColor(&argb, &opaque)))
            t.accent = RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
    }

    for (int i = 0; i < 3; ++i) {
        t.accentLight[i] = ShiftLightness(t.accent, 0.10 * (i + 1));
        t.accentDark[i] = ShiftLightness(t.accent, -0.08 * (i + 1));
    }
    t.onAccent = Luminance(t.accent) > 0.5 ? RGB(0, 0, 0) : RGB(255, 255, 255);

    if (t.dark) {
        t.background = RGB(32, 32, 32);
        t.surface = RGB(28, 28, 28);
        t.text = RGB(255, 255, 255);
        t.textSecondary = RGB(200, 200, 200);
        t.border = RGB(58, 58, 58);
        t.accentText = t.accentLight[1];
        t.selection = Mix(t.surface, t.accentDark[0], 38);
        t.selectionHot = Mix(t.surface, t.accentDark[0], 48);
        t.selectionInactive = Mix(t.surface, t.text, 12);
        t.headerHot = Mix(t.surface, t.text, 6);
        t.headerPressed = Mix(t.surface, t.text, 10);
        t.graphLine = t.accentLight[1];
        t.warning = RGB(252, 225, 0);
    } else {
        t.background = RGB(243, 243, 243);
        t.surface = RGB(255, 255, 255);
        t.text = RGB(27, 27, 27);
        t.textSecondary = RGB(96, 96, 96);
        t.border = RGB(229, 229, 229);
        t.accentText = t.accentDark[1];
        t.selection = Mix(t.surface, t.accentLight[1], 30);
        t.selectionHot = Mix(t.surface, t.accentLight[1], 40);
        t.selectionInactive = Mix(t.surface, t.text, 9);
        t.headerHot = Mix(t.surface, t.text, 4);
        t.headerPressed = Mix(t.surface, t.text, 8);
        t.graphLine = t.accentDark[0];
        t.warning = RGB(157, 93, 0);
    }
    t.selectionText = t.text;
    t.headerBackground = t.surface;
    t.graphFill = Mix(t.surface, t.graphLine, 20);
    return t;
}

bool IsThemeSettingChange(LPARAM lp) {
    const auto* area = reinterpret_cast<const wchar_t*>(lp);
    return area && (wcscmp(area, L"ImmersiveColorSet") == 0 || wcscmp(area, L"WindowsThemeElement") == 0);
}

void SetAppDarkMode(bool dark) {
    const auto& a = Api();
    if (!a.available) return;
    if (a.setPreferredAppMode)
        a.setPreferredAppMode(dark ? AppMode::ForceDark : AppMode::ForceLight);
    else if (a.allowDarkModeForApp)
        a.allowDarkModeForApp(dark);
    if (a.refreshImmersiveColorPolicyState) a.refreshImmersiveColorPolicyState();
    if (a.flushMenuThemes) a.flushMenuThemes();
}

void AllowDarkModeForWindow(HWND hwnd, bool dark) {
    const auto& a = Api();
    if (a.allowDarkModeForWindow) a.allowDarkModeForWindow(hwnd, dark);
}

void ApplyWindowTheme(HWND topLevel, const Theme& theme) {
    // Numeric attributes so this builds against any Windows 10 SDK.
    constexpr DWORD kUseImmersiveDarkMode = 20; // DWMWA_USE_IMMERSIVE_DARK_MODE (1809+ via 19, 20H1+ via 20)
    constexpr DWORD kSystemBackdropType = 38;   // DWMWA_SYSTEMBACKDROP_TYPE (Windows 11 22H2+)
    constexpr int kBackdropMainWindow = 2;      // DWMSBT_MAINWINDOW (Mica)

    BOOL dark = theme.dark ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(topLevel, kUseImmersiveDarkMode, &dark, sizeof dark)))
        DwmSetWindowAttribute(topLevel, 19, &dark, sizeof dark); // pre-20H1 attribute number
    int backdrop = kBackdropMainWindow;
    DwmSetWindowAttribute(topLevel, kSystemBackdropType, &backdrop, sizeof backdrop);
}

} // namespace lask
