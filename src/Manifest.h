#pragma once
#include <string>
#include <vector>
#include "FileScanner.h"

class Manifest {
public:
    Manifest() = default;
    explicit Manifest(const std::vector<FileEntry>& entries);

    std::string ToJSON() const;
    bool FromJSON(const std::string& json);

    const std::vector<FileEntry>& GetEntries() const { return m_entries; }
    int64_t GetTotalSize() const { return m_totalSize; }

private:
    std::vector<FileEntry> m_entries;
    int64_t m_totalSize = 0;
};
