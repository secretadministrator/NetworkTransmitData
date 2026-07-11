#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <deque>
#include <string>

class ConsoleLogView {
public:
    ConsoleLogView() = default;
    ~ConsoleLogView();

    bool Create(HINSTANCE hInst, HWND hParent, int controlId);
    HWND GetHWND() const { return m_hwnd; }
    void SetDpi(UINT dpi);
    void AddLine(const std::wstring& line);
    void Clear();
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    HINSTANCE m_hInst = nullptr;
    HWND m_hwnd = nullptr;
    UINT m_dpi = 96;
    std::deque<std::wstring> m_lines;
    HFONT m_hFont = nullptr;
    HBRUSH m_hBackgroundBrush = nullptr;
    HBRUSH m_hPanelBrush = nullptr;
    HBRUSH m_hBorderBrush = nullptr;

    void UpdateFont();
    void Paint(HDC hdc);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
};
