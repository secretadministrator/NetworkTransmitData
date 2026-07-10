#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <wbemidl.h>
#include <iphlpapi.h>
#include <vector>
#include <string>
#include "IpConfigurator.h"

// Explicit GUID definitions for MinGW compatibility (avoids -lwbemuuid)
static const CLSID CLSID_WbemLocator_ =
    {0x4590F811,0x1D3A,0x11D0,{0x89,0x1F,0x00,0xAA,0x00,0x4B,0x2E,0x24}};
static const IID IID_IWbemLocator_ =
    {0xDC12A687,0x737F,0x11CF,{0x88,0x4D,0x00,0xAA,0x00,0x4B,0x2E,0x24}};

int IpConfigurator::GetAdminState() {
    BOOL isAdmin = FALSE;
    PSID adminSid = NULL;
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuth, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &adminSid))
    {
        CheckTokenMembership(NULL, adminSid, &isAdmin);
        FreeSid(adminSid);
    }
    if (!isAdmin) return 0;

    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elev{};
        DWORD size = sizeof(elev);
        if (GetTokenInformation(hToken, TokenElevation, &elev, size, &size)) {
            BOOL elevated = elev.TokenIsElevated;
            CloseHandle(hToken);
            return elevated ? 2 : 1;
        }
        CloseHandle(hToken);
    }
    return 1;
}

bool IpConfigurator::EnumerateNICs(std::vector<std::wstring>& names) {
    names.clear();
    ULONG bufLen = 0;
    GetAdaptersAddresses(AF_INET, 0, NULL, NULL, &bufLen);
    if (bufLen == 0) return false;

    PIP_ADAPTER_ADDRESSES addrs = (PIP_ADAPTER_ADDRESSES)malloc(bufLen);
    if (!addrs) return false;

    if (GetAdaptersAddresses(AF_INET, 0, NULL, addrs, &bufLen) == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES a = addrs; a; a = a->Next) {
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            if (!a->OperStatus) continue;

            std::wstring name = a->FriendlyName;
            std::wstring lower;
            for (wchar_t c : name) lower += towlower(c);
            if (lower.find(L"virtual") != std::wstring::npos ||
                lower.find(L"vpn") != std::wstring::npos ||
                lower.find(L"bluetooth") != std::wstring::npos ||
                lower.find(L"loopback") != std::wstring::npos)
                continue;

            names.push_back(name);
        }
    }

    free(addrs);
    return !names.empty();
}

bool IpConfigurator::DetectNIC(std::wstring& nicName) {
    ULONG bufLen = 0;
    GetAdaptersAddresses(AF_INET, 0, NULL, NULL, &bufLen);
    if (bufLen == 0) return false;

    PIP_ADAPTER_ADDRESSES addrs = (PIP_ADAPTER_ADDRESSES)malloc(bufLen);
    if (!addrs) return false;

    bool found = false;
    if (GetAdaptersAddresses(AF_INET, 0, NULL, addrs, &bufLen) == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES a = addrs; a; a = a->Next) {
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            if (!a->OperStatus) continue;

            std::wstring name = a->FriendlyName;
            std::wstring lower;
            for (wchar_t c : name) lower += towlower(c);
            if (lower.find(L"wireless") != std::wstring::npos ||
                lower.find(L"wi-fi") != std::wstring::npos ||
                lower.find(L"wlan") != std::wstring::npos ||
                lower.find(L"bluetooth") != std::wstring::npos ||
                lower.find(L"virtual") != std::wstring::npos ||
                lower.find(L"vpn") != std::wstring::npos ||
                lower.find(L"loopback") != std::wstring::npos)
                continue;

            nicName = name;
            found = true;
            break;
        }
    }

    free(addrs);
    return found;
}

// ─── WMI helpers ──────────────────────────────────────────

static bool WmiConnect(IWbemServices** ppSvc) {
    if (!ppSvc) return false;
    *ppSvc = NULL;

    IWbemLocator* pLoc = NULL;
    HRESULT hr = CoCreateInstance(CLSID_WbemLocator_, NULL,
        CLSCTX_INPROC_SERVER, IID_IWbemLocator_, (LPVOID*)&pLoc);
    if (FAILED(hr)) return false;

    BSTR ns = SysAllocString(L"ROOT\\CIMV2");
    hr = pLoc->ConnectServer(ns, NULL, NULL, 0, 0, 0, NULL, ppSvc);
    SysFreeString(ns);
    pLoc->Release();
    if (FAILED(hr) || !*ppSvc) return false;

    hr = CoSetProxyBlanket(*ppSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
        RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
    if (FAILED(hr)) {
        (*ppSvc)->Release();
        *ppSvc = NULL;
        return false;
    }
    return true;
}

static int WmiQueryIndex(IWbemServices* pSvc, const std::wstring& whereClause) {
    std::wstring q = L"SELECT Index FROM Win32_NetworkAdapter WHERE " + whereClause;
    BSTR bstrLang = SysAllocString(L"WQL");
    BSTR bstrQ = SysAllocString(q.c_str());
    IEnumWbemClassObject* pEnum = NULL;
    HRESULT hr = pSvc->ExecQuery(bstrLang, bstrQ,
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &pEnum);
    SysFreeString(bstrQ);
    SysFreeString(bstrLang);

    int idx = -1;
    if (SUCCEEDED(hr) && pEnum) {
        IWbemClassObject* pObj = NULL;
        ULONG ret = 0;
        hr = pEnum->Next(WBEM_INFINITE, 1, &pObj, &ret);
        if (SUCCEEDED(hr) && ret > 0) {
            VARIANT vtIdx;
            VariantInit(&vtIdx);
            if (SUCCEEDED(pObj->Get(L"Index", 0, &vtIdx, NULL, NULL)))
                idx = vtIdx.intVal;
            VariantClear(&vtIdx);
            pObj->Release();
        }
        pEnum->Release();
    }
    return idx;
}

static std::wstring GetAdapterGUID(const std::wstring& nicName) {
    ULONG bufLen = 0;
    GetAdaptersAddresses(AF_INET, 0, NULL, NULL, &bufLen);
    if (bufLen == 0) return {};

    PIP_ADAPTER_ADDRESSES addrs = (PIP_ADAPTER_ADDRESSES)malloc(bufLen);
    if (!addrs) return {};

    std::wstring guid;
    if (GetAdaptersAddresses(AF_INET, 0, NULL, addrs, &bufLen) == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES a = addrs; a; a = a->Next) {
            if (nicName != a->FriendlyName) continue;
            // AdapterName format: "\DEVICE\TCPIP_{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}"
            if (a->AdapterName) {
                std::string ansi(a->AdapterName);
                auto pos = ansi.find_last_of('{');
                if (pos != std::string::npos) {
                    std::string g = ansi.substr(pos);
                    guid.assign(g.begin(), g.end());
                }
            }
            break;
        }
    }
    free(addrs);
    return guid;
}

static int WmiGetAdapterIndex(const std::wstring& nicName) {
    IWbemServices* pSvc = NULL;
    if (!WmiConnect(&pSvc)) return -1;

    int idx = -1;

    // Strategy 1: match by GUID (most reliable, avoids name-matching issues)
    std::wstring guid = GetAdapterGUID(nicName);
    if (!guid.empty())
        idx = WmiQueryIndex(pSvc, L"GUID='" + guid + L"'");

    // Strategy 2: fallback to NetConnectionID (original approach)
    if (idx < 0) {
        std::wstring escaped;
        for (wchar_t c : nicName) {
            if (c == L'\'') escaped += L"''";
            else escaped += c;
        }
        idx = WmiQueryIndex(pSvc, L"NetConnectionID='" + escaped + L"'");
    }

    pSvc->Release();
    return idx;
}

static bool WmiExecAdapterMethod(int adapterIndex, const wchar_t* methodName,
    const wchar_t* ip, const wchar_t* subnet)
{
    IWbemServices* pSvc = NULL;
    if (!WmiConnect(&pSvc)) return false;

    std::wstring cfgQ = L"SELECT * FROM Win32_NetworkAdapterConfiguration WHERE Index="
        + std::to_wstring(adapterIndex);
    BSTR bstrLang = SysAllocString(L"WQL");
    BSTR bstrCfgQ = SysAllocString(cfgQ.c_str());
    IEnumWbemClassObject* pEnum = NULL;
    HRESULT hr = pSvc->ExecQuery(bstrLang, bstrCfgQ,
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &pEnum);
    SysFreeString(bstrCfgQ);
    SysFreeString(bstrLang);

    bool ok = false;
    if (SUCCEEDED(hr) && pEnum) {
        IWbemClassObject* pCfg = NULL;
        ULONG ret = 0;
        hr = pEnum->Next(WBEM_INFINITE, 1, &pCfg, &ret);
        if (SUCCEEDED(hr) && ret > 0) {
            IWbemClassObject* pClass = NULL;
            BSTR bstrClass = SysAllocString(L"Win32_NetworkAdapterConfiguration");
            hr = pSvc->GetObject(bstrClass, 0, NULL, &pClass, NULL);
            SysFreeString(bstrClass);

            if (SUCCEEDED(hr) && pClass) {
                IWbemClassObject* pInParamsDef = NULL;
                BSTR bstrMethod = SysAllocString(methodName);
                hr = pClass->GetMethod(bstrMethod, 0, &pInParamsDef, NULL);
                SysFreeString(bstrMethod);
                pClass->Release();

                IWbemClassObject* pInParams = NULL;
                if (SUCCEEDED(hr) && pInParamsDef) {
                    hr = pInParamsDef->SpawnInstance(0, &pInParams);
                    pInParamsDef->Release();
                }

                if (SUCCEEDED(hr)) {
                    if (pInParams && ip && subnet) {
                        SAFEARRAYBOUND sab[1] = {1, 0};
                        LONG rgIdx = 0;

                        VARIANT vtIP; VariantInit(&vtIP);
                        vtIP.vt = VT_ARRAY | VT_BSTR;
                        vtIP.parray = SafeArrayCreate(VT_BSTR, 1, sab);
                        BSTR bIP = SysAllocString(ip);
                        SafeArrayPutElement(vtIP.parray, &rgIdx, bIP);
                        SysFreeString(bIP);
                        pInParams->Put(L"IPAddress", 0, &vtIP, 0);
                        VariantClear(&vtIP);

                        VARIANT vtSub; VariantInit(&vtSub);
                        vtSub.vt = VT_ARRAY | VT_BSTR;
                        vtSub.parray = SafeArrayCreate(VT_BSTR, 1, sab);
                        BSTR bSub = SysAllocString(subnet);
                        SafeArrayPutElement(vtSub.parray, &rgIdx, bSub);
                        SysFreeString(bSub);
                        pInParams->Put(L"SubnetMask", 0, &vtSub, 0);
                        VariantClear(&vtSub);
                    }

                    BSTR bstrMethod2 = SysAllocString(methodName);
                    VARIANT vtPath;
                    VariantInit(&vtPath);
                    hr = pCfg->Get(L"__PATH", 0, &vtPath, NULL, NULL);
                    if (SUCCEEDED(hr) && vtPath.bstrVal) {
                        IWbemClassObject* pOutParams = NULL;
                        hr = pSvc->ExecMethod(vtPath.bstrVal, bstrMethod2, 0,
                            NULL, pInParams, &pOutParams, NULL);
                        if (SUCCEEDED(hr) && pOutParams) {
                            VARIANT vtRetVal;
                            VariantInit(&vtRetVal);
                            hr = pOutParams->Get(L"ReturnValue", 0,
                                &vtRetVal, NULL, NULL);
                            if (SUCCEEDED(hr) && vtRetVal.vt == VT_I4)
                                ok = (vtRetVal.intVal == 0);
                            VariantClear(&vtRetVal);
                            pOutParams->Release();
                        }
                    }
                    VariantClear(&vtPath);
                    SysFreeString(bstrMethod2);
                }

                if (pInParams) pInParams->Release();
            }
            pCfg->Release();
        }
        pEnum->Release();
    }
    pSvc->Release();
    return ok;
}

bool IpConfigurator::SetStaticIP(const std::wstring& nicName,
    const std::wstring& ip, const std::wstring& subnetMask)
{
    if (GetAdminState() != 2) return false;
    int idx = WmiGetAdapterIndex(nicName);
    if (idx < 0) return false;
    return WmiExecAdapterMethod(idx, L"EnableStatic", ip.c_str(), subnetMask.c_str());
}

bool IpConfigurator::EnableDHCP(const std::wstring& nicName) {
    if (GetAdminState() != 2) return false;
    int idx = WmiGetAdapterIndex(nicName);
    if (idx < 0) return false;
    return WmiExecAdapterMethod(idx, L"EnableDHCP", NULL, NULL);
}
