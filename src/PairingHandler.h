#pragma once
#include <string>

class PairingHandler {
public:
    PairingHandler();

    std::wstring GenerateCode();
    void SetExpectedCode(const std::wstring& code) { m_expectedCode = code; }
    bool Verify(const std::wstring& input) const;

private:
    std::wstring m_expectedCode;
};
