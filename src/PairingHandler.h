#pragma once
#include <string>

class PairingHandler {
public:
    PairingHandler();

    std::wstring GenerateToken();
    void SetExpectedToken(const std::wstring& token) { m_expectedToken = token; }
    bool Verify(const std::wstring& input) const;

private:
    std::wstring m_expectedToken;
};
