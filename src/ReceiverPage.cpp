#include "ReceiverPage.h"
#include "Resource.h"
#include "DirPicker.h"
#include "MainWindow.h"
#include "AppConfig.h"
#include "IpConfigurator.h"
#include "Utils.h"
#include "AuditLogger.h"
#include <algorithm>

static const int STEP_IDS[3][7] = {
    { IDC_EDIT_DST_DIR, IDC_BTN_BROWSE, 0 },
    { IDC_COMBO_NIC, IDC_BTN_AUTO_IP, IDC_BTN_RESTORE_IP, IDC_EDIT_CUSTOM_IP, IDC_EDIT_CUSTOM_MASK, 0 },
    { IDC_COMBO_MODE, IDC_BTN_START, 0 },
};
static const int DOT_IDS[] = { IDC_STEP_DOT_1, IDC_STEP_DOT_2, IDC_STEP_DOT_3 };

static std::wstring GetSelectedNIC(HWND hParent) {
    HWND hCombo = GetDlgItem(hParent, IDC_COMBO_NIC);
    if (!hCombo) return {};
    int sel = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) return {};
    wchar_t buf[256] = {};
    SendMessageW(hCombo, CB_GETLBTEXT, sel, (LPARAM)buf);
    return buf;
}

ReceiverPage::ReceiverPage(HINSTANCE hInst, HWND hParent, const RECT& rc, HWND hMainWnd)
    : m_hInst(hInst), m_hParent(hParent), m_hMainWnd(hMainWnd) {
    CreateControls(rc);
    AdvanceToStep(STEP_DIR);
}

ReceiverPage::~ReceiverPage() {
    m_discovery.Stop();
    m_session.Stop();
}

bool ReceiverPage::HandleCommand(int id, HWND, UINT) {
    switch (id) {
        case IDC_BTN_BROWSE:     OnBrowse(); return true;
        case IDC_BTN_START:      OnStartListening(); return true;
        case IDC_BTN_AUTO_IP:    OnAutoIP(); return true;
        case IDC_BTN_RESTORE_IP: OnRestoreIP(); return true;
    }
    return false;
}

bool ReceiverPage::HandleMessage(UINT msg, WPARAM, LPARAM) {
    if (msg == WM_DISCOVERY_FOUND) {
        PeerInfo peer = m_discovery.GetLastPeer();
        m_senderIP = peer.ip;
        std::wstring status = L"\u5df2\u63a5\u6536\u5230\u53d1\u9001\u7aef: " + peer.machineName + L" (" + peer.ip + L")";
        SetDlgItemTextW(m_hParent, IDC_DISCOVERY_STATUS, status.c_str());
        MainWindow* mw = (MainWindow*)GetWindowLongPtrW(m_hMainWnd, GWLP_USERDATA);
        if (mw) mw->LogMessage(L"\u53d1\u73b0\u53d1\u9001\u7aef: " + peer.ip);
        return true;
    }
    if (msg == WM_DISCOVERY_FAILED) {
        SetDlgItemTextW(m_hParent, IDC_DISCOVERY_STATUS, L"\u81ea\u52a8\u53d1\u73b0\u5931\u8d25");
        return true;
    }
    return false;
}

void ReceiverPage::AdvanceToStep(int step) {
    if (step < STEP_DIR) step = STEP_DIR;
    if (step > STEP_MAX - 1) step = STEP_MAX - 1;
    m_currentStep = step;
    for (int i = 0; i < STEP_MAX; ++i)
        EnableGroup(i, i <= step);
    MainWindow* mw = (MainWindow*)GetWindowLongPtrW(m_hMainWnd, GWLP_USERDATA);
    if (mw) PostMessageW(m_hMainWnd, WM_STEP_CHANGED, (WPARAM)step, (LPARAM)1);
}

void ReceiverPage::EnableGroup(int group, bool enable) {
    for (int i = 0; STEP_IDS[group][i] != 0; ++i) {
        HWND h = GetDlgItem(m_hParent, STEP_IDS[group][i]);
        if (h) EnableWindow(h, enable);
    }
}

void ReceiverPage::CreateControls(const RECT& rc) {
    MainWindow* mw = (MainWindow*)GetWindowLongPtrW(m_hMainWnd, GWLP_USERDATA);
    HFONT hFont = mw ? mw->GetUIFont() : NULL;
    m_sessionToken = m_pairing.GenerateToken();

    int cx = rc.right - rc.left;
    int x = 14;
    int ctrlW = cx - 2 * x;
    int dotW = 14;
    int dotGap = 2;
    int xLabel = x + dotW + dotGap;
    int labelW = 66;
    int rowH = 22;
    int rowGap = 6;
    int gap2 = 4;
    int y = 8;

    auto MakeBtn = [&](int id, const wchar_t* text, int bx, int by, int bw, bool) {
        HWND h = CreateWindowExW(0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            bx, by, bw, rowH, m_hParent, (HMENU)(INT_PTR)id, m_hInst, NULL);
        if (hFont) SendMessageW(h, WM_SETFONT, (WPARAM)hFont, TRUE);
        return h;
    };

    auto MakeEdit = [&](int id, int ex, int ey, int ew, bool ro) {
        DWORD style = WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL;
        if (ro) style |= ES_READONLY;
        HWND h = CreateWindowExW(0, L"EDIT", L"",
            style, ex, ey, ew, rowH, m_hParent, (HMENU)(INT_PTR)id, m_hInst, NULL);
        if (hFont) SendMessageW(h, WM_SETFONT, (WPARAM)hFont, TRUE);
        return h;
    };

    int nextLabelId = 620;
    auto MakeLabel = [&](const wchar_t* text, int lx, int ly, int lw) {
        HWND h = CreateWindowExW(0, L"STATIC", text,
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
            lx, ly + 2, lw, rowH - 2, m_hParent,
            (HMENU)(INT_PTR)nextLabelId++, m_hInst, NULL);
        if (hFont) SendMessageW(h, WM_SETFONT, (WPARAM)hFont, TRUE);
        return h;
    };

    auto MakeDot = [&](int stepIdx) {
        HWND h = CreateWindowExW(0, L"STATIC", L"\u25cf",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            x, y + 4, dotW, 14,
            m_hParent, (HMENU)(INT_PTR)DOT_IDS[stepIdx], m_hInst, NULL);
        if (hFont) SendMessageW(h, WM_SETFONT, (WPARAM)hFont, TRUE);
    };

    // Row 0: Back
    MakeBtn(IDC_BTN_BACK, L"[ BACK ]", x, y, 55, false);
    y += rowH + rowGap + 2;

    // ── Step 1: Target directory ──
    MakeDot(0);
    MakeLabel(L"\u76ee\u6807\u76ee\u5f55:", xLabel, y, labelW);
    int browseW = 44;
    int editW = ctrlW - (xLabel - x) - labelW - browseW - gap2;
    MakeEdit(IDC_EDIT_DST_DIR, xLabel + labelW, y, editW, true);
    MakeBtn(IDC_BTN_BROWSE, L"[DIR]", xLabel + labelW + editW + gap2, y, browseW, false);
    y += rowH + rowGap;

    // Status (always visible)
    MakeLabel(L"\u72b6\u6001:", xLabel, y, 32);
    CreateWindowExW(0, L"STATIC", L"\u8bf7\u5148\u9009\u62e9\u76ee\u6807\u76ee\u5f55",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS | SS_NOPREFIX,
        xLabel + 34, y + 2, ctrlW - (xLabel - x) - 34, rowH - 2,
        m_hParent, (HMENU)IDC_DISCOVERY_STATUS, m_hInst, NULL);
    if (hFont) SendMessageW(GetDlgItem(m_hParent, IDC_DISCOVERY_STATUS), WM_SETFONT, (WPARAM)hFont, TRUE);
    y += rowH + rowGap;

    // ── Step 2: NIC configuration ──
    MakeDot(1);
    MakeLabel(L"\u7f51\u5361:", xLabel, y, labelW);
    int restorW = 44;
    int cfgIpW = 52;
    int comboW = ctrlW - (xLabel - x) - labelW - cfgIpW - restorW - gap2 * 2;
    HWND hNicCombo = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
        xLabel + labelW, y, comboW, 200, m_hParent, (HMENU)IDC_COMBO_NIC, m_hInst, NULL);
    if (hFont) SendMessageW(hNicCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
    std::vector<std::wstring> nics;
    if (IpConfigurator::EnumerateNICs(nics)) {
        for (const auto& n : nics)
            SendMessageW(hNicCombo, CB_ADDSTRING, 0, (LPARAM)n.c_str());
        SendMessageW(hNicCombo, CB_SETCURSEL, 0, 0);
    }
    MakeBtn(IDC_BTN_AUTO_IP, L"[ IP ]", xLabel + labelW + comboW + gap2, y, cfgIpW, false);
    MakeBtn(IDC_BTN_RESTORE_IP, L"[RST]", xLabel + labelW + comboW + cfgIpW + gap2 * 2, y, restorW, false);
    y += rowH + rowGap;

    // IP customization row (part of Step 2)
    MakeLabel(L"IP\u5730\u5740:", xLabel, y, labelW);
    MakeEdit(IDC_EDIT_CUSTOM_IP, xLabel + labelW, y, 130, false);
    SetDlgItemTextW(m_hParent, IDC_EDIT_CUSTOM_IP, L"192.168.88.2");
    MakeLabel(L"\u63a9\u7801:", xLabel + labelW + 130 + gap2, y, 28);
    MakeEdit(IDC_EDIT_CUSTOM_MASK, xLabel + labelW + 130 + gap2 + 28 + gap2, y, 80, false);
    SetDlgItemTextW(m_hParent, IDC_EDIT_CUSTOM_MASK, L"255.255.255.0");
    y += rowH + rowGap;

    // ── Step 3: Start ──
    MakeDot(2);
    MakeLabel(L"\u6a21\u5f0f:", xLabel, y, 48);
    HWND hCombo = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
        xLabel + 50, y, ctrlW - (xLabel - x) - 50, 200, m_hParent, (HMENU)IDC_COMBO_MODE, m_hInst, NULL);
    if (hFont) SendMessageW(hCombo, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"\u5b89\u5168\u590d\u5236");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"\u8986\u76d6\u590d\u5236");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"\u540c\u6b65\u955c\u50cf");
    SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
    y += rowH + rowGap + 4;

    MakeBtn(IDC_BTN_START, L"[ START RX ]", cx / 2 - 65, y, 130, true);

    AuditLogger::Instance().Write(
        L"\u63a5\u6536\u9875\u521b\u5efa\u5b8c\u6210\uff0c\u5df2\u521b\u5efa\u81ea\u52a8\u8bc6\u522b\u4f1a\u8bdd");
    Relayout(rc, mw ? mw->GetDpi() : 96);
}

void ReceiverPage::Relayout(const RECT& rc, UINT dpi) {
    auto dip = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    auto move = [&](int id, int x, int y, int w, int h) {
        HWND control = GetDlgItem(m_hParent, id);
        if (control) MoveWindow(control, x, y, (std::max)(0, w), h, TRUE);
    };
    const int width = rc.right - rc.left;
    const int margin = dip(12), dotW = dip(14), labelW = dip(78);
    const int rowH = dip(24), gap = dip(4);
    const int xLabel = margin + dotW + dip(6);
    const int contentRight = width - margin;
    int y = dip(12);

    move(IDC_BTN_BACK, margin, y, dip(68), rowH);
    y += rowH + gap;
    move(IDC_STEP_DOT_1, margin, y + dip(4), dotW, dip(14));
    move(620, xLabel, y, labelW, rowH);
    int browseW = dip(68);
    move(IDC_EDIT_DST_DIR, xLabel + labelW, y,
        contentRight - (xLabel + labelW) - browseW - gap, rowH);
    move(IDC_BTN_BROWSE, contentRight - browseW, y, browseW, rowH);
    y += rowH + gap;

    move(621, xLabel, y, dip(52), rowH);
    move(IDC_DISCOVERY_STATUS, xLabel + dip(56), y,
        contentRight - xLabel - dip(56), rowH);
    y += rowH + gap;

    move(IDC_STEP_DOT_2, margin, y + dip(4), dotW, dip(14));
    move(622, xLabel, y, labelW, rowH);
    int restoreW = dip(68), configW = dip(76);
    move(IDC_COMBO_NIC, xLabel + labelW, y,
        contentRight - (xLabel + labelW) - restoreW - configW - gap * 2, rowH);
    move(IDC_BTN_AUTO_IP, contentRight - restoreW - configW - gap, y, configW, rowH);
    move(IDC_BTN_RESTORE_IP, contentRight - restoreW, y, restoreW, rowH);
    y += rowH + gap;

    move(623, xLabel, y, labelW, rowH);
    move(IDC_EDIT_CUSTOM_IP, xLabel + labelW, y, dip(160), rowH);
    move(624, xLabel + labelW + dip(168), y, dip(52), rowH);
    move(IDC_EDIT_CUSTOM_MASK, xLabel + labelW + dip(224), y,
        (std::max)(dip(120), contentRight - (xLabel + labelW + dip(224))), rowH);
    y += rowH + gap;

    move(IDC_STEP_DOT_3, margin, y + dip(4), dotW, dip(14));
    move(625, xLabel, y, labelW, rowH);
    move(IDC_COMBO_MODE, xLabel + labelW, y,
        contentRight - (xLabel + labelW), dip(180));
    y += rowH + dip(6);
    move(IDC_BTN_START, width / 2 - dip(78), y, dip(156), dip(30));
}

void ReceiverPage::StartListener() {
    int port = AppConfig::Instance().port;
    m_discovery.StartReceiverListener(m_hMainWnd, port, m_sessionToken);
}

void ReceiverPage::OnBrowse() {
    std::wstring dir = DirPicker::BrowseForFolder(m_hMainWnd, L"\u9009\u62e9\u76ee\u6807\u76ee\u5f55");
    if (!dir.empty()) {
        m_targetDir = dir;
        SetDlgItemTextW(m_hParent, IDC_EDIT_DST_DIR, dir.c_str());
        MainWindow* mw = (MainWindow*)GetWindowLongPtrW(m_hMainWnd, GWLP_USERDATA);
        if (mw) mw->LogMessage(L"\u9009\u62e9\u7684\u76ee\u6807\u76ee\u5f55: " + dir);
        SetDlgItemTextW(m_hParent, IDC_DISCOVERY_STATUS,
            L"\u76ee\u6807\u76ee\u5f55\u5df2\u9009\u62e9\uff0c\u53ef\u914d\u7f6e IP \u6216\u76f4\u63a5\u5f00\u59cb\u63a5\u6536");
        AdvanceToStep(STEP_GO);
    }
}

void ReceiverPage::OnStartListening() {
    if (m_targetDir.empty()) {
        MessageBoxW(m_hMainWnd, L"\u8bf7\u5148\u9009\u62e9\u76ee\u6807\u76ee\u5f55", L"\u63d0\u793a", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (m_session.IsRunning()) {
        SetDlgItemTextW(m_hParent, IDC_DISCOVERY_STATUS, L"\u63a5\u6536\u5df2\u5728\u8fd0\u884c\uff0c\u8bf7\u7b49\u5f85\u5f53\u524d\u4f1a\u8bdd\u7ed3\u675f");
        MessageBoxW(m_hMainWnd, L"\u63a5\u6536\u5df2\u5728\u8fd0\u884c\uff0c\u4e0d\u9700\u8981\u91cd\u590d\u70b9\u51fb",
            L"\u63d0\u793a", MB_OK | MB_ICONINFORMATION);
        return;
    }

    MainWindow* mw = (MainWindow*)GetWindowLongPtrW(m_hMainWnd, GWLP_USERDATA);
    if (mw) mw->LogMessage(L"\u6b63\u5728\u542f\u52a8\u63a5\u6536...");

    int port = AppConfig::Instance().port;
    HWND hCombo = GetDlgItem(m_hParent, IDC_COMBO_MODE);
    int sel = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
    TransferMode mode = TransferMode::SAFE_COPY;
    if (sel == 1) mode = TransferMode::OVERWRITE;
    else if (sel == 2) mode = TransferMode::MIRROR;

    m_session.SetLogCallback([mw](const std::wstring& msg) {
        if (mw) PostMessageW(mw->GetHWND(), WM_TRANSFER_LOG, 0, (LPARAM)new std::wstring(msg));
    });
    m_session.SetProgressCallback([mw](const TransferStats& s) {
        if (mw) {
            TransferStats* copy = new TransferStats(s);
            PostMessageW(mw->GetHWND(), WM_TRANSFER_PROGRESS, (WPARAM)copy, 0);
        }
    });
    m_session.SetDoneCallback([mw](const TransferResult& comp) {
        if (mw) {
            TransferResult* copy = new TransferResult(comp);
            PostMessageW(mw->GetHWND(), WM_TRANSFER_DONE, (WPARAM)copy, 0);
        }
    });
    if (!m_session.StartAsReceiver(m_targetDir, port, m_sessionToken, mode)) {
        SetDlgItemTextW(m_hParent, IDC_DISCOVERY_STATUS, L"\u542f\u52a8\u63a5\u6536\u5931\u8d25");
        MessageBoxW(m_hMainWnd, L"\u542f\u52a8\u63a5\u6536\u5931\u8d25\uff0c\u8bf7\u7a0d\u540e\u91cd\u8bd5",
            L"\u9519\u8bef", MB_OK | MB_ICONERROR);
        return;
    }
    StartListener();
    SetDlgItemTextW(m_hParent, IDC_DISCOVERY_STATUS, L"\u6b63\u5728\u7b49\u5f85\u53d1\u9001\u7aef\u8fde\u63a5...");
}

void ReceiverPage::OnAutoIP() {
    std::wstring nicName = GetSelectedNIC(m_hParent);
    if (nicName.empty() && !IpConfigurator::DetectNIC(nicName)) {
        MessageBoxW(m_hMainWnd, L"\u672a\u627e\u5230\u53ef\u7528\u7684\u7f51\u5361", L"\u63d0\u793a", MB_OK | MB_ICONINFORMATION);
        return;
    }
    int adminState = IpConfigurator::GetAdminState();
    if (adminState == 0) {
        MessageBoxW(m_hMainWnd, L"\u4e0d\u662f\u7ba1\u7406\u5458\uff0c\u65e0\u6743\u4f7f\u7528\u6b64\u529f\u80fd", L"\u63d0\u793a", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (adminState == 1) {
        MessageBoxW(m_hMainWnd, L"\u8bf7\u53f3\u952e\u4ee5\u7ba1\u7406\u5458\u8eab\u4efd\u8fd0\u884c", L"\u63d0\u793a", MB_OK | MB_ICONINFORMATION);
        return;
    }
    wchar_t ipBuf[64] = {}, maskBuf[64] = {};
    GetDlgItemTextW(m_hParent, IDC_EDIT_CUSTOM_IP, ipBuf, 64);
    GetDlgItemTextW(m_hParent, IDC_EDIT_CUSTOM_MASK, maskBuf, 64);
    std::wstring customIP = ipBuf;
    std::wstring customMask = maskBuf;
    if (customIP.empty()) customIP = L"192.168.88.2";
    if (customMask.empty()) customMask = L"255.255.255.0";
    bool ok = IpConfigurator::SetStaticIP(nicName, customIP, customMask);
    if (ok) {
        AppConfig::Instance().configuredNicName = nicName;
        std::wstring msg = L"\u5df2\u8bbe\u7f6e " + nicName + L" = " + customIP;
        MainWindow* mw = (MainWindow*)GetWindowLongPtrW(m_hMainWnd, GWLP_USERDATA);
        if (mw) mw->LogMessage(msg);
        SetDlgItemTextW(m_hParent, IDC_DISCOVERY_STATUS, msg.c_str());
        SetDlgItemTextW(m_hParent, IDC_DISCOVERY_STATUS, L"\u7f51\u5361\u5df2\u914d\u7f6e\uff0c\u53ef\u4ee5\u5f00\u59cb\u63a5\u6536");
        AdvanceToStep(STEP_GO);
    } else {
        MessageBoxW(m_hMainWnd, L"IP \u8bbe\u7f6e\u5931\u8d25\u3002\u8bf7\u786e\u4fdd\u4ee5\u7ba1\u7406\u5458\u8fd0\u884c\uff0c\u5e76\u9009\u62e9\u6b63\u786e\u7684\u7f51\u5361", L"\u9519\u8bef", MB_OK | MB_ICONERROR);
    }
}

void ReceiverPage::OnRestoreIP() {
    std::wstring nicName = GetSelectedNIC(m_hParent);
    if (nicName.empty()) nicName = AppConfig::Instance().configuredNicName;
    if (nicName.empty()) {
        MessageBoxW(m_hMainWnd, L"\u672a\u9009\u62e9\u7f51\u5361", L"\u63d0\u793a", MB_OK | MB_ICONINFORMATION);
        return;
    }
    int adminState = IpConfigurator::GetAdminState();
    if (adminState == 0) {
        MessageBoxW(m_hMainWnd, L"\u4e0d\u662f\u7ba1\u7406\u5458\uff0c\u65e0\u6743\u4f7f\u7528\u6b64\u529f\u80fd", L"\u63d0\u793a", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (adminState == 1) {
        MessageBoxW(m_hMainWnd, L"\u8bf7\u53f3\u952e\u4ee5\u7ba1\u7406\u5458\u8eab\u4efd\u8fd0\u884c", L"\u63d0\u793a", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (IpConfigurator::EnableDHCP(nicName)) {
        AppConfig::Instance().configuredNicName.clear();
        std::wstring msg = L"\u5df2\u6062\u590d " + nicName + L" DHCP";
        MainWindow* mw = (MainWindow*)GetWindowLongPtrW(m_hMainWnd, GWLP_USERDATA);
        if (mw) mw->LogMessage(msg);
        SetDlgItemTextW(m_hParent, IDC_DISCOVERY_STATUS, msg.c_str());
    } else {
        MessageBoxW(m_hMainWnd, L"\u6062\u590d\u5931\u8d25", L"\u9519\u8bef", MB_OK | MB_ICONERROR);
    }
}

void ReceiverPage::OnBack() {
    MainWindow* mw = (MainWindow*)GetWindowLongPtrW(m_hMainWnd, GWLP_USERDATA);
    if (mw) mw->SwitchToPage(AppPage::ROLE_SELECT);
}
