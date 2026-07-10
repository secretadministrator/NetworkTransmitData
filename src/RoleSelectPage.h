#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "IPage.h"

class RoleSelectPage : public IPage {
public:
    RoleSelectPage(HWND hParent, const RECT& rc, HWND hMainWnd);
    ~RoleSelectPage();
    bool HandleCommand(int id, HWND hwndCtl, UINT codeNotify) override;

private:
    HWND m_hParent;
    HWND m_hMainWnd;
    void CreateControls(const RECT& rc);
};
