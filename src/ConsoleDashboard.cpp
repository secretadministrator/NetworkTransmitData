#include "ConsoleDashboard.h"
#include "ConsoleTheme.h"
#include "Utils.h"
#include <algorithm>

namespace {
constexpr wchar_t DASHBOARD_CLASS[] = L"DirectTransferConsoleDashboard";
constexpr int PANEL_MARGIN = 12;

void RegisterDashboardClass(HINSTANCE hInst) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = ConsoleDashboard::StaticWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = DASHBOARD_CLASS;
    RegisterClassW(&wc);
}

COLORREF StageColor(TransferStage stage) {
    if (stage == TransferStage::Reconnecting)
        return console_theme::WARNING;
    if (stage == TransferStage::Committing)
        return console_theme::ACCENT_CYAN;
    return console_theme::ACCENT;
}
} // namespace

ConsoleDashboard::~ConsoleDashboard() {
    if (m_hMono) DeleteObject(m_hMono);
    if (m_hMonoBold) DeleteObject(m_hMonoBold);
    if (m_hLabel) DeleteObject(m_hLabel);
    if (m_hBackgroundBrush) DeleteObject(m_hBackgroundBrush);
    if (m_hPanelBrush) DeleteObject(m_hPanelBrush);
    if (m_hPanelAltBrush) DeleteObject(m_hPanelAltBrush);
    if (m_hBorderBrush) DeleteObject(m_hBorderBrush);
    if (m_hAccentBrush) DeleteObject(m_hAccentBrush);
}

bool ConsoleDashboard::Create(HINSTANCE hInst, HWND hParent, int controlId) {
    m_hInst = hInst;
    RegisterDashboardClass(hInst);
    m_hwnd = CreateWindowExW(0, DASHBOARD_CLASS, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, 0, 0, hParent, (HMENU)(INT_PTR)controlId, hInst, this);
    if (!m_hwnd)
        return false;
    m_hBackgroundBrush = CreateSolidBrush(console_theme::BACKGROUND);
    m_hPanelBrush = CreateSolidBrush(console_theme::PANEL);
    m_hPanelAltBrush = CreateSolidBrush(console_theme::PANEL_ALT);
    m_hBorderBrush = CreateSolidBrush(console_theme::BORDER);
    m_hAccentBrush = CreateSolidBrush(console_theme::ACCENT);
    UpdateFonts();
    return true;
}

void ConsoleDashboard::SetDpi(UINT dpi) {
    m_dpi = dpi ? dpi : 96;
    UpdateFonts();
    if (m_hwnd)
        InvalidateRect(m_hwnd, nullptr, FALSE);
}

void ConsoleDashboard::SetStats(const TransferStats& stats) {
    m_stats = stats;
    m_hasStats = true;
    if (m_hwnd)
        InvalidateRect(m_hwnd, nullptr, FALSE);
}

void ConsoleDashboard::Clear() {
    m_hasStats = false;
    m_stats = TransferStats{};
    if (m_hwnd)
        InvalidateRect(m_hwnd, nullptr, FALSE);
}

void ConsoleDashboard::UpdateFonts() {
    HFONT mono = console_theme::CreateFontForDpi(9, m_dpi, L"Consolas");
    HFONT monoBold = console_theme::CreateFontForDpi(10, m_dpi, L"Consolas", FW_BOLD);
    HFONT label = console_theme::CreateFontForDpi(9, m_dpi, L"Microsoft YaHei UI");
    if (!mono || !monoBold || !label) {
        if (mono) DeleteObject(mono);
        if (monoBold) DeleteObject(monoBold);
        if (label) DeleteObject(label);
        return;
    }
    if (m_hMono) DeleteObject(m_hMono);
    if (m_hMonoBold) DeleteObject(m_hMonoBold);
    if (m_hLabel) DeleteObject(m_hLabel);
    m_hMono = mono;
    m_hMonoBold = monoBold;
    m_hLabel = label;
}

std::wstring ConsoleDashboard::StageText(TransferStage stage) {
    switch (stage) {
    case TransferStage::ScanningSource: return L"\u626b\u63cf\u6e90\u76ee\u5f55";
    case TransferStage::HashingSource: return L"\u6821\u9a8c\u6e90\u6587\u4ef6";
    case TransferStage::WaitingForPlan: return L"\u7b49\u5f85\u4f20\u8f93\u8ba1\u5212";
    case TransferStage::BuildingPlan: return L"\u5efa\u7acb\u4f20\u8f93\u8ba1\u5212";
    case TransferStage::VerifyingPlan: return L"\u6821\u9a8c\u4f20\u8f93\u8ba1\u5212";
    case TransferStage::Transferring: return L"\u4f20\u8f93\u4e2d";
    case TransferStage::Reconnecting: return L"\u91cd\u65b0\u8fde\u63a5";
    case TransferStage::Committing: return L"\u63d0\u4ea4\u4f20\u8f93";
    default: return L"\u5c31\u7eea";
    }
}

void ConsoleDashboard::DrawTextLine(HDC hdc, const std::wstring& text, RECT rect,
    HFONT font, COLORREF color, UINT format) const {
    HFONT oldFont = (HFONT)SelectObject(hdc, font);
    const COLORREF oldColor = SetTextColor(hdc, color);
    const int oldBk = SetBkMode(hdc, TRANSPARENT);
    DrawTextW(hdc, text.c_str(), static_cast<int>(text.size()), &rect, format);
    SetBkMode(hdc, oldBk);
    SetTextColor(hdc, oldColor);
    SelectObject(hdc, oldFont);
}

void ConsoleDashboard::DrawMetric(HDC hdc, RECT rect, const wchar_t* label,
    const std::wstring& value, COLORREF valueColor) const {
    FillRect(hdc, &rect, m_hPanelAltBrush);
    FrameRect(hdc, &rect, m_hBorderBrush);
    RECT labelRect = {rect.left + 10, rect.top + 6, rect.right - 10, rect.top + 22};
    RECT valueRect = {rect.left + 10, rect.top + 23, rect.right - 10, rect.bottom - 5};
    DrawTextLine(hdc, label, labelRect, m_hLabel, console_theme::TEXT_DIM,
        DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    DrawTextLine(hdc, value, valueRect, m_hMonoBold, valueColor,
        DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

void ConsoleDashboard::Paint(HDC hdc) {
    RECT client = {};
    GetClientRect(m_hwnd, &client);
    FillRect(hdc, &client, m_hBackgroundBrush);
    if (client.right <= 0 || client.bottom <= 0)
        return;

    const int clientWidth = static_cast<int>(client.right);
    const int clientHeight = static_cast<int>(client.bottom);
    RECT panel = {PANEL_MARGIN, PANEL_MARGIN,
        (std::max)(PANEL_MARGIN + 1, clientWidth - PANEL_MARGIN),
        (std::max)(PANEL_MARGIN + 1, clientHeight - PANEL_MARGIN)};
    FillRect(hdc, &panel, m_hPanelBrush);
    FrameRect(hdc, &panel, m_hBorderBrush);

    const TransferStats& stats = m_stats;
    const COLORREF statusColor = m_hasStats ? StageColor(stats.stage) : console_theme::TEXT_DIM;
    std::wstring status = m_hasStats
        ? L"[ STATUS ]  " + StageText(stats.stage)
        : L"[ STATUS ]  \u7b49\u5f85\u4f20\u8f93\u4f1a\u8bdd";
    if (m_hasStats && !stats.stageText.empty() && stats.stage != TransferStage::Transferring)
        status += L"  |  " + stats.stageText;
    const int statusRight = (std::max)(panel.left + 220, panel.right - 190);
    RECT statusRect = {panel.left + 12, panel.top + 8, statusRight, panel.top + 28};
    DrawTextLine(hdc, status, statusRect, m_hMonoBold, statusColor,
        DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    RECT bar = {panel.left + 12, panel.top + 34, panel.right - 12, panel.top + 47};
    FillRect(hdc, &bar, m_hPanelAltBrush);
    FrameRect(hdc, &bar, m_hBorderBrush);
    long double percent = 0.0L;
    if (m_hasStats && stats.overallWorkTotal > 0) {
        percent = static_cast<long double>(stats.overallWorkCompleted) /
            static_cast<long double>(stats.overallWorkTotal);
    } else if (m_hasStats && stats.totalBytes > 0) {
        percent = static_cast<long double>(stats.displayTransferredBytes) /
            static_cast<long double>(stats.totalBytes);
    }
    percent = (std::max)(0.0L, (std::min)(1.0L, percent));
    RECT fill = bar;
    fill.left += 1;
    fill.top += 1;
    fill.bottom -= 1;
    fill.right = fill.left + static_cast<LONG>((fill.right - fill.left) * percent);
    if (fill.right > fill.left)
        FillRect(hdc, &fill, m_hAccentBrush);

    const int gap = 8;
    const int metricTop = panel.top + 56;
    const int metricBottom = (std::min)(metricTop + 58, static_cast<int>(panel.bottom - 42));
    const int metricWidth = (panel.right - panel.left - 24 - gap * 2) / 3;
    const int metricLeft = panel.left + 12;
    std::wstring speed = m_hasStats && stats.recentSpeedBytesPerSec > 0
        ? utils::FormatSpeed(stats.recentSpeedBytesPerSec) : L"--";
    std::wstring elapsed = m_hasStats ? utils::FormatDuration(stats.elapsedSeconds) : L"--";
    std::wstring total = m_hasStats ? utils::FormatDuration(stats.estimatedTotalSeconds) : L"--";
    if (m_hasStats && stats.estimatedTotalSeconds < 0)
        total = L"\u8ba1\u7b97\u4e2d...";
    DrawMetric(hdc, {metricLeft, metricTop, metricLeft + metricWidth, metricBottom},
        L"[ SPEED ]", speed, console_theme::ACCENT_CYAN);
    DrawMetric(hdc, {metricLeft + metricWidth + gap, metricTop,
        metricLeft + metricWidth * 2 + gap, metricBottom},
        L"[ ELAPSED ]", elapsed, console_theme::ACCENT);
    DrawMetric(hdc, {metricLeft + metricWidth * 2 + gap * 2, metricTop,
        panel.right - 12, metricBottom},
        L"[ TOTAL TIME ]", total, console_theme::WARNING);

    RECT details = {panel.left + 12, metricBottom + 6, panel.right - 12, panel.bottom - 7};
    std::wstring line = m_hasStats
        ? L"\u6587\u4ef6 " + std::to_wstring(stats.completedFiles) + L" / " +
          std::to_wstring(stats.totalFiles) + L"    \u6570\u636e " +
          utils::FormatBytes(stats.displayTransferredBytes) + L" / " +
          utils::FormatBytes(stats.totalBytes)
        : L"\u6587\u4ef6 --    \u6570\u636e --";
    DrawTextLine(hdc, line, details, m_hMono, console_theme::TEXT,
        DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    details.top += 17;
    std::wstring current = m_hasStats && !stats.currentFile.empty()
        ? L"\u5f53\u524d: " + stats.currentFile
        : L"\u5f53\u524d: --";
    DrawTextLine(hdc, current, details, m_hMono, console_theme::TEXT_DIM,
        DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

LRESULT CALLBACK ConsoleDashboard::StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ConsoleDashboard* self = reinterpret_cast<ConsoleDashboard*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
        self = reinterpret_cast<ConsoleDashboard*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->WndProc(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT ConsoleDashboard::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
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
