#include "DirPicker.h"
#include <shlobj.h>
#include <shlwapi.h>

std::wstring DirPicker::BrowseForFolder(HWND hParent, const std::wstring& title) {
    std::wstring result;
    IFileDialog* pfd = NULL;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&pfd));
    if (FAILED(hr)) return result;

    DWORD options;
    pfd->GetOptions(&options);
    pfd->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    pfd->SetTitle(title.c_str());

    hr = pfd->Show(hParent);
    if (SUCCEEDED(hr)) {
        IShellItem* psi = NULL;
        hr = pfd->GetResult(&psi);
        if (SUCCEEDED(hr)) {
            wchar_t* folderPath = NULL;
            hr = psi->GetDisplayName(SIGDN_FILESYSPATH, &folderPath);
            if (SUCCEEDED(hr)) {
                result = folderPath;
                CoTaskMemFree(folderPath);
            }
            psi->Release();
        }
    }
    pfd->Release();
    return result;
}
