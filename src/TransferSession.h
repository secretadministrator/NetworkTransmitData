#pragma once
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <string>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <cstdint>
#include <vector>
#include "TransferPlanner.h"

enum class SessionRole { NONE, SENDER, RECEIVER };

// ── I/O result types ──
enum class IoStatus {
    Ok,
    Cancelled,
    PeerClosed,
    IdleTimeout,
    SocketError,
    ProtocolError
};

struct IoResult {
    IoStatus status = IoStatus::Ok;
    int wsaError = 0;

    bool IsOk() const { return status == IoStatus::Ok; }
};

// ── Transfer result codes ──
enum class TransferResultCode {
    Success,
    Cancelled,
    ConnectionLost,
    FileError,
    ProtocolError,
    InternalError
};

enum class TransferRole { NONE, SENDER, RECEIVER };

enum class TransferStage {
    Idle,
    ScanningSource,
    HashingSource,
    WaitingForPlan,
    BuildingPlan,
    VerifyingPlan,
    Transferring
};

struct TransferStats {
    int64_t totalBytes = 0;
    int64_t transferredBytes = 0;
    int64_t speedBytesPerSec = 0;
    int64_t recentSpeedBytesPerSec = 0;
    int64_t averageSpeedBytesPerSec = 0;
    int64_t estimatedRemainingSeconds = -1;
    bool waitingForIo = false;
    bool scanning = false;
    uint64_t scannedDirectories = 0;
    uint64_t inaccessibleDirectories = 0;
    int64_t scannedBytes = 0;
    int totalFiles = 0;
    int completedFiles = 0;
    int skippedFiles = 0;
    int failedFiles = 0;
    int interruptedFiles = 0;
    bool resumable = false;
    std::wstring currentFile;
    TransferStage stage = TransferStage::Idle;
    uint64_t stageProcessed = 0;
    uint64_t stageTotal = 0;
    int64_t stageBytes = 0;
    std::wstring stageText;
};

struct TransferResult {
    TransferResultCode code = TransferResultCode::InternalError;
    TransferRole role = TransferRole::NONE;
    int socketError = 0;
    bool resumable = false;
    std::wstring message;
    TransferStats stats;
};

// ── Receiver connection result ──
enum class ReceiverConnectionResult {
    Completed,
    HandshakeRejected,
    ConnectionLost,
    Cancelled,
    Failed
};

class TransferSession {
public:
    TransferSession();
    ~TransferSession();

    bool StartAsSender(const std::wstring& sourceDir, const std::wstring& peerIP,
        int port, const std::wstring& sessionToken);
    bool StartAsReceiver(const std::wstring& targetDir, int port,
        const std::wstring& sessionToken, TransferMode mode = TransferMode::SAFE_COPY);
    void Stop();
    bool IsRunning() const { return m_running; }

    using ProgressCallback = std::function<void(const TransferStats&)>;
    void SetProgressCallback(ProgressCallback cb) { m_progressCb = cb; }

    using LogCallback = std::function<void(const std::wstring&)>;
    void SetLogCallback(LogCallback cb) { m_logCb = cb; }

    using DoneCallback = std::function<void(const TransferResult&)>;
    void SetDoneCallback(DoneCallback cb) { m_doneCb = cb; }

    const TransferStats& GetStats() const { return m_stats; }

private:
    SOCKET m_sock = INVALID_SOCKET;
    SOCKET m_listenSock = INVALID_SOCKET;
    std::atomic<bool> m_running{false};
    std::mutex m_sockMutex;
    std::thread m_workerThread;
    SessionRole m_role = SessionRole::NONE;
    TransferStats m_stats;
    ProgressCallback m_progressCb;
    LogCallback m_logCb;
    DoneCallback m_doneCb;

    // ── Connection state ──
    std::mutex m_sendMutex;

    std::thread m_heartbeatThread;
    std::atomic<bool> m_heartbeatRunning{false};
    std::atomic<bool> m_connectionLost{false};

    std::atomic<ULONGLONG> m_lastSendProgressTick{0};
    std::atomic<ULONGLONG> m_lastRecvProgressTick{0};

    std::atomic<int> m_lastSocketError{0};
    std::atomic<uint64_t> m_heartbeatSequence{0};
    std::atomic<ULONGLONG> m_lastPeerActivityTick{0};

    // ── Constants ──
    static constexpr DWORD IO_WAIT_TIMEOUT_MS = 5000;
    static constexpr ULONGLONG CONNECTION_IDLE_MS = 90000;
    static constexpr ULONGLONG HEARTBEAT_INTERVAL_MS = 15000;
    static constexpr ULONGLONG PROGRESS_REPORT_INTERVAL_MS = 100;
    static constexpr int MAX_TIMEOUT_RETRIES = 18;
    static constexpr DWORD PLAN_PROGRESS_TIMEOUT_MS = 90000;

    std::atomic<ULONGLONG> m_lastProgressReportTick{0};

    void SenderWorker(const std::wstring& sourceDir, const std::wstring& peerIP,
        int port, const std::wstring& sessionToken);
    TransferResult InnerSenderWorker(const std::wstring& sourceDir, const std::wstring& peerIP,
        int port, const std::wstring& sessionToken);
    void ReceiverWorker(const std::wstring& targetDir, int port,
        const std::wstring& sessionToken, TransferMode mode);
    void InnerReceiverWorker(const std::wstring& targetDir, int port,
        const std::wstring& sessionToken, TransferMode mode);
    ReceiverConnectionResult HandleReceiverConnection(SOCKET sock, const std::wstring& targetDir,
        const std::wstring& sessionToken, TransferMode mode);

    // ── Legacy boolean wrappers (preserved for compatibility) ──
    bool SendPacket(SOCKET sock, uint8_t type, const std::vector<uint8_t>& payload);
    bool SendPacket(SOCKET sock, uint8_t type, const uint8_t* payload, size_t payloadSize);
    bool RecvPacket(SOCKET sock, uint8_t& type, std::vector<uint8_t>& payload);
    bool SendStringPacket(SOCKET sock, uint8_t type, const std::wstring& str);
    std::wstring RecvStringPacket(SOCKET sock, uint8_t expectedType);

    // ── New retryable I/O methods ──
    IoResult SendAllLocked(SOCKET sock, const uint8_t* data, size_t length);
    IoResult RecvExact(SOCKET sock, uint8_t* data, size_t length);
    IoResult SendPacketResult(SOCKET sock, uint8_t type,
        const uint8_t* payload, size_t payloadSize);
    IoResult RecvRawPacket(SOCKET sock, uint8_t& type, std::vector<uint8_t>& payload);
    IoResult RecvPacketResult(SOCKET sock, uint8_t& type,
        std::vector<uint8_t>& payload, DWORD businessTimeoutMs = INFINITE);

    // ── Heartbeat ──
    void StartHeartbeat(SOCKET sock);
    void StopHeartbeat();

    void MarkConnectionLost(SOCKET sock, const IoResult& result, const std::wstring& operation);
    static bool IsTemporarySocketError(int error);

    // ── String packet result helpers ──
    IoResult SendStringPacketResult(SOCKET sock, uint8_t type, const std::wstring& text);
    IoResult RecvStringPacketResult(SOCKET sock, uint8_t expectedType, std::wstring& output);

    TransferResult MakeIoFailureResult(const IoResult& io, const std::wstring& operation);

    // ── Receive loop (dedicated thread for heartbeat & packet queue) ──
    struct ReceivedPacket {
        uint8_t type = 0;
        std::vector<uint8_t> payload;
    };

    std::thread m_receiveThread;
    std::mutex m_packetQueueMutex;
    std::condition_variable m_packetQueueCv;
    std::deque<ReceivedPacket> m_packetQueue;
    size_t m_packetQueueBytes = 0;
    std::atomic<bool> m_receiveLoopRunning{false};

    static constexpr size_t MAX_PACKET_QUEUE_BYTES = 8 * 1024 * 1024;
    static constexpr size_t MAX_PACKET_QUEUE_COUNT = 64;

    void StartReceiveLoop(SOCKET sock);
    void StopReceiveLoop();
    void ReceiveLoop(SOCKET sock);

    // ── Resume entry validation ──
    static bool TryGetFileSize(const std::wstring& path, int64_t& size);
    static bool ValidateResumeEntry(
        PlanEntry& entry,
        const std::wstring& targetRoot);

    // ── Finalize result ──
    TransferResult FinalizeResult(TransferResult result);

    // ── Helpers ──
    void NotifyDone(TransferResultCode code, const std::wstring& message);
    void Log(const std::wstring& msg);
    void ReportProgress(bool force = false);
};
