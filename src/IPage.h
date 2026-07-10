#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct IPage {
    virtual ~IPage() = default;
    virtual bool HandleCommand(int id, HWND hwndCtl, UINT codeNotify) = 0;
    virtual bool HandleMessage(UINT msg, WPARAM wp, LPARAM lp) { return false; }
};
