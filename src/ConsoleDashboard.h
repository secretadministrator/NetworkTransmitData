#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "TransferSession.h"

class ConsoleDashboard {
public:
    ConsoleDashboard() = default;
    ~ConsoleDashboard();

    bool Create(HINSTANCE hInst, HWND hParent, int controlId);
    HWND GetHWND() const { return m_hwnd; }
    void SetDpi(UINT dpi);
    void SetStats(const TransferStats& stats);
    void Clear();
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    HINSTANCE m_hInst = nullptr;
    HWND m_hwnd = nullptr;
    UINT m_dpi = 96;
    bool m_hasStats = false;
    TransferStats m_stats;
    HFONT m_hMono = nullptr;
    HFONT m_hMonoBold = nullptr;
    HFONT m_hLabel = nullptr;
    HBRUSH m_hBackgroundBrush = nullptr;
    HBRUSH m_hPanelBrush = nullptr;
    HBRUSH m_hPanelAltBrush = nullptr;
    HBRUSH m_hBorderBrush = nullptr;
    HBRUSH m_hAccentBrush = nullptr;

    void UpdateFonts();
    void Paint(HDC hdc);
    void DrawTextLine(HDC hdc, const std::wstring& text, RECT rect,
        HFONT font, COLORREF color, UINT format) const;
    void DrawMetric(HDC hdc, RECT rect, const wchar_t* label,
        const std::wstring& value, COLORREF valueColor) const;
    static std::wstring StageText(TransferStage stage);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
};
