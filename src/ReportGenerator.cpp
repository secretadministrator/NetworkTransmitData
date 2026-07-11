#include "ReportGenerator.h"
#include "Utils.h"
#include "Version.h"
#include <fstream>
#include <filesystem>

std::wstring ReportGenerator::GenerateReport(const TransferStats& stats, const std::wstring& sourceDir,
    const std::wstring& targetDir)
{
    std::wstring report;
    report += L"DirectTransfer 传输报告\n";
    report += L"========================\n\n";
    report += L"\u7248\u672c: " + std::wstring(version::APP_VERSION) + L"\n";
    report += L"\u534f\u8bae\u7248\u672c: " + std::to_wstring(version::PROTOCOL_VERSION) + L"\n";
    report += L"传输时间: " + utils::GetTimestamp() + L"\n";
    report += L"源目录: " + sourceDir + L"\n";
    report += L"目标目录: " + targetDir + L"\n\n";
    report += L"总计文件: " + std::to_wstring(stats.totalFiles) + L"\n";
    report += L"已传输: " + std::to_wstring(stats.completedFiles) + L"\n";
    report += L"已跳过: " + std::to_wstring(stats.skippedFiles) + L"\n";
    report += L"失败: " + std::to_wstring(stats.failedFiles) + L"\n";
    report += L"总计大小: " + utils::FormatBytes(stats.totalBytes) + L"\n";
    report += L"传输速度: " + utils::FormatSpeed(stats.averageSpeedBytesPerSec) + L"\n";
    report += L"\n报告结束\n";
    return report;
}

bool ReportGenerator::SaveReport(const std::wstring& report, const std::wstring& dir) {
    CreateDirectoryW(dir.c_str(), NULL);
    std::wstring path = dir + L"\\report_" + utils::GetTimestampForFilename() + L".txt";
    std::ofstream ofs(std::filesystem::path(path), std::ios::out | std::ios::binary);
    if (!ofs) return false;
    std::string utf8 = utils::ToUtf8(report);
    ofs.write(utf8.data(), (std::streamsize)utf8.size());
    return true;
}
