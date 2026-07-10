#pragma once
#include <string>
#include <vector>
#include <cstdint>

enum class PacketType : uint8_t {
    DISCOVER_RESPONSE = 0x01,
    PAIRING_REQUEST = 0x02,
    PAIRING_RESPONSE = 0x03,
    MANIFEST = 0x04,
    TRANSFER_PLAN = 0x05,
    FILE_HEADER = 0x06,
    FILE_CHUNK = 0x07,
    FILE_DONE = 0x08,
    FILE_VERIFY = 0x09,
    PROGRESS = 0x0A,
    FILE_HASH = 0x0B,
    FILE_DONE_ACK = 0x0C,
    DONE_ACK = 0x0D,

    HEARTBEAT = 0x0E,
    HEARTBEAT_ACK = 0x0F,

    ERROR_MSG = 0xFF,
    DONE = 0x00
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
