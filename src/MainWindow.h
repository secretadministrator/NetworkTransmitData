#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <cstdint>
#include <string>

#include "IPage.h"

struct TransferResult;

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
    void OnTransferProgress(int64_t transferred, int64_t total, int64_t speed, const std::wstring& currentFile);

    HWND GetLogList() const { return m_hLogList; }
    HWND GetProgressBar() const { return m_hProgressBar; }
    HWND GetProgressText() const { return m_hProgressText; }

    HFONT GetUIFont() const { return m_hSimSunFont; }
    HFONT GetCodeFont() const { return m_hCodeFont; }
    int GetSenderStep() const { return m_senderStep; }
    int GetReceiverStep() const { return m_receiverStep; }

private:
    HWND m_hwnd = nullptr;
    HINSTANCE m_hInst = nullptr;
    AppPage m_currentPage = AppPage::ROLE_SELECT;
    HWND m_pageContainer = nullptr;
    IPage* m_currentPageObj = nullptr;

    HWND m_hLogList = nullptr;
    HWND m_hStatusBar = nullptr;
    HWND m_hProgressBar = nullptr;
    HWND m_hProgressText = nullptr;

    HFONT m_hSimSunFont = nullptr;
    HFONT m_hCodeFont = nullptr;
    HBRUSH m_hDarkBgBrush = nullptr;
    HBRUSH m_hDarkEditBrush = nullptr;
    int m_senderStep = 0;
    int m_receiverStep = 0;

    void RegisterWindowClass();
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void CreateLayout();
    void DestroyPageContent();
    void DrawButton(DRAWITEMSTRUCT* dis);

    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK PageContainerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR uId, DWORD_PTR dwRefData);
};
