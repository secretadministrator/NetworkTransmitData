#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace console_theme {

inline constexpr COLORREF BACKGROUND = RGB(255, 255, 255);
inline constexpr COLORREF PANEL = RGB(255, 255, 255);
inline constexpr COLORREF PANEL_ALT = RGB(248, 248, 248);
inline constexpr COLORREF BORDER = RGB(160, 160, 160);
inline constexpr COLORREF TEXT = RGB(0, 0, 0);
inline constexpr COLORREF TEXT_DIM = RGB(80, 80, 80);
inline constexpr COLORREF ACCENT = RGB(0, 0, 0);
inline constexpr COLORREF ACCENT_CYAN = RGB(32, 32, 32);
inline constexpr COLORREF WARNING = RGB(0, 0, 0);
inline constexpr COLORREF ERROR_COLOR = RGB(0, 0, 0);
inline constexpr COLORREF HIGHLIGHT_BLUE = RGB(0, 92, 184);
inline constexpr COLORREF HIGHLIGHT_GREEN = RGB(0, 122, 82);
inline constexpr COLORREF HIGHLIGHT_AMBER = RGB(176, 88, 0);

inline HFONT CreateFontForDpi(int pointSize, UINT dpi, const wchar_t* face,
    int weight = FW_NORMAL) {
    const int height = -MulDiv(pointSize, static_cast<int>(dpi), 72);
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

} // namespace console_theme
