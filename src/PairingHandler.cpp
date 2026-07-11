#include "PairingHandler.h"
#include <windows.h>
#include <bcrypt.h>
#include <cstdint>

PairingHandler::PairingHandler() {
    GenerateToken();
}

std::wstring PairingHandler::GenerateToken() {
    uint8_t bytes[16] = {};
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        m_expectedToken.clear();
        return {};
    }

    static constexpr wchar_t HEX[] = L"0123456789abcdef";
    m_expectedToken.resize(sizeof(bytes) * 2);
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        m_expectedToken[i * 2] = HEX[bytes[i] >> 4];
        m_expectedToken[i * 2 + 1] = HEX[bytes[i] & 0x0F];
    }
    return m_expectedToken;
}

bool PairingHandler::Verify(const std::wstring& input) const {
    return !m_expectedToken.empty() && input == m_expectedToken;
}
