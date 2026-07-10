#include "FileScanner.h"
#include <filesystem>
#include <ctime>

namespace fs = std::filesystem;

std::vector<FileEntry> FileScanner::ScanDirectory(const std::wstring& rootDir) {
    std::vector<FileEntry> results;
    if (!fs::exists(rootDir)) return results;
    ScanRecursive(rootDir, rootDir, results);
    return results;
}

static std::wstring FileTimeToString(const fs::file_time_type& ftime) {
    using namespace std::chrono;
    auto sysTime = time_point_cast<system_clock::duration>(
        ftime - fs::file_time_type::clock::now() + system_clock::now());
    time_t tt = system_clock::to_time_t(sysTime);

    struct tm tmbuf;
    localtime_s(&tmbuf, &tt);
    wchar_t buf[32] = {};
    wcsftime(buf, 32, L"%Y-%m-%d %H:%M:%S", &tmbuf);
    return buf;
}

void FileScanner::ScanRecursive(const std::wstring& rootDir, const std::wstring& currentPath,
    std::vector<FileEntry>& results)
{
    if (m_excludeCb && m_excludeCb(currentPath))
        return;

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(currentPath, fs::directory_options::skip_permission_denied, ec)) {
        std::wstring fullPath = entry.path().wstring();

        if (m_excludeCb && m_excludeCb(fullPath))
            continue;

        if (entry.is_directory(ec)) {
            ScanRecursive(rootDir, fullPath, results);
        } else if (entry.is_regular_file(ec)) {
            FileEntry fe;
            fe.relativePath = fullPath.substr(rootDir.length());
            if (!fe.relativePath.empty() && fe.relativePath[0] == L'\\')
                fe.relativePath = fe.relativePath.substr(1);
            fe.size = entry.file_size(ec);

            auto ftime = entry.last_write_time(ec);
            if (ftime != fs::file_time_type::min())
                fe.lastWriteTime = FileTimeToString(ftime);

            fe.attributes = FILE_ATTRIBUTE_NORMAL;
            results.push_back(fe);
        }
    }
}
