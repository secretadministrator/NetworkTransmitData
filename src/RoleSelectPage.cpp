#include "RoleSelectPage.h"
#include "Resource.h"
#include "MainWindow.h"

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

    int btnW = 170;
    int btnH = 44;
    int gap = 24;
    int totalW = btnW * 2 + gap;
    int startX = (cx - totalW) / 2;
    int startY = cy / 2 - btnH / 2;

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
    int cx = rc.right - rc.left;
    int cy = rc.bottom - rc.top;
    int btnW = dip(190);
    int btnH = dip(48);
    int gap = dip(24);
    int totalW = btnW * 2 + gap;
    int startX = (cx - totalW) / 2;
    int startY = (cy - btnH) / 2;
    MoveWindow(GetDlgItem(m_hParent, IDC_BTN_SEND), startX, startY,
        btnW, btnH, TRUE);
    MoveWindow(GetDlgItem(m_hParent, IDC_BTN_RECV), startX + btnW + gap,
        startY, btnW, btnH, TRUE);
}
