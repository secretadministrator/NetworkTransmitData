#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace console_theme {

inline constexpr COLORREF BACKGROUND = RGB(7, 12, 16);
inline constexpr COLORREF PANEL = RGB(12, 20, 25);
inline constexpr COLORREF PANEL_ALT = RGB(16, 27, 32);
inline constexpr COLORREF BORDER = RGB(34, 67, 72);
inline constexpr COLORREF TEXT = RGB(210, 236, 229);
inline constexpr COLORREF TEXT_DIM = RGB(116, 151, 149);
inline constexpr COLORREF ACCENT = RGB(61, 255, 157);
inline constexpr COLORREF ACCENT_CYAN = RGB(0, 219, 255);
inline constexpr COLORREF WARNING = RGB(255, 194, 77);
inline constexpr COLORREF ERROR_COLOR = RGB(255, 92, 104);

inline HFONT CreateFontForDpi(int pointSize, UINT dpi, const wchar_t* face,
    int weight = FW_NORMAL) {
    const int height = -MulDiv(pointSize, static_cast<int>(dpi), 72);
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

} // namespace console_theme
