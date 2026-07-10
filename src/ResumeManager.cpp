#include "ResumeManager.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int64_t ResumeManager::GetResumeOffset(const std::wstring& partFilePath) {
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    if (!GetFileAttributesExW(partFilePath.c_str(), GetFileExInfoStandard, &info))
        return 0;
    ULARGE_INTEGER uli;
    uli.LowPart = info.nFileSizeLow;
    uli.HighPart = info.nFileSizeHigh;
    return (int64_t)uli.QuadPart;
}

bool ResumeManager::CleanPartFile(const std::wstring& partFilePath) {
    return DeleteFileW(partFilePath.c_str()) != FALSE;
}

std::wstring ResumeManager::GetPartPath(const std::wstring& finalPath) {
    return finalPath + L".dtpart";
}
