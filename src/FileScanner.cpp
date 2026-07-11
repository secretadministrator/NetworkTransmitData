#include "FileScanner.h"
#include <filesystem>

namespace fs = std::filesystem;

std::vector<FileEntry> FileScanner::ScanDirectory(const std::wstring& rootDir) {
    std::vector<FileEntry> results;
    m_progress = ScanProgress{};
    m_progress.currentPath = rootDir;
    m_lastProgressTime = std::chrono::steady_clock::now();

    std::error_code ec;
    if (!fs::exists(rootDir, ec) || ec) {
        if (ec)
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

    std::error_code ec;
    fs::directory_iterator it(currentPath, fs::directory_options::none, ec);
    if (ec) {
        m_progress.inaccessibleDirectories++;
        ReportProgress(true);
        return;
    }

    const fs::directory_iterator end;
    while (it != end) {
        const auto entry = *it;
        m_progress.currentPath = currentPath;
        std::wstring fullPath = entry.path().wstring();

        if (!m_excludeCb || !m_excludeCb(fullPath)) {
            std::error_code typeEc;
            if (entry.is_directory(typeEc)) {
                ScanRecursive(rootDir, fullPath, results);
            } else if (!typeEc && entry.is_regular_file(typeEc)) {
                std::error_code sizeEc;
                const auto fileSize = entry.file_size(sizeEc);
                if (!sizeEc) {
                    FileEntry fe;
                    fe.relativePath = fullPath.substr(rootDir.length());
                    if (!fe.relativePath.empty() && fe.relativePath[0] == L'\\')
                        fe.relativePath = fe.relativePath.substr(1);
                    fe.size = static_cast<int64_t>(fileSize);
                    fe.attributes = FILE_ATTRIBUTE_NORMAL;
                    results.push_back(fe);

                    m_progress.scannedFiles++;
                    m_progress.scannedBytes += fileSize;
                    ReportProgress();
                }
            }
        }

        it.increment(ec);
        if (ec) {
            m_progress.inaccessibleDirectories++;
            ReportProgress(true);
            break;
        }
    }
}
