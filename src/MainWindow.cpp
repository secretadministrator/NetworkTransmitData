#include "MainWindow.h"
#include "Resource.h"
#include "RoleSelectPage.h"
#include "SenderPage.h"
#include "ReceiverPage.h"
#include "TransferSession.h"
#include "Utils.h"
#include "AuditLogger.h"
#include <vector>

static constexpr int WINDOW_WIDTH = 700;
static constexpr int WINDOW_HEIGHT = 500;
static constexpr int LOG_HEIGHT = 104;
static constexpr int STATUS_HEIGHT = 20;
static constexpr int PAGE_TOP = 4;

static constexpr COLORREF COL_TEXT = RGB(51, 51, 51);
static constexpr COLORREF COL_TEXT_DIM = RGB(153, 153, 153);
static constexpr COLORREF COL_ACCENT = RGB(0, 103, 192);
static constexpr COLORREF COL_WHITE = RGB(255, 255, 255);
static constexpr COLORREF COL_BORDER = RGB(208, 208, 208);
static constexpr COLORREF COL_ACCENT_DIM = RGB(0, 80, 160);
static constexpr COLORREF COL_STEP_DONE = RGB(0, 170, 0);
static constexpr COLORREF COL_STEP_PENDING = RGB(220, 40, 40);

static HFONT CreateSimSun(int pointSize) {
    HDC hdc = GetDC(NULL);
    int height = -MulDiv(pointSize, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    ReleaseDC(NULL, hdc);
    return CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"SimSun");
}

static HFONT CreateConsolas(int pointSize) {
    HDC hdc = GetDC(NULL);
    int height = -MulDiv(pointSize, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    ReleaseDC(NULL, hdc);
    return CreateFontW(height, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
}

MainWindow::MainWindow() {
    m_hSimSunFont = CreateSimSun(10);
    m_hCodeFont = CreateConsolas(22);
    m_hDarkBgBrush = CreateSolidBrush(COL_WHITE);
    m_hDarkEditBrush = CreateSolidBrush(COL_WHITE);
}

MainWindow::~MainWindow() {
    delete m_currentPageObj;
    if (m_hSimSunFont) DeleteObject(m_hSimSunFont);
    if (m_hCodeFont) DeleteObject(m_hCodeFont);
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
    int x = (desk.right - WINDOW_WIDTH) / 2;
    int y = (desk.bottom - WINDOW_HEIGHT) / 2;

    m_hwnd = CreateWindowExW(0, L"DirectTransferMain", L"DirectTransfer - \u76f4\u8fde\u4f20\u8f93\u5de5\u5177",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        x, y, WINDOW_WIDTH, WINDOW_HEIGHT,
        NULL, NULL, m_hInst, this);

    if (!m_hwnd) return false;

    CreateLayout();
    ShowWindow(m_hwnd, nCmdShow);
    UpdateWindow(m_hwnd);

    SwitchToPage(AppPage::ROLE_SELECT);
    return true;
}

void MainWindow::CreateLayout() {
    RECT client;
    GetClientRect(m_hwnd, &client);

    int gap = 3;
    int progBarH = 14;
    int progTextH = 36;
    int logH = LOG_HEIGHT;
    int statusH = STATUS_HEIGHT;

    int statusY = client.bottom - statusH;
    int logY = statusY - gap - logH;
    int progTextY = logY - gap - progTextH;
    int progBarY = progTextY - gap - progBarH;
    int pageBottom = progBarY - gap;

    // Page container
    m_pageContainer = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE,
        0, PAGE_TOP, client.right, pageBottom - PAGE_TOP,
        m_hwnd, NULL, m_hInst, NULL);
    SetWindowSubclass(m_pageContainer, MainWindow::PageContainerProc, 0, (DWORD_PTR)m_hwnd);

    // Progress bar
    m_hProgressBar = CreateWindowExW(0, PROGRESS_CLASSW, L"",
        WS_CHILD | PBS_SMOOTH,
        12, progBarY, client.right - 24, progBarH,
        m_hwnd, NULL, m_hInst, NULL);
    SendMessageW(m_hProgressBar, PBM_SETBKCOLOR, 0, RGB(230, 230, 230));
    SendMessageW(m_hProgressBar, PBM_SETBARCOLOR, 0, COL_ACCENT);

    // Progress text
    m_hProgressText = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | SS_LEFT | SS_NOPREFIX,
        12, progTextY, client.right - 24, progTextH,
        m_hwnd, NULL, m_hInst, NULL);
    if (m_hSimSunFont) SendMessageW(m_hProgressText, WM_SETFONT, (WPARAM)m_hSimSunFont, TRUE);

    // Log list
    m_hLogList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_HASSTRINGS,
        12, logY, client.right - 24, logH,
        m_hwnd, NULL, m_hInst, NULL);
    if (m_hSimSunFont) SendMessageW(m_hLogList, WM_SETFONT, (WPARAM)m_hSimSunFont, TRUE);

    // Status bar
    m_hStatusBar = CreateWindowExW(0, L"STATIC", L"\u5c31\u7eea",
        WS_CHILD | WS_VISIBLE,
        0, statusY, client.right, statusH,
        m_hwnd, NULL, m_hInst, NULL);

    // Reconnect button (hidden by default)
    m_hReconnectButton = CreateWindowExW(0, L"BUTTON", L"\u91cd\u65b0\u8fde\u63a5\u5e76\u7eed\u4f20",
        WS_CHILD | BS_PUSHBUTTON | BS_OWNERDRAW,
        client.right / 2 - 80, progBarY - 30, 160, 24,
        m_hwnd, (HMENU)(INT_PTR)IDC_BTN_RECONNECT, m_hInst, NULL);
    if (m_hSimSunFont) SendMessageW(m_hReconnectButton, WM_SETFONT, (WPARAM)m_hSimSunFont, TRUE);
    ShowWindow(m_hReconnectButton, SW_HIDE);
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
    AuditLogger::Instance().Write(msg);
}

void MainWindow::SetStatusText(const std::wstring& text) {
    if (m_hStatusBar)
        SetWindowTextW(m_hStatusBar, text.c_str());
}

void MainWindow::OnTransferProgress(int64_t transferred, int64_t total, int64_t speed, const std::wstring& currentFile) {
    if (m_hProgressBar && total > 0) {
        int64_t range = (total + 1023) / 1024;
        int64_t pos = (transferred + 1023) / 1024;
        if (range < 1) range = 1;
        if (pos > range) pos = range;
        SendMessageW(m_hProgressBar, PBM_SETRANGE32, 0, (LPARAM)range);
        SendMessageW(m_hProgressBar, PBM_SETPOS, (WPARAM)pos, 0);
    }
    if (m_hProgressText) {
        int64_t remainingSeconds = -1;
        if (total > 0 && transferred >= total) {
            remainingSeconds = 0;
        } else if (speed > 0 && total > transferred) {
            remainingSeconds = (total - transferred + speed - 1) / speed;
        }

        std::wstring text = utils::FormatBytes(transferred) + L" / " + utils::FormatBytes(total)
            + L"  |  " + utils::FormatSpeed(speed)
            + L"  |  \u9884\u8ba1\u5269\u4f59: " + utils::FormatDuration(remainingSeconds);
        if (!currentFile.empty())
            text += L"\r\n\u5f53\u524d\u6587\u4ef6: " + currentFile;
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
        case IDC_BTN_CONFIRM_CODE:
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
            if (ctrlId == IDC_PAIRING_DISPLAY) {
                SelectObject(hdc, m_hCodeFont);
                SetTextColor(hdc, COL_ACCENT);
            } else if (hCtrl == m_hStatusBar) {
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
                OnTransferProgress(s->transferredBytes, s->totalBytes,
                    s->speedBytesPerSec, s->currentFile);
                delete s;
            }
            return 0;
        }

        case WM_TRANSFER_DONE: {
            TransferResult* comp = (TransferResult*)wp;

            bool preserveConnectionLostUi = false;

            if (comp) {
                preserveConnectionLostUi =
                    (comp->code == TransferResultCode::ConnectionLost);

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
