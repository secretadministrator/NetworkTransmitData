#include "RoleSelectPage.h"
#include "Resource.h"
#include "MainWindow.h"
#include <algorithm>

static constexpr int ICON_LEFT = 20;
static constexpr int ICON_SIZE = 32;
static constexpr int TEXT_LEFT = 65;
static constexpr int LABEL_GAP = 4;

RoleSelectPage::RoleSelectPage(HWND hParent, const RECT& rc, HWND hMainWnd)
    : m_hParent(hParent), m_hMainWnd(hMainWnd) {
    CreateControls(rc);
}

RoleSelectPage::~RoleSelectPage() {
    if (m_hDOSFont) DeleteObject(m_hDOSFont);
}

bool RoleSelectPage::HandleCommand(int id, HWND, UINT) {
    return false;
}

HFONT RoleSelectPage::CreateDOSFont(UINT dpi) {
    int height = -MulDiv(9, static_cast<int>(dpi), 72);
    return CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        OEM_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, DEFAULT_PITCH | FF_MODERN, L"Terminal");
}

void RoleSelectPage::CreateControls(const RECT& rc) {
    MainWindow* mw = reinterpret_cast<MainWindow*>(
        GetWindowLongPtrW(m_hMainWnd, GWLP_USERDATA));
    HFONT font = mw ? mw->GetUIFont() : nullptr;
    int cx = rc.right - rc.left;

    // Icon
    m_hIconControl = CreateWindowExW(0, L"STATIC", nullptr,
        WS_CHILD | WS_VISIBLE | SS_ICON,
        ICON_LEFT, 16, ICON_SIZE, ICON_SIZE, m_hParent,
        (HMENU)(INT_PTR)700, NULL, NULL);
    HICON hIcon = mw ? LoadIconW(mw->GetInstance(), MAKEINTRESOURCEW(IDI_MAIN_APP)) : nullptr;
    if (hIcon) SendMessageW(m_hIconControl, STM_SETICON, (WPARAM)hIcon, 0);

    // DOS font for English text
    UINT initDpi = mw ? mw->GetDpi() : 96;
    m_hDOSFont = CreateDOSFont(initDpi);

    auto CreateInfoLabel = [&](int id, const wchar_t* text, int y, int h, bool dosFont) {
        HWND label = CreateWindowExW(0, L"STATIC", text,
            WS_CHILD | SS_CENTER | SS_CENTERIMAGE | SS_NOPREFIX,
            TEXT_LEFT, y, (std::max)(0, cx - TEXT_LEFT - 20), h,
            m_hParent, (HMENU)(INT_PTR)id, NULL, NULL);
        SendMessageW(label, WM_SETFONT,
            (WPARAM)(dosFont ? m_hDOSFont : font), TRUE);
    };

    // Chinese title (English line below)
    CreateInfoLabel(610, L"DirectTransfer", 16, 26, false);
    CreateInfoLabel(710, L"Direct Peer-to-Peer Folder Transfer", 42, 18, true);

    // Chinese line 1
    CreateInfoLabel(611,
        L"\u65e0\u9700\u5171\u4eab\u6587\u4ef6\u5939\uff1a\u5728\u5c40\u57df\u7f51\u6216\u76f4\u8fde\u7f51\u7edc\u4e2d\u5b89\u5168\u4f20\u9001\u76ee\u5f55\u3002",
        68, 24, false);
    CreateInfoLabel(711,
        L"No shared folder required - transfer over LAN or direct connection",
        92, 18, true);

    // Chinese line 2
    CreateInfoLabel(612,
        L"\u4f7f\u7528\u65b9\u6cd5\uff1a1. \u5148\u5728\u63a5\u6536\u7aef\u9009\u62e9\u76ee\u6807\u76ee\u5f55\u5e76\u5f00\u59cb\u63a5\u6536\uff1b",
        118, 24, false);
    CreateInfoLabel(712,
        L"How to use: 1. Receiver selects target folder and starts listening",
        142, 18, true);

    // Chinese line 3
    CreateInfoLabel(613,
        L"2. \u518d\u5728\u53d1\u9001\u7aef\u9009\u62e9\u6e90\u76ee\u5f55\uff0c\u53d1\u73b0\u63a5\u6536\u7aef\u540e\u5f00\u59cb\u53d1\u9001\u3002",
        168, 24, false);
    CreateInfoLabel(713,
        L"2. Sender selects source, discovers receiver, and starts transfer",
        192, 18, true);

    int btnW = 170;
    int btnH = 44;
    int gap = 24;
    int totalW = btnW * 2 + gap;
    int startX = (cx - totalW) / 2;
    int startY = 222;

    HWND sendButton = CreateWindowExW(0, L"BUTTON", L"[ TX ]  \u53d1\u9001\u6587\u4ef6",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        startX, startY, btnW, btnH, m_hParent, (HMENU)IDC_BTN_SEND, NULL, NULL);
    if (font) SendMessageW(sendButton, WM_SETFONT, (WPARAM)font, TRUE);

    HWND receiveButton = CreateWindowExW(0, L"BUTTON", L"[ RX ]  \u63a5\u6536\u6587\u4ef6",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        startX + btnW + gap, startY, btnW, btnH, m_hParent, (HMENU)IDC_BTN_RECV, NULL, NULL);
    if (font) SendMessageW(receiveButton, WM_SETFONT, (WPARAM)font, TRUE);
}

void RoleSelectPage::Relayout(const RECT& rc, UINT dpi) {
    auto dip = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    const int infoLabelIds[] = {610, 611, 612, 613, 710, 711, 712, 713};
    for (int id : infoLabelIds) {
        HWND label = GetDlgItem(m_hParent, id);
        if (label) ShowWindow(label, SW_HIDE);
    }

    // Recreate DOS font with new DPI
    if (m_hDOSFont) DeleteObject(m_hDOSFont);
    m_hDOSFont = CreateDOSFont(dpi);

    int cx = rc.right - rc.left;
    int cy = rc.bottom - rc.top;
    int textWidth = (std::max)(0, cx - dip(TEXT_LEFT) - dip(20));

    // Icon
    MoveWindow(m_hIconControl, dip(ICON_LEFT), dip(16),
        dip(ICON_SIZE), dip(ICON_SIZE), TRUE);

    // Title
    MoveWindow(GetDlgItem(m_hParent, 610), dip(TEXT_LEFT), dip(16),
        textWidth, dip(26), TRUE);
    MoveWindow(GetDlgItem(m_hParent, 710), dip(TEXT_LEFT), dip(42),
        textWidth, dip(18), TRUE);

    // Line 1
    MoveWindow(GetDlgItem(m_hParent, 611), dip(TEXT_LEFT), dip(68),
        textWidth, dip(24), TRUE);
    MoveWindow(GetDlgItem(m_hParent, 711), dip(TEXT_LEFT), dip(92),
        textWidth, dip(18), TRUE);

    // Line 2
    MoveWindow(GetDlgItem(m_hParent, 612), dip(TEXT_LEFT), dip(118),
        textWidth, dip(24), TRUE);
    MoveWindow(GetDlgItem(m_hParent, 712), dip(TEXT_LEFT), dip(142),
        textWidth, dip(18), TRUE);

    // Line 3
    MoveWindow(GetDlgItem(m_hParent, 613), dip(TEXT_LEFT), dip(168),
        textWidth, dip(24), TRUE);
    MoveWindow(GetDlgItem(m_hParent, 713), dip(TEXT_LEFT), dip(192),
        textWidth, dip(18), TRUE);

    // Buttons
    MainWindow* mw = reinterpret_cast<MainWindow*>(
        GetWindowLongPtrW(m_hMainWnd, GWLP_USERDATA));
    HFONT font = mw ? mw->GetUIFont() : nullptr;
    int btnW = dip(190);
    int btnH = dip(48);
    int gap = dip(24);
    int totalW = btnW * 2 + gap;
    int startX = (cx - totalW) / 2;
    int startY = dip(222);
    MoveWindow(GetDlgItem(m_hParent, IDC_BTN_SEND), startX, startY,
        btnW, btnH, TRUE);
    MoveWindow(GetDlgItem(m_hParent, IDC_BTN_RECV), startX + btnW + gap,
        startY, btnW, btnH, TRUE);

    // Ensure English labels have DOS font (UpdateFonts may have overridden it)
    const int enLabelIds[] = {710, 711, 712, 713};
    for (int id : enLabelIds) {
        HWND label = GetDlgItem(m_hParent, id);
        if (label) SendMessageW(label, WM_SETFONT, (WPARAM)m_hDOSFont, TRUE);
    }

    RedrawWindow(m_hParent, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    for (int id : infoLabelIds) {
        HWND label = GetDlgItem(m_hParent, id);
        if (label) ShowWindow(label, SW_SHOW);
    }
}
