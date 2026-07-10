#include "AppConfig.h"

AppConfig& AppConfig::Instance() {
    static AppConfig config;
    return config;
}

bool AppConfig::ParseCommandLine(int argc, wchar_t* argv[]) {
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--port" && i + 1 < argc) {
            port = _wtoi(argv[++i]);
        } else if (arg == L"--min-free" && i + 1 < argc) {
            minFreeGB = _wtoi(argv[++i]);
        } else if (arg == L"--source-dir" && i + 1 < argc) {
            sourceDir = argv[++i];
        } else if (arg == L"--target-dir" && i + 1 < argc) {
            targetDir = argv[++i];
        } else if (arg == L"--help" || arg == L"/?" || arg == L"-?") {
            return false;
        }
    }
    return true;
}
