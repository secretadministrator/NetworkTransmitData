#include "RoleSelectPage.h"
#include "Resource.h"

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
    int cx = rc.right - rc.left;
    int cy = rc.bottom - rc.top;

    int btnW = 170;
    int btnH = 44;
    int gap = 24;
    int totalW = btnW * 2 + gap;
    int startX = (cx - totalW) / 2;
    int startY = cy / 2 - btnH / 2;

    CreateWindowExW(0, L"BUTTON", L"\u2192  \u53d1\u9001\u6587\u4ef6",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        startX, startY, btnW, btnH, m_hParent, (HMENU)IDC_BTN_SEND, NULL, NULL);

    CreateWindowExW(0, L"BUTTON", L"\u2190  \u63a5\u6536\u6587\u4ef6",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        startX + btnW + gap, startY, btnW, btnH, m_hParent, (HMENU)IDC_BTN_RECV, NULL, NULL);
}
