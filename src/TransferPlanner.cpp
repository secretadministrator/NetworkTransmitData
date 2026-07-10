#include "TransferPlanner.h"
#include "ResumeManager.h"
#include "Utils.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <unordered_set>
#include <functional>

namespace fs = std::filesystem;

TransferPlanner::Plan TransferPlanner::BuildPlan(const Manifest& manifest, const std::wstring& targetDir, TransferMode mode) {
    Plan plan;
    ResumeManager resumer;

    bool overwrite = (mode == TransferMode::OVERWRITE || mode == TransferMode::MIRROR);

    for (const auto& entry : manifest.GetEntries()) {
        std::wstring targetPath = targetDir + L"\\" + entry.relativePath;
        std::error_code ec;
        bool exists = fs::exists(targetPath, ec);

        PlanEntry pe;
        pe.relativePath = entry.relativePath;
        pe.size = entry.size;
        pe.sha256 = entry.sha256;

        std::wstring partPath = targetPath + L".dtpart";
        pe.offset = overwrite ? 0 : resumer.GetResumeOffset(partPath);
        if (pe.offset < 0 || pe.offset > entry.size)
            pe.offset = 0;
        if (pe.offset > 0) {
            pe.resumeHash = utils::ComputeSHA256(partPath, pe.offset);
            if (pe.resumeHash.empty()) {
                pe.offset = 0;
            }
        }

        auto addTransfer = [&]() {
            int64_t remaining = entry.size - pe.offset;
            if (remaining < 0) remaining = 0;
            plan.totalBytes += remaining;
            plan.totalFiles++;
        };

        if (!exists) {
            pe.action = FileAction::TRANSFER;
            addTransfer();
        } else if (overwrite) {
            pe.action = FileAction::OVERWRITE;
            pe.offset = 0;
            addTransfer();
        } else {
            auto existingSize = fs::file_size(targetPath, ec);
            if (!ec && existingSize == entry.size && !entry.sha256.empty()
                    && utils::ComputeSHA256(targetPath) == entry.sha256) {
                pe.action = FileAction::SKIP;
                plan.skipFiles++;
            } else {
                pe.action = FileAction::TRANSFER;
                addTransfer();
            }
        }

        plan.entries.push_back(pe);
    }

    return plan;
}

int TransferPlanner::DeleteExtraFiles(const Manifest& manifest, const std::wstring& targetDir) {
    // Build a set of expected paths from manifest
    std::unordered_set<std::wstring> expected;
    for (const auto& e : manifest.GetEntries()) {
        std::wstring full = targetDir + L"\\" + e.relativePath;
        expected.insert(full);
    }

    int deleted = 0;
    // Recursively scan target directory
    std::function<void(const std::wstring&)> scan = [&](const std::wstring& dir) {
        WIN32_FIND_DATAW ffd = {};
        std::wstring pattern = dir + L"\\*";
        HANDLE hFind = FindFirstFileW(pattern.c_str(), &ffd);
        if (hFind == INVALID_HANDLE_VALUE) return;

        do {
            std::wstring name = ffd.cFileName;
            if (name == L"." || name == L"..") continue;
            std::wstring full = dir + L"\\" + name;

            if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                scan(full);
                // Remove directory if empty
                RemoveDirectoryW(full.c_str());
            } else {
                if (expected.find(full) == expected.end()) {
                    DeleteFileW(full.c_str());
                    deleted++;
                }
            }
        } while (FindNextFileW(hFind, &ffd));
        FindClose(hFind);
    };

    scan(targetDir);
    return deleted;
}

std::string TransferPlanner::SerializePlan(const Plan& plan) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : plan.entries) {
        nlohmann::json obj;
        obj["path"] = utils::ToUtf8(e.relativePath);
        obj["size"] = e.size;
        obj["offset"] = e.offset;
        obj["sha256"] = e.sha256;
        obj["resumeHash"] = e.resumeHash;
        obj["action"] = static_cast<int>(e.action);
        arr.push_back(obj);
    }
    return arr.dump();
}

TransferPlanner::Plan TransferPlanner::ParsePlanString(const std::string& json) {
    Plan plan;
    try {
        auto arr = nlohmann::json::parse(json);
        if (!arr.is_array()) return plan;

        for (const auto& obj : arr) {
            PlanEntry e;
            std::string pathA = obj["path"].get<std::string>();
            e.relativePath = utils::FromUtf8(pathA);
            e.size = obj["size"].get<int64_t>();
            e.offset = obj.value("offset", (int64_t)0);
            if (e.offset < 0 || e.offset > e.size)
                e.offset = 0;
            e.sha256 = obj.value("sha256", std::string());
            e.resumeHash = obj.value("resumeHash", std::string());
            e.action = static_cast<FileAction>(obj["action"].get<int>());

            if (e.action == FileAction::TRANSFER || e.action == FileAction::OVERWRITE) {
                int64_t remaining = e.size - e.offset;
                if (remaining < 0) remaining = 0;
                plan.totalBytes += remaining;
                plan.totalFiles++;
            }
            if (e.action == FileAction::SKIP) {
                plan.skipFiles++;
            }
            plan.entries.push_back(e);
        }
    } catch (...) {
        return {};
    }
    return plan;
}
