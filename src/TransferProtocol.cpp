#include "TransferProtocol.h"
#include "Utils.h"
#include <cstring>

std::vector<uint8_t> Packet::Serialize() const {
    uint32_t payloadLen = (uint32_t)payload.size();
    std::vector<uint8_t> buf(5 + payloadLen);
    buf[0] = (uint8_t)type;
    buf[1] = (uint8_t)(payloadLen & 0xFF);
    buf[2] = (uint8_t)((payloadLen >> 8) & 0xFF);
    buf[3] = (uint8_t)((payloadLen >> 16) & 0xFF);
    buf[4] = (uint8_t)((payloadLen >> 24) & 0xFF);
    if (payloadLen > 0)
        memcpy(buf.data() + 5, payload.data(), payloadLen);
    return buf;
}

Packet Packet::Deserialize(const uint8_t* data, size_t len) {
    Packet pkt;
    if (len < 5) return pkt;
    pkt.type = (PacketType)data[0];
    uint32_t payloadLen = (uint32_t)data[1] | ((uint32_t)data[2] << 8)
        | ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 24);
    if (5 + payloadLen <= len) {
        pkt.payload.assign(data + 5, data + 5 + payloadLen);
    }
    return pkt;
}

std::vector<uint8_t> MakeStringPacket(PacketType type, const std::wstring& str) {
    std::string utf8 = utils::ToUtf8(str);
    Packet pkt;
    pkt.type = type;
    pkt.payload.assign(utf8.begin(), utf8.end());
    return pkt.Serialize();
}

std::wstring ParseStringPacket(const Packet& pkt) {
    if (pkt.payload.empty()) return L"";
    return utils::FromUtf8(std::string((const char*)pkt.payload.data(), pkt.payload.size()));
}
