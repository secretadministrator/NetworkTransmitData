#include "ReportGenerator.h"
#include "Utils.h"
#include "Version.h"
#include <filesystem>
#include <fstream>

std::wstring ReportGenerator::GenerateReport(const TransferStats& stats,
    const std::wstring& sourceDir, const std::wstring& targetDir) {
    std::wstring report;
    report += L"DirectTransfer 传输报告\n";
    report += L"========================\n\n";
    report += L"版本: " + std::wstring(version::APP_VERSION) + L"\n";
    report += L"协议版本: " + std::to_wstring(version::PROTOCOL_VERSION) + L"\n";
    report += L"传输时间: " + utils::GetTimestamp() + L"\n";
    report += L"源目录: " + sourceDir + L"\n";
    report += L"目标目录: " + targetDir + L"\n\n";
    report += L"总计文件: " + std::to_wstring(stats.totalFiles) + L"\n";
    report += L"已提交: " + std::to_wstring(stats.completedFiles) + L"\n";
    report += L"已跳过: " + std::to_wstring(stats.skippedFiles) + L"\n";
    report += L"失败: " + std::to_wstring(stats.failedFiles) + L"\n";
    report += L"总计大小: " + utils::FormatBytes(stats.totalBytes) + L"\n";
    report += L"平均速度: " + utils::FormatSpeed(stats.averageSpeedBytesPerSec) + L"\n";
    report += L"总用时: " + utils::FormatDuration(stats.elapsedSeconds) + L"\n";
    report += L"自动重连: " + std::to_wstring(stats.retryCount) + L" 次\n";
    report += L"\n报告结束\n";
    return report;
}

bool ReportGenerator::SaveReport(const std::wstring& report, const std::wstring& dir) {
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\report_" +
        utils::GetTimestampForFilename() + L".txt";
    std::ofstream output(std::filesystem::path(path), std::ios::out | std::ios::binary);
    if (!output)
        return false;
    const std::string utf8 = utils::ToUtf8(report);
    output.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    return output.good();
}
