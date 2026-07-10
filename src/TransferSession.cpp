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

void TransferSession::NotifyDone(TransferOutcome outcome, const std::wstring& message) {
    if (!m_doneCb) return;
    TransferResult result;
    result.outcome = outcome;
    result.message = message;
    result.stats = m_stats;
    m_doneCb(result);
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

static TransferResult MakeResult(TransferOutcome outcome, const std::wstring& msg) {
    TransferResult r;
    r.outcome = outcome;
    r.message = msg;
    return r;
}

static bool SendAll(SOCKET sock, const uint8_t* data, size_t len) {
    while (len > 0) {
        int toSend = (len > INT_MAX) ? INT_MAX : (int)len;
        int sent = send(sock, (const char*)data, toSend, 0);
        if (sent <= 0) return false;
        data += sent;
        len -= (size_t)sent;
    }
    return true;
}

bool TransferSession::SendPacket(SOCKET sock, uint8_t type, const std::vector<uint8_t>& payload) {
    const uint8_t* data = payload.empty() ? nullptr : payload.data();
    return SendPacket(sock, type, data, payload.size());
}

bool TransferSession::SendPacket(SOCKET sock, uint8_t type, const uint8_t* payload, size_t payloadSize) {
    if (payloadSize > MAX_PAYLOAD_BYTES) return false;

    uint32_t len = (uint32_t)payloadSize;
    uint8_t header[5] = {};
    header[0] = type;
    header[1] = (uint8_t)(len & 0xFF);
    header[2] = (uint8_t)((len >> 8) & 0xFF);
    header[3] = (uint8_t)((len >> 16) & 0xFF);
    header[4] = (uint8_t)((len >> 24) & 0xFF);

    if (!SendAll(sock, header, sizeof(header))) return false;
    if (len > 0) {
        if (!SendAll(sock, payload, payloadSize)) return false;
    }
    return true;
}

bool TransferSession::RecvPacket(SOCKET sock, uint8_t& type, std::vector<uint8_t>& payload) {
    uint8_t header[5] = {};
    int received = 0;
    while (received < 5) {
        int r = recv(sock, (char*)header + received, 5 - received, 0);
        if (r <= 0) return false;
        received += r;
    }
    type = header[0];
    uint32_t len = (uint32_t)header[1] | ((uint32_t)header[2] << 8)
        | ((uint32_t)header[3] << 16) | ((uint32_t)header[4] << 24);

    if (len > MAX_PAYLOAD_BYTES) return false;

    payload.resize(len);
    received = 0;
    while (received < (int)len) {
        int r = recv(sock, (char*)payload.data() + received, len - received, 0);
        if (r <= 0) return false;
        received += r;
    }
    return true;
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

static void SetSocketTimeouts(SOCKET sock, DWORD timeoutMs) {
    DWORD timeout = timeoutMs;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));
}

static void EnableLowLatencyTcp(SOCKET sock) {
    BOOL noDelay = TRUE;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&noDelay, sizeof(noDelay));
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
    TransferResult result;
    try {
        result = InnerSenderWorker(sourceDir, peerIP, port, pairingCode);
    } catch (const std::exception& e) {
        result.outcome = TransferOutcome::Failed;
        result.message = L"\u53d1\u9001\u7ebf\u7a0b\u5f02\u5e38";
        result.stats = m_stats;
    } catch (...) {
        result.outcome = TransferOutcome::Failed;
        result.message = L"\u53d1\u9001\u7ebf\u7a0b\u53d1\u751f\u672a\u77e5\u5f02\u5e38";
        result.stats = m_stats;
    }
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
        WSACleanup(); return MakeResult(TransferOutcome::Failed, L"\u521b\u5efa Socket \u5931\u8d25");
    }
    m_sock = sock;
    EnableLowLatencyTcp(sock);
    SetSocketTimeouts(sock, HANDSHAKE_TIMEOUT_MS);

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    std::string ipA = utils::ToUtf8(peerIP);
    if (InetPtonA(AF_INET, ipA.c_str(), &addr.sin_addr) != 1) {
        Log(L"\u5bf9\u65b9 IP \u5730\u5740\u65e0\u6548: " + peerIP);
        closesocket(sock); m_sock = INVALID_SOCKET;
        WSACleanup(); return MakeResult(TransferOutcome::Failed, L"\u5bf9\u65b9 IP \u5730\u5740\u65e0\u6548");
    }

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        std::wstring errMsg = L"\u8fde\u63a5\u63a5\u6536\u7aef\u5931\u8d25\uff0c\u9519\u8bef\u7801: " + std::to_wstring(WSAGetLastError());
        Log(errMsg);
        closesocket(sock); m_sock = INVALID_SOCKET;
        WSACleanup(); return MakeResult(TransferOutcome::Failed, errMsg);
    }

    Log(L"\u5df2\u8fde\u63a5\uff0c\u6b63\u5728\u9a8c\u8bc1\u914d\u5bf9\u7801...");

    if (!SendStringPacket(sock, (uint8_t)PacketType::PAIRING_REQUEST, pairingCode)) {
        Log(L"\u53d1\u9001\u914d\u5bf9\u7801\u5931\u8d25");
        closesocket(sock); m_sock = INVALID_SOCKET;
        WSACleanup(); return MakeResult(TransferOutcome::Failed, L"\u53d1\u9001\u914d\u5bf9\u7801\u5931\u8d25");
    }

    std::wstring resp = RecvStringPacket(sock, (uint8_t)PacketType::PAIRING_RESPONSE);
    if (resp != L"OK") {
        Log(L"\u914d\u5bf9\u7801\u9a8c\u8bc1\u5931\u8d25");
        closesocket(sock); m_sock = INVALID_SOCKET;
        WSACleanup(); return MakeResult(TransferOutcome::Failed, L"\u914d\u5bf9\u7801\u9a8c\u8bc1\u5931\u8d25");
    }
    SetSocketTimeouts(sock, 0);

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
        WSACleanup(); return MakeResult(TransferOutcome::Cancelled, L"\u4f20\u8f93\u5df2\u53d6\u6d88");
    }

    Manifest manifest(files);
    Log(L"\u5171 " + std::to_wstring(files.size()) + L" \u4e2a\u6587\u4ef6\uff0c"
        + utils::FormatBytes(manifest.GetTotalSize()));

    std::string manifestJson = manifest.ToJSON();
    std::vector<uint8_t> manifestPayload(manifestJson.begin(), manifestJson.end());
    if (!SendPacket(sock, (uint8_t)PacketType::MANIFEST, manifestPayload)) {
        Log(L"\u53d1\u9001\u6587\u4ef6\u6e05\u5355\u5931\u8d25");
        closesocket(sock); m_sock = INVALID_SOCKET;
        WSACleanup(); return MakeResult(TransferOutcome::Failed, L"\u53d1\u9001\u6587\u4ef6\u6e05\u5355\u5931\u8d25");
    }

    Log(L"\u5df2\u53d1\u9001\u6587\u4ef6\u6e05\u5355\uff0c\u7b49\u5f85\u63a5\u6536\u7aef\u8ba1\u5212...");

    std::wstring planStr = RecvStringPacket(sock, (uint8_t)PacketType::TRANSFER_PLAN);
    if (planStr.empty()) {
        Log(L"\u63a5\u6536\u4f20\u8f93\u8ba1\u5212\u5931\u8d25");
        closesocket(sock); m_sock = INVALID_SOCKET;
        WSACleanup(); return MakeResult(TransferOutcome::Failed, L"\u63a5\u6536\u4f20\u8f93\u8ba1\u5212\u5931\u8d25");
    }

    std::string planUtf8 = utils::ToUtf8(planStr);
    TransferPlanner::Plan plan = TransferPlanner::ParsePlanString(planUtf8);
    for (auto& entry : plan.entries) {
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
    result.outcome = TransferOutcome::Success;
    result.message = L"\u4f20\u8f93\u5df2\u5b8c\u6210";

    for (const auto& entry : plan.entries) {
        if (!m_running) {
            result = MakeResult(TransferOutcome::Cancelled, L"\u4f20\u8f93\u5df2\u53d6\u6d88");
            break;
        }

        // On-demand SKIP verification: receiver stored target hash in resumeHash
        if (entry.offset == 0 && !entry.resumeHash.empty()) {
            std::wstring skipCheckPath = sourceDir + L"\\" + entry.relativePath;
            std::string sourceHash = utils::ComputeSHA256(skipCheckPath);
            if (!sourceHash.empty() && sourceHash == entry.resumeHash) {
                Log(L"\u5df2\u8df3\u8fc7(\u54c8\u5e0c\u5339\u914d): " + entry.relativePath);
                completed++;
                m_stats.completedFiles = completed;
                m_stats.skippedFiles++;
                progress.AddTransferred(entry.size);
                m_stats.transferredBytes = progress.GetTransferred();
                m_stats.currentFile = entry.relativePath;
                ReportProgress();
                continue;
            }
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
            SendStringPacket(sock, (uint8_t)PacketType::ERROR_MSG, L"OPEN_FAILED: " + entry.relativePath);
            continue;
        }

        std::wstring headerStr = entry.relativePath + L"\n" + std::to_wstring(fileSize)
            + L"\n" + std::to_wstring(offset);
        if (!SendStringPacket(sock, (uint8_t)PacketType::FILE_HEADER, headerStr)) {
            Log(L"\u53d1\u9001\u6587\u4ef6\u5934\u5931\u8d25: " + entry.relativePath);
            m_stats.failedFiles++;
            CloseHandle(hFile);
            continue;
        }

        // Initialize incremental SHA-256
        Sha256Context hashCtx;
        bool hashOk = StartSha256(hashCtx);
        if (!hashOk) {
            Log(L"\u521d\u59cb\u5316 SHA-256 \u5931\u8d25: " + entry.relativePath);
            m_stats.failedFiles++;
            SendStringPacket(sock, (uint8_t)PacketType::ERROR_MSG, L"HASH_INIT_FAILED");
            SendStringPacket(sock, (uint8_t)PacketType::FILE_DONE, entry.relativePath);
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

            if (!SendPacket(sock, (uint8_t)PacketType::FILE_CHUNK, buf.data(), bytesRead)) {
                Log(L"\u53d1\u9001\u6570\u636e\u5931\u8d25: " + entry.relativePath);
                chunkSendFailed = true;
                break;
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
            result = MakeResult(TransferOutcome::Cancelled, L"\u4f20\u8f93\u5df2\u53d6\u6d88");
            break;
        }

        if (chunkSendFailed) {
            CloseSha256(hashCtx);
            m_stats.failedFiles++;
            result = MakeResult(TransferOutcome::Failed, L"\u53d1\u9001\u6570\u636e\u5931\u8d25");
            break;
        }

        if (readFileFailed || bytesRemaining > 0) {
            Log(L"\u8bfb\u53d6\u6587\u4ef6\u5931\u8d25: " + entry.relativePath);
            m_stats.failedFiles++;
            SendStringPacket(sock, (uint8_t)PacketType::ERROR_MSG, L"READ_FAILED");
            SendStringPacket(sock, (uint8_t)PacketType::FILE_DONE, entry.relativePath);
            CloseSha256(hashCtx);
            continue;
        }

        // Finalize SHA-256 and send FILE_HASH
        std::string sha256Hex = hashOk ? FinishSha256(hashCtx) : std::string();
        if (sha256Hex.empty()) {
            Log(L"\u8ba1\u7b97\u6587\u4ef6 SHA-256 \u5931\u8d25: " + entry.relativePath);
            m_stats.failedFiles++;
            SendStringPacket(sock, (uint8_t)PacketType::ERROR_MSG, L"HASH_FAILED");
            SendStringPacket(sock, (uint8_t)PacketType::FILE_DONE, entry.relativePath);
            continue;
        }
        {
            std::vector<uint8_t> hashPayload(sha256Hex.begin(), sha256Hex.end());
            if (!SendPacket(sock, (uint8_t)PacketType::FILE_HASH, hashPayload)) {
                Log(L"\u53d1\u9001\u6587\u4ef6\u6821\u9a8c\u503c\u5931\u8d25: " + entry.relativePath);
                m_stats.failedFiles++;
                result = MakeResult(TransferOutcome::Failed, L"\u53d1\u9001\u6587\u4ef6\u6821\u9a8c\u503c\u5931\u8d25");
                break;
            }
        }

        if (!SendStringPacket(sock, (uint8_t)PacketType::FILE_DONE, entry.relativePath)) {
            Log(L"\u53d1\u9001\u6587\u4ef6\u5b8c\u6210\u4fe1\u53f7\u5931\u8d25");
            m_stats.failedFiles++;
            result = MakeResult(TransferOutcome::Failed, L"\u53d1\u9001\u6587\u4ef6\u5b8c\u6210\u4fe1\u53f7\u5931\u8d25");
            break;
        }

        uint8_t ackType = 0;
        std::vector<uint8_t> ackPayload;
        if (!RecvPacket(sock, ackType, ackPayload) || ackType != (uint8_t)PacketType::FILE_DONE_ACK) {
            Log(L"\u63a5\u6536\u7aef\u9a8c\u8bc1\u54cd\u5e94\u5931\u8d25: " + entry.relativePath);
            m_stats.failedFiles++;
            result = MakeResult(TransferOutcome::Failed, L"\u63a5\u6536\u7aef\u9a8c\u8bc1\u54cd\u5e94\u5931\u8d25");
            break;
        }
        std::wstring ack = utils::FromUtf8(std::string((const char*)ackPayload.data(), ackPayload.size()));
        size_t ackSep = ack.find(L'\n');
        std::wstring ackStatus = (ackSep == std::wstring::npos) ? ack : ack.substr(0, ackSep);
        std::wstring ackFile = (ackSep == std::wstring::npos) ? L"" : ack.substr(ackSep + 1);
        if (ackStatus != L"OK" || ackFile != entry.relativePath) {
            Log(L"\u63a5\u6536\u7aef\u9a8c\u8bc1\u5931\u8d25: " + entry.relativePath
                + L" (" + (ack.empty() ? L"\u65e0\u54cd\u5e94" : ack) + L")");
            m_stats.failedFiles++;
            SendStringPacket(sock, (uint8_t)PacketType::ERROR_MSG, L"TRANSFER_ABORTED");
            result = MakeResult(TransferOutcome::Failed, L"\u63a5\u6536\u7aef\u9a8c\u8bc1\u5931\u8d25");
            break;
        } else {
            completed++;
            m_stats.completedFiles = completed;
            Log(L"\u5df2\u5b8c\u6210: " + entry.relativePath);
        }
        ReportProgress();
    }

    if (!m_running && result.outcome == TransferOutcome::Success) {
        result = MakeResult(TransferOutcome::Cancelled, L"\u4f20\u8f93\u5df2\u53d6\u6d88");
    }

    if (result.outcome == TransferOutcome::Cancelled) {
        Log(L"\u4f20\u8f93\u672a\u5b8c\u6574\u5b8c\u6210\uff0c\u5df2\u505c\u6b62\u4f1a\u8bdd");
        SendStringPacket(sock, (uint8_t)PacketType::ERROR_MSG, L"TRANSFER_ABORTED");
    } else if (result.outcome == TransferOutcome::Failed || m_stats.failedFiles > 0) {
        result.outcome = TransferOutcome::Failed;
        result.message = L"\u90e8\u5206\u6587\u4ef6\u4f20\u8f93\u5931\u8d25";
        Log(L"\u4f20\u8f93\u672a\u5b8c\u6574\u5b8c\u6210\uff0c\u5df2\u505c\u6b62\u4f1a\u8bdd");
        SendStringPacket(sock, (uint8_t)PacketType::ERROR_MSG, L"TRANSFER_ABORTED");
    } else {
        if (!SendStringPacket(sock, (uint8_t)PacketType::DONE, L"")) {
            result = MakeResult(TransferOutcome::Failed, L"\u53d1\u9001\u4f20\u8f93\u5b8c\u6210\u4fe1\u53f7\u5931\u8d25");
        } else {
            std::wstring doneAck = RecvStringPacket(sock, (uint8_t)PacketType::DONE_ACK);
            if (doneAck != L"OK") {
                result = MakeResult(TransferOutcome::Failed, L"\u672a\u6536\u5230\u63a5\u6536\u7aef\u7684\u4f20\u8f93\u5b8c\u6210\u786e\u8ba4");
            } else {
                result = MakeResult(TransferOutcome::Success, L"\u4f20\u8f93\u5df2\u5b8c\u6210");
            }
        }
    }

    m_stats.speedBytesPerSec = progress.GetSpeed();
    result.stats = m_stats;
    std::wstring report = ReportGenerator::GenerateReport(m_stats, sourceDir, L"");
    ReportGenerator::SaveReport(report, utils::GetExecutableDir() + L"\\reports");

    shutdown(sock, SD_BOTH);
    closesocket(sock);
    m_sock = INVALID_SOCKET;
    WSACleanup();

    return result;
}

// ============== RECEIVER ==============

void TransferSession::ReceiverWorker(const std::wstring& targetDir, int port,
    const std::wstring& expectedPairingCode, TransferMode mode)
{
    TransferResult result;
    try {
        InnerReceiverWorker(targetDir, port, expectedPairingCode, mode);
    } catch (const std::exception& e) {
        result.outcome = TransferOutcome::Failed;
        result.message = L"\u63a5\u6536\u7ebf\u7a0b\u5f02\u5e38";
        result.stats = m_stats;
        if (m_doneCb) m_doneCb(result);
    } catch (...) {
        result.outcome = TransferOutcome::Failed;
        result.message = L"\u63a5\u6536\u7ebf\u7a0b\u53d1\u751f\u672a\u77e5\u5f02\u5e38";
        result.stats = m_stats;
        if (m_doneCb) m_doneCb(result);
    }
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
        NotifyDone(TransferOutcome::Failed, L"\u521b\u5efa\u76d1\u542c Socket \u5931\u8d25");
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
        NotifyDone(TransferOutcome::Failed, msg);
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
        NotifyDone(TransferOutcome::Failed, msg);
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
        SetSocketTimeouts(sock, HANDSHAKE_TIMEOUT_MS);

        TransferResult result = HandleReceiverConnection(sock, targetDir, expectedPairingCode, mode);

        bool closeSock = false;
        {
            std::lock_guard<std::mutex> lock(m_sockMutex);
            if (m_sock == sock) {
                m_sock = INVALID_SOCKET;
                closeSock = true;
            }
        }
        if (closeSock) {
            shutdown(sock, SD_BOTH);
            closesocket(sock);
        }

        if (m_doneCb) m_doneCb(result);

        if (m_running)
            Log(L"\u63a5\u6536\u7aef\u4fdd\u6301\u76d1\u542c\uff0c\u7b49\u5f85\u4e0b\u4e00\u6b21\u8fde\u63a5...");
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

TransferResult TransferSession::HandleReceiverConnection(SOCKET sock, const std::wstring& targetDir,
    const std::wstring& expectedPairingCode, TransferMode mode)
{
    Log(L"\u5df2\u8fde\u63a5\uff0c\u7b49\u5f85\u914d\u5bf9\u7801...");

    std::wstring code = RecvStringPacket(sock, (uint8_t)PacketType::PAIRING_REQUEST);
    if (code.empty()) {
        Log(L"\u63a5\u6536\u914d\u5bf9\u7801\u5931\u8d25");
        return MakeResult(TransferOutcome::Failed, L"\u63a5\u6536\u914d\u5bf9\u7801\u5931\u8d25");
    }

    if (code != expectedPairingCode) {
        Log(L"\u914d\u5bf9\u7801\u9519\u8bef\uff01\u671f\u671b: " + expectedPairingCode
            + L"\uff0c\u6536\u5230: " + code);
        SendStringPacket(sock, (uint8_t)PacketType::PAIRING_RESPONSE, L"FAIL");
        return MakeResult(TransferOutcome::Failed, L"\u914d\u5bf9\u7801\u9519\u8bef");
    }

    SendStringPacket(sock, (uint8_t)PacketType::PAIRING_RESPONSE, L"OK");
    SetSocketTimeouts(sock, 0);
    Log(L"\u914d\u5bf9\u7801\u9a8c\u8bc1\u6210\u529f\uff01");
    m_stats = TransferStats{};

    uint8_t manifestType = 0;
    std::vector<uint8_t> manifestPayload;
    if (!RecvPacket(sock, manifestType, manifestPayload) || manifestType != (uint8_t)PacketType::MANIFEST) {
        Log(L"\u63a5\u6536\u6587\u4ef6\u6e05\u5355\u5931\u8d25");
        return MakeResult(TransferOutcome::Failed, L"\u63a5\u6536\u6587\u4ef6\u6e05\u5355\u5931\u8d25");
    }

    std::string manifestJson((char*)manifestPayload.data(), manifestPayload.size());
    Manifest manifest;
    if (!manifest.FromJSON(manifestJson)) {
        Log(L"\u89e3\u6790\u6587\u4ef6\u6e05\u5355\u5931\u8d25");
        return MakeResult(TransferOutcome::Failed, L"\u89e3\u6790\u6587\u4ef6\u6e05\u5355\u5931\u8d25");
    }

    Log(L"\u5df2\u63a5\u6536\u6587\u4ef6\u6e05\u5355: " + std::to_wstring(manifest.GetEntries().size())
        + L" \u4e2a\u6587\u4ef6\uff0c" + utils::FormatBytes(manifest.GetTotalSize()));

    std::vector<std::wstring> extraFiles;
    if (mode == TransferMode::MIRROR) {
        extraFiles = TransferPlanner::FindExtraFiles(manifest, targetDir);
    }

    TransferPlanner planner;
    TransferPlanner::Plan plan = planner.BuildPlan(manifest, targetDir, mode);
    Log(L"\u8ba1\u5212\u63a5\u6536 " + std::to_wstring(plan.totalFiles)
        + L" \u4e2a\u6587\u4ef6\uff0c\u8df3\u8fc7 " + std::to_wstring(plan.skipFiles));

    std::string planJson = TransferPlanner::SerializePlan(plan);
    std::vector<uint8_t> planPayload(planJson.begin(), planJson.end());
    if (!SendPacket(sock, (uint8_t)PacketType::TRANSFER_PLAN, planPayload)) {
        Log(L"\u53d1\u9001\u4f20\u8f93\u8ba1\u5212\u5931\u8d25");
        return MakeResult(TransferOutcome::Failed, L"\u53d1\u9001\u4f20\u8f93\u8ba1\u5212\u5931\u8d25");
    }

    m_stats.totalBytes = plan.totalBytes;
    m_stats.totalFiles = plan.totalFiles;
    m_stats.skippedFiles = plan.skipFiles;

    std::unordered_map<std::wstring, int64_t> plannedOffsets;
    for (const auto& entry : plan.entries)
        plannedOffsets[entry.relativePath] = entry.offset;

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
            auto plannedIt = plannedOffsets.find(fileName);
            if (plannedIt != plannedOffsets.end() && offset < plannedIt->second) {
                m_stats.totalBytes += (plannedIt->second - offset);
                plannedIt->second = offset;
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
                LARGE_INTEGER li;
                li.QuadPart = offset;
                SetFilePointerEx(hFile, li, NULL, FILE_BEGIN);
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
        return MakeResult(
            m_running ? TransferOutcome::Failed : TransferOutcome::Cancelled,
            failureMessage);
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

            return MakeResult(TransferOutcome::Failed, message);
        }

        Log(L"\u955c\u50cf\u5220\u9664\u5b8c\u6210: "
            + std::to_wstring(deleteResult.deleted) + L" \u4e2a\u6587\u4ef6");
    }

    if (!SendStringPacket(sock, (uint8_t)PacketType::DONE_ACK, L"OK")) {
        return MakeResult(TransferOutcome::Failed,
            L"\u955c\u50cf\u63d0\u4ea4\u6210\u529f\uff0c\u4f46\u53d1\u9001\u6700\u7ec8\u786e\u8ba4\u5931\u8d25");
    }

    return MakeResult(
        TransferOutcome::Success,
        mode == TransferMode::MIRROR
            ? L"\u955c\u50cf\u540c\u6b65\u5df2\u5b8c\u6210"
            : L"\u6587\u4ef6\u4f20\u8f93\u5df2\u5b8c\u6210");
}
