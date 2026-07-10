#include "Manifest.h"
#include "Utils.h"
#include <nlohmann/json.hpp>

Manifest::Manifest(const std::vector<FileEntry>& entries)
    : m_entries(entries) {
    for (const auto& e : entries)
        m_totalSize += e.size;
}

std::string Manifest::ToJSON() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : m_entries) {
        nlohmann::json obj;
        obj["path"] = utils::ToUtf8(e.relativePath);
        obj["size"] = e.size;
        obj["sha256"] = e.sha256;
        arr.push_back(obj);
    }
    return arr.dump();
}

bool Manifest::FromJSON(const std::string& json) {
    m_entries.clear();
    m_totalSize = 0;
    try {
        auto arr = nlohmann::json::parse(json);
        if (!arr.is_array()) return false;
        for (const auto& obj : arr) {
            FileEntry entry;
            std::string pathA = obj["path"].get<std::string>();
            entry.relativePath = utils::FromUtf8(pathA);
            entry.size = obj["size"].get<int64_t>();
            entry.sha256 = obj.value("sha256", std::string());
            m_totalSize += entry.size;
            m_entries.push_back(entry);
        }
        return true;
    } catch (...) {
        return false;
    }
}
