#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <string>
#include <vector>

class IpConfigurator {
public:
    static int GetAdminState();
    static bool SetStaticIP(const std::wstring& nicName, const std::wstring& ip, const std::wstring& subnetMask);
    static bool EnableDHCP(const std::wstring& nicName);
    static bool DetectNIC(std::wstring& nicName);
    static bool EnumerateNICs(std::vector<std::wstring>& names);
};
