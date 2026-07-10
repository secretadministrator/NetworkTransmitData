#pragma once
#include <string>
#include <vector>
#include <functional>

class ExcludeRules {
public:
    ExcludeRules();
    bool IsExcluded(const std::wstring& path) const;
    void AddRule(std::function<bool(const std::wstring&)> rule);
    void Clear();

private:
    std::vector<std::function<bool(const std::wstring&)>> m_rules;
};
