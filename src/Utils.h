#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <cstdint>
#include <functional>

namespace utils {

std::string ToUtf8(const std::wstring& value);
std::wstring FromUtf8(const std::string& value);
std::wstring GetTimestamp();
std::wstring GetTimestampForFilename();
std::wstring FormatBytes(int64_t bytes);
std::wstring FormatSpeed(int64_t bytesPerSec);
std::wstring FormatDuration(int64_t seconds);
void WaitMilliseconds(DWORD ms);
std::wstring GetExecutableDir();
std::wstring GetLocalIPString();
std::wstring NormalizePath(const std::wstring& path);
bool CreateDirectoryTree(const std::wstring& path);
std::string ComputeSHA256(const std::wstring& filePath);
std::string ComputeSHA256(const std::wstring& filePath, int64_t maxBytes);
using HashProgressCallback = std::function<bool(int64_t)>;
std::string ComputeSHA256(const std::wstring& filePath, int64_t maxBytes,
    const HashProgressCallback& progressCallback);
std::string BytesToHex(const uint8_t* data, size_t len);

} // namespace utils
