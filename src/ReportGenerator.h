#pragma once
#include <string>
#include "TransferSession.h"

class ReportGenerator {
public:
    static std::wstring GenerateReport(const TransferStats& stats, const std::wstring& sourceDir,
        const std::wstring& targetDir);
    static bool SaveReport(const std::wstring& report, const std::wstring& dir);
};
