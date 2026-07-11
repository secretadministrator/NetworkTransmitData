#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <chrono>

struct ScanProgress {
    uint64_t scannedFiles = 0;
    uint64_t scannedDirectories = 0;
    uint64_t scannedBytes = 0;
    uint64_t inaccessibleDirectories = 0;
    std::wstring currentPath;
};

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
    void SetProgressCallback(std::function<void(const ScanProgress&)> cb) { m_progressCb = cb; }

private:
    std::function<bool(const std::wstring&)> m_excludeCb;
    std::function<void(const ScanProgress&)> m_progressCb;
    ScanProgress m_progress;
    std::chrono::steady_clock::time_point m_lastProgressTime;
    void ScanRecursive(const std::wstring& rootDir, const std::wstring& currentPath,
        std::vector<FileEntry>& results);
    void ReportProgress(bool force = false);
};
