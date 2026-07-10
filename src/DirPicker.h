#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

class DirPicker {
public:
    static std::wstring BrowseForFolder(HWND hParent, const std::wstring& title);
};
