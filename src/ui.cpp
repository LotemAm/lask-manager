#include "ui.h"

#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <iterator>

namespace lask {
namespace {

void FillHeaderRect(HDC dc, const RECT& rc, COLORREF fill, const Theme& theme) {
    FillSolid(dc, rc, fill);
    FillSolid(dc, {rc.left, rc.bottom - 1, rc.right, rc.bottom}, theme.border);
}

LRESULT DrawHeader(NMCUSTOMDRAW* cd, const Chrome& chrome) {
    const Theme& theme = chrome.theme;
    switch (cd->dwDrawStage) {
    case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
    case CDDS_POSTPAINT: {
        // The theme leaves the filler right of the last column light; paint it to match.
        HWND header = cd->hdr.hwndFrom;
        RECT client{};
        GetClientRect(header, &client);
        LONG left = client.left;
        for (int i = 0, n = Header_GetItemCount(header); i < n; ++i) {
            RECT item{};
            if (Header_GetItemRect(header, i, &item)) left = std::max(left, item.right);
        }
        if (left < client.right)
            FillHeaderRect(cd->hdc, {left, client.top, client.right, client.bottom}, theme.headerBackground, theme);
        return CDRF_DODEFAULT;
    }
    case CDDS_ITEMPREPAINT: {
        HDC dc = cd->hdc;
        const RECT rc = cd->rc;

        wchar_t title[64] = {};
        HDITEMW hi{};
        hi.mask = HDI_TEXT | HDI_FORMAT;
        hi.pszText = title;
        hi.cchTextMax = static_cast<int>(std::size(title));
        Header_GetItem(cd->hdr.hwndFrom, static_cast<int>(cd->dwItemSpec), &hi);
        const bool sorted = (hi.fmt & (HDF_SORTUP | HDF_SORTDOWN)) != 0;

        const COLORREF fill = (cd->uItemState & CDIS_SELECTED) ? theme.headerPressed
                              : (cd->uItemState & CDIS_HOT)    ? theme.headerHot
                                                               : theme.headerBackground;
        FillHeaderRect(dc, rc, fill, theme);
        if (sorted)
            FillSolid(dc, {rc.left + chrome.scale(1), rc.bottom - chrome.scale(2), rc.right - chrome.scale(1), rc.bottom},
                      theme.accentText);

        HGDIOBJ oldFont = SelectObject(dc, chrome.font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, sorted ? theme.text : theme.textSecondary);
        RECT textRect = rc;
        textRect.left += chrome.scale(6);
        textRect.right -= chrome.scale(6);
        const UINT format = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX |
                            ((hi.fmt & HDF_RIGHT) ? DT_RIGHT : DT_LEFT);
        DrawTextW(dc, title, -1, &textRect, format);

        if (sorted) {
            const int cx = (rc.left + rc.right) / 2;
            const int top = rc.top + chrome.scale(2);
            const int s = chrome.scale(3);
            POINT up[3] = {{cx - s, top + s}, {cx + s, top + s}, {cx, top}};
            POINT down[3] = {{cx - s, top}, {cx + s, top}, {cx, top + s}};
            SetDCBrushColor(dc, theme.accentText);
            SetDCPenColor(dc, theme.accentText);
            HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
            HGDIOBJ oldPen = SelectObject(dc, GetStockObject(DC_PEN));
            Polygon(dc, (hi.fmt & HDF_SORTUP) ? up : down, 3);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
        }
        SelectObject(dc, oldFont);
        return CDRF_SKIPDEFAULT;
    }
    default: return CDRF_DODEFAULT;
    }
}

// The header's parent is the ListView, so its notifications are caught here.
LRESULT CALLBACK GridSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    if (msg == WM_NOTIFY) {
        auto* h = reinterpret_cast<NMHDR*>(lp);
        if (h->hwndFrom == ListView_GetHeader(hwnd)) {
            if (h->code == NM_CUSTOMDRAW)
                return DrawHeader(reinterpret_cast<NMCUSTOMDRAW*>(lp), *reinterpret_cast<const Chrome*>(ref));
            if (h->code == NM_RCLICK) {
                SendMessageW(GetParent(hwnd), kMsgHeaderMenu, reinterpret_cast<WPARAM>(hwnd), 0);
                return TRUE;
            }
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK HeaderSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    if (msg == WM_ERASEBKGND) {
        const Theme& theme = reinterpret_cast<const Chrome*>(ref)->theme;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillHeaderRect(reinterpret_cast<HDC>(wp), rc, theme.headerBackground, theme);
        return 1;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// Windows 11 (build 22000) ships Segoe Fluent Icons; Windows 10 has Segoe MDL2 Assets with the same
// code points. A version check, because enumerating fonts costs tens of milliseconds at startup.
const wchar_t* GlyphFace() {
    static const wchar_t* face = [] {
        OSVERSIONINFOEXW v{};
        v.dwOSVersionInfoSize = sizeof v;
        v.dwBuildNumber = 22000;
        const ULONGLONG mask = VerSetConditionMask(0, VER_BUILDNUMBER, VER_GREATER_EQUAL);
        return VerifyVersionInfoW(&v, VER_BUILDNUMBER, mask) ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    }();
    return face;
}

HFONT CreateGlyphFont(int pixels) {
    LOGFONTW lf{};
    lf.lfHeight = -pixels;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, GlyphFace());
    return CreateFontIndirectW(&lf);
}

void Replace(HFONT& font, HFONT next) {
    if (font) DeleteObject(font);
    font = next;
}

} // namespace

void Chrome::recreateFonts(HWND measure) {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi);
    HFONT normal = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_SEMIBOLD;
    HFONT semibold = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfHeight = -scale(20);
    HFONT heading = CreateFontIndirectW(&ncm.lfMessageFont);

    HDC dc = GetDC(measure);
    HGDIOBJ old = SelectObject(dc, normal);
    TEXTMETRICW tm{};
    if (GetTextMetricsW(dc, &tm)) fontHeight = tm.tmHeight;
    SelectObject(dc, old);
    ReleaseDC(measure, dc);

    Replace(font, normal);
    Replace(bold, semibold);
    Replace(title, heading);
    Replace(glyphs, CreateGlyphFont(scale(16)));
    Replace(smallGlyphs, CreateGlyphFont(scale(9)));
    Replace(menuGlyph, CreateGlyphFont(scale(13)));
}

void FillSolid(HDC dc, const RECT& rc, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void RoundFill(HDC dc, const RECT& rc, int radius, COLORREF color) {
    SetDCBrushColor(dc, color);
    SetDCPenColor(dc, color);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(DC_PEN));
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
}

HWND CreateGrid(HWND parent, int id, const Chrome& chrome) {
    HWND grid = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
                                    LVS_SHAREIMAGELISTS,
                                0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                GetModuleHandleW(nullptr), nullptr);
    ListView_SetExtendedListViewStyle(grid, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP |
                                                LVS_EX_LABELTIP);
    HWND header = ListView_GetHeader(grid);
    SetWindowLongPtrW(header, GWL_STYLE, GetWindowLongPtrW(header, GWL_STYLE) | HDS_HOTTRACK);
    const auto ref = reinterpret_cast<DWORD_PTR>(&chrome);
    SetWindowSubclass(grid, GridSubclassProc, 1, ref);
    SetWindowSubclass(header, HeaderSubclassProc, 1, ref);
    SendMessageW(grid, WM_SETFONT, reinterpret_cast<WPARAM>(chrome.font), FALSE);
    return grid;
}

void ApplyGridTheme(HWND grid, const Theme& theme) {
    // ItemsView is the theme Explorer's own list uses: dark scrollbars and hover. No visual styles at
    // all in high contrast, where the system colors must win.
    const wchar_t* name = theme.highContrast ? L"" : (theme.dark ? L"DarkMode_ItemsView" : L"ItemsView");
    AllowDarkModeForWindow(grid, theme.dark);
    SetWindowTheme(grid, name, nullptr);
    ListView_SetBkColor(grid, theme.surface);
    ListView_SetTextBkColor(grid, theme.surface);
    ListView_SetTextColor(grid, theme.text);
}

bool PaintSelectedRow(NMLVCUSTOMDRAW* cd, HWND grid, const Theme& theme) {
    const int i = static_cast<int>(cd->nmcd.dwItemSpec);
    if (!(ListView_GetItemState(grid, i, LVIS_SELECTED) & LVIS_SELECTED)) return false;
    const bool hot = (cd->nmcd.uItemState & CDIS_HOT) != 0;
    const COLORREF fill = GetFocus() != grid ? theme.selectionInactive : hot ? theme.selectionHot : theme.selection;
    // With the selected state cleared the theme draws a plain row, and clrTextBk fills every cell.
    cd->nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_HOT | CDIS_FOCUS);
    cd->clrTextBk = fill;
    cd->clrText = theme.selectionText;
    return true;
}

bool Contains(const wchar_t* haystack, const wchar_t* needle) {
    return haystack && *haystack && FindStringOrdinal(FIND_FROMSTART, haystack, -1, needle, -1, TRUE) >= 0;
}

bool MatchesFilter(const ProcessRow& row, const ProcessFacts* facts, const wchar_t* needle) {
    if (!needle[0]) return true;
    if (Contains(row.name, needle)) return true;
    wchar_t pid[16];
    swprintf_s(pid, L"%u", row.pid);
    if (Contains(pid, needle)) return true;
    return facts && (Contains(facts->description.c_str(), needle) || Contains(facts->user.c_str(), needle) ||
                     Contains(facts->path.c_str(), needle) || Contains(facts->commandLine.c_str(), needle));
}

void FormatMemory(uint64_t bytes, wchar_t* out, size_t count) {
    swprintf_s(out, count, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

HBITMAP CreateShieldBitmap(int size) {
    SHSTOCKICONINFO info{};
    info.cbSize = sizeof info;
    if (FAILED(SHGetStockIconInfo(SIID_SHIELD, SHGSI_ICON | SHGSI_SMALLICON, &info))) return nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap) {
        HGDIOBJ old = SelectObject(dc, bitmap);
        DrawIconEx(dc, 0, 0, info.hIcon, size, size, 0, nullptr, DI_NORMAL); // keeps the icon's alpha
        SelectObject(dc, old);
    }
    DeleteDC(dc);
    DestroyIcon(info.hIcon);
    return bitmap;
}

std::vector<int> GridWidths(HWND grid, UINT dpi) {
    std::vector<int> widths;
    if (!grid) return widths;
    for (int i = 0, n = Header_GetItemCount(ListView_GetHeader(grid)); i < n; ++i)
        widths.push_back(MulDiv(ListView_GetColumnWidth(grid, i), 96, static_cast<int>(dpi)));
    return widths;
}

std::vector<int> GridOrder(HWND grid) {
    if (!grid) return {};
    std::vector<int> order(static_cast<size_t>(Header_GetItemCount(ListView_GetHeader(grid))));
    ListView_GetColumnOrderArray(grid, static_cast<int>(order.size()), order.data());
    return order;
}

void SetGridLayout(HWND grid, const std::vector<int>& widths, const std::vector<int>& order, UINT dpi) {
    if (!grid) return;
    const int n = Header_GetItemCount(ListView_GetHeader(grid));
    if (static_cast<int>(widths.size()) == n)
        for (int i = 0; i < n; ++i)
            if (widths[i] > 0) ListView_SetColumnWidth(grid, i, MulDiv(widths[i], static_cast<int>(dpi), 96));
    if (static_cast<int>(order.size()) == n) {
        std::vector<int> sorted = order;
        std::sort(sorted.begin(), sorted.end());
        for (int i = 0; i < n; ++i)
            if (sorted[i] != i) return; // not a permutation: leave the default order
        ListView_SetColumnOrderArray(grid, n, const_cast<int*>(order.data()));
    }
}

void SwitchToWindow(HWND window) {
    if (!IsWindow(window)) return;
    if (IsIconic(window)) ShowWindowAsync(window, SW_RESTORE);
    SetForegroundWindow(window);
}

} // namespace lask
