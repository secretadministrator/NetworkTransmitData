#pragma once
#include <string>
#include <vector>

struct AppConfig {
    static constexpr int DEFAULT_PORT = 49321;
    static constexpr int CHUNK_SIZE = 1024 * 1024;
    static constexpr int MIN_FREE_GB = 1;
    static constexpr int DISCOVERY_TIMEOUT_MS = 3000;
    static constexpr int PING_TIMEOUT_MS = 1000;

    int port = DEFAULT_PORT;
    int minFreeGB = MIN_FREE_GB;

    std::wstring sourceDir;
    std::wstring targetDir;
    std::wstring configuredNicName;

    static AppConfig& Instance();
    bool ParseCommandLine(int argc, wchar_t* argv[]);
};
