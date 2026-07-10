#include "PairingHandler.h"
#include <random>

PairingHandler::PairingHandler() {
    GenerateCode();
}

std::wstring PairingHandler::GenerateCode() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, 9);

    wchar_t code[7] = {};
    for (int i = 0; i < 6; ++i)
        code[i] = L'0' + dist(gen);
    code[6] = L'\0';
    m_expectedCode = code;
    return m_expectedCode;
}

bool PairingHandler::Verify(const std::wstring& input) const {
    return input == m_expectedCode;
}
