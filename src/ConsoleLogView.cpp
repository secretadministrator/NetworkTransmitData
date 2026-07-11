#include "ConsoleLogView.h"
#include "ConsoleTheme.h"
#include <algorithm>

namespace {
constexpr wchar_t LOG_CLASS[] = L"DirectTransferConsoleLog";
constexpr size_t MAX_LOG_LINES = 256;

void RegisterLogClass(HINSTANCE hInst) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = ConsoleLogView::StaticWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = LOG_CLASS;
    RegisterClassW(&wc);
}
} // namespace

ConsoleLogView::~ConsoleLogView() {
    if (m_hFont) DeleteObject(m_hFont);
    if (m_hBackgroundBrush) DeleteObject(m_hBackgroundBrush);
    if (m_hPanelBrush) DeleteObject(m_hPanelBrush);
    if (m_hBorderBrush) DeleteObject(m_hBorderBrush);
}

bool ConsoleLogView::Create(HINSTANCE hInst, HWND hParent, int controlId) {
    m_hInst = hInst;
    RegisterLogClass(hInst);
    m_hwnd = CreateWindowExW(0, LOG_CLASS, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, 0, 0, hParent, (HMENU)(INT_PTR)controlId, hInst, this);
    if (!m_hwnd)
        return false;
    m_hBackgroundBrush = CreateSolidBrush(console_theme::BACKGROUND);
    m_hPanelBrush = CreateSolidBrush(console_theme::PANEL);
    m_hBorderBrush = CreateSolidBrush(console_theme::BORDER);
    UpdateFont();
    return true;
}

void ConsoleLogView::SetDpi(UINT dpi) {
    m_dpi = dpi ? dpi : 96;
    UpdateFont();
    if (m_hwnd)
        InvalidateRect(m_hwnd, nullptr, FALSE);
}

void ConsoleLogView::UpdateFont() {
    HFONT font = console_theme::CreateFontForDpi(9, m_dpi, L"Consolas");
    if (!font)
        return;
    if (m_hFont) DeleteObject(m_hFont);
    m_hFont = font;
}

void ConsoleLogView::AddLine(const std::wstring& line) {
    if (m_lines.size() >= MAX_LOG_LINES)
        m_lines.pop_front();
    m_lines.push_back(line);
    if (m_hwnd)
        InvalidateRect(m_hwnd, nullptr, FALSE);
}

void ConsoleLogView::Clear() {
    m_lines.clear();
    if (m_hwnd)
        InvalidateRect(m_hwnd, nullptr, FALSE);
}

void ConsoleLogView::Paint(HDC hdc) {
    RECT client = {};
    GetClientRect(m_hwnd, &client);
    FillRect(hdc, &client, m_hBackgroundBrush);
    const int clientWidth = static_cast<int>(client.right);
    const int clientHeight = static_cast<int>(client.bottom);
    RECT panel = {12, 4, (std::max)(13, clientWidth - 12),
        (std::max)(5, clientHeight - 4)};
    FillRect(hdc, &panel, m_hPanelBrush);
    FrameRect(hdc, &panel, m_hBorderBrush);
    HFONT oldFont = (HFONT)SelectObject(hdc, m_hFont);
    const COLORREF oldColor = SetTextColor(hdc, console_theme::TEXT_DIM);
    const int oldBk = SetBkMode(hdc, TRANSPARENT);
    RECT header = {panel.left + 10, panel.top + 5, panel.right - 10, panel.top + 20};
    DrawTextW(hdc, L"[ LOG ]", -1, &header, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    SetTextColor(hdc, console_theme::TEXT);
    TEXTMETRICW metrics = {};
    GetTextMetricsW(hdc, &metrics);
    const int lineHeight = (std::max)(14, static_cast<int>(metrics.tmHeight) + 2);
    int y = panel.top + 23;
    const int visibleLines = (std::max)(0, static_cast<int>(panel.bottom - y - 4) / lineHeight);
    const int first = (std::max)(0, static_cast<int>(m_lines.size()) - visibleLines);
    for (int i = first; i < static_cast<int>(m_lines.size()); ++i) {
        RECT line = {panel.left + 10, y, panel.right - 10, y + lineHeight};
        DrawTextW(hdc, m_lines[static_cast<size_t>(i)].c_str(), -1, &line,
            DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        y += lineHeight;
    }
    SetBkMode(hdc, oldBk);
    SetTextColor(hdc, oldColor);
    SelectObject(hdc, oldFont);
}

LRESULT CALLBACK ConsoleLogView::StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ConsoleLogView* self = reinterpret_cast<ConsoleLogView*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
        self = reinterpret_cast<ConsoleLogView*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->WndProc(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT ConsoleLogView::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC paintDc = BeginPaint(hwnd, &ps);
        RECT client = {};
        GetClientRect(hwnd, &client);
        HDC bufferDc = CreateCompatibleDC(paintDc);
        HBITMAP bitmap = CreateCompatibleBitmap(paintDc,
            (std::max)(1, static_cast<int>(client.right)),
            (std::max)(1, static_cast<int>(client.bottom)));
        HBITMAP oldBitmap = (HBITMAP)SelectObject(bufferDc, bitmap);
        Paint(bufferDc);
        BitBlt(paintDc, 0, 0, client.right, client.bottom, bufferDc, 0, 0, SRCCOPY);
        SelectObject(bufferDc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(bufferDc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
