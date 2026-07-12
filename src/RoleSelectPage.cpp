#include "RoleSelectPage.h"
#include "Resource.h"
#include "MainWindow.h"
#include <algorithm>

RoleSelectPage::RoleSelectPage(HWND hParent, const RECT& rc, HWND hMainWnd)
    : m_hParent(hParent), m_hMainWnd(hMainWnd) {
    CreateControls(rc);
}

RoleSelectPage::~RoleSelectPage() {
}

bool RoleSelectPage::HandleCommand(int id, HWND, UINT) {
    return false;
}

void RoleSelectPage::CreateControls(const RECT& rc) {
    MainWindow* mw = reinterpret_cast<MainWindow*>(
        GetWindowLongPtrW(m_hMainWnd, GWLP_USERDATA));
    HFONT font = mw ? mw->GetUIFont() : nullptr;
    int cx = rc.right - rc.left;
    int cy = rc.bottom - rc.top;

    auto CreateInfoLabel = [&](int id, const wchar_t* text, int y, int h) {
        HWND label = CreateWindowExW(0, L"STATIC", text,
            WS_CHILD | SS_CENTER | SS_CENTERIMAGE | SS_NOPREFIX,
            20, y, cx - 40, h, m_hParent, (HMENU)(INT_PTR)id, NULL, NULL);
        if (font) SendMessageW(label, WM_SETFONT, (WPARAM)font, TRUE);
    };
    CreateInfoLabel(610, L"DirectTransfer \u76f4\u8fde\u6587\u4ef6\u4f20\u8f93",
        16, 26);
    CreateInfoLabel(611,
        L"\u65e0\u9700\u5171\u4eab\u6587\u4ef6\u5939\uff1a\u5728\u5c40\u57df\u7f51\u6216\u76f4\u8fde\u7f51\u7edc\u4e2d\u5b89\u5168\u4f20\u9001\u76ee\u5f55\u3002",
        50, 24);
    CreateInfoLabel(612,
        L"\u4f7f\u7528\u65b9\u6cd5\uff1a1. \u5148\u5728\u63a5\u6536\u7aef\u9009\u62e9\u76ee\u6807\u76ee\u5f55\u5e76\u5f00\u59cb\u63a5\u6536\uff1b",
        82, 24);
    CreateInfoLabel(613,
        L"2. \u518d\u5728\u53d1\u9001\u7aef\u9009\u62e9\u6e90\u76ee\u5f55\uff0c\u53d1\u73b0\u63a5\u6536\u7aef\u540e\u5f00\u59cb\u53d1\u9001\u3002",
        110, 24);

    int btnW = 170;
    int btnH = 44;
    int gap = 24;
    int totalW = btnW * 2 + gap;
    int startX = (cx - totalW) / 2;
    int startY = 150;

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
    const int infoLabelIds[] = {610, 611, 612, 613};
    for (int id : infoLabelIds) {
        HWND label = GetDlgItem(m_hParent, id);
        if (label) ShowWindow(label, SW_HIDE);
    }

    int cx = rc.right - rc.left;
    int cy = rc.bottom - rc.top;
    int btnW = dip(190);
    int btnH = dip(48);
    int gap = dip(24);
    int totalW = btnW * 2 + gap;
    int startX = (cx - totalW) / 2;
    const int titleY = dip(16);
    MoveWindow(GetDlgItem(m_hParent, 610), dip(20), titleY,
        (std::max)(0, cx - dip(40)), dip(26), TRUE);
    MoveWindow(GetDlgItem(m_hParent, 611), dip(20), dip(50),
        (std::max)(0, cx - dip(40)), dip(24), TRUE);
    MoveWindow(GetDlgItem(m_hParent, 612), dip(20), dip(82),
        (std::max)(0, cx - dip(40)), dip(24), TRUE);
    MoveWindow(GetDlgItem(m_hParent, 613), dip(20), dip(110),
        (std::max)(0, cx - dip(40)), dip(24), TRUE);
    int startY = dip(150);
    MoveWindow(GetDlgItem(m_hParent, IDC_BTN_SEND), startX, startY,
        btnW, btnH, TRUE);
    MoveWindow(GetDlgItem(m_hParent, IDC_BTN_RECV), startX + btnW + gap,
        startY, btnW, btnH, TRUE);

    RedrawWindow(m_hParent, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    for (int id : infoLabelIds) {
        HWND label = GetDlgItem(m_hParent, id);
        if (label) ShowWindow(label, SW_SHOW);
    }
}
