#include "ExcludeRules.h"
#include <algorithm>

ExcludeRules::ExcludeRules() {
    AddRule([](const std::wstring& path) -> bool {
        std::wstring name = path;
        size_t pos = path.find_last_of(L"\\/");
        if (pos != std::wstring::npos)
            name = path.substr(pos + 1);

        static const std::wstring excluded[] = {
            L"System Volume Information",
            L"$RECYCLE.BIN",
            L"Recovery",
            L"pagefile.sys",
            L"hiberfil.sys",
            L"swapfile.sys"
        };
        for (const auto& e : excluded) {
            if (_wcsicmp(name.c_str(), e.c_str()) == 0)
                return true;
        }
        return false;
    });
}

bool ExcludeRules::IsExcluded(const std::wstring& path) const {
    for (const auto& rule : m_rules) {
        if (rule(path)) return true;
    }
    return false;
}

void ExcludeRules::AddRule(std::function<bool(const std::wstring&)> rule) {
    m_rules.push_back(std::move(rule));
}

void ExcludeRules::Clear() {
    m_rules.clear();
}
