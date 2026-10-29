#include "theme.h"

#include <dwmapi.h>
#include <winternl.h>

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

    if (t.dark) {
        t.background = RGB(32, 32, 32);
        t.surface = RGB(28, 28, 28);
        t.text = RGB(255, 255, 255);
        t.textSecondary = RGB(200, 200, 200);
        t.border = RGB(58, 58, 58);
        t.selection = Mix(t.surface, t.accent, 40);
    } else {
        t.background = RGB(243, 243, 243);
        t.surface = RGB(255, 255, 255);
        t.text = RGB(27, 27, 27);
        t.textSecondary = RGB(96, 96, 96);
        t.border = RGB(229, 229, 229);
        t.selection = Mix(t.surface, t.accent, 22);
    }
    t.headerBackground = t.surface;
    return t;
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
