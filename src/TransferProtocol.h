#pragma once
#include <string>
#include <vector>
#include <cstdint>

enum class PacketType : uint8_t {
    DISCOVER_RESPONSE = 0x01,
    SERVER_HELLO = 0x02,
    CLIENT_HELLO = 0x03,
    HELLO_ACK = 0x10,
    SESSION_BEGIN = 0x15,
    SESSION_READY = 0x16,
    CATALOG_CHUNK = 0x17,
    CATALOG_DONE = 0x18,
    BATCH_DATA = 0x19,
    BATCH_ACK = 0x1A,
    FILE_BEGIN_V5 = 0x1B,
    FILE_DATA_V5 = 0x1C,
    FILE_END_V5 = 0x1D,
    FILE_ACK_V5 = 0x1E,
    SESSION_COMMIT = 0x1F,
    SESSION_COMMIT_ACK = 0x20,

    ERROR_MSG = 0xFF
};

struct Packet {
    PacketType type;
    std::vector<uint8_t> payload;

    std::vector<uint8_t> Serialize() const;
    static Packet Deserialize(const uint8_t* data, size_t len);
};

// Protocol helpers
std::vector<uint8_t> MakeStringPacket(PacketType type, const std::wstring& str);
std::wstring ParseStringPacket(const Packet& pkt);
