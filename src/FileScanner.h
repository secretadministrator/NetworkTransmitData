#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include <functional>

struct FileEntry {
    std::wstring relativePath;
    int64_t size = 0;
    std::string sha256;
    std::wstring lastWriteTime;
    DWORD attributes = 0;
};

class FileScanner {
public:
    std::vector<FileEntry> ScanDirectory(const std::wstring& rootDir);
    void SetExcludeCallback(std::function<bool(const std::wstring&)> cb) { m_excludeCb = cb; }

private:
    std::function<bool(const std::wstring&)> m_excludeCb;
    void ScanRecursive(const std::wstring& rootDir, const std::wstring& currentPath,
        std::vector<FileEntry>& results);
};
