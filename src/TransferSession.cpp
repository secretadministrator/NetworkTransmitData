#include "TransferSession.h"
#include "FileScanner.h"
#include "Manifest.h"
#include "TransferPlanner.h"
#include "ExcludeRules.h"
#include "TransferProtocol.h"
#include "ResumeManager.h"
#include "ProgressTracker.h"
#include "ReportGenerator.h"
#include "Utils.h"
#include "AppConfig.h"
#include <bcrypt.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <vector>
#include <filesystem>
#include <cstring>
#include <climits>
#include <unordered_map>

namespace fs = std::filesystem;

static constexpr size_t MAX_PAYLOAD_BYTES = 64 * 1024 * 1024; // 64 MB
static constexpr DWORD HANDSHAKE_TIMEOUT_MS = 30000;

TransferSession::TransferSession() {}
TransferSession::~TransferSession() { Stop(); }

void TransferSession::Log(const std::wstring& msg) {
    if (m_logCb) m_logCb(msg);
}

void TransferSession::ReportProgress() {
    if (m_progressCb) m_progressCb(m_stats);
}

void TransferSession::NotifyDone(TransferResultCode code, const std::wstring& message) {
    if (!m_doneCb) return;
    TransferResult result;
    result.code = code;
    result.role = m_role == SessionRole::SENDER
        ? TransferRole::SENDER
        : TransferRole::RECEIVER;
    result.message = message;
    result.socketError = m_lastSocketError.load();
    result.resumable = m_stats.resumable;
    result.stats = m_stats;
    m_doneCb(result);
}

TransferResult TransferSession::FinalizeResult(TransferResult result) {
    result.socketError = m_lastSocketError.load();
    result.resumable = m_stats.resumable;
    result.stats = m_stats;
    result.role = m_role == SessionRole::SENDER
        ? TransferRole::SENDER
        : TransferRole::RECEIVER;
    return result;
}

bool TransferSession::TryGetFileSize(const std::wstring& path, int64_t& size) {
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
        return false;
    LARGE_INTEGER li;
    li.LowPart = info.nFileSizeLow;
    li.HighPart = info.nFileSizeHigh;
    size = li.QuadPart;
    return true;
}

bool TransferSession::ValidateResumeEntry(
    PlanEntry& entry,
    const std::wstring& targetRoot)
{
    if (entry.offset <= 0) {
        entry.offset = 0;
        entry.resumeHash.clear();
        return true;
    }

    std::wstring partPath =
        targetRoot + L"\\" + entry.relativePath + L".dtpart";

    int64_t actualSize = 0;

    if (!TryGetFileSize(partPath, actualSize)) {
        entry.offset = 0;
        entry.resumeHash.clear();
        return true;
    }

    if (actualSize != entry.offset ||
        actualSize > entry.size) {

        entry.offset = 0;
        entry.resumeHash.clear();

        HANDLE file = CreateFileW(
            partPath.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (file == INVALID_HANDLE_VALUE) {
            return false;
        }

        CloseHandle(file);
    }

    return true;
}

bool TransferSession::StartAsSender(const std::wstring& sourceDir, const std::wstring& peerIP,
    int port, const std::wstring& pairingCode)
{
    if (m_running) return false;
    if (m_workerThread.joinable())
        m_workerThread.join();
    m_stats = TransferStats{};
    m_role = SessionRole::SENDER;
    m_running = true;
    m_workerThread = std::thread(&TransferSession::SenderWorker, this,
        sourceDir, peerIP, port, pairingCode);
    return true;
}

bool TransferSession::StartAsReceiver(const std::wstring& targetDir, int port,
    const std::wstring& expectedPairingCode, TransferMode mode)
{
    if (m_running) return false;
    if (m_workerThread.joinable())
        m_workerThread.join();
    m_stats = TransferStats{};
    m_role = SessionRole::RECEIVER;
    m_running = true;
    m_workerThread = std::thread(&TransferSession::ReceiverWorker, this,
        targetDir, port, expectedPairingCode, mode);
    return true;
}

void TransferSession::Stop() {
    m_running = false;
    {
        std::lock_guard<std::mutex> lock(m_sockMutex);
        if (m_sock != INVALID_SOCKET) {
            shutdown(m_sock, SD_BOTH);
            closesocket(m_sock);
            m_sock = INVALID_SOCKET;
        }
        if (m_listenSock != INVALID_SOCKET) {
            closesocket(m_listenSock);
            m_listenSock = INVALID_SOCKET;
        }
    }
    if (m_workerThread.joinable())
        m_workerThread.join();
}

static TransferResult MakeResult(TransferResultCode code, const std::wstring& msg) {
    TransferResult r;
    r.code = code;
    r.message = msg;
    r.socketError = 0;
    r.resumable = false;
    return r;
}

// ── Temporary / hard socket error helpers ──

bool TransferSession::IsTemporarySocketError(int error)
{
    return error == WSAETIMEDOUT ||
           error == WSAEWOULDBLOCK ||
           error == WSAEINTR;
}

// ── Retryable send (with send mutex, partial-send safe) ──

IoResult TransferSession::SendAllLocked(
    SOCKET sock,
    const uint8_t* data,
    size_t length)
{
    size_t sentTotal = 0;
    int timeoutCount = 0;
    ULONGLONG lastProgress = GetTickCount64();

    while (sentTotal < length) {
        if (!m_running) {
            return {IoStatus::Cancelled, 0};
        }

        size_t remaining = length - sentTotal;
        int requestLength =
            remaining > static_cast<size_t>(INT_MAX)
            ? INT_MAX
            : static_cast<int>(remaining);

        int sent = send(
            sock,
            reinterpret_cast<const char*>(data + sentTotal),
            requestLength,
            0);

        if (sent > 0) {
            sentTotal += static_cast<size_t>(sent);

            ULONGLONG now = GetTickCount64();
            lastProgress = now;
            m_lastSendProgressTick.store(now);
            timeoutCount = 0;
            continue;
        }

        if (sent == 0) {
            IoResult r{IoStatus::PeerClosed, 0};
            MarkConnectionLost(sock, r, L"\u53d1\u9001\u65f6\u5bf9\u7aef\u5173\u95ed\u8fde\u63a5");
            return r;
        }

        int error = WSAGetLastError();

        if (!m_running) {
            return {IoStatus::Cancelled, error};
        }

        if (IsTemporarySocketError(error)) {
            ++timeoutCount;

            ULONGLONG now = GetTickCount64();

            if (now - lastProgress >= CONNECTION_IDLE_MS ||
                timeoutCount >= MAX_TIMEOUT_RETRIES) {
                IoResult r{IoStatus::IdleTimeout, error};
                MarkConnectionLost(sock, r, L"\u53d1\u9001\u8d85\u65f6");
                return r;
            }

            continue;
        }

        IoResult r{IoStatus::SocketError, error};
        MarkConnectionLost(sock, r, L"\u53d1\u9001 Socket I/O \u9519\u8bef");
        return r;
    }

    return {IoStatus::Ok, 0};
}

IoResult TransferSession::RecvExact(
    SOCKET sock,
    uint8_t* data,
    size_t length)
{
    size_t receivedTotal = 0;
    int timeoutCount = 0;
    ULONGLONG lastProgress = GetTickCount64();

    while (receivedTotal < length) {
        if (!m_running) {
            return {IoStatus::Cancelled, 0};
        }

        size_t remaining = length - receivedTotal;
        int requestLength =
            remaining > static_cast<size_t>(INT_MAX)
            ? INT_MAX
            : static_cast<int>(remaining);

        int received = recv(
            sock,
            reinterpret_cast<char*>(data + receivedTotal),
            requestLength,
            0);

        if (received > 0) {
            receivedTotal += static_cast<size_t>(received);

            ULONGLONG now = GetTickCount64();
            lastProgress = now;
            m_lastRecvProgressTick.store(now);
            timeoutCount = 0;
            continue;
        }

        if (received == 0) {
            IoResult r{IoStatus::PeerClosed, 0};
            MarkConnectionLost(sock, r, L"\u63a5\u6536\u65f6\u5bf9\u7aef\u5173\u95ed\u8fde\u63a5");
            return r;
        }

        int error = WSAGetLastError();

        if (!m_running) {
            return {IoStatus::Cancelled, error};
        }

        if (IsTemporarySocketError(error)) {
            ++timeoutCount;

            ULONGLONG now = GetTickCount64();

            if (now - lastProgress >= CONNECTION_IDLE_MS ||
                timeoutCount >= MAX_TIMEOUT_RETRIES) {
                IoResult r{IoStatus::IdleTimeout, error};
                MarkConnectionLost(sock, r, L"\u63a5\u6536\u8d85\u65f6");
                return r;
            }

            continue;
        }

        IoResult r{IoStatus::SocketError, error};
        MarkConnectionLost(sock, r, L"\u63a5\u6536 Socket I/O \u9519\u8bef");
        return r;
    }

    return {IoStatus::Ok, 0};
}

// ── Packet send with mutex ──

IoResult TransferSession::SendPacketResult(
    SOCKET sock,
    uint8_t type,
    const uint8_t* payload,
    size_t payloadSize)
{
    if (payloadSize > MAX_PAYLOAD_BYTES) {
        return {IoStatus::ProtocolError, 0};
    }

    std::lock_guard<std::mutex> lock(m_sendMutex);

    uint32_t length = static_cast<uint32_t>(payloadSize);

    uint8_t header[5] = {};
    header[0] = type;
    header[1] = static_cast<uint8_t>(length & 0xFF);
    header[2] = static_cast<uint8_t>((length >> 8) & 0xFF);
    header[3] = static_cast<uint8_t>((length >> 16) & 0xFF);
    header[4] = static_cast<uint8_t>((length >> 24) & 0xFF);

    IoResult result = SendAllLocked(sock, header, sizeof(header));
    if (!result.IsOk()) {
        return result;
    }

    if (payloadSize > 0) {
        result = SendAllLocked(sock, payload, payloadSize);
    }

    return result;
}

// ── Raw packet receive (no heartbeat filtering) ──

IoResult TransferSession::RecvRawPacket(
    SOCKET sock,
    uint8_t& type,
    std::vector<uint8_t>& payload)
{
    uint8_t header[5] = {};

    IoResult result = RecvExact(sock, header, sizeof(header));
    if (!result.IsOk()) {
        return result;
    }

    type = header[0];

    uint32_t length =
        static_cast<uint32_t>(header[1]) |
        (static_cast<uint32_t>(header[2]) << 8) |
        (static_cast<uint32_t>(header[3]) << 16) |
        (static_cast<uint32_t>(header[4]) << 24);

    if (length > MAX_PAYLOAD_BYTES) {
        return {IoStatus::ProtocolError, 0};
    }

    payload.resize(length);

    if (length > 0) {
        result = RecvExact(sock, payload.data(), length);
    }

    return result;
}

// ── Business packet receive (reads from queue when receive loop active) ──

IoResult TransferSession::RecvPacketResult(
    SOCKET sock,
    uint8_t& type,
    std::vector<uint8_t>& payload)
{
    if (m_receiveLoopRunning.load()) {
        std::unique_lock<std::mutex> lock(m_packetQueueMutex);

        while (m_packetQueue.empty()) {
            if (!m_running || m_connectionLost) {
                return {IoStatus::Cancelled, 0};
            }

            m_packetQueueCv.wait_for(
                lock,
                std::chrono::milliseconds(1000));

            if (m_connectionLost) {
                return {IoStatus::SocketError,
                        m_lastSocketError.load()};
            }

            if (!m_running) {
                return {IoStatus::Cancelled, 0};
            }
        }

        ReceivedPacket pkt = std::move(m_packetQueue.front());
        m_packetQueue.pop_front();
        lock.unlock();

        type = pkt.type;
        payload = std::move(pkt.payload);
        m_lastPeerActivityTick.store(GetTickCount64());
        return {IoStatus::Ok, 0};
    }

    // Fallback: direct socket read (used before receive loop starts)
    for (;;) {
        IoResult result = RecvRawPacket(sock, type, payload);

        if (!result.IsOk()) {
            return result;
        }

        if (type == static_cast<uint8_t>(PacketType::HEARTBEAT)) {
            IoResult ackResult = SendPacketResult(
                sock,
                static_cast<uint8_t>(PacketType::HEARTBEAT_ACK),
                payload.empty() ? nullptr : payload.data(),
                payload.size());

            if (!ackResult.IsOk()) {
                return ackResult;
            }

            continue;
        }

        if (type == static_cast<uint8_t>(PacketType::HEARTBEAT_ACK)) {
            m_lastPeerActivityTick.store(GetTickCount64());
            continue;
        }

        m_lastPeerActivityTick.store(GetTickCount64());
        return {IoStatus::Ok, 0};
    }
}

// ── Legacy boolean send wrappers ──

bool TransferSession::SendPacket(SOCKET sock, uint8_t type, const std::vector<uint8_t>& payload) {
    const uint8_t* data = payload.empty() ? nullptr : payload.data();
    return SendPacket(sock, type, data, payload.size());
}

bool TransferSession::SendPacket(SOCKET sock, uint8_t type, const uint8_t* payload, size_t payloadSize) {
    return SendPacketResult(sock, type, payload, payloadSize).IsOk();
}

// ── Legacy boolean recv wrapper ──

bool TransferSession::RecvPacket(SOCKET sock, uint8_t& type, std::vector<uint8_t>& payload) {
    IoResult result = RecvPacketResult(sock, type, payload);
    return result.IsOk();
}

bool TransferSession::SendStringPacket(SOCKET sock, uint8_t type, const std::wstring& str) {
    std::string utf8 = utils::ToUtf8(str);
    std::vector<uint8_t> payload(utf8.begin(), utf8.end());
    return SendPacket(sock, type, payload);
}

std::wstring TransferSession::RecvStringPacket(SOCKET sock, uint8_t expectedType) {
    uint8_t type = 0;
    std::vector<uint8_t> payload;
    if (!RecvPacket(sock, type, payload)) return L"";
    if (type != expectedType) return L"";
    return utils::FromUtf8(std::string((const char*)payload.data(), payload.size()));
}

IoResult TransferSession::SendStringPacketResult(
    SOCKET sock,
    uint8_t type,
    const std::wstring& text)
{
    std::string utf8 = utils::ToUtf8(text);
    return SendPacketResult(
        sock,
        type,
        reinterpret_cast<const uint8_t*>(utf8.data()),
        utf8.size());
}

IoResult TransferSession::RecvStringPacketResult(
    SOCKET sock,
    uint8_t expectedType,
    std::wstring& output)
{
    output.clear();

    uint8_t actualType = 0;
    std::vector<uint8_t> payload;

    IoResult result = RecvPacketResult(sock, actualType, payload);
    if (!result.IsOk()) {
        return result;
    }

    if (actualType != expectedType) {
        return {IoStatus::ProtocolError, 0};
    }

    output = utils::FromUtf8(
        std::string(
            reinterpret_cast<const char*>(payload.data()),
            payload.size()));

    return {IoStatus::Ok, 0};
}

TransferResult TransferSession::MakeIoFailureResult(
    const IoResult& io,
    const std::wstring& operation)
{
    switch (io.status) {
    case IoStatus::Cancelled:
        return MakeResult(TransferResultCode::Cancelled, L"\u4f20\u8f93\u5df2\u53d6\u6d88");

    case IoStatus::PeerClosed:
        return MakeResult(
            TransferResultCode::ConnectionLost,
            operation + L"\uff1a\u5bf9\u7aef\u5df2\u5173\u95ed\u8fde\u63a5");

    case IoStatus::IdleTimeout:
        return MakeResult(
            TransferResultCode::ConnectionLost,
            operation + L"\uff1a\u8fde\u7eed 90 \u79d2\u6ca1\u6709\u7f51\u7edc\u54cd\u5e94");

    case IoStatus::SocketError:
        return MakeResult(
            TransferResultCode::ConnectionLost,
            operation + L"\uff1a\u7f51\u7edc\u9519\u8bef " + std::to_wstring(io.wsaError));

    case IoStatus::ProtocolError:
        return MakeResult(
            TransferResultCode::ProtocolError,
            operation + L"\uff1a\u534f\u8bae\u6570\u636e\u9519\u8bef");

    case IoStatus::Ok:
    default:
        return MakeResult(
            TransferResultCode::InternalError,
            operation + L"\uff1a\u672a\u77e5\u9519\u8bef");
    }
}

static bool SetSocketTimeouts(SOCKET sock, DWORD timeoutMs, int& outError) {
    outError = 0;
    DWORD timeout = timeoutMs;

    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
            reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == SOCKET_ERROR) {
        outError = WSAGetLastError();
        return false;
    }

    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
            reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == SOCKET_ERROR) {
        outError = WSAGetLastError();
        return false;
    }

    return true;
}

static void EnableLowLatencyTcp(SOCKET sock) {
    BOOL noDelay = TRUE;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&noDelay, sizeof(noDelay));
}

static bool EnableTcpKeepAlive(SOCKET sock)
{
    tcp_keepalive settings = {};
    settings.onoff = 1;
    settings.keepalivetime = 30000;
    settings.keepaliveinterval = 5000;

    DWORD bytesReturned = 0;

    return WSAIoctl(
        sock,
        SIO_KEEPALIVE_VALS,
        &settings,
        sizeof(settings),
        nullptr,
        0,
        &bytesReturned,
        nullptr,
        nullptr) != SOCKET_ERROR;
}

// ── Heartbeat ──

void TransferSession::StartHeartbeat(SOCKET sock)
{
    StopHeartbeat();

    const ULONGLONG now = GetTickCount64();

    m_lastSendProgressTick.store(now);
    m_lastRecvProgressTick.store(now);
    m_lastPeerActivityTick.store(now);

    m_lastSocketError.store(0);
    m_connectionLost.store(false);
    m_heartbeatSequence.store(0);
    m_heartbeatRunning.store(true);

    m_heartbeatThread = std::thread([this, sock]() {
        while (m_heartbeatRunning &&
               m_running &&
               !m_connectionLost) {

            for (int i = 0; i < 10; ++i) {
                if (!m_heartbeatRunning ||
                    !m_running ||
                    m_connectionLost) {
                    return;
                }

                Sleep(500);
            }

            ULONGLONG nowTick = GetTickCount64();

            // Use max of local send/recv progress as idle indicator.
            // Active file transfer prevents false idle timeout.
            const ULONGLONG lastSend =
                m_lastSendProgressTick.load();
            const ULONGLONG lastRecv =
                m_lastRecvProgressTick.load();
            const ULONGLONG lastIo =
                (lastSend > lastRecv) ? lastSend : lastRecv;

            if (nowTick - lastIo >= CONNECTION_IDLE_MS) {
                IoResult timeout{
                    IoStatus::IdleTimeout,
                    WSAETIMEDOUT
                };

                MarkConnectionLost(
                    sock,
                    timeout,
                    L"\u8fde\u63a5\u8fde\u7eed 90 \u79d2\u6ca1\u6709\u6570\u636e\u8fdb\u5c55");

                return;
            }

            ULONGLONG lastSendOnly =
                m_lastSendProgressTick.load();

            if (nowTick - lastSendOnly < HEARTBEAT_INTERVAL_MS) {
                continue;
            }

            uint64_t sequence =
                ++m_heartbeatSequence;

            uint8_t payload[sizeof(sequence)] = {};
            memcpy(payload, &sequence, sizeof(sequence));

            IoResult result = SendPacketResult(
                sock,
                static_cast<uint8_t>(PacketType::HEARTBEAT),
                payload,
                sizeof(payload));

            if (!result.IsOk()) {
                MarkConnectionLost(
                    sock,
                    result,
                    L"\u53d1\u9001\u5fc3\u8df3\u5305");

                return;
            }
        }
    });
}

void TransferSession::StopHeartbeat()
{
    m_heartbeatRunning.store(false);

    if (m_heartbeatThread.joinable() &&
        m_heartbeatThread.get_id() != std::this_thread::get_id()) {
        m_heartbeatThread.join();
    }
}

// ── Receive loop (dedicated thread) ──

void TransferSession::StartReceiveLoop(SOCKET sock)
{
    StopReceiveLoop();

    m_receiveLoopRunning.store(true);

    {
        std::lock_guard<std::mutex> lock(m_packetQueueMutex);
        m_packetQueue.clear();
    }

    m_receiveThread = std::thread(
        &TransferSession::ReceiveLoop,
        this,
        sock);
}

void TransferSession::StopReceiveLoop()
{
    m_receiveLoopRunning.store(false);

    {
        std::lock_guard<std::mutex> lock(m_packetQueueMutex);
        m_packetQueueCv.notify_all();
    }

    if (m_receiveThread.joinable() &&
        m_receiveThread.get_id() != std::this_thread::get_id()) {
        m_receiveThread.join();
    }
}

void TransferSession::ReceiveLoop(SOCKET sock)
{
    while (m_receiveLoopRunning &&
           m_running &&
           !m_connectionLost) {

        uint8_t type = 0;
        std::vector<uint8_t> payload;
        IoResult result = RecvRawPacket(sock, type, payload);

        if (!result.IsOk()) {
            MarkConnectionLost(
                sock,
                result,
                L"\u63a5\u6536\u7ebf\u7a0b\u8bfb\u53d6\u6570\u636e\u5931\u8d25");
            break;
        }

        if (type == static_cast<uint8_t>(PacketType::HEARTBEAT)) {
            IoResult ackResult = SendPacketResult(
                sock,
                static_cast<uint8_t>(PacketType::HEARTBEAT_ACK),
                payload.empty() ? nullptr : payload.data(),
                payload.size());

            if (!ackResult.IsOk()) {
                MarkConnectionLost(
                    sock,
                    ackResult,
                    L"\u63a5\u6536\u7ebf\u7a0b\u56de\u590d\u5fc3\u8df3 ACK \u5931\u8d25");
                break;
            }

            continue;
        }

        if (type == static_cast<uint8_t>(PacketType::HEARTBEAT_ACK)) {
            m_lastPeerActivityTick.store(GetTickCount64());
            continue;
        }

        // Business packet: push to queue
        {
            std::lock_guard<std::mutex> lock(m_packetQueueMutex);
            m_packetQueue.push_back({type, std::move(payload)});
        }

        m_packetQueueCv.notify_one();
        m_lastRecvProgressTick.store(GetTickCount64());
    }
}

void TransferSession::MarkConnectionLost(
    SOCKET sock,
    const IoResult& result,
    const std::wstring& operation)
{
    if (!m_running) {
        return;
    }

    bool alreadyLost = m_connectionLost.exchange(true);
    if (alreadyLost) {
        return;
    }

    m_lastSocketError.store(result.wsaError);

    std::wstring message =
        L"\u8fde\u63a5\u5df2\u4e2d\u65ad\uff1a" + operation;

    if (result.status == IoStatus::IdleTimeout) {
        message += L"\uff0c\u8fde\u7eed 90 \u79d2\u6ca1\u6709\u7f51\u7edc\u54cd\u5e94";
    } else if (result.status == IoStatus::PeerClosed) {
        message += L"\uff0c\u5bf9\u7aef\u5df2\u5173\u95ed\u8fde\u63a5";
    } else if (result.wsaError != 0) {
        message += L"\uff0cWinsock \u9519\u8bef\u7801 "
                 + std::to_wstring(result.wsaError);
    }

    Log(message);

    shutdown(sock, SD_BOTH);
}

struct Sha256Context {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
};

static void CloseSha256(Sha256Context& ctx) {
    if (ctx.hash) {
        BCryptDestroyHash(ctx.hash);
        ctx.hash = NULL;
    }
    if (ctx.alg) {
        BCryptCloseAlgorithmProvider(ctx.alg, 0);
        ctx.alg = NULL;
    }
}

static bool StartSha256(Sha256Context& ctx) {
    CloseSha256(ctx);
    if (BCryptOpenAlgorithmProvider(&ctx.alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0)
        return false;
    if (BCryptCreateHash(ctx.alg, &ctx.hash, NULL, 0, NULL, 0, 0) < 0) {
        CloseSha256(ctx);
        return false;
    }
    return true;
}

static bool UpdateSha256(Sha256Context& ctx, const uint8_t* data, DWORD len) {
    if (!ctx.hash) return false;
    if (len == 0) return true;
    return BCryptHashData(ctx.hash, (PUCHAR)data, len, 0) >= 0;
}

static bool UpdateSha256FromFile(Sha256Context& ctx, const std::wstring& path, int64_t bytes) {
    if (bytes < 0) return false;
    std::wstring normPath = utils::NormalizePath(path);
    HANDLE hFile = CreateFileW(normPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    constexpr DWORD BUF_SIZE = 65536;
    std::vector<uint8_t> buf(BUF_SIZE);
    int64_t remaining = bytes;
    bool ok = true;
    while (remaining > 0) {
        DWORD toRead = (remaining > (int64_t)BUF_SIZE) ? BUF_SIZE : (DWORD)remaining;
        DWORD readNow = 0;
        if (!ReadFile(hFile, buf.data(), toRead, &readNow, NULL) || readNow == 0) {
            ok = false;
            break;
        }
        if (!UpdateSha256(ctx, buf.data(), readNow)) {
            ok = false;
            break;
        }
        remaining -= readNow;
    }

    CloseHandle(hFile);
    return ok && remaining == 0;
}

static std::string FinishSha256(Sha256Context& ctx) {
    std::string result;
    if (ctx.hash) {
        uint8_t hashValue[32];
        if (BCryptFinishHash(ctx.hash, hashValue, 32, 0) >= 0)
            result = utils::BytesToHex(hashValue, 32);
    }
    CloseSha256(ctx);
    return result;
}

static void RecalculatePlanTotals(TransferPlanner::Plan& plan) {
    plan.totalBytes = 0;
    plan.totalFiles = 0;
    plan.skipFiles = 0;
    for (const auto& e : plan.entries) {
        if (e.action == FileAction::TRANSFER || e.action == FileAction::OVERWRITE) {
            int64_t remaining = e.size - e.offset;
            if (remaining < 0) remaining = 0;
            plan.totalBytes += remaining;
            plan.totalFiles++;
        } else if (e.action == FileAction::SKIP) {
            plan.skipFiles++;
        }
    }
}

// ============== SENDER ==============

void TransferSession::SenderWorker(const std::wstring& sourceDir, const std::wstring& peerIP,
    int port, const std::wstring& pairingCode)
{
    StopHeartbeat();

    TransferResult result;
    try {
        result = InnerSenderWorker(sourceDir, peerIP, port, pairingCode);
    } catch (const std::exception& e) {
        result.code = TransferResultCode::InternalError;
        result.message = L"\u53d1\u9001\u7ebf\u7a0b\u5f02\u5e38";
        result.stats = m_stats;
    } catch (...) {
        result.code = TransferResultCode::InternalError;
        result.message = L"\u53d1\u9001\u7ebf\u7a0b\u53d1\u751f\u672a\u77e5\u5f02\u5e38";
        result.stats = m_stats;
    }

    StopHeartbeat();

    std::lock_guard<std::mutex> lock(m_sockMutex);
    if (m_sock != INVALID_SOCKET) {
        if (m_running) shutdown(m_sock, SD_BOTH);
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
    }
    if (m_listenSock != INVALID_SOCKET) {
        closesocket(m_listenSock);
        m_listenSock = INVALID_SOCKET;
    }
    m_running = false;
    if (m_doneCb) m_doneCb(result);
}

TransferResult TransferSession::InnerSenderWorker(const std::wstring& sourceDir, const std::wstring& peerIP,
    int port, const std::wstring& pairingCode)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    Log(L"\u8fde\u63a5\u5230 " + peerIP + L":" + std::to_wstring(port) + L"...");

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        Log(L"\u521b\u5efa socket \u5931\u8d25");
        WSACleanup(); return MakeResult(TransferResultCode::FileError, L"\u521b\u5efa Socket \u5931\u8d25");
    }
    m_sock = sock;
    EnableLowLatencyTcp(sock);
    {
        int err = 0;
        if (!SetSocketTimeouts(sock, HANDSHAKE_TIMEOUT_MS, err)) {
            Log(L"\u8bbe\u7f6e\u7f51\u7edc\u8d85\u65f6\u5931\u8d25\uff0c\u9519\u8bef\u7801: " + std::to_wstring(err));
            closesocket(sock); m_sock = INVALID_SOCKET;
            WSACleanup();
            return MakeResult(TransferResultCode::InternalError, L"\u8bbe\u7f6e\u7f51\u7edc\u8d85\u65f6\u5931\u8d25");
        }
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    std::string ipA = utils::ToUtf8(peerIP);
    if (InetPtonA(AF_INET, ipA.c_str(), &addr.sin_addr) != 1) {
        Log(L"\u5bf9\u65b9 IP \u5730\u5740\u65e0\u6548: " + peerIP);
        closesocket(sock); m_sock = INVALID_SOCKET;
        WSACleanup(); return MakeResult(TransferResultCode::FileError, L"\u5bf9\u65b9 IP \u5730\u5740\u65e0\u6548");
    }

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        std::wstring errMsg = L"\u8fde\u63a5\u63a5\u6536\u7aef\u5931\u8d25\uff0c\u9519\u8bef\u7801: " + std::to_wstring(WSAGetLastError());
        Log(errMsg);
        closesocket(sock); m_sock = INVALID_SOCKET;
        WSACleanup(); return MakeResult(TransferResultCode::FileError, errMsg);
    }

    Log(L"\u5df2\u8fde\u63a5\uff0c\u6b63\u5728\u9a8c\u8bc1\u914d\u5bf9\u7801...");

    if (!SendStringPacket(sock, (uint8_t)PacketType::PAIRING_REQUEST, pairingCode)) {
        Log(L"\u53d1\u9001\u914d\u5bf9\u7801\u5931\u8d25");
        closesocket(sock); m_sock = INVALID_SOCKET;
        WSACleanup(); return MakeResult(TransferResultCode::ProtocolError, L"\u53d1\u9001\u914d\u5bf9\u7801\u5931\u8d25");
    }

    std::wstring resp = RecvStringPacket(sock, (uint8_t)PacketType::PAIRING_RESPONSE);
    if (resp != L"OK") {
        Log(L"\u914d\u5bf9\u7801\u9a8c\u8bc1\u5931\u8d25");
        closesocket(sock); m_sock = INVALID_SOCKET;
        WSACleanup(); return MakeResult(TransferResultCode::ProtocolError, L"\u914d\u5bf9\u7801\u9a8c\u8bc1\u5931\u8d25");
    }

    {
        int err = 0;
        if (!SetSocketTimeouts(sock, IO_WAIT_TIMEOUT_MS, err)) {
            std::wstring msg = L"\u8bbe\u7f6e\u4f20\u8f93\u8d85\u65f6\u5931\u8d25\uff0c\u9519\u8bef\u7801: " + std::to_wstring(err);
            Log(msg);
            closesocket(sock); m_sock = INVALID_SOCKET;
            WSACleanup();
            return MakeResult(TransferResultCode::InternalError, msg);
        }
    }

    EnableTcpKeepAlive(sock);
    StartHeartbeat(sock);
    StartReceiveLoop(sock);

    Log(L"\u914d\u5bf9\u7801\u9a8c\u8bc1\u6210\u529f\uff01");
    Log(L"\u6b63\u5728\u626b\u63cf\u6587\u4ef6...");

    FileScanner scanner;
    ExcludeRules excludeRules;
    scanner.SetExcludeCallback([&](const std::wstring& path) { return excludeRules.IsExcluded(path); });

    auto files = scanner.ScanDirectory(sourceDir);
    if (files.empty()) {
        Log(L"\u6e90\u76ee\u5f55\u4e2d\u6ca1\u6709\u6587\u4ef6\uff0c\u5c06\u53d1\u9001\u7a7a\u6587\u4ef6\u6e05\u5355");
    }

    if (!m_running) {
        closesocket(sock); m_sock = INVALID_SOCKET;
        WSACleanup(); return MakeResult(TransferResultCode::Cancelled, L"\u4f20\u8f93\u5df2\u53d6\u6d88");
    }

    Manifest manifest(files);
    Log(L"\u5171 " + std::to_wstring(files.size()) + L" \u4e2a\u6587\u4ef6\uff0c"
        + utils::FormatBytes(manifest.GetTotalSize()));

    std::string manifestJson = manifest.ToJSON();
    std::vector<uint8_t> manifestPayload(manifestJson.begin(), manifestJson.end());
    {
        IoResult io = SendPacketResult(
            sock,
            static_cast<uint8_t>(PacketType::MANIFEST),
            manifestPayload.data(),
            manifestPayload.size());

        if (!io.IsOk()) {
            if (io.status != IoStatus::Cancelled) {
                m_stats.resumable = true;
            }
            closesocket(sock); m_sock = INVALID_SOCKET;
            WSACleanup();
            return MakeIoFailureResult(io, L"\u53d1\u9001\u6587\u4ef6\u6e05\u5355\u5931\u8d25");
        }
    }

    Log(L"\u5df2\u53d1\u9001\u6587\u4ef6\u6e05\u5355\uff0c\u7b49\u5f85\u63a5\u6536\u7aef\u8ba1\u5212...");

    std::wstring planStr;
    {
        IoResult io = RecvStringPacketResult(
            sock,
            static_cast<uint8_t>(PacketType::TRANSFER_PLAN),
            planStr);

        if (!io.IsOk()) {
            if (io.status != IoStatus::Cancelled) {
                m_stats.resumable = true;
            }
            closesocket(sock); m_sock = INVALID_SOCKET;
            WSACleanup();
            return MakeIoFailureResult(io, L"\u63a5\u6536\u4f20\u8f93\u8ba1\u5212\u5931\u8d25");
        }
    }

    std::string planUtf8 = utils::ToUtf8(planStr);
    TransferPlanner::Plan plan = TransferPlanner::ParsePlanString(planUtf8);
    for (auto& entry : plan.entries) {
        if (entry.action == FileAction::SKIP && !entry.resumeHash.empty()) {
            std::wstring fullPath = sourceDir + L"\\" + entry.relativePath;
            std::string sourceHash = utils::ComputeSHA256(fullPath);
            if (!sourceHash.empty() && sourceHash == entry.resumeHash) {
                Log(L"\u5df2\u786e\u8ba4\u8df3\u8fc7(\u54c8\u5e0c\u5339\u914d): " + entry.relativePath);
            } else {
                Log(L"\u540c\u540d\u540c\u5927\u5c0f\u6587\u4ef6\u54c8\u5e0c\u4e0d\u5339\u914d\uff0c\u5c06\u4f20\u8f93: " + entry.relativePath);
                entry.action = FileAction::TRANSFER;
                entry.offset = 0;
                entry.resumeHash.clear();
            }
        }

        if ((entry.action == FileAction::TRANSFER || entry.action == FileAction::OVERWRITE)
                && entry.offset > 0) {
            std::wstring fullPath = sourceDir + L"\\" + entry.relativePath;
            std::string sourcePrefixHash = utils::ComputeSHA256(fullPath, entry.offset);
            if (sourcePrefixHash.empty() || entry.resumeHash.empty()
                    || sourcePrefixHash != entry.resumeHash) {
                Log(L"\u7eed\u4f20\u524d\u7f00\u6821\u9a8c\u5931\u8d25\uff0c\u5c06\u91cd\u65b0\u4f20\u8f93: " + entry.relativePath);
                entry.offset = 0;
                entry.resumeHash.clear();
            }
        }
    }
    RecalculatePlanTotals(plan);
    m_stats.totalBytes = plan.totalBytes;
    m_stats.totalFiles = plan.totalFiles;
    m_stats.skippedFiles = plan.skipFiles;

    Log(L"\u8ba1\u5212\u4f20\u8f93 " + std::to_wstring(plan.totalFiles) + L" \u4e2a\u6587\u4ef6\uff0c"
        + utils::FormatBytes(plan.totalBytes) + L"\uff0c\u8df3\u8fc7 "
        + std::to_wstring(plan.skipFiles) + L" \u4e2a");

    ProgressTracker progress;
    progress.Reset(plan.totalBytes);
    int completed = 0;
    TransferResult result;
    result.code = TransferResultCode::Success;
    result.message = L"\u4f20\u8f93\u5df2\u5b8c\u6210";

    for (const auto& entry : plan.entries) {
        if (!m_running) {
            result = MakeResult(TransferResultCode::Cancelled, L"\u4f20\u8f93\u5df2\u53d6\u6d88");
            break;
        }

        if (entry.action == FileAction::SKIP) {
            Log(L"\u5df2\u8df3\u8fc7: " + entry.relativePath);
            m_stats.currentFile = entry.relativePath;
            ReportProgress();
            continue;
        }

        if (entry.action != FileAction::TRANSFER && entry.action != FileAction::OVERWRITE)
            continue;

        std::wstring fullPath = sourceDir + L"\\" + entry.relativePath;
        int64_t fileSize = entry.size;
        int64_t offset = entry.offset;
        if (offset < 0 || offset > fileSize)
            offset = 0;

        m_stats.currentFile = entry.relativePath;
        ReportProgress();

        HANDLE hFile = CreateFileW(utils::NormalizePath(fullPath).c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
            Log(L"\u65e0\u6cd5\u6253\u5f00\u6587\u4ef6: " + entry.relativePath);
            m_stats.failedFiles++;
            IoResult io = SendStringPacketResult(
                sock,
                static_cast<uint8_t>(PacketType::ERROR_MSG),
                L"OPEN_FAILED: " + entry.relativePath);
            if (!io.IsOk()) {
                if (io.status == IoStatus::Cancelled) {
                    result = MakeResult(TransferResultCode::Cancelled, L"\u4f20\u8f93\u5df2\u53d6\u6d88");
                } else {
                    result = MakeIoFailureResult(io, L"\u53d1\u9001\u9519\u8bef\u4fe1\u606f\u5931\u8d25");
                    m_stats.interruptedFiles++;
                    m_stats.resumable = true;
                }
                break;
            }
            continue;
        }

        std::wstring headerStr = entry.relativePath + L"\n" + std::to_wstring(fileSize)
            + L"\n" + std::to_wstring(offset);
        {
            IoResult io = SendStringPacketResult(
                sock,
                static_cast<uint8_t>(PacketType::FILE_HEADER),
                headerStr);

            if (!io.IsOk()) {
                CloseHandle(hFile);
                if (io.status != IoStatus::Cancelled) {
                    m_stats.interruptedFiles++;
                    m_stats.resumable = true;
                }
                result = MakeIoFailureResult(io, L"\u53d1\u9001\u6587\u4ef6\u5934\u5931\u8d25");
                break;
            }
        }

        // Initialize incremental SHA-256
        Sha256Context hashCtx;
        bool hashOk = StartSha256(hashCtx);
        if (!hashOk) {
            Log(L"\u521d\u59cb\u5316 SHA-256 \u5931\u8d25: " + entry.relativePath);
            m_stats.failedFiles++;
            {
                IoResult io = SendStringPacketResult(
                    sock,
                    static_cast<uint8_t>(PacketType::ERROR_MSG),
                    L"HASH_INIT_FAILED");
                if (!io.IsOk()) break;
            }
            {
                IoResult io = SendStringPacketResult(
                    sock,
                    static_cast<uint8_t>(PacketType::FILE_DONE),
                    entry.relativePath);
                if (!io.IsOk()) break;
            }
            CloseHandle(hFile);
            continue;
        }
        if (offset > 0 && !UpdateSha256FromFile(hashCtx, fullPath, offset)) {
            Log(L"\u8bfb\u53d6\u7eed\u4f20\u524d\u7f00\u5931\u8d25(\u54c8\u5e0c): " + entry.relativePath);
            hashOk = false;
        }

        bool readFileFailed = false;
        if (offset > 0) {
            LARGE_INTEGER offsetPos;
            offsetPos.QuadPart = offset;
            if (!SetFilePointerEx(hFile, offsetPos, NULL, FILE_BEGIN))
                readFileFailed = true;
        }

        constexpr DWORD CHUNK = 256 * 1024;
        std::vector<uint8_t> buf(CHUNK);
        DWORD bytesRead = 0;
        int64_t bytesRemaining = fileSize - offset;

        bool chunkSendFailed = false;

        while (!readFileFailed && bytesRemaining > 0) {
            DWORD toRead = (bytesRemaining > (int64_t)CHUNK) ? CHUNK : (DWORD)bytesRemaining;
            if (!ReadFile(hFile, buf.data(), toRead, &bytesRead, NULL) || bytesRead == 0) {
                readFileFailed = true;
                break;
            }

            if (hashOk && !UpdateSha256(hashCtx, buf.data(), bytesRead)) {
                Log(L"SHA-256 \u8ba1\u7b97\u5931\u8d25: " + entry.relativePath);
                hashOk = false;
            }

            {
                IoResult io = SendPacketResult(
                    sock,
                    static_cast<uint8_t>(PacketType::FILE_CHUNK),
                    buf.data(),
                    bytesRead);

                if (!io.IsOk()) {
                    Log(L"\u53d1\u9001\u6570\u636e\u5931\u8d25: " + entry.relativePath);
                    chunkSendFailed = true;
                    break;
                }
            }

            bytesRemaining -= bytesRead;
            if (!m_running) break;

            progress.AddTransferred(bytesRead);
            m_stats.transferredBytes = progress.GetTransferred();
            m_stats.speedBytesPerSec = progress.GetSpeed();
            m_stats.currentFile = entry.relativePath;
            ReportProgress();
        }

        CloseHandle(hFile);

        if (!m_running) {
            CloseSha256(hashCtx);
            result = MakeResult(TransferResultCode::Cancelled, L"\u4f20\u8f93\u5df2\u53d6\u6d88");
            break;
        }

        if (chunkSendFailed) {
            CloseSha256(hashCtx);
            if (m_connectionLost) {
                result = MakeResult(TransferResultCode::ConnectionLost, L"\u53d1\u9001\u6570\u636e\u65f6\u8fde\u63a5\u4e2d\u65ad");
                m_stats.interruptedFiles++;
                m_stats.resumable = true;
            } else {
                m_stats.failedFiles++;
                result = MakeResult(TransferResultCode::FileError, L"\u53d1\u9001\u6570\u636e\u5931\u8d25");
            }
            break;
        }

        if (readFileFailed || bytesRemaining > 0) {
            Log(L"\u8bfb\u53d6\u6587\u4ef6\u5931\u8d25: " + entry.relativePath);
            m_stats.failedFiles++;
            {
                IoResult io = SendStringPacketResult(
                    sock,
                    static_cast<uint8_t>(PacketType::ERROR_MSG),
                    L"READ_FAILED");
                if (!io.IsOk()) break;
            }
            {
                IoResult io = SendStringPacketResult(
                    sock,
                    static_cast<uint8_t>(PacketType::FILE_DONE),
                    entry.relativePath);
                if (!io.IsOk()) break;
            }
            CloseSha256(hashCtx);
            continue;
        }

        // Finalize SHA-256 and send FILE_HASH
        std::string sha256Hex = hashOk ? FinishSha256(hashCtx) : std::string();
        if (sha256Hex.empty()) {
            Log(L"\u8ba1\u7b97\u6587\u4ef6 SHA-256 \u5931\u8d25: " + entry.relativePath);
            m_stats.failedFiles++;
            {
                IoResult io = SendStringPacketResult(
                    sock,
                    static_cast<uint8_t>(PacketType::ERROR_MSG),
                    L"HASH_FAILED");
                if (!io.IsOk()) break;
            }
            {
                IoResult io = SendStringPacketResult(
                    sock,
                    static_cast<uint8_t>(PacketType::FILE_DONE),
                    entry.relativePath);
                if (!io.IsOk()) break;
            }
            continue;
        }
        {
            std::vector<uint8_t> hashPayload(sha256Hex.begin(), sha256Hex.end());
            IoResult io = SendPacketResult(
                sock,
                static_cast<uint8_t>(PacketType::FILE_HASH),
                hashPayload.data(),
                hashPayload.size());

            if (!io.IsOk()) {
                Log(L"\u53d1\u9001\u6587\u4ef6\u6821\u9a8c\u503c\u5931\u8d25: " + entry.relativePath);
                if (io.status == IoStatus::Cancelled) {
                    result = MakeResult(TransferResultCode::Cancelled, L"\u4f20\u8f93\u5df2\u53d6\u6d88");
                } else {
                    m_stats.interruptedFiles++;
                    m_stats.resumable = true;
                    result = MakeIoFailureResult(io, L"\u53d1\u9001\u6587\u4ef6\u6821\u9a8c\u503c\u5931\u8d25");
                }
                break;
            }
        }

        {
            IoResult io = SendStringPacketResult(
                sock,
                static_cast<uint8_t>(PacketType::FILE_DONE),
                entry.relativePath);

            if (!io.IsOk()) {
                Log(L"\u53d1\u9001\u6587\u4ef6\u5b8c\u6210\u4fe1\u53f7\u5931\u8d25");
                if (io.status == IoStatus::Cancelled) {
                    result = MakeResult(TransferResultCode::Cancelled, L"\u4f20\u8f93\u5df2\u53d6\u6d88");
                } else {
                    m_stats.interruptedFiles++;
                    m_stats.resumable = true;
                    result = MakeIoFailureResult(io, L"\u53d1\u9001 FILE_DONE \u5931\u8d25");
                }
                break;
            }
        }

        uint8_t ackType = 0;
        std::vector<uint8_t> ackPayload;
        {
            IoResult io = RecvPacketResult(sock, ackType, ackPayload);
            if (!io.IsOk() || ackType != (uint8_t)PacketType::FILE_DONE_ACK) {
                Log(L"\u63a5\u6536\u7aef\u9a8c\u8bc1\u54cd\u5e94\u5931\u8d25: " + entry.relativePath);
                if (io.status == IoStatus::Cancelled) {
                    result = MakeResult(TransferResultCode::Cancelled, L"\u4f20\u8f93\u5df2\u53d6\u6d88");
                } else {
                    m_stats.interruptedFiles++;
                    m_stats.resumable = true;
                    result = MakeIoFailureResult(io, L"\u63a5\u6536 FILE_DONE_ACK \u5931\u8d25");
                }
                break;
            }
        }
        std::wstring ack = utils::FromUtf8(std::string((const char*)ackPayload.data(), ackPayload.size()));
        size_t ackSep = ack.find(L'\n');
        std::wstring ackStatus = (ackSep == std::wstring::npos) ? ack : ack.substr(0, ackSep);
        std::wstring ackFile = (ackSep == std::wstring::npos) ? L"" : ack.substr(ackSep + 1);
        if (ackStatus != L"OK" || ackFile != entry.relativePath) {
            Log(L"\u63a5\u6536\u7aef\u9a8c\u8bc1\u5931\u8d25: " + entry.relativePath
                + L" (" + (ack.empty() ? L"\u65e0\u54cd\u5e94" : ack) + L")");
            m_stats.failedFiles++;
            SendStringPacketResult(sock,
                static_cast<uint8_t>(PacketType::ERROR_MSG),
                L"TRANSFER_ABORTED");
            result = MakeResult(TransferResultCode::FileError, L"\u63a5\u6536\u7aef\u9a8c\u8bc1\u5931\u8d25");
            break;
        } else {
            completed++;
            m_stats.completedFiles = completed;
            Log(L"\u5df2\u5b8c\u6210: " + entry.relativePath);
        }
        ReportProgress();
    }

    if (!m_running && result.code == TransferResultCode::Success) {
        result = MakeResult(TransferResultCode::Cancelled, L"\u4f20\u8f93\u5df2\u53d6\u6d88");
    }

    if (result.code == TransferResultCode::ConnectionLost) {
        result.message = L"\u8fde\u63a5\u5df2\u4e2d\u65ad\uff0c\u53ef\u4ee5\u91cd\u65b0\u8fde\u63a5\u7eed\u4f20";
        result.socketError = m_lastSocketError.load();
        result.resumable = true;
        Log(result.message);
    } else if (result.code == TransferResultCode::Cancelled) {
        Log(L"\u4f20\u8f93\u672a\u5b8c\u6574\u5b8c\u6210\uff0c\u5df2\u505c\u6b62\u4f1a\u8bdd");
        SendStringPacketResult(sock,
            static_cast<uint8_t>(PacketType::ERROR_MSG),
            L"TRANSFER_ABORTED");
    } else if (result.code == TransferResultCode::FileError || m_stats.failedFiles > 0) {
        result.code = TransferResultCode::FileError;
        result.message = L"\u90e8\u5206\u6587\u4ef6\u4f20\u8f93\u5931\u8d25";
        Log(L"\u4f20\u8f93\u672a\u5b8c\u6574\u5b8c\u6210\uff0c\u5df2\u505c\u6b62\u4f1a\u8bdd");
        SendStringPacketResult(sock,
            static_cast<uint8_t>(PacketType::ERROR_MSG),
            L"TRANSFER_ABORTED");
    } else {
        {
            IoResult io = SendStringPacketResult(
                sock,
                static_cast<uint8_t>(PacketType::DONE),
                L"");

            if (!io.IsOk()) {
                result = MakeIoFailureResult(io, L"\u53d1\u9001\u4f20\u8f93\u5b8c\u6210\u4fe1\u53f7\u5931\u8d25");
            } else {
                std::wstring doneAck;
                IoResult ioAck = RecvStringPacketResult(
                    sock,
                    static_cast<uint8_t>(PacketType::DONE_ACK),
                    doneAck);

                if (!ioAck.IsOk()) {
                    result = MakeIoFailureResult(ioAck, L"\u63a5\u6536\u4f20\u8f93\u5b8c\u6210\u786e\u8ba4\u5931\u8d25");
                } else if (doneAck != L"OK") {
                    result = MakeResult(TransferResultCode::FileError, L"\u672a\u6536\u5230\u63a5\u6536\u7aef\u7684\u4f20\u8f93\u5b8c\u6210\u786e\u8ba4");
                } else {
                    result = MakeResult(TransferResultCode::Success, L"\u4f20\u8f93\u5df2\u5b8c\u6210");
                }
            }
        }
    }

    m_stats.speedBytesPerSec = progress.GetSpeed();

    StopHeartbeat();

    if (sock != INVALID_SOCKET) {
        shutdown(sock, SD_BOTH);
    }

    StopReceiveLoop();

    result = FinalizeResult(std::move(result));

    std::wstring report = ReportGenerator::GenerateReport(m_stats, sourceDir, L"");
    ReportGenerator::SaveReport(report, utils::GetExecutableDir() + L"\\reports");

    if (sock != INVALID_SOCKET) {
        closesocket(sock);
    }
    m_sock = INVALID_SOCKET;
    WSACleanup();

    return result;
}

// ============== RECEIVER ==============

void TransferSession::ReceiverWorker(const std::wstring& targetDir, int port,
    const std::wstring& expectedPairingCode, TransferMode mode)
{
    StopHeartbeat();

    try {
        InnerReceiverWorker(targetDir, port, expectedPairingCode, mode);
    } catch (const std::exception& e) {
        NotifyDone(TransferResultCode::InternalError, L"\u63a5\u6536\u7ebf\u7a0b\u5f02\u5e38");
    } catch (...) {
        NotifyDone(TransferResultCode::InternalError, L"\u63a5\u6536\u7ebf\u7a0b\u53d1\u751f\u672a\u77e5\u5f02\u5e38");
    }
}

void TransferSession::InnerReceiverWorker(const std::wstring& targetDir, int port,
    const std::wstring& expectedPairingCode, TransferMode mode)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    Log(L"\u6b63\u5728\u76d1\u542c\u7aef\u53e3 " + std::to_wstring(port));

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) {
        Log(L"\u521b\u5efa socket \u5931\u8d25");
        NotifyDone(TransferResultCode::FileError, L"\u521b\u5efa\u76d1\u542c Socket \u5931\u8d25");
        WSACleanup(); return;
    }
    {
        std::lock_guard<std::mutex> lock(m_sockMutex);
        m_listenSock = listenSock;
    }

    int reuse = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        std::wstring msg = L"\u7ed1\u5b9a\u63a5\u6536\u7aef\u53e3\u5931\u8d25\uff0c\u9519\u8bef\u7801: " + std::to_wstring(WSAGetLastError());
        Log(msg);
        {
            std::lock_guard<std::mutex> lock(m_sockMutex);
            if (m_listenSock == listenSock)
                m_listenSock = INVALID_SOCKET;
        }
        closesocket(listenSock);
        NotifyDone(TransferResultCode::FileError, msg);
        WSACleanup(); return;
    }

    if (listen(listenSock, 1) == SOCKET_ERROR) {
        std::wstring msg = L"\u76d1\u542c\u5931\u8d25\uff0c\u9519\u8bef\u7801: " + std::to_wstring(WSAGetLastError());
        Log(msg);
        {
            std::lock_guard<std::mutex> lock(m_sockMutex);
            if (m_listenSock == listenSock)
                m_listenSock = INVALID_SOCKET;
        }
        closesocket(listenSock);
        NotifyDone(TransferResultCode::FileError, msg);
        WSACleanup(); return;
    }

    while (m_running) {
        Log(L"\u7b49\u5f85\u53d1\u9001\u7aef\u8fde\u63a5...");

        sockaddr_in from = {};
        int fromLen = sizeof(from);
        SOCKET sock = accept(listenSock, (sockaddr*)&from, &fromLen);
        if (sock == INVALID_SOCKET) {
            int err = WSAGetLastError();
            if (m_running)
                Log(L"\u63a5\u53d7\u8fde\u63a5\u5931\u8d25\uff0c\u9519\u8bef\u7801: " + std::to_wstring(err));
            break;
        }

        {
            std::lock_guard<std::mutex> lock(m_sockMutex);
            m_sock = sock;
        }
        EnableLowLatencyTcp(sock);
        {
            int err = 0;
            if (!SetSocketTimeouts(sock, HANDSHAKE_TIMEOUT_MS, err)) {
                Log(L"\u8bbe\u7f6e\u7f51\u7edc\u8d85\u65f6\u5931\u8d25\uff0c\u9519\u8bef\u7801: " + std::to_wstring(err));
                std::lock_guard<std::mutex> lock(m_sockMutex);
                if (m_sock == sock) m_sock = INVALID_SOCKET;
                shutdown(sock, SD_BOTH);
                closesocket(sock);
                continue;
            }
        }

        StopHeartbeat();

        ReceiverConnectionResult connResult =
            HandleReceiverConnection(sock, targetDir, expectedPairingCode, mode);

        StopHeartbeat();

        if (sock != INVALID_SOCKET) {
            shutdown(sock, SD_BOTH);
        }

        StopReceiveLoop();

        bool closeSock = false;
        {
            std::lock_guard<std::mutex> lock(m_sockMutex);
            if (m_sock == sock) {
                m_sock = INVALID_SOCKET;
                closeSock = true;
            }
        }
        if (closeSock) {
            closesocket(sock);
        }

        if (!m_running) break;

        switch (connResult) {
        case ReceiverConnectionResult::Completed:
            NotifyDone(TransferResultCode::Success,
                L"\u63a5\u6536\u5df2\u5b8c\u6210\uff0c\u6b63\u5728\u7b49\u5f85\u4e0b\u6b21\u8fde\u63a5...");
            break;

        case ReceiverConnectionResult::ConnectionLost:
            NotifyDone(TransferResultCode::ConnectionLost,
                L"\u8fde\u63a5\u5df2\u4e2d\u65ad\uff0c\u672a\u5b8c\u6210\u6587\u4ef6\u5df2\u4fdd\u7559\uff0c"
                L"\u6b63\u5728\u7b49\u5f85\u53d1\u9001\u7aef\u91cd\u65b0\u8fde\u63a5\u7eed\u4f20...");
            break;

        case ReceiverConnectionResult::PairingRejected:
            Log(L"\u914d\u5bf9\u7801\u9519\u8bef\uff0c\u7b49\u5f85\u4e0b\u4e00\u6b21\u8fde\u63a5...");
            break;

        case ReceiverConnectionResult::Cancelled:
            NotifyDone(TransferResultCode::Cancelled, L"\u63a5\u6536\u5df2\u53d6\u6d88");
            return;

        case ReceiverConnectionResult::Failed:
            NotifyDone(TransferResultCode::FileError, L"\u63a5\u6536\u5931\u8d25");
            break;
        }
    }

    bool closeListen = false;
    {
        std::lock_guard<std::mutex> lock(m_sockMutex);
        if (m_listenSock == listenSock) {
            m_listenSock = INVALID_SOCKET;
            closeListen = true;
        }
    }
    if (closeListen)
        closesocket(listenSock);
    WSACleanup();
}

struct MirrorDeleteResult {
    int attempted = 0;
    int deleted = 0;
    std::vector<std::wstring> failedFiles;
    std::vector<DWORD> errorCodes;

    bool Success() const {
        return failedFiles.empty();
    }
};

static MirrorDeleteResult ApplyMirrorDeletes(const std::vector<std::wstring>& files) {
    MirrorDeleteResult result;

    for (const auto& file : files) {
        ++result.attempted;

        if (DeleteFileW(file.c_str())) {
            ++result.deleted;
            continue;
        }

        DWORD error = GetLastError();

        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            ++result.deleted;
            continue;
        }

        result.failedFiles.push_back(file);
        result.errorCodes.push_back(error);
    }

    return result;
}

ReceiverConnectionResult TransferSession::HandleReceiverConnection(SOCKET sock, const std::wstring& targetDir,
    const std::wstring& expectedPairingCode, TransferMode mode)
{
    Log(L"\u5df2\u8fde\u63a5\uff0c\u7b49\u5f85\u914d\u5bf9\u7801...");

    std::wstring code = RecvStringPacket(sock, (uint8_t)PacketType::PAIRING_REQUEST);
    if (code.empty()) {
        Log(L"\u63a5\u6536\u914d\u5bf9\u7801\u5931\u8d25");
        return ReceiverConnectionResult::Failed;
    }

    if (code != expectedPairingCode) {
        Log(L"\u914d\u5bf9\u7801\u9519\u8bef\uff01\u671f\u671b: " + expectedPairingCode
            + L"\uff0c\u6536\u5230: " + code);
        SendStringPacket(sock, (uint8_t)PacketType::PAIRING_RESPONSE, L"FAIL");
        return ReceiverConnectionResult::PairingRejected;
    }

    SendStringPacket(sock, (uint8_t)PacketType::PAIRING_RESPONSE, L"OK");
    {
        int err = 0;
        if (!SetSocketTimeouts(sock, IO_WAIT_TIMEOUT_MS, err)) {
            Log(L"\u8bbe\u7f6e\u4f20\u8f93\u8d85\u65f6\u5931\u8d25\uff0c\u9519\u8bef\u7801: " + std::to_wstring(err));
            return ReceiverConnectionResult::Failed;
        }
    }
    EnableTcpKeepAlive(sock);
    StartHeartbeat(sock);
    StartReceiveLoop(sock);
    Log(L"\u914d\u5bf9\u7801\u9a8c\u8bc1\u6210\u529f\uff01");
    m_stats = TransferStats{};

    uint8_t manifestType = 0;
    std::vector<uint8_t> manifestPayload;
    if (!RecvPacket(sock, manifestType, manifestPayload) || manifestType != (uint8_t)PacketType::MANIFEST) {
        Log(L"\u63a5\u6536\u6587\u4ef6\u6e05\u5355\u5931\u8d25");
        if (m_connectionLost) return ReceiverConnectionResult::ConnectionLost;
        return ReceiverConnectionResult::Failed;
    }

    std::string manifestJson((char*)manifestPayload.data(), manifestPayload.size());
    Manifest manifest;
    if (!manifest.FromJSON(manifestJson)) {
        Log(L"\u89e3\u6790\u6587\u4ef6\u6e05\u5355\u5931\u8d25");
        return ReceiverConnectionResult::Failed;
    }

    Log(L"\u5df2\u63a5\u6536\u6587\u4ef6\u6e05\u5355: " + std::to_wstring(manifest.GetEntries().size())
        + L" \u4e2a\u6587\u4ef6\uff0c" + utils::FormatBytes(manifest.GetTotalSize()));

    std::vector<std::wstring> extraFiles;
    if (mode == TransferMode::MIRROR) {
        extraFiles = TransferPlanner::FindExtraFiles(manifest, targetDir);
    }

    Log(L"\u6b63\u5728\u751f\u6210\u4f20\u8f93\u8ba1\u5212...");

    TransferPlanner planner;
    TransferPlanner::Plan plan = planner.BuildPlan(manifest, targetDir, mode);

    // Validate .dtpart entries BEFORE sending plan
    {
        bool allValid = true;
        for (auto& entry : plan.entries) {
            if (!ValidateResumeEntry(entry, targetDir)) {
                Log(L"\u65e0\u6cd5\u9a8c\u8bc1\u7eed\u4f20\u6587\u4ef6: " + entry.relativePath);
                allValid = false;
                break;
            }
        }
        if (!allValid) {
            return ReceiverConnectionResult::Failed;
        }
        RecalculatePlanTotals(plan);
    }

    Log(L"\u8ba1\u5212\u63a5\u6536 " + std::to_wstring(plan.totalFiles)
        + L" \u4e2a\u6587\u4ef6\uff0c\u8df3\u8fc7 " + std::to_wstring(plan.skipFiles));

    std::string planJson = TransferPlanner::SerializePlan(plan);
    std::vector<uint8_t> planPayload(planJson.begin(), planJson.end());
    if (!SendPacket(sock, (uint8_t)PacketType::TRANSFER_PLAN, planPayload)) {
        Log(L"\u53d1\u9001\u4f20\u8f93\u8ba1\u5212\u5931\u8d25");
        if (m_connectionLost) return ReceiverConnectionResult::ConnectionLost;
        return ReceiverConnectionResult::Failed;
    }

    m_stats.totalBytes = plan.totalBytes;
    m_stats.totalFiles = plan.totalFiles;
    m_stats.skippedFiles = plan.skipFiles;

    std::unordered_map<std::wstring, int64_t> plannedRemainingBytes;
    std::unordered_map<std::wstring, bool> plannedSkipFiles;
    for (const auto& entry : plan.entries) {
        int64_t remaining = 0;
        if (entry.action == FileAction::TRANSFER || entry.action == FileAction::OVERWRITE) {
            remaining = entry.size - entry.offset;
            if (remaining < 0) remaining = 0;
        }
        plannedRemainingBytes[entry.relativePath] = remaining;
        plannedSkipFiles[entry.relativePath] = (entry.action == FileAction::SKIP);
    }

    int received = 0;
    bool receivedDone = false;
    bool receiverFailed = false;
    std::wstring failureMessage;

    while (m_running) {
        uint8_t type = 0;
        std::vector<uint8_t> payload;

        if (!RecvPacket(sock, type, payload)) {
            failureMessage = L"\u7f51\u7edc\u8fde\u63a5\u4e2d\u65ad\u6216\u63a5\u6536\u6570\u636e\u5931\u8d25";
            Log(failureMessage);
            receiverFailed = true;
            break;
        }

        if (type == (uint8_t)PacketType::DONE) {
            if (receiverFailed || m_stats.failedFiles > 0) {
                failureMessage = L"\u6536\u5230\u5b8c\u6210\u4fe1\u53f7\uff0c\u4f46\u5f53\u524d\u4f1a\u8bdd\u5b58\u5728\u5931\u8d25\u6587\u4ef6";
                receiverFailed = true;
                SendStringPacket(sock, (uint8_t)PacketType::DONE_ACK, L"FAIL\n\u6587\u4ef6\u4f20\u8f93\u6216\u6821\u9a8c\u5b58\u5728\u5931\u8d25");
                break;
            }
            receivedDone = true;
            Log(L"\u6240\u6709\u6587\u4ef6\u63a5\u6536\u5e76\u6821\u9a8c\u5b8c\u6210\uff0c\u51c6\u5907\u63d0\u4ea4\u955c\u50cf\u64cd\u4f5c");
            break;
        }

        if (type == (uint8_t)PacketType::ERROR_MSG) {
            std::wstring errMsg = utils::FromUtf8(std::string((const char*)payload.data(), payload.size()));
            Log(L"\u53d1\u9001\u7aef\u62a5\u9519: " + errMsg);
            if (errMsg == L"TRANSFER_ABORTED") {
                receiverFailed = true;
                failureMessage = L"\u53d1\u9001\u7aef\u4e2d\u6b62\u4e86\u4f20\u8f93";
                break;
            }
            ++m_stats.failedFiles;
            receiverFailed = true;
            failureMessage = errMsg;
            continue;
        }

        if (type == (uint8_t)PacketType::FILE_HEADER) {
            std::string headerStr((char*)payload.data(), payload.size());
            size_t p1 = headerStr.find('\n');
            size_t p2 = headerStr.find('\n', p1 + 1);
            if (p1 == std::string::npos || p2 == std::string::npos) {
                Log(L"\u89e3\u6790\u6587\u4ef6\u5934\u5931\u8d25\uff0c\u65ad\u5f00\u8fde\u63a5");
                SendStringPacket(sock, (uint8_t)PacketType::ERROR_MSG, L"BAD_HEADER");
                receiverFailed = true;
                failureMessage = L"\u89e3\u6790\u6587\u4ef6\u5934\u5931\u8d25";
                break;
            }

            std::string nameA = headerStr.substr(0, p1);
            int64_t fileSize = 0;
            int64_t offset = 0;
            try {
                fileSize = std::stoll(headerStr.substr(p1 + 1, p2 - p1 - 1));
                if (p2 != std::string::npos)
                    offset = std::stoll(headerStr.substr(p2 + 1));
            } catch (...) {
                Log(L"\u89e3\u6790\u6587\u4ef6\u5934\u6570\u503c\u5931\u8d25");
                SendStringPacket(sock, (uint8_t)PacketType::ERROR_MSG, L"BAD_HEADER");
                receiverFailed = true;
                failureMessage = L"\u89e3\u6790\u6587\u4ef6\u5934\u6570\u503c\u5931\u8d25";
                break;
            }
            if (fileSize < 0 || offset < 0 || offset > fileSize) {
                Log(L"\u6587\u4ef6\u5934\u504f\u79fb\u65e0\u6548");
                SendStringPacket(sock, (uint8_t)PacketType::ERROR_MSG, L"BAD_HEADER");
                receiverFailed = true;
                failureMessage = L"\u6587\u4ef6\u5934\u504f\u79fb\u65e0\u6548";
                break;
            }

            std::wstring fileName = utils::FromUtf8(nameA);

            std::wstring fullPath = targetDir + L"\\" + fileName;
            int64_t actualRemaining = fileSize - offset;
            if (actualRemaining < 0) actualRemaining = 0;
            auto plannedIt = plannedRemainingBytes.find(fileName);
            if (plannedIt != plannedRemainingBytes.end() && actualRemaining > plannedIt->second) {
                m_stats.totalBytes += (actualRemaining - plannedIt->second);
                auto skipIt = plannedSkipFiles.find(fileName);
                if (skipIt != plannedSkipFiles.end() && skipIt->second) {
                    m_stats.totalFiles++;
                    if (m_stats.skippedFiles > 0)
                        m_stats.skippedFiles--;
                    skipIt->second = false;
                }
                plannedIt->second = actualRemaining;
            }

            m_stats.currentFile = fileName;
            ReportProgress();
            Log(L"\u63a5\u6536: " + fileName);

            // Create target directory recursively
            std::wstring dir = fullPath.substr(0, fullPath.find_last_of(L'\\'));
            utils::CreateDirectoryTree(dir);

            // Receive file chunks
            std::wstring partPath = fullPath + L".dtpart";
            std::wstring normPartPath = utils::NormalizePath(partPath);
            Sha256Context hashCtx;
            bool hashOk = StartSha256(hashCtx);
            bool fileOk = hashOk;
            if (!hashOk) {
                Log(L"\u521d\u59cb\u5316 SHA-256 \u5931\u8d25: " + fileName);
            } else if (offset > 0 && !UpdateSha256FromFile(hashCtx, partPath, offset)) {
                Log(L"\u7eed\u4f20\u524d\u7f00\u8bfb\u53d6\u5931\u8d25: " + fileName);
                fileOk = false;
            }

            DWORD createFlags = (offset > 0) ? OPEN_EXISTING : CREATE_ALWAYS;
            HANDLE hFile = CreateFileW(normPartPath.c_str(), GENERIC_WRITE, 0, NULL,
                createFlags, FILE_ATTRIBUTE_NORMAL, NULL);

            bool canWrite = (hFile != INVALID_HANDLE_VALUE);
            if (hFile == INVALID_HANDLE_VALUE) {
                Log(L"\u65e0\u6cd5\u521b\u5efa\u6587\u4ef6: " + fileName);
                fileOk = false;
            } else if (offset > 0) {
                // Verify .dtpart file size matches expected offset
                LARGE_INTEGER fileSizeActual;
                if (!GetFileSizeEx(hFile, &fileSizeActual)) {
                    Log(L"\u65e0\u6cd5\u83b7\u53d6 .dtpart \u6587\u4ef6\u5927\u5c0f: " + fileName);
                    CloseHandle(hFile);
                    fileOk = false;
                } else if (fileSizeActual.QuadPart != offset) {
                    Log(L".dtpart \u6587\u4ef6\u5927\u5c0f\u4e0d\u5339\u914d\uff0c\u8bf7\u91cd\u65b0\u751f\u6210\u4f20\u8f93\u8ba1\u5212: " + fileName);
                    CloseHandle(hFile);
                    SendStringPacketResult(sock,
                        static_cast<uint8_t>(PacketType::ERROR_MSG),
                        L"RESUME_OFFSET_MISMATCH");
                    receiverFailed = true;
                    failureMessage = L"\u7eed\u4f20\u504f\u79fb\u4e0e .dtpart \u5927\u5c0f\u4e0d\u4e00\u81f4";
                    break;
                } else {
                    LARGE_INTEGER li;
                    li.QuadPart = offset;
                    SetFilePointerEx(hFile, li, NULL, FILE_BEGIN);
                }
            }

            int64_t remaining = fileSize - offset;
            bool senderAborted = false;

            while (remaining > 0 && m_running) {
                uint8_t chunkType = 0;
                std::vector<uint8_t> chunk;
                if (!RecvPacket(sock, chunkType, chunk) || chunkType != (uint8_t)PacketType::FILE_CHUNK) {
                    if (chunkType == (uint8_t)PacketType::ERROR_MSG) {
                        std::wstring errMsg2 = utils::FromUtf8(std::string((const char*)chunk.data(), chunk.size()));
                        Log(L"\u53d1\u9001\u7aef\u62a5\u9519: " + errMsg2);
                        senderAborted = true;
                    } else {
                        Log(L"\u63a5\u6536\u6570\u636e\u5931\u8d25: " + fileName);
                    }
                    fileOk = false;
                    break;
                }

                if (chunk.size() > (size_t)remaining) {
                    Log(L"\u6570\u636e\u8d85\u51fa\u6587\u4ef6\u5927\u5c0f: " + fileName);
                    fileOk = false;
                    break;
                }

                DWORD written = 0;
                if (hashOk && !UpdateSha256(hashCtx, chunk.data(), (DWORD)chunk.size())) {
                    Log(L"SHA-256 \u8ba1\u7b97\u5931\u8d25: " + fileName);
                    fileOk = false;
                    hashOk = false;
                }

                if (fileOk && canWrite && (!WriteFile(hFile, chunk.data(), (DWORD)chunk.size(), &written, NULL)
                        || written != (DWORD)chunk.size())) {
                    Log(L"\u5199\u5165\u6587\u4ef6\u5931\u8d25: " + fileName);
                    fileOk = false;
                }

                remaining -= (int64_t)chunk.size();
                if (fileOk && canWrite) {
                    m_stats.transferredBytes += (int64_t)chunk.size();
                    ReportProgress();
                }
            }

            if (hFile != INVALID_HANDLE_VALUE)
                CloseHandle(hFile);

            if (senderAborted) {
                uint8_t doneType = 0;
                std::vector<uint8_t> donePayload;
                if (!RecvPacket(sock, doneType, donePayload) || doneType != (uint8_t)PacketType::FILE_DONE) {
                    Log(L"\u63a5\u6536 FILE_DONE \u5931\u8d25");
                    receiverFailed = true;
                    failureMessage = L"\u63a5\u6536 FILE_DONE \u5931\u8d25";
                    break;
                }
                m_stats.failedFiles++;
                CloseSha256(hashCtx);
                continue;
            }

            if (remaining > 0) {
                m_stats.failedFiles++;
                CloseSha256(hashCtx);
                receiverFailed = true;
                failureMessage = L"\u63a5\u6536\u6570\u636e\u672a\u5b8c\u6574";
                break;
            }

            std::string expectedHash;
            bool senderAbortedAfterData = false;
            uint8_t hashType = 0;
            std::vector<uint8_t> hashPayload;
            if (!RecvPacket(sock, hashType, hashPayload)) {
                Log(L"\u63a5\u6536 SHA-256 \u6821\u9a8c\u503c\u5931\u8d25: " + fileName);
                CloseSha256(hashCtx);
                receiverFailed = true;
                failureMessage = L"\u63a5\u6536 SHA-256 \u6821\u9a8c\u503c\u5931\u8d25";
                break;
            }
            if (hashType != (uint8_t)PacketType::FILE_HASH) {
                if (hashType == (uint8_t)PacketType::ERROR_MSG) {
                    std::wstring errMsg2 = utils::FromUtf8(std::string((const char*)hashPayload.data(), hashPayload.size()));
                    Log(L"\u53d1\u9001\u7aef\u62a5\u9519: " + errMsg2);
                    senderAbortedAfterData = true;
                } else {
                    Log(L"\u63a5\u6536 SHA-256 \u6821\u9a8c\u503c\u5931\u8d25: " + fileName);
                }
                fileOk = false;
            } else {
                expectedHash.assign((char*)hashPayload.data(), hashPayload.size());
            }

            uint8_t doneType = 0;
            std::vector<uint8_t> donePayload;
            if (!RecvPacket(sock, doneType, donePayload) || doneType != (uint8_t)PacketType::FILE_DONE) {
                Log(L"\u63a5\u6536 FILE_DONE \u5931\u8d25");
                CloseSha256(hashCtx);
                receiverFailed = true;
                failureMessage = L"\u63a5\u6536 FILE_DONE \u5931\u8d25";
                break;
            }
            if (senderAbortedAfterData) {
                m_stats.failedFiles++;
                CloseSha256(hashCtx);
                continue;
            }
            std::wstring doneFile = utils::FromUtf8(std::string((const char*)donePayload.data(), donePayload.size()));
            if (doneFile != fileName) {
                Log(L"\u534f\u8bae\u9519\u8bef: FILE_DONE \u6587\u4ef6\u540d\u4e0d\u5339\u914d");
                fileOk = false;
            }

            std::string actualHash = FinishSha256(hashCtx);
            if (fileOk) {
                if (expectedHash.empty() || actualHash.empty() || actualHash != expectedHash) {
                    Log(L"SHA-256 \u4e0d\u5339\u914d: " + fileName);
                    fileOk = false;
                } else {
                    Log(L"SHA-256 \u9a8c\u8bc1\u901a\u8fc7: " + fileName);
                }
            }

            if (fileOk && canWrite) {
                std::wstring normFinalPath = utils::NormalizePath(fullPath);
                if (MoveFileExW(normPartPath.c_str(), normFinalPath.c_str(),
                        MOVEFILE_REPLACE_EXISTING)) {
                    // Verified before rename; final path replacement is now the commit step.
                } else {
                    Log(L"\u91cd\u547d\u540d\u6587\u4ef6\u5931\u8d25: " + fileName);
                    fileOk = false;
                }
            }

            // Send FILE_DONE_ACK
            {
                std::wstring ackData = (fileOk ? L"OK\n" : L"FAIL\n") + fileName;
                if (!SendStringPacket(sock, (uint8_t)PacketType::FILE_DONE_ACK, ackData)) {
                    receiverFailed = true;
                    failureMessage = L"\u53d1\u9001 FILE_DONE_ACK \u5931\u8d25";
                    break;
                }
            }

            if (fileOk) {
                received++;
                m_stats.completedFiles = received;
                Log(L"\u5b8c\u6210: " + fileName);
            } else {
                m_stats.failedFiles++;
                receiverFailed = true;
                failureMessage = L"\u6587\u4ef6\u9a8c\u8bc1\u5931\u8d25: " + fileName;
                break;
            }
        }
    }

    Log(L"\u63a5\u6536\u7ed3\u675f");

    if (!receivedDone || receiverFailed || m_stats.failedFiles > 0) {
        if (failureMessage.empty()) {
            failureMessage = L"\u4f20\u8f93\u4f1a\u8bdd\u672a\u6b63\u5e38\u5b8c\u6210";
        }
        if (m_connectionLost) {
            Log(L"\u8fde\u63a5\u5df2\u4e2d\u65ad\uff0c\u4fdd\u7559 .dtpart \u6587\u4ef6");
            m_stats.interruptedFiles++;
            m_stats.resumable = true;
            return ReceiverConnectionResult::ConnectionLost;
        }
        return m_running ? ReceiverConnectionResult::Failed : ReceiverConnectionResult::Cancelled;
    }

    if (mode == TransferMode::MIRROR && !extraFiles.empty()) {
        Log(L"\u5f00\u59cb\u63d0\u4ea4\u955c\u50cf\u5220\u9664\uff0c\u5171 "
            + std::to_wstring(extraFiles.size()) + L" \u4e2a\u6587\u4ef6");

        MirrorDeleteResult deleteResult = ApplyMirrorDeletes(extraFiles);

        if (!deleteResult.Success()) {
            std::wstring message =
                L"\u955c\u50cf\u5220\u9664\u672a\u5b8c\u6574\u5b8c\u6210\uff1a\u6210\u529f "
                + std::to_wstring(deleteResult.deleted)
                + L" \u4e2a\uff0c\u5931\u8d25 "
                + std::to_wstring(deleteResult.failedFiles.size())
                + L" \u4e2a";
            Log(message);

            for (size_t i = 0; i < deleteResult.failedFiles.size(); ++i) {
                Log(L"\u5220\u9664\u5931\u8d25: " + deleteResult.failedFiles[i]
                    + L"\uff0c\u9519\u8bef\u7801: " + std::to_wstring(deleteResult.errorCodes[i]));
            }

            SendStringPacket(sock, (uint8_t)PacketType::DONE_ACK,
                L"FAIL\n" + message);

            return ReceiverConnectionResult::Failed;
        }

        Log(L"\u955c\u50cf\u5220\u9664\u5b8c\u6210: "
            + std::to_wstring(deleteResult.deleted) + L" \u4e2a\u6587\u4ef6");
    }

    if (!SendStringPacket(sock, (uint8_t)PacketType::DONE_ACK, L"OK")) {
        return ReceiverConnectionResult::Failed;
    }

    return ReceiverConnectionResult::Completed;
}
