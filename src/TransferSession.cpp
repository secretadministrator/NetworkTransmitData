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
#include "Version.h"
#include <nlohmann/json.hpp>
#include <bcrypt.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <vector>
#include <filesystem>
#include <cstring>
#include <climits>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

static constexpr size_t MAX_PAYLOAD_BYTES = 64 * 1024 * 1024; // 64 MB
static constexpr DWORD HANDSHAKE_TIMEOUT_MS = 30000;

static std::wstring BuildHelloPayload(const std::wstring& token) {
    nlohmann::json hello;
    hello["magic"] = "DirectTransfer";
    hello["protocol"] = version::PROTOCOL_VERSION;
    hello["appVersion"] = DT_VERSION_STRING;
    hello["token"] = utils::ToUtf8(token);
    return utils::FromUtf8(hello.dump());
}

static bool ParseHelloPayload(const std::wstring& payload,
    std::wstring& token, std::wstring& appVersion) {
    try {
        auto hello = nlohmann::json::parse(utils::ToUtf8(payload));
        if (hello.value("magic", std::string()) != "DirectTransfer" ||
            hello.value("protocol", 0) != version::PROTOCOL_VERSION)
            return false;
        token = utils::FromUtf8(hello.value("token", std::string()));
        appVersion = utils::FromUtf8(hello.value("appVersion", std::string()));
        return !token.empty();
    } catch (...) {
        return false;
    }
}

static std::wstring SerializePlanProgress(const TransferPlanner::Progress& progress) {
    nlohmann::json value;
    value["processed"] = progress.processedFiles;
    value["total"] = progress.totalFiles;
    value["hashedBytes"] = progress.hashedBytes;
    value["path"] = utils::ToUtf8(progress.currentPath);
    value["stage"] = utils::ToUtf8(progress.stage);
    return utils::FromUtf8(value.dump());
}

static bool ParsePlanProgress(const std::vector<uint8_t>& payload,
    TransferStats& stats) {
    try {
        auto value = nlohmann::json::parse(
            std::string(reinterpret_cast<const char*>(payload.data()), payload.size()));
        stats.stage = TransferStage::BuildingPlan;
        stats.stageProcessed = value.value("processed", uint64_t{0});
        stats.stageTotal = value.value("total", uint64_t{0});
        stats.stageBytes = value.value("hashedBytes", int64_t{0});
        stats.currentFile = utils::FromUtf8(value.value("path", std::string()));
        stats.stageText = utils::FromUtf8(value.value("stage", std::string()));
        return true;
    } catch (...) {
        return false;
    }
}

TransferSession::TransferSession() {}
TransferSession::~TransferSession() { Stop(); }

void TransferSession::Log(const std::wstring& msg) {
    if (m_logCb) m_logCb(msg);
}

void TransferSession::ReportProgress(bool force) {
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG last = m_lastProgressReportTick.load();
    if (!force && last != 0 && now - last < PROGRESS_REPORT_INTERVAL_MS)
        return;
    m_lastProgressReportTick.store(now);
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
    int port, const std::wstring& sessionToken)
{
    if (m_running) return false;
    if (m_workerThread.joinable())
        m_workerThread.join();
    m_stats = TransferStats{};
    m_lastProgressReportTick.store(0);
    m_role = SessionRole::SENDER;
    m_running = true;
    m_workerThread = std::thread(&TransferSession::SenderWorker, this,
        sourceDir, peerIP, port, sessionToken);
    return true;
}

bool TransferSession::StartAsReceiver(const std::wstring& targetDir, int port,
    const std::wstring& sessionToken, TransferMode mode)
{
    if (m_running) return false;
    if (m_workerThread.joinable())
        m_workerThread.join();
    m_stats = TransferStats{};
    m_lastProgressReportTick.store(0);
    m_role = SessionRole::RECEIVER;
    m_running = true;
    m_workerThread = std::thread(&TransferSession::ReceiverWorker, this,
        targetDir, port, sessionToken, mode);
    return true;
}

void TransferSession::Stop() {
    m_running = false;
    m_packetQueueCv.notify_all();
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
    std::vector<uint8_t>& payload,
    DWORD businessTimeoutMs)
{
    if (m_receiveLoopRunning.load()) {
        std::unique_lock<std::mutex> lock(m_packetQueueMutex);
        const ULONGLONG waitStarted = GetTickCount64();

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
            if (businessTimeoutMs != INFINITE &&
                GetTickCount64() - waitStarted >= businessTimeoutMs) {
                return {IoStatus::IdleTimeout, WSAETIMEDOUT};
            }
        }

        ReceivedPacket pkt = std::move(m_packetQueue.front());
        m_packetQueueBytes -= pkt.payload.size();
        m_packetQueue.pop_front();
        lock.unlock();
        m_packetQueueCv.notify_all();

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
            operation + L"\uff1a\u8fde\u7eed 90 \u79d2\u6ca1\u6709\u6570\u636e\u6216\u9636\u6bb5\u8fdb\u5c55");

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
        m_packetQueueBytes = 0;
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

    {
        std::lock_guard<std::mutex> lock(m_packetQueueMutex);
        m_packetQueue.clear();
        m_packetQueueBytes = 0;
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

        // Business packet: push to the bounded queue. One oversized packet is
        // allowed when the queue is empty so the producer can always make
        // progress up to the protocol payload limit.
        {
            std::unique_lock<std::mutex> lock(m_packetQueueMutex);
            m_packetQueueCv.wait(lock, [this, &payload]() {
                if (!m_receiveLoopRunning || !m_running || m_connectionLost)
                    return true;
                const bool countAvailable =
                    m_packetQueue.size() < MAX_PACKET_QUEUE_COUNT;
                const bool bytesAvailable = m_packetQueue.empty() ||
                    m_packetQueueBytes + payload.size() <= MAX_PACKET_QUEUE_BYTES;
                return countAvailable && bytesAvailable;
            });

            if (!m_receiveLoopRunning || !m_running || m_connectionLost)
                break;

            m_packetQueueBytes += payload.size();
            m_packetQueue.push_back({type, std::move(payload)});
        }

        m_packetQueueCv.notify_all();
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
    m_packetQueueCv.notify_all();

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

static void UpdateProgressStats(const ProgressTracker& progress, TransferStats& stats) {
    stats.transferredBytes = progress.GetTransferred();
    stats.averageSpeedBytesPerSec = progress.GetAverageSpeed();
    if (!progress.HasSpeedSample()) {
        stats.speedBytesPerSec = 0;
        stats.recentSpeedBytesPerSec = 0;
        stats.estimatedRemainingSeconds = progress.GetTransferred() >= progress.GetTotal()
            ? 0
            : -1;
        stats.waitingForIo = false;
        return;
    }

    stats.speedBytesPerSec = progress.GetSpeed();
    stats.recentSpeedBytesPerSec = progress.GetRecentSpeed();
    stats.estimatedRemainingSeconds = progress.GetEstimatedRemainingSeconds();
    static constexpr int64_t WAITING_SPEED_THRESHOLD = 1024;
    stats.waitingForIo = progress.GetTransferred() < progress.GetTotal() &&
        stats.recentSpeedBytesPerSec < WAITING_SPEED_THRESHOLD;
}

static constexpr size_t DETAILED_FILE_LOG_LIMIT = 100;
static constexpr int FILE_LOG_BATCH_SIZE = 100;

static bool UseDetailedFileLogs(size_t plannedEntries) {
    return plannedEntries <= DETAILED_FILE_LOG_LIMIT;
}

static bool ShouldLogFileBatch(int completedFiles, int totalFiles) {
    return completedFiles > 0 &&
        (completedFiles % FILE_LOG_BATCH_SIZE == 0 || completedFiles == totalFiles);
}

enum class PreparedEventType {
    FileStart,
    OpenFailed,
    Data,
    ReadFailed,
    HashFailed,
    FileEnd,
    PreparationFailed
};

struct PreparedFileEvent {
    PreparedEventType type = PreparedEventType::PreparationFailed;
    size_t planIndex = 0;
    std::vector<uint8_t> data;
    std::string sha256;
};

class PreparedFileQueue {
public:
    PreparedFileQueue(size_t maxBytes, size_t maxItems)
        : m_maxBytes(maxBytes), m_maxItems(maxItems) {}

    bool Push(PreparedFileEvent event, const std::atomic<bool>& running) {
        const size_t eventBytes = event.data.size();
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [&]() {
            if (m_cancelled || !running.load())
                return true;
            const bool countAvailable = m_items.size() < m_maxItems;
            const bool bytesAvailable = eventBytes == 0 || m_items.empty() ||
                m_bufferedBytes + eventBytes <= m_maxBytes;
            return countAvailable && bytesAvailable;
        });

        if (m_cancelled || !running.load())
            return false;

        m_bufferedBytes += eventBytes;
        m_items.push_back(std::move(event));
        lock.unlock();
        m_cv.notify_all();
        return true;
    }

    bool Pop(PreparedFileEvent& event, const std::atomic<bool>& running) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [&]() {
            return m_cancelled || !running.load() || !m_items.empty() || m_finished;
        });

        if (m_cancelled || !running.load() || m_items.empty())
            return false;

        event = std::move(m_items.front());
        m_bufferedBytes -= event.data.size();
        m_items.pop_front();
        lock.unlock();
        m_cv.notify_all();
        return true;
    }

    void Finish() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_finished = true;
        }
        m_cv.notify_all();
    }

    void Cancel() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_cancelled = true;
            m_items.clear();
            m_bufferedBytes = 0;
        }
        m_cv.notify_all();
    }

private:
    const size_t m_maxBytes;
    const size_t m_maxItems;
    size_t m_bufferedBytes = 0;
    bool m_finished = false;
    bool m_cancelled = false;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<PreparedFileEvent> m_items;
};

class ScopedFileHandle {
public:
    explicit ScopedFileHandle(HANDLE handle) : m_handle(handle) {}
    ScopedFileHandle(const ScopedFileHandle&) = delete;
    ScopedFileHandle& operator=(const ScopedFileHandle&) = delete;
    ~ScopedFileHandle() {
        if (m_handle != INVALID_HANDLE_VALUE)
            CloseHandle(m_handle);
    }

    HANDLE Get() const { return m_handle; }

private:
    HANDLE m_handle = INVALID_HANDLE_VALUE;
};

class ScopedSha256Context {
public:
    ScopedSha256Context() = default;
    ScopedSha256Context(const ScopedSha256Context&) = delete;
    ScopedSha256Context& operator=(const ScopedSha256Context&) = delete;
    ~ScopedSha256Context() { CloseSha256(context); }
    Sha256Context context;
};

static void PrepareFiles(
    const TransferPlanner::Plan& plan,
    const std::wstring& sourceDir,
    DWORD chunkSize,
    PreparedFileQueue& queue,
    const std::atomic<bool>& running)
{
    try {
        std::vector<uint8_t> prefixBuffer(chunkSize);

        for (size_t planIndex = 0; planIndex < plan.entries.size(); ++planIndex) {
            if (!running.load())
                break;

            const PlanEntry& entry = plan.entries[planIndex];
            if (entry.action != FileAction::TRANSFER &&
                entry.action != FileAction::OVERWRITE) {
                continue;
            }

            const std::wstring fullPath = sourceDir + L"\\" + entry.relativePath;
            ScopedFileHandle file(CreateFileW(
                utils::NormalizePath(fullPath).c_str(),
                GENERIC_READ,
                FILE_SHARE_READ,
                NULL,
                OPEN_EXISTING,
                FILE_FLAG_SEQUENTIAL_SCAN,
                NULL));

            if (file.Get() == INVALID_HANDLE_VALUE) {
                PreparedFileEvent failed;
                failed.type = PreparedEventType::OpenFailed;
                failed.planIndex = planIndex;
                if (!queue.Push(std::move(failed), running))
                    break;
                continue;
            }

            PreparedFileEvent start;
            start.type = PreparedEventType::FileStart;
            start.planIndex = planIndex;
            if (!queue.Push(std::move(start), running))
                break;

            ScopedSha256Context hash;
            if (!StartSha256(hash.context)) {
                PreparedFileEvent failed;
                failed.type = PreparedEventType::HashFailed;
                failed.planIndex = planIndex;
                if (!queue.Push(std::move(failed), running))
                    break;
                continue;
            }

            int64_t offset = entry.offset;
            if (offset < 0 || offset > entry.size)
                offset = 0;

            bool fileFailed = false;
            bool failureQueued = false;
            bool queueStopped = false;
            int64_t prefixRemaining = offset;
            while (prefixRemaining > 0 && running.load()) {
                const DWORD toRead = prefixRemaining > static_cast<int64_t>(chunkSize)
                    ? chunkSize
                    : static_cast<DWORD>(prefixRemaining);
                DWORD bytesRead = 0;
                if (!ReadFile(file.Get(), prefixBuffer.data(), toRead, &bytesRead, NULL) ||
                    bytesRead == 0) {
                    fileFailed = true;
                    break;
                }
                if (!UpdateSha256(hash.context, prefixBuffer.data(), bytesRead)) {
                    PreparedFileEvent failed;
                    failed.type = PreparedEventType::HashFailed;
                    failed.planIndex = planIndex;
                    failureQueued = queue.Push(std::move(failed), running);
                    queueStopped = !failureQueued;
                    fileFailed = true;
                    break;
                }
                prefixRemaining -= bytesRead;
            }

            if (!running.load())
                break;

            if (queueStopped)
                break;

            if (fileFailed) {
                if (!failureQueued && prefixRemaining > 0) {
                    PreparedFileEvent failed;
                    failed.type = PreparedEventType::ReadFailed;
                    failed.planIndex = planIndex;
                    if (!queue.Push(std::move(failed), running))
                        break;
                }
                continue;
            }

            int64_t bytesRemaining = entry.size - offset;
            while (bytesRemaining > 0 && running.load()) {
                const DWORD toRead = bytesRemaining > static_cast<int64_t>(chunkSize)
                    ? chunkSize
                    : static_cast<DWORD>(bytesRemaining);

                PreparedFileEvent data;
                data.type = PreparedEventType::Data;
                data.planIndex = planIndex;
                data.data.resize(toRead);

                DWORD bytesRead = 0;
                if (!ReadFile(file.Get(), data.data.data(), toRead, &bytesRead, NULL) ||
                    bytesRead == 0) {
                    PreparedFileEvent failed;
                    failed.type = PreparedEventType::ReadFailed;
                    failed.planIndex = planIndex;
                    failureQueued = queue.Push(std::move(failed), running);
                    queueStopped = !failureQueued;
                    fileFailed = true;
                    break;
                }

                data.data.resize(bytesRead);
                if (!UpdateSha256(hash.context, data.data.data(), bytesRead)) {
                    PreparedFileEvent failed;
                    failed.type = PreparedEventType::HashFailed;
                    failed.planIndex = planIndex;
                    failureQueued = queue.Push(std::move(failed), running);
                    queueStopped = !failureQueued;
                    fileFailed = true;
                    break;
                }

                bytesRemaining -= bytesRead;
                if (!queue.Push(std::move(data), running)) {
                    fileFailed = true;
                    break;
                }
            }

            if (!running.load())
                break;
            if (queueStopped)
                break;
            if (fileFailed)
                continue;

            PreparedFileEvent end;
            end.type = PreparedEventType::FileEnd;
            end.planIndex = planIndex;
            end.sha256 = FinishSha256(hash.context);
            if (end.sha256.empty())
                end.type = PreparedEventType::HashFailed;
            if (!queue.Push(std::move(end), running))
                break;
        }
    } catch (...) {
        PreparedFileEvent failed;
        failed.type = PreparedEventType::PreparationFailed;
        queue.Push(std::move(failed), running);
    }

    queue.Finish();
}

// ============== SENDER ==============

void TransferSession::SenderWorker(const std::wstring& sourceDir, const std::wstring& peerIP,
    int port, const std::wstring& sessionToken)
{
    StopHeartbeat();

    TransferResult result;
    try {
        result = InnerSenderWorker(sourceDir, peerIP, port, sessionToken);
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
    {
        std::lock_guard<std::mutex> lock(m_sockMutex);
        if (m_sock != INVALID_SOCKET)
            shutdown(m_sock, SD_BOTH);
    }
    StopReceiveLoop();

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
    int port, const std::wstring& sessionToken)
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

    Log(L"\u5df2\u8fde\u63a5\uff0c\u6b63\u5728\u8bc6\u522b DirectTransfer \u63a5\u6536\u7aef...");

    std::wstring serverHello = RecvStringPacket(sock, (uint8_t)PacketType::SERVER_HELLO);
    std::wstring serverToken;
    std::wstring peerVersion;
    if (!ParseHelloPayload(serverHello, serverToken, peerVersion)) {
        Log(L"\u5bf9\u7aef\u4e0d\u662f\u517c\u5bb9\u7684 DirectTransfer \u63a5\u6536\u7aef");
        closesocket(sock); m_sock = INVALID_SOCKET;
        WSACleanup(); return MakeResult(TransferResultCode::ProtocolError,
            L"\u5bf9\u7aef\u534f\u8bae\u6216\u7248\u672c\u4e0d\u517c\u5bb9");
    }

    if (!sessionToken.empty() && sessionToken != serverToken) {
        Log(L"\u53d1\u73b0\u4f1a\u8bdd\u4e0e\u5b9e\u9645\u8fde\u63a5\u4e0d\u5339\u914d");
        closesocket(sock); m_sock = INVALID_SOCKET;
        WSACleanup(); return MakeResult(TransferResultCode::ProtocolError,
            L"\u81ea\u52a8\u53d1\u73b0\u4f1a\u8bdd\u5df2\u5931\u6548\uff0c\u8bf7\u91cd\u65b0\u53d1\u73b0\u63a5\u6536\u7aef");
    }

    if (!SendStringPacket(sock, (uint8_t)PacketType::CLIENT_HELLO,
            BuildHelloPayload(serverToken))) {
        closesocket(sock); m_sock = INVALID_SOCKET;
        WSACleanup(); return MakeResult(TransferResultCode::ProtocolError,
            L"\u53d1\u9001\u81ea\u52a8\u8bc6\u522b\u4fe1\u606f\u5931\u8d25");
    }

    std::wstring resp = RecvStringPacket(sock, (uint8_t)PacketType::HELLO_ACK);
    if (resp != L"OK") {
        Log(L"DirectTransfer \u81ea\u52a8\u8bc6\u522b\u5931\u8d25");
        closesocket(sock); m_sock = INVALID_SOCKET;
        WSACleanup(); return MakeResult(TransferResultCode::ProtocolError,
            L"DirectTransfer \u81ea\u52a8\u8bc6\u522b\u5931\u8d25");
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

    Log(L"\u5df2\u81ea\u52a8\u8bc6\u522b\u63a5\u6536\u7aef\uff0c\u5bf9\u7aef\u7248\u672c " + peerVersion);
    Log(L"\u6b63\u5728\u626b\u63cf\u6587\u4ef6...");

    FileScanner scanner;
    ExcludeRules excludeRules;
    scanner.SetExcludeCallback([&](const std::wstring& path) { return excludeRules.IsExcluded(path); });
    m_stats.scanning = true;
    m_stats.stage = TransferStage::ScanningSource;
    m_stats.stageText = L"\u6b63\u5728\u626b\u63cf\u6e90\u76ee\u5f55";
    scanner.SetProgressCallback([&](const ScanProgress& scan) {
        m_stats.totalFiles = static_cast<int>(scan.scannedFiles);
        m_stats.scannedDirectories = scan.scannedDirectories;
        m_stats.scannedBytes = static_cast<int64_t>(scan.scannedBytes);
        m_stats.inaccessibleDirectories = scan.inaccessibleDirectories;
        m_stats.currentFile = scan.currentPath;
        ReportProgress();
    });

    auto files = scanner.ScanDirectory(sourceDir);
    ReportProgress(true);
    m_stats.scanning = false;
    m_stats.currentFile.clear();
    if (files.empty()) {
        Log(L"\u6e90\u76ee\u5f55\u4e2d\u6ca1\u6709\u6587\u4ef6\uff0c\u5c06\u53d1\u9001\u7a7a\u6587\u4ef6\u6e05\u5355");
    }

    if (!m_running) {
        closesocket(sock); m_sock = INVALID_SOCKET;
        WSACleanup(); return MakeResult(TransferResultCode::Cancelled, L"\u4f20\u8f93\u5df2\u53d6\u6d88");
    }

    m_stats.stage = TransferStage::HashingSource;
    m_stats.stageText = L"\u6b63\u5728\u751f\u6210\u6e90\u6587\u4ef6 SHA-256 \u6e05\u5355";
    m_stats.stageProcessed = 0;
    m_stats.stageTotal = files.size();
    m_stats.stageBytes = 0;
    for (auto& file : files) {
        const std::wstring fullPath = sourceDir + L"\\" + file.relativePath;
        m_stats.currentFile = file.relativePath;
        file.sha256 = utils::ComputeSHA256(fullPath, -1,
            [&](int64_t bytes) {
                m_stats.stageBytes += bytes;
                ReportProgress();
                return m_running.load();
            });
        if (!m_running) {
            closesocket(sock); m_sock = INVALID_SOCKET;
            WSACleanup();
            return MakeResult(TransferResultCode::Cancelled, L"\u4f20\u8f93\u5df2\u53d6\u6d88");
        }
        if (file.sha256.empty()) {
            const std::wstring message = L"\u65e0\u6cd5\u8bfb\u53d6\u6216\u8ba1\u7b97\u6e90\u6587\u4ef6 SHA-256: "
                + file.relativePath;
            Log(message);
            closesocket(sock); m_sock = INVALID_SOCKET;
            WSACleanup();
            return MakeResult(TransferResultCode::FileError, message);
        }
        ++m_stats.stageProcessed;
        ReportProgress();
    }
    ReportProgress(true);

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
    m_stats.stage = TransferStage::WaitingForPlan;
    m_stats.stageText = L"\u7b49\u5f85\u63a5\u6536\u7aef\u751f\u6210\u4f20\u8f93\u8ba1\u5212";
    m_stats.currentFile.clear();
    ReportProgress(true);

    std::wstring planStr;
    for (;;) {
        uint8_t type = 0;
        std::vector<uint8_t> payload;
        IoResult io = RecvPacketResult(sock, type, payload, PLAN_PROGRESS_TIMEOUT_MS);
        if (!io.IsOk()) {
            if (io.status != IoStatus::Cancelled) {
                m_stats.resumable = true;
            }
            closesocket(sock); m_sock = INVALID_SOCKET;
            WSACleanup();
            return MakeIoFailureResult(io, L"\u63a5\u6536\u4f20\u8f93\u8ba1\u5212\u5931\u8d25");
        }
        if (type == static_cast<uint8_t>(PacketType::PLAN_PROGRESS)) {
            if (ParsePlanProgress(payload, m_stats))
                ReportProgress();
            continue;
        }
        if (type == static_cast<uint8_t>(PacketType::ERROR_MSG)) {
            std::wstring remoteError = utils::FromUtf8(std::string(
                reinterpret_cast<const char*>(payload.data()), payload.size()));
            closesocket(sock); m_sock = INVALID_SOCKET;
            WSACleanup();
            return MakeResult(TransferResultCode::FileError,
                L"\u63a5\u6536\u7aef\u65e0\u6cd5\u751f\u6210\u8ba1\u5212\uff1a" + remoteError);
        }
        if (type != static_cast<uint8_t>(PacketType::TRANSFER_PLAN)) {
            closesocket(sock); m_sock = INVALID_SOCKET;
            WSACleanup();
            return MakeResult(TransferResultCode::ProtocolError,
                L"\u7b49\u5f85\u4f20\u8f93\u8ba1\u5212\u65f6\u6536\u5230\u672a\u77e5\u6570\u636e\u5305");
        }
        planStr = utils::FromUtf8(std::string(
            reinterpret_cast<const char*>(payload.data()), payload.size()));
        break;
    }

    std::string planUtf8 = utils::ToUtf8(planStr);
    TransferPlanner::Plan plan = TransferPlanner::ParsePlanString(planUtf8);
    if (plan.entries.size() != files.size()) {
        closesocket(sock); m_sock = INVALID_SOCKET;
        WSACleanup();
        return MakeResult(TransferResultCode::ProtocolError,
            L"\u63a5\u6536\u7aef\u8fd4\u56de\u7684\u4f20\u8f93\u8ba1\u5212\u4e0d\u5b8c\u6574");
    }
    Log(L"\u5df2\u6536\u5230\u63a5\u6536\u7aef\u8ba1\u5212\uff0c\u6b63\u5728\u6821\u9a8c\u8df3\u8fc7\u9879\u548c\u7eed\u4f20\u524d\u7f00...");
    m_stats.stage = TransferStage::VerifyingPlan;
    m_stats.stageText = L"\u6b63\u5728\u6821\u9a8c\u8df3\u8fc7\u9879\u548c\u7eed\u4f20\u524d\u7f00";
    m_stats.stageProcessed = 0;
    m_stats.stageTotal = plan.entries.size();
    m_stats.stageBytes = 0;
    for (auto& entry : plan.entries) {
        if (entry.action == FileAction::SKIP) {
            if (entry.sha256.empty()) {
                entry.action = FileAction::TRANSFER;
                entry.offset = 0;
                entry.resumeHash.clear();
            }
        }

        if ((entry.action == FileAction::TRANSFER || entry.action == FileAction::OVERWRITE)
                && entry.offset > 0) {
            std::wstring fullPath = sourceDir + L"\\" + entry.relativePath;
            std::string sourcePrefixHash = utils::ComputeSHA256(fullPath, entry.offset,
                [&](int64_t bytes) {
                    m_stats.stageBytes += bytes;
                    m_stats.currentFile = entry.relativePath;
                    ReportProgress();
                    return m_running.load();
                });
            if (sourcePrefixHash.empty() || entry.resumeHash.empty()
                    || sourcePrefixHash != entry.resumeHash) {
                Log(L"\u7eed\u4f20\u524d\u7f00\u6821\u9a8c\u5931\u8d25\uff0c\u5c06\u91cd\u65b0\u4f20\u8f93: " + entry.relativePath);
                entry.offset = 0;
                entry.resumeHash.clear();
            }
        }
        ++m_stats.stageProcessed;
        ReportProgress();
    }
    RecalculatePlanTotals(plan);
    m_stats.totalBytes = plan.totalBytes;
    m_stats.totalFiles = plan.totalFiles;
    m_stats.skippedFiles = plan.skipFiles;
    m_stats.stage = TransferStage::Transferring;
    m_stats.stageText = L"\u6b63\u5728\u4f20\u8f93\u6587\u4ef6";
    m_stats.stageProcessed = 0;
    m_stats.stageTotal = plan.totalFiles;
    m_stats.stageBytes = 0;

    Log(L"\u8ba1\u5212\u4f20\u8f93 " + std::to_wstring(plan.totalFiles) + L" \u4e2a\u6587\u4ef6\uff0c"
        + utils::FormatBytes(plan.totalBytes) + L"\uff0c\u8df3\u8fc7 "
        + std::to_wstring(plan.skipFiles) + L" \u4e2a");

    ProgressTracker progress;
    progress.Reset(plan.totalBytes);
    int completed = 0;
    TransferResult result;
    result.code = TransferResultCode::Success;
    result.message = L"\u4f20\u8f93\u5df2\u5b8c\u6210";

    const DWORD chunkSize = static_cast<DWORD>(AppConfig::CHUNK_SIZE);
    PreparedFileQueue preparedQueue(
        static_cast<size_t>(chunkSize) * 2,
        64);
    std::thread preparationThread(
        PrepareFiles,
        std::cref(plan),
        std::cref(sourceDir),
        chunkSize,
        std::ref(preparedQueue),
        std::cref(m_running));

    auto stopPreparation = [&]() {
        preparedQueue.Cancel();
        if (preparationThread.joinable())
            preparationThread.join();
    };

    try {
    for (size_t planIndex = 0; planIndex < plan.entries.size(); ++planIndex) {
        const auto& entry = plan.entries[planIndex];
        if (!m_running) {
            result = MakeResult(TransferResultCode::Cancelled, L"\u4f20\u8f93\u5df2\u53d6\u6d88");
            break;
        }

        if (entry.action == FileAction::SKIP) {
            if (UseDetailedFileLogs(plan.entries.size()))
                Log(L"\u5df2\u8df3\u8fc7: " + entry.relativePath);
            m_stats.currentFile = entry.relativePath;
            ReportProgress();
            continue;
        }

        if (entry.action != FileAction::TRANSFER && entry.action != FileAction::OVERWRITE)
            continue;

        int64_t fileSize = entry.size;
        int64_t offset = entry.offset;
        if (offset < 0 || offset > fileSize)
            offset = 0;

        m_stats.currentFile = entry.relativePath;
        ReportProgress();

        PreparedFileEvent prepared;
        if (!preparedQueue.Pop(prepared, m_running)) {
            result = m_running
                ? MakeResult(TransferResultCode::InternalError,
                    L"\u6587\u4ef6\u51c6\u5907\u7ebf\u7a0b\u63d0\u524d\u7ed3\u675f")
                : MakeResult(TransferResultCode::Cancelled, L"\u4f20\u8f93\u5df2\u53d6\u6d88");
            break;
        }

        if (prepared.type == PreparedEventType::PreparationFailed) {
            Log(L"\u6587\u4ef6\u51c6\u5907\u7ebf\u7a0b\u53d1\u751f\u5f02\u5e38");
            m_stats.failedFiles++;
            result = MakeResult(TransferResultCode::InternalError,
                L"\u6587\u4ef6\u51c6\u5907\u5931\u8d25");
            break;
        }

        if (prepared.planIndex != planIndex) {
            Log(L"\u6587\u4ef6\u51c6\u5907\u961f\u5217\u987a\u5e8f\u9519\u8bef");
            m_stats.failedFiles++;
            result = MakeResult(TransferResultCode::InternalError,
                L"\u6587\u4ef6\u51c6\u5907\u961f\u5217\u987a\u5e8f\u9519\u8bef");
            break;
        }

        if (prepared.type == PreparedEventType::OpenFailed) {
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

        if (prepared.type != PreparedEventType::FileStart) {
            Log(L"\u6587\u4ef6\u51c6\u5907\u961f\u5217\u7f3a\u5c11\u6587\u4ef6\u5934: " + entry.relativePath);
            m_stats.failedFiles++;
            result = MakeResult(TransferResultCode::InternalError,
                L"\u6587\u4ef6\u51c6\u5907\u961f\u5217\u6570\u636e\u9519\u8bef");
            break;
        }

        std::wstring headerStr = entry.relativePath + L"\n" + std::to_wstring(fileSize)
            + L"\n" + std::to_wstring(offset);
        {
            IoResult io = SendStringPacketResult(
                sock,
                static_cast<uint8_t>(PacketType::FILE_HEADER),
                headerStr);

            if (!io.IsOk()) {
                if (io.status != IoStatus::Cancelled) {
                    m_stats.interruptedFiles++;
                    m_stats.resumable = true;
                }
                result = MakeIoFailureResult(io, L"\u53d1\u9001\u6587\u4ef6\u5934\u5931\u8d25");
                break;
            }
        }

        int64_t bytesRemaining = fileSize - offset;
        std::string sha256Hex;
        bool fileAborted = false;
        bool stopTransfer = false;

        auto sendFileAbort = [&](const std::wstring& errorCode) {
            IoResult io = SendStringPacketResult(
                sock,
                static_cast<uint8_t>(PacketType::ERROR_MSG),
                errorCode);
            if (!io.IsOk()) {
                result = MakeIoFailureResult(io, L"\u53d1\u9001\u6587\u4ef6\u9519\u8bef\u4fe1\u606f\u5931\u8d25");
                if (io.status != IoStatus::Cancelled) {
                    m_stats.interruptedFiles++;
                    m_stats.resumable = true;
                }
                return false;
            }

            io = SendStringPacketResult(
                sock,
                static_cast<uint8_t>(PacketType::FILE_DONE),
                entry.relativePath);
            if (!io.IsOk()) {
                result = MakeIoFailureResult(io, L"\u53d1\u9001 FILE_DONE \u5931\u8d25");
                if (io.status != IoStatus::Cancelled) {
                    m_stats.interruptedFiles++;
                    m_stats.resumable = true;
                }
                return false;
            }
            return true;
        };

        while (m_running) {
            PreparedFileEvent next;
            if (!preparedQueue.Pop(next, m_running)) {
                result = m_running
                    ? MakeResult(TransferResultCode::InternalError,
                        L"\u6587\u4ef6\u51c6\u5907\u6570\u636e\u4e0d\u5b8c\u6574")
                    : MakeResult(TransferResultCode::Cancelled, L"\u4f20\u8f93\u5df2\u53d6\u6d88");
                stopTransfer = true;
                break;
            }

            if (next.type == PreparedEventType::PreparationFailed ||
                next.planIndex != planIndex) {
                Log(L"\u6587\u4ef6\u51c6\u5907\u961f\u5217\u6570\u636e\u9519\u8bef: " + entry.relativePath);
                m_stats.failedFiles++;
                if (!sendFileAbort(L"READ_FAILED")) {
                    stopTransfer = true;
                    break;
                }
                result = MakeResult(TransferResultCode::InternalError,
                    L"\u6587\u4ef6\u51c6\u5907\u961f\u5217\u6570\u636e\u9519\u8bef");
                stopTransfer = true;
                break;
            }

            if (next.type == PreparedEventType::Data) {
                if (next.data.empty() ||
                    next.data.size() > static_cast<size_t>(bytesRemaining)) {
                    Log(L"\u6587\u4ef6\u51c6\u5907\u5b57\u8282\u6570\u9519\u8bef: " + entry.relativePath);
                    m_stats.failedFiles++;
                    if (!sendFileAbort(L"READ_FAILED")) {
                        stopTransfer = true;
                        break;
                    }
                    result = MakeResult(TransferResultCode::InternalError,
                        L"\u6587\u4ef6\u51c6\u5907\u5b57\u8282\u6570\u9519\u8bef");
                    stopTransfer = true;
                    break;
                }

                IoResult io = SendPacketResult(
                    sock,
                    static_cast<uint8_t>(PacketType::FILE_CHUNK),
                    next.data.data(),
                    next.data.size());
                if (!io.IsOk()) {
                    Log(L"\u53d1\u9001\u6570\u636e\u5931\u8d25: " + entry.relativePath);
                    result = MakeIoFailureResult(io, L"\u53d1\u9001\u6587\u4ef6\u6570\u636e\u5931\u8d25");
                    if (io.status != IoStatus::Cancelled) {
                        m_stats.interruptedFiles++;
                        m_stats.resumable = true;
                    }
                    stopTransfer = true;
                    break;
                }

                const int64_t sentBytes = static_cast<int64_t>(next.data.size());
                bytesRemaining -= sentBytes;
                progress.AddTransferred(sentBytes);
                UpdateProgressStats(progress, m_stats);
                m_stats.currentFile = entry.relativePath;
                ReportProgress();
                continue;
            }

            if (next.type == PreparedEventType::ReadFailed ||
                next.type == PreparedEventType::HashFailed) {
                const bool readFailed = next.type == PreparedEventType::ReadFailed;
                Log((readFailed
                    ? L"\u8bfb\u53d6\u6587\u4ef6\u5931\u8d25: "
                    : L"\u8ba1\u7b97\u6587\u4ef6 SHA-256 \u5931\u8d25: ") + entry.relativePath);
                m_stats.failedFiles++;
                if (!sendFileAbort(readFailed ? L"READ_FAILED" : L"HASH_FAILED")) {
                    stopTransfer = true;
                    break;
                }
                fileAborted = true;
                break;
            }

            if (next.type == PreparedEventType::FileEnd) {
                if (bytesRemaining != 0 || next.sha256.empty()) {
                    Log(L"\u6587\u4ef6\u51c6\u5907\u6570\u636e\u4e0d\u5b8c\u6574: " + entry.relativePath);
                    m_stats.failedFiles++;
                    if (!sendFileAbort(L"READ_FAILED")) {
                        stopTransfer = true;
                        break;
                    }
                    result = MakeResult(TransferResultCode::InternalError,
                        L"\u6587\u4ef6\u51c6\u5907\u6570\u636e\u4e0d\u5b8c\u6574");
                    stopTransfer = true;
                    break;
                }
                sha256Hex = std::move(next.sha256);
                break;
            }

            Log(L"\u6587\u4ef6\u51c6\u5907\u961f\u5217\u5305\u7c7b\u578b\u9519\u8bef: " + entry.relativePath);
            m_stats.failedFiles++;
            if (!sendFileAbort(L"READ_FAILED"))
                stopTransfer = true;
            result = MakeResult(TransferResultCode::InternalError,
                L"\u6587\u4ef6\u51c6\u5907\u961f\u5217\u5305\u7c7b\u578b\u9519\u8bef");
            stopTransfer = true;
            break;
        }

        if (!m_running) {
            result = MakeResult(TransferResultCode::Cancelled, L"\u4f20\u8f93\u5df2\u53d6\u6d88");
            break;
        }
        if (stopTransfer)
            break;
        if (fileAborted)
            continue;

        // Send the hash produced by the disk preparation thread.
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
                } else if (io.IsOk()) {
                    result = MakeResult(TransferResultCode::ProtocolError,
                        L"\u63a5\u6536\u7aef\u8fd4\u56de\u4e86\u975e FILE_DONE_ACK \u6570\u636e\u5305");
                    m_stats.interruptedFiles++;
                    m_stats.resumable = true;
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
            if (UseDetailedFileLogs(plan.entries.size())) {
                Log(L"\u5df2\u5b8c\u6210: " + entry.relativePath);
            } else if (ShouldLogFileBatch(completed, plan.totalFiles)) {
                Log(L"\u5df2\u5b8c\u6210 " + std::to_wstring(completed) + L"/"
                    + std::to_wstring(plan.totalFiles) + L" \u4e2a\u6587\u4ef6\uff0c\u6700\u8fd1: "
                    + entry.relativePath);
            }
        }
        ReportProgress();
    }
    } catch (...) {
        stopPreparation();
        throw;
    }

    stopPreparation();
    ReportProgress(true);

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
    } else if (result.code != TransferResultCode::Success || m_stats.failedFiles > 0) {
        if (result.code == TransferResultCode::Success ||
            result.code == TransferResultCode::FileError) {
            result.code = TransferResultCode::FileError;
            result.message = L"\u90e8\u5206\u6587\u4ef6\u4f20\u8f93\u5931\u8d25";
        }
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

    UpdateProgressStats(progress, m_stats);

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
    const std::wstring& sessionToken, TransferMode mode)
{
    StopHeartbeat();

    try {
        InnerReceiverWorker(targetDir, port, sessionToken, mode);
    } catch (const std::exception& e) {
        NotifyDone(TransferResultCode::InternalError, L"\u63a5\u6536\u7ebf\u7a0b\u5f02\u5e38");
    } catch (...) {
        NotifyDone(TransferResultCode::InternalError, L"\u63a5\u6536\u7ebf\u7a0b\u53d1\u751f\u672a\u77e5\u5f02\u5e38");
    }
    StopHeartbeat();
    {
        std::lock_guard<std::mutex> lock(m_sockMutex);
        if (m_sock != INVALID_SOCKET)
            shutdown(m_sock, SD_BOTH);
    }
    StopReceiveLoop();
}

void TransferSession::InnerReceiverWorker(const std::wstring& targetDir, int port,
    const std::wstring& sessionToken, TransferMode mode)
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
            HandleReceiverConnection(sock, targetDir, sessionToken, mode);

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

        case ReceiverConnectionResult::HandshakeRejected:
            Log(L"\u81ea\u52a8\u8bc6\u522b\u5931\u8d25\uff0c\u7b49\u5f85\u4e0b\u4e00\u6b21\u8fde\u63a5...");
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
    const std::wstring& sessionToken, TransferMode mode)
{
    Log(L"\u5df2\u8fde\u63a5\uff0c\u6b63\u5728\u8fdb\u884c DirectTransfer \u81ea\u52a8\u8bc6\u522b...");

    if (!SendStringPacket(sock, (uint8_t)PacketType::SERVER_HELLO,
            BuildHelloPayload(sessionToken))) {
        Log(L"\u53d1\u9001\u63a5\u6536\u7aef\u8bc6\u522b\u4fe1\u606f\u5931\u8d25");
        return ReceiverConnectionResult::Failed;
    }

    std::wstring clientHello = RecvStringPacket(sock, (uint8_t)PacketType::CLIENT_HELLO);
    std::wstring clientToken;
    std::wstring peerVersion;
    if (!ParseHelloPayload(clientHello, clientToken, peerVersion) ||
        clientToken != sessionToken) {
        Log(L"\u5bf9\u7aef\u534f\u8bae\u3001\u7248\u672c\u6216\u4f1a\u8bdd\u4fe1\u606f\u4e0d\u5339\u914d");
        SendStringPacket(sock, (uint8_t)PacketType::HELLO_ACK, L"FAIL");
        return ReceiverConnectionResult::HandshakeRejected;
    }

    SendStringPacket(sock, (uint8_t)PacketType::HELLO_ACK, L"OK");
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
    Log(L"\u5df2\u81ea\u52a8\u8bc6\u522b\u53d1\u9001\u7aef\uff0c\u5bf9\u7aef\u7248\u672c " + peerVersion);
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
    ULONGLONG lastPlanProgress = 0;
    auto reportPlanProgress = [&](const TransferPlanner::Progress& progress) {
        if (!m_running) return false;
        m_stats.stage = TransferStage::BuildingPlan;
        m_stats.stageText = progress.stage;
        m_stats.stageProcessed = progress.processedFiles;
        m_stats.stageTotal = progress.totalFiles;
        m_stats.stageBytes = progress.hashedBytes;
        m_stats.currentFile = progress.currentPath;
        ReportProgress();

        const ULONGLONG now = GetTickCount64();
        if (lastPlanProgress == 0 || now - lastPlanProgress >= 500) {
            if (!SendStringPacket(sock, (uint8_t)PacketType::PLAN_PROGRESS,
                    SerializePlanProgress(progress)))
                return false;
            lastPlanProgress = now;
        }
        return true;
    };

    if (mode == TransferMode::MIRROR) {
        extraFiles = TransferPlanner::FindExtraFiles(manifest, targetDir,
            reportPlanProgress);
        if (!m_running || m_connectionLost)
            return m_connectionLost ? ReceiverConnectionResult::ConnectionLost
                                    : ReceiverConnectionResult::Cancelled;
    }

    Log(L"\u6b63\u5728\u751f\u6210\u4f20\u8f93\u8ba1\u5212...");

    TransferPlanner planner;
    TransferPlanner::Plan plan = planner.BuildPlan(manifest, targetDir, mode,
        reportPlanProgress);
    if (!m_running || m_connectionLost) {
        return m_connectionLost ? ReceiverConnectionResult::ConnectionLost
                                : ReceiverConnectionResult::Cancelled;
    }
    if (plan.entries.size() != manifest.GetEntries().size()) {
        const std::wstring error = L"\u751f\u6210\u4f20\u8f93\u8ba1\u5212\u65f6\u8bfb\u53d6\u6216\u6821\u9a8c\u6587\u4ef6\u5931\u8d25";
        Log(error);
        SendStringPacket(sock, (uint8_t)PacketType::ERROR_MSG, error);
        return ReceiverConnectionResult::Failed;
    }

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
            SendStringPacket(sock, (uint8_t)PacketType::ERROR_MSG,
                L"\u65e0\u6cd5\u9a8c\u8bc1\u6216\u91cd\u7f6e\u7eed\u4f20\u6587\u4ef6");
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
    m_stats.stage = TransferStage::Transferring;
    m_stats.stageText = L"\u6b63\u5728\u63a5\u6536\u6587\u4ef6";

    ProgressTracker progress;
    progress.Reset(plan.totalBytes);

    std::unordered_map<std::wstring, int64_t> plannedRemainingBytes;
    std::unordered_map<std::wstring, bool> plannedSkipFiles;
    std::unordered_set<std::wstring> preparedDirectories;
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
                progress.SetTotal(m_stats.totalBytes);
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
            if (UseDetailedFileLogs(plan.entries.size()))
                Log(L"\u63a5\u6536: " + fileName);

            // Create target directory recursively
            std::wstring dir = fullPath.substr(0, fullPath.find_last_of(L'\\'));
            if (preparedDirectories.find(dir) == preparedDirectories.end() &&
                utils::CreateDirectoryTree(dir)) {
                preparedDirectories.insert(dir);
            }

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
                    progress.AddTransferred(static_cast<int64_t>(chunk.size()));
                    UpdateProgressStats(progress, m_stats);
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
                } else if (UseDetailedFileLogs(plan.entries.size())) {
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
                if (UseDetailedFileLogs(plan.entries.size())) {
                    Log(L"\u5b8c\u6210: " + fileName);
                } else if (ShouldLogFileBatch(received, m_stats.totalFiles)) {
                    Log(L"\u5df2\u63a5\u6536 " + std::to_wstring(received) + L"/"
                        + std::to_wstring(m_stats.totalFiles) + L" \u4e2a\u6587\u4ef6\uff0c\u6700\u8fd1: "
                        + fileName);
                }
            } else {
                m_stats.failedFiles++;
                receiverFailed = true;
                failureMessage = L"\u6587\u4ef6\u9a8c\u8bc1\u5931\u8d25: " + fileName;
                break;
            }
        }
    }

    UpdateProgressStats(progress, m_stats);
    ReportProgress(true);
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
