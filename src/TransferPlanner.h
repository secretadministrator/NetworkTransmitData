#pragma once
#include <string>
#include <vector>
#include "FileScanner.h"
#include "Manifest.h"

enum class FileAction { TRANSFER, SKIP, OVERWRITE };

enum class TransferMode { SAFE_COPY, OVERWRITE, MIRROR };

struct PlanEntry {
    std::wstring relativePath;
    int64_t size = 0;
    int64_t offset = 0;
    std::string sha256;
    std::string resumeHash;
    FileAction action = FileAction::TRANSFER;
};

class TransferPlanner {
public:
    struct Plan {
        std::vector<PlanEntry> entries;
        int64_t totalBytes = 0;
        int totalFiles = 0;
        int skipFiles = 0;
    };

    Plan BuildPlan(const Manifest& manifest, const std::wstring& targetDir, TransferMode mode = TransferMode::SAFE_COPY);

    static std::vector<std::wstring> FindExtraFiles(const Manifest& manifest, const std::wstring& targetDir);
    static int DeleteExtraFiles(const Manifest& manifest, const std::wstring& targetDir);

    static std::string SerializePlan(const Plan& plan);
    static Plan ParsePlanString(const std::string& json);
};
