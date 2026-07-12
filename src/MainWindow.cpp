#include "MainWindow.h"
#include "Resource.h"
#include "RoleSelectPage.h"
#include "SenderPage.h"
#include "ReceiverPage.h"
#include "AuditLogger.h"
#include "ConsoleTheme.h"
#include "Utils.h"
#include "Version.h"
#include <algorithm>
#include <vector>

static constexpr int WINDOW_WIDTH = 820;
static constexpr int WINDOW_HEIGHT = 580;
static constexpr int LOG_HEIGHT = 78;
static constexpr int DASHBOARD_HEIGHT = 120;
static constexpr int STATUS_HEIGHT = 22;
static constexpr int PAGE_TOP = 6;

static HFONT CreateUIFont(int pointSize, UINT dpi, int weight = FW_NORMAL) {
    return console_theme::CreateFontForDpi(pointSize, dpi, L"Consolas", weight);
}

static UINT QueryWindowDpi(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static auto fn = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (fn && hwnd) return fn(hwnd);
    HDC hdc = GetDC(hwnd);
    UINT dpi = hdc ? static_cast<UINT>(GetDeviceCaps(hdc, LOGPIXELSX)) : 96;
    if (hdc) ReleaseDC(hwnd, hdc);
    return dpi ? dpi : 96;
}

MainWindow::MainWindow() {
    m_hBackgroundBrush = CreateSolidBrush(console_theme::BACKGROUND);
    m_hEditBrush = CreateSolidBrush(console_theme::PANEL_ALT);
    m_hPrimaryButtonBrush = CreateSolidBrush(console_theme::ACCENT);
    m_hPrimaryPressedBrush = CreateSolidBrush(RGB(55, 55, 55));
    m_hButtonBrush = CreateSolidBrush(console_theme::PANEL_ALT);
    m_hButtonPressedBrush = CreateSolidBrush(console_theme::PANEL);
    m_hPrimaryButtonPen = CreatePen(PS_SOLID, 1, console_theme::ACCENT);
    m_hButtonPen = CreatePen(PS_SOLID, 1, console_theme::BORDER);
}

MainWindow::~MainWindow() {
    delete m_currentPageObj;
    if (m_hSimSunFont) DeleteObject(m_hSimSunFont);
    if (m_hBackgroundBrush) DeleteObject(m_hBackgroundBrush);
    if (m_hEditBrush) DeleteObject(m_hEditBrush);
    if (m_hPrimaryButtonBrush) DeleteObject(m_hPrimaryButtonBrush);
    if (m_hPrimaryPressedBrush) DeleteObject(m_hPrimaryPressedBrush);
    if (m_hButtonBrush) DeleteObject(m_hButtonBrush);
    if (m_hButtonPressedBrush) DeleteObject(m_hButtonPressedBrush);
    if (m_hPrimaryButtonPen) DeleteObject(m_hPrimaryButtonPen);
    if (m_hButtonPen) DeleteObject(m_hButtonPen);
}

void MainWindow::RegisterWindowClass() {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = MainWindow::StaticWndProc;
    wc.hInstance = m_hInst;
    wc.hIcon = LoadIconA(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"DirectTransferMain";
    RegisterClassW(&wc);
}

bool MainWindow::Create(HINSTANCE hInst, int nCmdShow) {
    m_hInst = hInst;
    RegisterWindowClass();

    RECT desk;
    GetWindowRect(GetDesktopWindow(), &desk);
    UINT initialDpi = QueryWindowDpi(nullptr);
    int windowWidth = MulDiv(WINDOW_WIDTH, initialDpi, 96);
    int windowHeight = MulDiv(WINDOW_HEIGHT, initialDpi, 96);
    int x = (desk.right - windowWidth) / 2;
    int y = (desk.bottom - windowHeight) / 2;

    std::wstring title = L"DirectTransfer " + std::wstring(version::APP_VERSION)
        + L" - \u76f4\u8fde\u4f20\u8f93\u5de5\u5177";
    m_hwnd = CreateWindowExW(0, L"DirectTransferMain", title.c_str(),
        WS_OVERLAPPEDWINDOW, x, y, windowWidth, windowHeight,
        NULL, NULL, m_hInst, this);
    if (!m_hwnd) return false;

    m_dpi = QueryWindowDpi(m_hwnd);
    UpdateFonts(m_dpi);
    CreateLayout();
    ShowWindow(m_hwnd, nCmdShow);
    UpdateWindow(m_hwnd);
    SwitchToPage(AppPage::ROLE_SELECT);
    return true;
}

void MainWindow::CreateLayout() {
    RECT client = {};
    GetClientRect(m_hwnd, &client);
    const int gap = Dip(6);
    const int dashboardH = Dip(DASHBOARD_HEIGHT);
    const int logH = Dip(LOG_HEIGHT);
    const int statusH = Dip(STATUS_HEIGHT);
    const int statusY = client.bottom - statusH;
    const int logY = statusY - gap - logH;
    const int dashboardY = logY - gap - dashboardH;
    const int pageBottom = dashboardY - gap;

    m_pageContainer = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE, 0, Dip(PAGE_TOP), client.right,
        (std::max)(0, pageBottom - Dip(PAGE_TOP)), m_hwnd, NULL, m_hInst, NULL);
    SetWindowSubclass(m_pageContainer, MainWindow::PageContainerProc, 0,
        (DWORD_PTR)m_hwnd);

    m_dashboard.Create(m_hInst, m_hwnd, IDC_PROGRESS);
    m_logView.Create(m_hInst, m_hwnd, IDC_LIST_LOG);
    m_hStatusBar = CreateWindowExW(0, L"STATIC", L"[ READY ]",
        WS_CHILD | WS_VISIBLE, 0, statusY, client.right, statusH,
        m_hwnd, NULL, m_hInst, NULL);
    if (m_hSimSunFont)
        SendMessageW(m_hStatusBar, WM_SETFONT, (WPARAM)m_hSimSunFont, TRUE);

    m_hReconnectButton = CreateWindowExW(0, L"BUTTON", L"[ RECONNECT ]",
        WS_CHILD | BS_PUSHBUTTON | BS_OWNERDRAW,
        client.right - Dip(186), dashboardY + Dip(20), Dip(170), Dip(30),
        m_hwnd, (HMENU)(INT_PTR)IDC_BTN_RECONNECT, m_hInst, NULL);
    if (m_hSimSunFont)
        SendMessageW(m_hReconnectButton, WM_SETFONT, (WPARAM)m_hSimSunFont, TRUE);

    ShowWindow(m_dashboard.GetHWND(), SW_HIDE);
    ShowWindow(m_hReconnectButton, SW_HIDE);
    LayoutChildren();
}

void MainWindow::UpdateFonts(UINT dpi) {
    HFONT newFont = CreateUIFont(10, dpi);
    if (!newFont) return;
    HFONT oldFont = m_hSimSunFont;
    m_hSimSunFont = newFont;
    if (m_hwnd) {
        EnumChildWindows(m_hwnd, [](HWND child, LPARAM font) -> BOOL {
            SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
            return TRUE;
        }, reinterpret_cast<LPARAM>(newFont));
    }
    if (oldFont) DeleteObject(oldFont);
    m_dashboard.SetDpi(m_dpi);
    m_logView.SetDpi(m_dpi);
}

void MainWindow::LayoutChildren() {
    if (!m_hwnd || !m_pageContainer) return;
    RECT client = {};
    GetClientRect(m_hwnd, &client);
    const int gap = Dip(4);
    const int margin = Dip(10);
    const int statusH = Dip(STATUS_HEIGHT);
    const int logH = Dip(LOG_HEIGHT);
    const int dashboardH = Dip(DASHBOARD_HEIGHT);
    const int statusY = client.bottom - statusH;
    const bool dashboardVisible = IsWindowVisible(m_dashboard.GetHWND()) != FALSE;
    const int combinedLogH = dashboardVisible ? logH : logH + gap + dashboardH;
    const int logY = statusY - gap - combinedLogH;
    const int dashboardY = logY - gap - dashboardH;
    const int pageTop = Dip(PAGE_TOP);
    const int pageBottom = dashboardVisible ? dashboardY - gap : logY - gap;

    MoveWindow(m_pageContainer, 0, pageTop, client.right,
        (std::max)(0, pageBottom - pageTop), TRUE);
    MoveWindow(m_dashboard.GetHWND(), 0, dashboardY, client.right, dashboardH, TRUE);
    MoveWindow(m_logView.GetHWND(), 0, logY, client.right, combinedLogH, TRUE);
    MoveWindow(m_hStatusBar, 0, statusY, client.right, statusH, TRUE);
    MoveWindow(m_hReconnectButton, client.right - margin - Dip(170),
        dashboardY + Dip(20), Dip(170), Dip(30), TRUE);

    if (m_currentPageObj) {
        RECT page = {};
        GetClientRect(m_pageContainer, &page);
        m_currentPageObj->Relayout(page, m_dpi);
    }
}

void MainWindow::DestroyPageContent() {
    delete m_currentPageObj;
    m_currentPageObj = nullptr;

    std::vector<HWND> children;
    EnumChildWindows(m_pageContainer, [](HWND h, LPARAM lp) -> BOOL {
        ((std::vector<HWND>*)lp)->push_back(h);
        return TRUE;
    }, (LPARAM)&children);
    for (HWND h : children)
        DestroyWindow(h);
}

void MainWindow::SwitchToPage(AppPage page) {
    DestroyPageContent();
    m_currentPage = page;
    RECT rc = {};
    GetClientRect(m_pageContainer, &rc);
    IPage* newPage = nullptr;

    switch (page) {
    case AppPage::ROLE_SELECT:
        newPage = new RoleSelectPage(m_pageContainer, rc, m_hwnd);
        break;
    case AppPage::SENDER_SETUP:
        newPage = new SenderPage(m_hInst, m_pageContainer, rc, m_hwnd);
        break;
    case AppPage::RECEIVER_SETUP:
        newPage = new ReceiverPage(m_hInst, m_pageContainer, rc, m_hwnd);
        break;
    }

    m_currentPageObj = newPage;
    ShowWindow(m_dashboard.GetHWND(), SW_HIDE);
    ShowWindow(m_hReconnectButton, SW_HIDE);
    LayoutChildren();
}

void MainWindow::RunMessageLoop() {
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void MainWindow::LogMessage(const std::wstring& msg) {
    m_logView.AddLine(msg);
    AuditLogger::Instance().Write(msg);
}

void MainWindow::SetStatusText(const std::wstring& text) {
    if (m_hStatusBar)
        SetWindowTextW(m_hStatusBar, text.c_str());
}

void MainWindow::OnTransferProgress(const TransferStats& stats) {
    const bool wasHidden = IsWindowVisible(m_dashboard.GetHWND()) == FALSE;
    ShowWindow(m_dashboard.GetHWND(), SW_SHOW);
    if (wasHidden)
        LayoutChildren();
    m_dashboard.SetStats(stats);
    if (stats.stage == TransferStage::Reconnecting)
        SetStatusText(L"[ RECONNECTING ]");
    else if (stats.scanning)
        SetStatusText(L"[ SCANNING ]");
    else if (stats.stage == TransferStage::Transferring)
        SetStatusText(L"[ TRANSFERRING ]");
    else
        SetStatusText(L"[ PREPARING ]");
}

void MainWindow::DrawButton(DRAWITEMSTRUCT* dis) {
    wchar_t text[128] = {};
    GetWindowTextW(dis->hwndItem, text, 128);
    const bool primary = dis->CtlID == IDC_BTN_SEND ||
        dis->CtlID == IDC_BTN_RECV || dis->CtlID == IDC_BTN_START;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool focused = (dis->itemState & ODS_FOCUS) != 0;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    HBRUSH brush = primary
        ? (pressed ? m_hPrimaryPressedBrush : m_hPrimaryButtonBrush)
        : (pressed ? m_hButtonPressedBrush : m_hButtonBrush);
    HPEN pen = primary ? m_hPrimaryButtonPen : m_hButtonPen;
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    FillRect(hdc, &rc, brush);
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    HFONT oldFont = (HFONT)SelectObject(hdc, m_hSimSunFont);
    SetBkMode(hdc, TRANSPARENT);
    const COLORREF textColor = dis->CtlID == IDC_BTN_START
        ? console_theme::BACKGROUND
        : (disabled ? console_theme::TEXT_DIM
                    : (primary ? console_theme::BACKGROUND : console_theme::TEXT));
    SetTextColor(hdc, textColor);
    DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, oldFont);
    if (focused && !disabled) {
        RECT focusRect = rc;
        InflateRect(&focusRect, -3, -3);
        DrawFocusRect(hdc, &focusRect);
    }
}

LRESULT MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc = {};
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, m_hBackgroundBrush);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        LayoutChildren();
        return 0;
    case WM_DPICHANGED: {
        UINT newDpi = HIWORD(wp);
        RECT* suggested = reinterpret_cast<RECT*>(lp);
        m_dpi = newDpi ? newDpi : 96;
        if (suggested) {
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        UpdateFonts(m_dpi);
        LayoutChildren();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lp);
        info->ptMinTrackSize.x = Dip(680);
        info->ptMinTrackSize.y = Dip(540);
        return 0;
    }
    case WM_COMMAND: {
        WORD id = LOWORD(wp);
        if (id == IDC_BTN_SEND) {
            SwitchToPage(AppPage::SENDER_SETUP);
            return 0;
        }
        if (id == IDC_BTN_RECV) {
            SwitchToPage(AppPage::RECEIVER_SETUP);
            return 0;
        }
        if (id == IDC_BTN_BACK) {
            SwitchToPage(AppPage::ROLE_SELECT);
            return 0;
        }
        if (id == IDC_BTN_RECONNECT) {
            EnableWindow(m_hReconnectButton, FALSE);
            ShowWindow(m_hReconnectButton, SW_HIDE);
            ShowWindow(m_dashboard.GetHWND(), SW_SHOW);
            SetStatusText(L"[ RECONNECTING ]");
            if (m_currentPageObj)
                m_currentPageObj->HandleCommand(IDC_BTN_RECONNECT, NULL, 0);
            return 0;
        }
        if (m_currentPageObj && m_currentPageObj->HandleCommand(
                id, (HWND)lp, HIWORD(wp)))
            return 0;
        break;
    }
    case WM_DISCOVERY_FOUND:
    case WM_DISCOVERY_FAILED:
        if (m_currentPageObj && m_currentPageObj->HandleMessage(msg, wp, lp))
            return 0;
        break;
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (dis && dis->CtlType == ODT_BUTTON) {
            DrawButton(dis);
            return TRUE;
        }
        if (dis && dis->CtlType == ODT_COMBOBOX) {
            HDC hdc = dis->hDC;
            RECT rc = dis->rcItem;
            FillRect(hdc, &rc, m_hEditBrush);
            HPEN oldPen = (HPEN)SelectObject(hdc, m_hButtonPen);
            HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
            Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            wchar_t text[256] = {};
            if (dis->itemID != (UINT)-1)
                SendMessageW(dis->hwndItem, CB_GETLBTEXT, dis->itemID, (LPARAM)text);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, (dis->itemState & ODS_DISABLED) ? console_theme::TEXT_DIM : console_theme::TEXT);
            SelectObject(hdc, m_hSimSunFont);
            InflateRect(&rc, -8, 0);
            DrawTextW(hdc, text, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        HWND control = (HWND)lp;
        const int id = GetDlgCtrlID(control);
        SetBkMode(hdc, TRANSPARENT);
        SelectObject(hdc, m_hSimSunFont);
        if (id >= IDC_STEP_DOT_1 && id <= IDC_STEP_DOT_4) {
            const int dotIndex = id - IDC_STEP_DOT_1;
            const bool receiver = m_currentPage == AppPage::RECEIVER_SETUP;
            const int currentStep = receiver ? m_receiverStep : m_senderStep;
            SetTextColor(hdc, dotIndex <= currentStep
                ? console_theme::ACCENT : console_theme::TEXT_DIM);
        } else {
            SetTextColor(hdc, control == m_hStatusBar
                ? console_theme::TEXT_DIM : console_theme::TEXT);
        }
        return (LRESULT)m_hBackgroundBrush;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wp;
        SelectObject(hdc, m_hSimSunFont);
        SetTextColor(hdc, console_theme::TEXT);
        SetBkColor(hdc, console_theme::PANEL_ALT);
        return (LRESULT)m_hEditBrush;
    }
    case WM_CTLCOLORLISTBOX: {
        HDC hdc = (HDC)wp;
        SelectObject(hdc, m_hSimSunFont);
        SetTextColor(hdc, console_theme::TEXT);
        SetBkColor(hdc, console_theme::PANEL_ALT);
        return (LRESULT)m_hEditBrush;
    }
    case WM_STEP_CHANGED: {
        const int step = (int)wp;
        if (lp) m_receiverStep = step;
        else m_senderStep = step;
        RedrawWindow(m_hwnd, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN);
        return 0;
    }
    case WM_TRANSFER_LOG: {
        std::wstring* str = reinterpret_cast<std::wstring*>(lp);
        if (str) {
            LogMessage(*str);
            delete str;
        }
        return 0;
    }
    case WM_TRANSFER_PROGRESS: {
        TransferStats* stats = reinterpret_cast<TransferStats*>(wp);
        if (stats) {
            OnTransferProgress(*stats);
            delete stats;
        }
        return 0;
    }
    case WM_TRANSFER_DONE: {
        TransferResult* result = reinterpret_cast<TransferResult*>(wp);
        bool preserveConnectionLostUi = false;
        if (result) {
            preserveConnectionLostUi = result->code == TransferResultCode::ConnectionLost &&
                result->role == TransferRole::SENDER && result->resumable;
            if (preserveConnectionLostUi)
                m_dashboard.SetStats(result->stats);

            std::wstring message;
            switch (result->code) {
            case TransferResultCode::Success:
                message = L"\u4f20\u8f93\u5df2\u5b8c\u6210";
                break;
            case TransferResultCode::Cancelled:
                message = L"\u4f20\u8f93\u5df2\u53d6\u6d88";
                break;
            case TransferResultCode::ConnectionLost:
                message = L"\u8fde\u63a5\u5df2\u4e2d\u65ad\uff0c\u53ef\u4ee5\u91cd\u65b0\u8fde\u63a5\u7eed\u4f20";
                break;
            default:
                message = result->message.empty() ? L"\u4f20\u8f93\u5931\u8d25" : result->message;
                break;
            }
            LogMessage(message);
            if (result->code == TransferResultCode::Success) {
                LogMessage(L"\u4f20\u8f93\u603b\u7528\u65f6: " +
                    utils::FormatDuration(result->stats.elapsedSeconds));
            }
            delete result;
        }
        if (!preserveConnectionLostUi) {
            m_dashboard.Clear();
            ShowWindow(m_dashboard.GetHWND(), SW_HIDE);
            ShowWindow(m_hReconnectButton, SW_HIDE);
            LayoutChildren();
            SetStatusText(L"[ READY ]");
        } else {
            ShowWindow(m_dashboard.GetHWND(), SW_SHOW);
            ShowWindow(m_hReconnectButton, SW_SHOW);
            EnableWindow(m_hReconnectButton, TRUE);
            SetStatusText(L"[ CONNECTION LOST ]");
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK MainWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        LPCREATESTRUCTW cs = reinterpret_cast<LPCREATESTRUCTW>(lp);
        self = reinterpret_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->WndProc(hwnd, msg, wp, lp)
                : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK MainWindow::PageContainerProc(HWND hwnd, UINT msg, WPARAM wp,
    LPARAM lp, UINT_PTR, DWORD_PTR dwRefData) {
    HWND mainWindow = reinterpret_cast<HWND>(dwRefData);
    if (msg == WM_COMMAND) {
        PostMessageW(mainWindow, msg, wp, lp);
        return 0;
    }
    if (msg == WM_DRAWITEM || msg == WM_MEASUREITEM ||
        msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLOREDIT ||
        msg == WM_CTLCOLORLISTBOX) {
        return SendMessageW(mainWindow, msg, wp, lp);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}
