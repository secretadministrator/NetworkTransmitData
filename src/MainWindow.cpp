#include "MainWindow.h"
#include "Resource.h"
#include "RoleSelectPage.h"
#include "SenderPage.h"
#include "ReceiverPage.h"
#include "TransferSession.h"
#include "Utils.h"
#include "AuditLogger.h"
#include "Version.h"
#include <vector>
#include <algorithm>
#include <climits>

static constexpr int WINDOW_WIDTH = 820;
static constexpr int WINDOW_HEIGHT = 640;
static constexpr int LOG_HEIGHT = 120;
static constexpr int STATUS_HEIGHT = 24;
static constexpr int PAGE_TOP = 8;

static constexpr COLORREF COL_TEXT = RGB(51, 51, 51);
static constexpr COLORREF COL_TEXT_DIM = RGB(153, 153, 153);
static constexpr COLORREF COL_ACCENT = RGB(0, 103, 192);
static constexpr COLORREF COL_WHITE = RGB(255, 255, 255);
static constexpr COLORREF COL_BORDER = RGB(208, 208, 208);
static constexpr COLORREF COL_ACCENT_DIM = RGB(0, 80, 160);
static constexpr COLORREF COL_STEP_DONE = RGB(0, 170, 0);
static constexpr COLORREF COL_STEP_PENDING = RGB(220, 40, 40);

static HFONT CreateUIFont(int pointSize, UINT dpi, int weight = FW_NORMAL) {
    int height = -MulDiv(pointSize, static_cast<int>(dpi), 72);
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
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
    m_hDarkBgBrush = CreateSolidBrush(COL_WHITE);
    m_hDarkEditBrush = CreateSolidBrush(COL_WHITE);
}

MainWindow::~MainWindow() {
    delete m_currentPageObj;
    if (m_hSimSunFont) DeleteObject(m_hSimSunFont);
    if (m_hDarkBgBrush) DeleteObject(m_hDarkBgBrush);
    if (m_hDarkEditBrush) DeleteObject(m_hDarkEditBrush);
}

void MainWindow::RegisterWindowClass() {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = MainWindow::StaticWndProc;
    wc.hInstance = m_hInst;
    wc.hIcon = LoadIconA(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
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
        WS_OVERLAPPEDWINDOW,
        x, y, windowWidth, windowHeight,
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
    RECT client;
    GetClientRect(m_hwnd, &client);

    int gap = Dip(6);
    int progBarH = Dip(16);
    int progTextH = Dip(56);
    int logH = Dip(LOG_HEIGHT);
    int statusH = Dip(STATUS_HEIGHT);

    int statusY = client.bottom - statusH;
    int logY = statusY - gap - logH;
    int progTextY = logY - gap - progTextH;
    int progBarY = progTextY - gap - progBarH;
    int pageBottom = progBarY - gap;

    // Page container
    m_pageContainer = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE,
        0, Dip(PAGE_TOP), client.right, pageBottom - Dip(PAGE_TOP),
        m_hwnd, NULL, m_hInst, NULL);
    SetWindowSubclass(m_pageContainer, MainWindow::PageContainerProc, 0, (DWORD_PTR)m_hwnd);

    // Progress bar
    m_hProgressBar = CreateWindowExW(0, PROGRESS_CLASSW, L"",
        WS_CHILD | PBS_SMOOTH,
        Dip(16), progBarY, client.right - Dip(32), progBarH,
        m_hwnd, NULL, m_hInst, NULL);
    SendMessageW(m_hProgressBar, PBM_SETBKCOLOR, 0, RGB(230, 230, 230));
    SendMessageW(m_hProgressBar, PBM_SETBARCOLOR, 0, COL_ACCENT);

    // Progress text
    m_hProgressText = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | SS_LEFT | SS_NOPREFIX,
        Dip(16), progTextY, client.right - Dip(32), progTextH,
        m_hwnd, NULL, m_hInst, NULL);
    if (m_hSimSunFont) SendMessageW(m_hProgressText, WM_SETFONT, (WPARAM)m_hSimSunFont, TRUE);

    // Log list
    m_hLogList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT | LBS_HASSTRINGS,
        Dip(16), logY, client.right - Dip(32), logH,
        m_hwnd, NULL, m_hInst, NULL);
    if (m_hSimSunFont) SendMessageW(m_hLogList, WM_SETFONT, (WPARAM)m_hSimSunFont, TRUE);

    // Status bar
    m_hStatusBar = CreateWindowExW(0, L"STATIC", L"\u5c31\u7eea",
        WS_CHILD | WS_VISIBLE,
        0, statusY, client.right, statusH,
        m_hwnd, NULL, m_hInst, NULL);
    if (m_hSimSunFont) SendMessageW(m_hStatusBar, WM_SETFONT, (WPARAM)m_hSimSunFont, TRUE);

    // Reconnect button (hidden by default)
    m_hReconnectButton = CreateWindowExW(0, L"BUTTON", L"\u91cd\u65b0\u8fde\u63a5\u5e76\u7eed\u4f20",
        WS_CHILD | BS_PUSHBUTTON | BS_OWNERDRAW,
        client.right / 2 - Dip(90), progBarY - Dip(36), Dip(180), Dip(30),
        m_hwnd, (HMENU)(INT_PTR)IDC_BTN_RECONNECT, m_hInst, NULL);
    if (m_hSimSunFont) SendMessageW(m_hReconnectButton, WM_SETFONT, (WPARAM)m_hSimSunFont, TRUE);
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
}

void MainWindow::LayoutChildren() {
    if (!m_hwnd || !m_pageContainer) return;
    RECT client = {};
    GetClientRect(m_hwnd, &client);
    int gap = Dip(6);
    int margin = Dip(16);
    int statusH = Dip(STATUS_HEIGHT);
    int logH = Dip(LOG_HEIGHT);
    int progressTextH = Dip(56);
    int progressBarH = Dip(16);
    int statusY = client.bottom - statusH;
    int logY = statusY - gap - logH;
    int progressTextY = logY - gap - progressTextH;
    int progressBarY = progressTextY - gap - progressBarH;
    int pageTop = Dip(PAGE_TOP);
    int pageBottom = progressBarY - gap;

    MoveWindow(m_pageContainer, 0, pageTop, client.right,
        (std::max)(0, pageBottom - pageTop), TRUE);
    MoveWindow(m_hProgressBar, margin, progressBarY,
        (std::max)(0, static_cast<int>(client.right) - margin * 2), progressBarH, TRUE);
    MoveWindow(m_hProgressText, margin, progressTextY,
        (std::max)(0, static_cast<int>(client.right) - margin * 2), progressTextH, TRUE);
    MoveWindow(m_hLogList, margin, logY,
        (std::max)(0, static_cast<int>(client.right) - margin * 2), logH, TRUE);
    MoveWindow(m_hStatusBar, 0, statusY, client.right, statusH, TRUE);
    MoveWindow(m_hReconnectButton, client.right / 2 - Dip(90),
        progressBarY - Dip(36), Dip(180), Dip(30), TRUE);

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

    RECT rc;
    GetClientRect(m_pageContainer, &rc);

    IPage* newPage = nullptr;

    switch (page) {
        case AppPage::ROLE_SELECT:
            newPage = new RoleSelectPage(m_pageContainer, rc, m_hwnd);
            ShowWindow(m_hProgressBar, SW_HIDE);
            ShowWindow(m_hProgressText, SW_HIDE);
            break;

        case AppPage::SENDER_SETUP:
            newPage = new SenderPage(m_hInst, m_pageContainer, rc, m_hwnd);
            ShowWindow(m_hProgressBar, SW_HIDE);
            ShowWindow(m_hProgressText, SW_HIDE);
            break;

        case AppPage::RECEIVER_SETUP:
            newPage = new ReceiverPage(m_hInst, m_pageContainer, rc, m_hwnd);
            ShowWindow(m_hProgressBar, SW_HIDE);
            ShowWindow(m_hProgressText, SW_HIDE);
            break;
    }

    m_currentPageObj = newPage;
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
    if (!m_hLogList) return;
    int idx = SendMessageW(m_hLogList, LB_ADDSTRING, 0, (LPARAM)msg.c_str());
    SendMessageW(m_hLogList, LB_SETTOPINDEX, idx, 0);
    HDC hdc = GetDC(m_hLogList);
    if (hdc) {
        HFONT old = (HFONT)SelectObject(hdc, m_hSimSunFont);
        SIZE size = {};
        if (GetTextExtentPoint32W(hdc, msg.c_str(), static_cast<int>(msg.size()), &size)) {
            LRESULT current = SendMessageW(m_hLogList, LB_GETHORIZONTALEXTENT, 0, 0);
            if (size.cx + Dip(24) > current)
                SendMessageW(m_hLogList, LB_SETHORIZONTALEXTENT, size.cx + Dip(24), 0);
        }
        SelectObject(hdc, old);
        ReleaseDC(m_hLogList, hdc);
    }
    AuditLogger::Instance().Write(msg);
}

void MainWindow::SetStatusText(const std::wstring& text) {
    if (m_hStatusBar)
        SetWindowTextW(m_hStatusBar, text.c_str());
}

void MainWindow::OnTransferProgress(const TransferStats& stats) {
    if (stats.scanning) {
        if (m_hProgressBar) {
            SendMessageW(m_hProgressBar, PBM_SETRANGE32, 0, 100);
            SendMessageW(m_hProgressBar, PBM_SETPOS, 0, 0);
        }
        if (m_hProgressText) {
            std::wstring text = L"\u6b63\u5728\u626b\u63cf  |  \u6587\u4ef6: " + std::to_wstring(stats.totalFiles)
                + L"  |  \u76ee\u5f55: " + std::to_wstring(stats.scannedDirectories)
                + L"  |  \u5df2\u53d1\u73b0: " + utils::FormatBytes(stats.scannedBytes);
            if (stats.inaccessibleDirectories > 0)
                text += L"  |  \u65e0\u6cd5\u8bbf\u95ee: " + std::to_wstring(stats.inaccessibleDirectories);
            if (!stats.currentFile.empty())
                text += L"\r\n\u5f53\u524d\u76ee\u5f55: " + stats.currentFile;
            SetWindowTextW(m_hProgressText, text.c_str());
        }
        return;
    }

    if (stats.stage == TransferStage::HashingSource ||
        stats.stage == TransferStage::WaitingForPlan ||
        stats.stage == TransferStage::BuildingPlan ||
        stats.stage == TransferStage::VerifyingPlan) {
        if (m_hProgressBar) {
            int total = stats.stageTotal > 0 && stats.stageTotal <= INT_MAX
                ? static_cast<int>(stats.stageTotal) : 100;
            int processed = stats.stageTotal > 0 && stats.stageProcessed <= INT_MAX
                ? static_cast<int>(stats.stageProcessed) : 0;
            SendMessageW(m_hProgressBar, PBM_SETRANGE32, 0, total);
            SendMessageW(m_hProgressBar, PBM_SETPOS, processed, 0);
        }
        if (m_hProgressText) {
            std::wstring text = stats.stageText.empty()
                ? L"\u6b63\u5728\u51c6\u5907\u4f20\u8f93" : stats.stageText;
            if (stats.stageTotal > 0)
                text += L"  |  " + std::to_wstring(stats.stageProcessed)
                    + L" / " + std::to_wstring(stats.stageTotal);
            if (stats.stageBytes > 0)
                text += L"  |  \u5df2\u6821\u9a8c " + utils::FormatBytes(stats.stageBytes);
            if (!stats.currentFile.empty())
                text += L"\r\n\u5f53\u524d: " + stats.currentFile;
            SetWindowTextW(m_hProgressText, text.c_str());
        }
        return;
    }

    if (m_hProgressBar && stats.totalBytes > 0) {
        int64_t range = (stats.totalBytes + 1023) / 1024;
        int64_t pos = (stats.transferredBytes + 1023) / 1024;
        if (range < 1) range = 1;
        if (pos > range) pos = range;
        SendMessageW(m_hProgressBar, PBM_SETRANGE32, 0, (LPARAM)range);
        SendMessageW(m_hProgressBar, PBM_SETPOS, (WPARAM)pos, 0);
    }
    if (m_hProgressText) {
        std::wstring text = utils::FormatBytes(stats.transferredBytes) + L" / "
            + utils::FormatBytes(stats.totalBytes)
            + L"  |  \u6700\u8fd1: " + utils::FormatSpeed(stats.recentSpeedBytesPerSec)
            + L"  |  \u5e73\u5747: " + utils::FormatSpeed(stats.averageSpeedBytesPerSec)
            + L"  |  \u9884\u8ba1\u5269\u4f59: "
            + utils::FormatDuration(stats.estimatedRemainingSeconds);
        if (stats.waitingForIo)
            text += L"  (\u6b63\u5728\u7b49\u5f85\u78c1\u76d8/\u7f51\u7edc)";
        if (!stats.currentFile.empty())
            text += L"\r\n\u5f53\u524d\u6587\u4ef6: " + stats.currentFile;
        SetWindowTextW(m_hProgressText, text.c_str());
    }
}

void MainWindow::DrawButton(DRAWITEMSTRUCT* dis) {
    wchar_t text[128] = {};
    GetWindowTextW(dis->hwndItem, text, 128);

    bool primary = false;
    switch (dis->CtlID) {
        case IDC_BTN_SEND:
        case IDC_BTN_RECV:
        case IDC_BTN_START:
            primary = true;
            break;
    }

    RECT rc = dis->rcItem;
    HDC hdc = dis->hDC;

    bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    bool focused = (dis->itemState & ODS_FOCUS) != 0;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;

    COLORREF bgColor, textColor, borderColor;

    if (primary) {
        bgColor = pressed ? COL_ACCENT_DIM : COL_ACCENT;
        textColor = COL_WHITE;
        borderColor = COL_ACCENT;
    } else {
        bgColor = pressed ? RGB(230, 230, 230) : COL_WHITE;
        textColor = disabled ? COL_TEXT_DIM : COL_TEXT;
        borderColor = COL_BORDER;
    }

    HBRUSH hBg = CreateSolidBrush(bgColor);
    FillRect(hdc, &rc, hBg);
    DeleteObject(hBg);

    HPEN hPen = CreatePen(PS_SOLID, 1, borderColor);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, hOldPen);
    SelectObject(hdc, hOldBrush);
    DeleteObject(hPen);

    SelectObject(hdc, m_hSimSunFont);
    SetTextColor(hdc, textColor);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    if (focused && !disabled) {
        RECT focusRect = rc;
        InflateRect(&focusRect, -3, -3);
        DrawFocusRect(hdc, &focusRect);
    }
}

LRESULT MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
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
            info->ptMinTrackSize.y = Dip(560);
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
                ShowWindow(m_hProgressBar, SW_SHOW);
                ShowWindow(m_hProgressText, SW_SHOW);
                SetWindowTextW(m_hProgressText, L"\u6b63\u5728\u91cd\u65b0\u8fde\u63a5...");
                if (m_currentPageObj) {
                    m_currentPageObj->HandleCommand(IDC_BTN_RECONNECT, NULL, 0);
                }
                return 0;
            }
            if (m_currentPageObj &&
                m_currentPageObj->HandleCommand(id, (HWND)lp, HIWORD(wp))) {
                return 0;
            }
            break;
        }

        case WM_DISCOVERY_FOUND:
        case WM_DISCOVERY_FAILED:
            if (m_currentPageObj && m_currentPageObj->HandleMessage(msg, wp, lp))
                return 0;
            break;

        case WM_DRAWITEM: {
            DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lp;
            if (dis->CtlType == ODT_BUTTON) {
                DrawButton(dis);
                return TRUE;
            }
            break;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wp;
            SetBkMode(hdc, TRANSPARENT);
            HWND hCtrl = (HWND)lp;
            int ctrlId = GetDlgCtrlID(hCtrl);
            if (ctrlId >= IDC_STEP_DOT_1 && ctrlId <= IDC_STEP_DOT_4) {
                int dotIdx = ctrlId - IDC_STEP_DOT_1;
                bool isReceiver = (m_currentPage == AppPage::RECEIVER_SETUP);
                int currentStep = isReceiver ? m_receiverStep : m_senderStep;
                SelectObject(hdc, m_hSimSunFont);
                SetTextColor(hdc, dotIdx <= currentStep ? COL_STEP_DONE : COL_STEP_PENDING);
                return (LRESULT)GetStockObject(NULL_BRUSH);
            }
            if (hCtrl == m_hStatusBar) {
                SelectObject(hdc, m_hSimSunFont);
                SetTextColor(hdc, COL_TEXT_DIM);
            } else {
                SelectObject(hdc, m_hSimSunFont);
                SetTextColor(hdc, COL_TEXT);
            }
            return (LRESULT)m_hDarkBgBrush;
        }

        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wp;
            SelectObject(hdc, m_hSimSunFont);
            SetTextColor(hdc, COL_TEXT);
            SetBkColor(hdc, COL_WHITE);
            return (LRESULT)m_hDarkEditBrush;
        }

        case WM_CTLCOLORLISTBOX: {
            HDC hdc = (HDC)wp;
            SelectObject(hdc, m_hSimSunFont);
            SetTextColor(hdc, COL_TEXT);
            SetBkColor(hdc, COL_WHITE);
            return (LRESULT)m_hDarkEditBrush;
        }

        case WM_STEP_CHANGED: {
            int step = (int)wp;
            bool isReceiver = (lp != 0);
            if (isReceiver) m_receiverStep = step;
            else m_senderStep = step;
            RedrawWindow(m_hwnd, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN);
            return 0;
        }

        case WM_TRANSFER_LOG: {
            std::wstring* str = (std::wstring*)lp;
            if (str) {
                LogMessage(*str);
                delete str;
            }
            return 0;
        }

        case WM_TRANSFER_PROGRESS: {
            TransferStats* s = (TransferStats*)wp;
            if (s) {
                ShowWindow(m_hProgressBar, SW_SHOW);
                ShowWindow(m_hProgressText, SW_SHOW);
                OnTransferProgress(*s);
                delete s;
            }
            return 0;
        }

        case WM_TRANSFER_DONE: {
            TransferResult* comp = (TransferResult*)wp;

            bool preserveConnectionLostUi = false;

            if (comp) {
                preserveConnectionLostUi =
                    (comp->code == TransferResultCode::ConnectionLost &&
                     comp->role == TransferRole::SENDER &&
                     comp->resumable);

                std::wstring msg;

                switch (comp->code) {
                case TransferResultCode::Success:
                    msg = L"\u4f20\u8f93\u5df2\u5b8c\u6210";
                    break;

                case TransferResultCode::Cancelled:
                    msg = L"\u4f20\u8f93\u5df2\u53d6\u6d88";
                    break;

                case TransferResultCode::ConnectionLost:
                    msg = L"\u8fde\u63a5\u5df2\u4e2d\u65ad\uff0c\u53ef\u4ee5\u91cd\u65b0\u8fde\u63a5\u7eed\u4f20";
                    break;

                case TransferResultCode::FileError:
                case TransferResultCode::ProtocolError:
                case TransferResultCode::InternalError:
                default:
                    msg = comp->message.empty()
                        ? L"\u4f20\u8f93\u5931\u8d25"
                        : comp->message;
                    break;
                }

                LogMessage(msg);

                delete comp;
                comp = nullptr;
            }

            if (!preserveConnectionLostUi) {
                SendMessageW(m_hProgressBar, PBM_SETPOS, 0, 0);
                SendMessageW(m_hProgressBar, PBM_SETRANGE32, 0, 100);
                SetWindowTextW(m_hProgressText, L"");
                ShowWindow(m_hProgressBar, SW_HIDE);
                ShowWindow(m_hProgressText, SW_HIDE);
            }

            if (preserveConnectionLostUi && m_hReconnectButton) {
                ShowWindow(m_hReconnectButton, SW_SHOW);
                EnableWindow(m_hReconnectButton, TRUE);
            }

            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK MainWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainWindow* pThis = nullptr;
    if (msg == WM_NCCREATE) {
        LPCREATESTRUCTW cs = (LPCREATESTRUCTW)lp;
        pThis = (MainWindow*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)pThis);
    } else {
        pThis = (MainWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    if (pThis) return pThis->WndProc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK MainWindow::PageContainerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR dwRefData) {
    if (msg == WM_COMMAND) {
        HWND hMainWnd = (HWND)dwRefData;
        PostMessageW(hMainWnd, msg, wp, lp);
        return 0;
    }
    if (msg == WM_DRAWITEM || msg == WM_MEASUREITEM ||
        msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX) {
        HWND hMainWnd = (HWND)dwRefData;
        return SendMessageW(hMainWnd, msg, wp, lp);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}
