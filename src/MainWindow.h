#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <cstdint>
#include <string>

#include "IPage.h"
#include "ConsoleDashboard.h"
#include "ConsoleLogView.h"

struct TransferResult;
struct TransferStats;

enum class AppPage {
    ROLE_SELECT,
    SENDER_SETUP,
    RECEIVER_SETUP
};

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    bool Create(HINSTANCE hInst, int nCmdShow);
    void RunMessageLoop();
    void SwitchToPage(AppPage page);
    void LogMessage(const std::wstring& msg);
    void SetStatusText(const std::wstring& text);

    HWND GetHWND() const { return m_hwnd; }
    HINSTANCE GetInstance() const { return m_hInst; }
    AppPage GetCurrentPage() const { return m_currentPage; }
    void OnTransferProgress(const TransferStats& stats);

    HWND GetLogList() const { return m_logView.GetHWND(); }
    HWND GetProgressBar() const { return m_dashboard.GetHWND(); }
    HWND GetProgressText() const { return m_dashboard.GetHWND(); }

    HFONT GetUIFont() const { return m_hSimSunFont; }
    UINT GetDpi() const { return m_dpi; }
    int GetSenderStep() const { return m_senderStep; }
    int GetReceiverStep() const { return m_receiverStep; }

private:
    HWND m_hwnd = nullptr;
    HINSTANCE m_hInst = nullptr;
    AppPage m_currentPage = AppPage::ROLE_SELECT;
    HWND m_pageContainer = nullptr;
    IPage* m_currentPageObj = nullptr;

    HWND m_hStatusBar = nullptr;
    HWND m_hReconnectButton = nullptr;

    HFONT m_hSimSunFont = nullptr;
    HBRUSH m_hBackgroundBrush = nullptr;
    HBRUSH m_hEditBrush = nullptr;
    HBRUSH m_hPrimaryButtonBrush = nullptr;
    HBRUSH m_hPrimaryPressedBrush = nullptr;
    HBRUSH m_hButtonBrush = nullptr;
    HBRUSH m_hButtonPressedBrush = nullptr;
    HPEN m_hPrimaryButtonPen = nullptr;
    HPEN m_hButtonPen = nullptr;
    ConsoleDashboard m_dashboard;
    ConsoleLogView m_logView;
    int m_senderStep = 0;
    int m_receiverStep = 0;
    UINT m_dpi = 96;

    std::wstring m_lastSourceDirectory;
    std::wstring m_lastPeerIp;
    int m_lastPort = 49321;

    void RegisterWindowClass();
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void CreateLayout();
    void LayoutChildren();
    void UpdateFonts(UINT dpi);
    int Dip(int value) const { return MulDiv(value, static_cast<int>(m_dpi), 96); }
    void DestroyPageContent();
    void DrawButton(DRAWITEMSTRUCT* dis);

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK PageContainerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR uId, DWORD_PTR dwRefData);
};
