#pragma once
#include <string>
#include <cstdint>

class ResumeManager {
public:
    int64_t GetResumeOffset(const std::wstring& partFilePath);
    bool CleanPartFile(const std::wstring& partFilePath);
    static std::wstring GetPartPath(const std::wstring& finalPath);
};
