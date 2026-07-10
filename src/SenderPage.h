#pragma once
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <string>
#include "IPage.h"
#include "NetworkDiscovery.h"
#include "TransferSession.h"

class SenderPage : public IPage {
public:
    SenderPage(HINSTANCE hInst, HWND hParent, const RECT& rc, HWND hMainWnd);
    ~SenderPage();
    bool HandleCommand(int id, HWND hwndCtl, UINT codeNotify) override;
    bool HandleMessage(UINT msg, WPARAM wp, LPARAM lp) override;

private:
    enum { STEP_DIR = 0, STEP_NET, STEP_PAIR, STEP_GO, STEP_MAX };

    HINSTANCE m_hInst;
    HWND m_hParent;
    HWND m_hMainWnd;
    std::wstring m_sourceDir;
    std::wstring m_peerIP;
    std::wstring m_pairingCode;
    int m_currentStep = 0;

    NetworkDiscovery m_discovery;
    TransferSession m_session;

    void CreateControls(const RECT& rc);
    void AdvanceToStep(int step);
    void EnableGroup(int group, bool enable);
    void OnBrowse();
    void OnStartTransfer();
    void OnConfirmCode();
    void OnAutoIP();
    void OnRestoreIP();
    void OnSetIP();
    void OnBack();
    void StartDiscovery();
};
