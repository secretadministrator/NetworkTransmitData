#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "Utils.h"

namespace utils {

std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) return {};

    int len = WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(),
        NULL, 0, NULL, NULL);
    if (len <= 0) return {};

    std::string result(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), (int)value.size(),
        result.data(), len, NULL, NULL);
    return result;
}

std::wstring FromUtf8(const std::string& value) {
    if (value.empty()) return {};

    int len = MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(),
        NULL, 0);
    if (len <= 0) return {};

    std::wstring result(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), (int)value.size(),
        result.data(), len);
    return result;
}

std::wstring GetTimestamp() {
    wchar_t buf[64] = {};
    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf(buf, 64, L"%04d-%02d-%02d %02d:%02d:%02d",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::wstring GetTimestampForFilename() {
    wchar_t buf[32] = {};
    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf(buf, 32, L"%04d%02d%02d_%02d%02d%02d",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::wstring FormatBytes(int64_t bytes) {
    wchar_t buf[32] = {};
    const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    int unitIdx = 0;
    double val = (double)bytes;
    while (val >= 1024.0 && unitIdx < 4) {
        val /= 1024.0;
        ++unitIdx;
    }
    if (unitIdx == 0)
        swprintf(buf, 32, L"%.0f %s", val, units[unitIdx]);
    else
        swprintf(buf, 32, L"%.1f %s", val, units[unitIdx]);
    return buf;
}

std::wstring FormatSpeed(int64_t bytesPerSec) {
    return FormatBytes(bytesPerSec) + L"/s";
}

std::wstring FormatDuration(int64_t seconds) {
    if (seconds < 0) return L"--";

    wchar_t buf[32] = {};
    if (seconds < 60) {
        swprintf(buf, 32, L"%lld \u79d2", seconds);
    } else if (seconds < 3600) {
        swprintf(buf, 32, L"%lld \u5206 %lld \u79d2", seconds / 60, seconds % 60);
    } else {
        swprintf(buf, 32, L"%lld \u5c0f\u65f6 %lld \u5206", seconds / 3600, (seconds % 3600) / 60);
    }
    return buf;
}

void WaitMilliseconds(DWORD ms) {
    Sleep(ms);
}

std::wstring GetExecutableDir() {
    DWORD bufLen = MAX_PATH;
    std::wstring path(bufLen, L'\0');
    DWORD ret = GetModuleFileNameW(NULL, path.data(), bufLen);
    while (ret == bufLen) {
        bufLen *= 2;
        path.resize(bufLen);
        ret = GetModuleFileNameW(NULL, path.data(), bufLen);
    }
    if (ret == 0) return L".";
    path.resize(ret);
    size_t last = path.find_last_of(L'\\');
    if (last != std::wstring::npos)
        path.resize(last);
    return path;
}

std::wstring GetLocalIPString() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return L"0.0.0.0";

    std::wstring result = L"0.0.0.0";
    ULONG bufLen = 0;
    GetAdaptersAddresses(AF_INET, 0, NULL, NULL, &bufLen);
    if (bufLen == 0) { WSACleanup(); return result; }

    PIP_ADAPTER_ADDRESSES addrs = (PIP_ADAPTER_ADDRESSES)malloc(bufLen);
    if (!addrs) { WSACleanup(); return result; }

    if (GetAdaptersAddresses(AF_INET, 0, NULL, addrs, &bufLen) == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES a = addrs; a; a = a->Next) {
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            if (!a->OperStatus) continue;
            for (PIP_ADAPTER_UNICAST_ADDRESS u = a->FirstUnicastAddress; u; u = u->Next) {
                sockaddr_in* sa = (sockaddr_in*)u->Address.lpSockaddr;
                char ip[64] = {};
                InetNtopA(AF_INET, &sa->sin_addr, ip, sizeof(ip));
                result = FromUtf8(ip);
                goto done;
            }
        }
    }
done:
    free(addrs);
    WSACleanup();
    return result;
}

std::wstring NormalizePath(const std::wstring& path) {
    if (path.empty()) return path;
    if (path.substr(0, 4) == L"\\\\?\\") return path;
    if (path.substr(0, 2) == L"\\\\") {
        return L"\\\\?\\UNC\\" + path.substr(2);
    }
    if (path.size() >= 2 && path[1] == L':') {
        return L"\\\\?\\" + path;
    }
    return path;
}

bool CreateDirectoryTree(const std::wstring& path) {
    if (path.empty()) return false;
    if (CreateDirectoryW(NormalizePath(path).c_str(), NULL))
        return true;
    DWORD err = GetLastError();
    if (err == ERROR_ALREADY_EXISTS)
        return true;
    if (err != ERROR_PATH_NOT_FOUND)
        return false;

    size_t sep = path.find_last_of(L"\\/");
    if (sep == std::wstring::npos) return false;
    if (!CreateDirectoryTree(path.substr(0, sep)))
        return false;
    return CreateDirectoryW(path.c_str(), NULL) != FALSE
        || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::string BytesToHex(const uint8_t* data, size_t len) {
    static const char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        result += hex[(data[i] >> 4) & 0xF];
        result += hex[data[i] & 0xF];
    }
    return result;
}

std::string ComputeSHA256(const std::wstring& filePath) {
    return ComputeSHA256(filePath, -1);
}

std::string ComputeSHA256(const std::wstring& filePath, int64_t maxBytes) {
    return ComputeSHA256(filePath, maxBytes, {});
}

std::string ComputeSHA256(const std::wstring& filePath, int64_t maxBytes,
    const HashProgressCallback& progressCallback) {
    std::wstring normPath = NormalizePath(filePath);
    HANDLE hFile = CreateFileW(normPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return {};

    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_HASH_HANDLE hHash = NULL;
    std::string result;

    do {
        if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0)
            break;
        if (BCryptCreateHash(hAlg, &hHash, NULL, 0, NULL, 0, 0) < 0)
            break;

        constexpr DWORD BUF_SIZE = 65536;
        std::vector<uint8_t> buf(BUF_SIZE);
        DWORD bytesRead = 0;
        int64_t remaining = maxBytes;
        bool readOk = true;
        while (maxBytes < 0 || remaining > 0) {
            DWORD toRead = BUF_SIZE;
            if (maxBytes >= 0 && remaining < (int64_t)toRead)
                toRead = (DWORD)remaining;
            if (toRead == 0)
                break;
            if (!ReadFile(hFile, buf.data(), toRead, &bytesRead, NULL)) {
                readOk = false;
                break;
            }
            if (bytesRead == 0)
                break;
            if (BCryptHashData(hHash, buf.data(), bytesRead, 0) < 0) {
                readOk = false;
                break;
            }
            if (progressCallback && !progressCallback(bytesRead)) {
                readOk = false;
                break;
            }
            if (maxBytes >= 0)
                remaining -= bytesRead;
        }
        if (!readOk || (maxBytes >= 0 && remaining > 0))
            break;

        uint8_t hashValue[32];
        if (BCryptFinishHash(hHash, hashValue, 32, 0) < 0)
            break;

        result = BytesToHex(hashValue, 32);
    } while (false);

    if (hHash) BCryptDestroyHash(hHash);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    CloseHandle(hFile);
    return result;
}

} // namespace utils
