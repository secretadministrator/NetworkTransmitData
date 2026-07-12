#pragma once
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "TransferPlanner.h"

enum class SessionRole { NONE, SENDER, RECEIVER };

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
    Transferring,
    Reconnecting,
    Committing
};

struct TransferStats {
    int64_t totalBytes = 0;
    int64_t transferredBytes = 0;
    int64_t displayTransferredBytes = 0;
    int64_t speedBytesPerSec = 0;
    int64_t recentSpeedBytesPerSec = 0;
    int64_t averageSpeedBytesPerSec = 0;
    int64_t estimatedRemainingSeconds = -1;
    int64_t elapsedSeconds = 0;
    int64_t estimatedTotalSeconds = -1;
    int64_t overallWorkTotal = 0;
    int64_t overallWorkCompleted = 0;
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
    int retryCount = 0;
    bool resumable = false;
    std::wstring currentFile;
    TransferStage stage = TransferStage::Idle;
    uint64_t stageProcessed = 0;
    uint64_t stageTotal = 0;
    int64_t stageBytes = 0;
    std::wstring stageText;
};

struct TransferFailureInfo {
    std::wstring relativePath;
    std::wstring reason;
    DWORD systemError = 0;
    int attempts = 0;
};

struct TransferResult {
    TransferResultCode code = TransferResultCode::InternalError;
    TransferRole role = TransferRole::NONE;
    int socketError = 0;
    bool resumable = false;
    std::wstring message;
    TransferStats stats;
    std::vector<TransferFailureInfo> failedFiles;
};

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
    using LogCallback = std::function<void(const std::wstring&)>;
    using DoneCallback = std::function<void(const TransferResult&)>;

    void SetProgressCallback(ProgressCallback cb) { m_progressCb = std::move(cb); }
    void SetLogCallback(LogCallback cb) { m_logCb = std::move(cb); }
    void SetDoneCallback(DoneCallback cb) { m_doneCb = std::move(cb); }
    const TransferStats& GetStats() const { return m_stats; }

private:
    struct SenderContext;
    struct ReceiverTaskState;

    SOCKET m_sock = INVALID_SOCKET;
    SOCKET m_listenSock = INVALID_SOCKET;
    std::atomic<bool> m_running{false};
    std::mutex m_sockMutex;
    std::mutex m_sendMutex;
    std::thread m_workerThread;
    SessionRole m_role = SessionRole::NONE;
    TransferStats m_stats;
    ProgressCallback m_progressCb;
    LogCallback m_logCb;
    DoneCallback m_doneCb;
    std::atomic<int> m_lastSocketError{0};
    std::atomic<ULONGLONG> m_lastProgressReportTick{0};
    std::mutex m_failureMutex;
    std::vector<TransferFailureInfo> m_failedFiles;

    static constexpr DWORD HANDSHAKE_TIMEOUT_MS = 30000;
    static constexpr DWORD IO_WAIT_TIMEOUT_MS = 5000;
    static constexpr ULONGLONG CONNECTION_IDLE_MS = 90000;
    static constexpr ULONGLONG PROGRESS_REPORT_INTERVAL_MS = 100;

    void SenderWorker(const std::wstring& sourceDir, const std::wstring& peerIP,
        int port, const std::wstring& sessionToken);
    TransferResult RunSender(const std::wstring& sourceDir, const std::wstring& peerIP,
        int port, const std::wstring& sessionToken);
    TransferResult RunSenderConnection(const std::wstring& sourceDir,
        const std::wstring& peerIP, int port, const std::wstring& sessionToken,
        SenderContext& context);
    void ReceiverWorker(const std::wstring& targetDir, int port,
        const std::wstring& sessionToken, TransferMode mode);
    void RunReceiver(const std::wstring& targetDir, int port,
        const std::wstring& sessionToken, TransferMode mode);
    ReceiverConnectionResult HandleReceiverConnection(SOCKET sock,
        const std::wstring& targetDir, const std::wstring& sessionToken,
        TransferMode mode, ReceiverTaskState& state);

    IoResult SendAll(SOCKET sock, const uint8_t* data, size_t length);
    IoResult RecvExact(SOCKET sock, uint8_t* data, size_t length);
    IoResult SendPacket(SOCKET sock, uint8_t type, const uint8_t* payload, size_t size);
    IoResult SendPacket(SOCKET sock, uint8_t type, const std::vector<uint8_t>& payload);
    IoResult RecvPacket(SOCKET sock, uint8_t& type, std::vector<uint8_t>& payload);
    IoResult SendString(SOCKET sock, uint8_t type, const std::wstring& text);
    IoResult RecvString(SOCKET sock, uint8_t expectedType, std::wstring& text);

    void CloseActiveSocket(SOCKET expected = INVALID_SOCKET);
    void Log(const std::wstring& message);
    void RecordFileFailure(const std::wstring& relativePath,
        const std::wstring& reason, DWORD systemError, int attempts, bool finalFailure);
    void ReportProgress(bool force = false);
    TransferResult FinalizeResult(TransferResult result);
};
