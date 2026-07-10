#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include "MainWindow.h"
#include "AppConfig.h"
#include "AuditLogger.h"
#include "IpConfigurator.h"
#include "Utils.h"

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    INITCOMMONCONTROLSEX icex = {};
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    AppConfig::Instance();

    AuditLogger::Instance().SetLogDir(utils::GetExecutableDir() + L"\\logs");
    AuditLogger::Instance().Write(L"DirectTransfer \u542f\u52a8");

    MainWindow mainWnd;
    if (!mainWnd.Create(hInst, nCmdShow)) {
        AuditLogger::Instance().Write(L"\u7a97\u53e3\u521b\u5efa\u5931\u8d25");
        return 1;
    }

    mainWnd.RunMessageLoop();

    if (!AppConfig::Instance().configuredNicName.empty()) {
        IpConfigurator::EnableDHCP(AppConfig::Instance().configuredNicName);
    }
    AuditLogger::Instance().Write(L"DirectTransfer \u9000\u51fa");
    CoUninitialize();
    return 0;
}
