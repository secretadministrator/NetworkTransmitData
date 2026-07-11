#include "FileScanner.h"
#include "Utils.h"

static std::wstring FileTimeToStableString(const FILETIME& ft) {
    uint64_t value = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) |
        static_cast<uint64_t>(ft.dwLowDateTime);
    wchar_t buf[32] = {};
    swprintf(buf, 32, L"%016llx", value);
    return buf;
}

std::vector<FileEntry> FileScanner::ScanDirectory(const std::wstring& rootDir) {
    std::vector<FileEntry> results;
    m_progress = ScanProgress{};
    m_progress.currentPath = rootDir;
    m_lastProgressTime = std::chrono::steady_clock::now();

    const DWORD attributes = GetFileAttributesW(utils::NormalizePath(rootDir).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        m_progress.inaccessibleDirectories++;
        ReportProgress(true);
        return results;
    }

    ScanRecursive(rootDir, rootDir, results);
    ReportProgress(true);
    return results;
}

void FileScanner::ReportProgress(bool force) {
    if (!m_progressCb)
        return;

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_lastProgressTime).count();
    if (!force && elapsed < 200)
        return;

    m_lastProgressTime = now;
    m_progressCb(m_progress);
}

void FileScanner::ScanRecursive(const std::wstring& rootDir, const std::wstring& currentPath,
    std::vector<FileEntry>& results)
{
    if (m_excludeCb && m_excludeCb(currentPath))
        return;

    m_progress.scannedDirectories++;
    m_progress.currentPath = currentPath;
    ReportProgress();

    const std::wstring pattern = utils::NormalizePath(currentPath + L"\\*");
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
        FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE && GetLastError() == ERROR_INVALID_PARAMETER) {
        find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
            FindExSearchNameMatch, nullptr, 0);
    }
    if (find == INVALID_HANDLE_VALUE) {
        m_progress.inaccessibleDirectories++;
        ReportProgress(true);
        return;
    }

    do {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L"..")
            continue;
        const std::wstring fullPath = currentPath + L"\\" + name;
        if (m_excludeCb && m_excludeCb(fullPath))
            continue;
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
                ScanRecursive(rootDir, fullPath, results);
            continue;
        }
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            continue;

        ULARGE_INTEGER size{};
        size.LowPart = data.nFileSizeLow;
        size.HighPart = data.nFileSizeHigh;
        if (size.QuadPart > static_cast<uint64_t>(INT64_MAX))
            continue;

        FileEntry entry;
        entry.relativePath = fullPath.substr(rootDir.length());
        while (!entry.relativePath.empty() &&
            (entry.relativePath.front() == L'\\' || entry.relativePath.front() == L'/')) {
            entry.relativePath.erase(entry.relativePath.begin());
        }
        entry.size = static_cast<int64_t>(size.QuadPart);
        entry.attributes = data.dwFileAttributes;
        entry.lastWriteTime = FileTimeToStableString(data.ftLastWriteTime);
        results.push_back(std::move(entry));
        ++m_progress.scannedFiles;
        m_progress.scannedBytes += size.QuadPart;
        ReportProgress();
    } while (FindNextFileW(find, &data));

    const DWORD error = GetLastError();
    FindClose(find);
    if (error != ERROR_NO_MORE_FILES) {
        ++m_progress.inaccessibleDirectories;
        ReportProgress(true);
    }
}
