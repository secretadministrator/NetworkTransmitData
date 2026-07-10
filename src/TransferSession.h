#pragma once
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <string>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <vector>
#include "TransferPlanner.h"

enum class SessionRole { NONE, SENDER, RECEIVER };

struct TransferStats {
    int64_t totalBytes = 0;
    int64_t transferredBytes = 0;
    int64_t speedBytesPerSec = 0;
    int totalFiles = 0;
    int completedFiles = 0;
    int skippedFiles = 0;
    int failedFiles = 0;
    std::wstring currentFile;
};

enum class TransferResult {
    Success,
    PartialSuccess,
    Failed,
    Cancelled,
    ConnectionLost
};

struct TransferCompletion {
    TransferResult result;
    TransferStats stats;
    std::wstring errorMessage;
};

class TransferSession {
public:
    TransferSession();
    ~TransferSession();

    bool StartAsSender(const std::wstring& sourceDir, const std::wstring& peerIP,
        int port, const std::wstring& pairingCode);
    bool StartAsReceiver(const std::wstring& targetDir, int port,
        const std::wstring& expectedPairingCode, TransferMode mode = TransferMode::SAFE_COPY);
    void Stop();
    bool IsRunning() const { return m_running; }

    using ProgressCallback = std::function<void(const TransferStats&)>;
    void SetProgressCallback(ProgressCallback cb) { m_progressCb = cb; }

    using LogCallback = std::function<void(const std::wstring&)>;
    void SetLogCallback(LogCallback cb) { m_logCb = cb; }

    using DoneCallback = std::function<void(const TransferCompletion&)>;
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

    void SenderWorker(const std::wstring& sourceDir, const std::wstring& peerIP,
        int port, const std::wstring& pairingCode);
    void InnerSenderWorker(const std::wstring& sourceDir, const std::wstring& peerIP,
        int port, const std::wstring& pairingCode);
    void ReceiverWorker(const std::wstring& targetDir, int port,
        const std::wstring& expectedPairingCode, TransferMode mode);
    void InnerReceiverWorker(const std::wstring& targetDir, int port,
        const std::wstring& expectedPairingCode, TransferMode mode);
    bool HandleReceiverConnection(SOCKET sock, const std::wstring& targetDir,
        const std::wstring& expectedPairingCode, TransferMode mode);

    bool SendPacket(SOCKET sock, uint8_t type, const std::vector<uint8_t>& payload);
    bool SendPacket(SOCKET sock, uint8_t type, const uint8_t* payload, size_t payloadSize);
    bool RecvPacket(SOCKET sock, uint8_t& type, std::vector<uint8_t>& payload);
    bool SendStringPacket(SOCKET sock, uint8_t type, const std::wstring& str);
    std::wstring RecvStringPacket(SOCKET sock, uint8_t expectedType);

    void Log(const std::wstring& msg);
    void ReportProgress();
};
