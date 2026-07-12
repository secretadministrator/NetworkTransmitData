#include "TransferSession.h"
#include "AppConfig.h"
#include "ExcludeRules.h"
#include "FileScanner.h"
#include "Manifest.h"
#include "ProgressTracker.h"
#include "ReportGenerator.h"
#include "TransferProtocol.h"
#include "Utils.h"
#include "Version.h"
#include <nlohmann/json.hpp>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <objbase.h>
#include <winioctl.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace {

constexpr size_t MAX_PAYLOAD_BYTES = 64ULL * 1024 * 1024;
constexpr size_t CATALOG_CHUNK_BYTES = 4ULL * 1024 * 1024;
constexpr int64_t SMALL_FILE_LIMIT = 64LL * 1024;
constexpr size_t SMALL_BATCH_MAX_FILES = 512;
constexpr size_t SMALL_BATCH_MAX_BYTES = 8ULL * 1024 * 1024;
constexpr size_t SMALL_BATCH_WINDOW = 4;
constexpr DWORD LARGE_FILE_CHUNK = 1024 * 1024;

void AppendU8(std::vector<uint8_t>& out, uint8_t value) {
    out.push_back(value);
}

void AppendU32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 24));
}

void AppendU64(std::vector<uint8_t>& out, uint64_t value) {
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

void WriteU32(std::vector<uint8_t>& out, size_t offset, uint32_t value) {
    if (offset + 4 > out.size())
        return;
    out[offset] = static_cast<uint8_t>(value);
    out[offset + 1] = static_cast<uint8_t>(value >> 8);
    out[offset + 2] = static_cast<uint8_t>(value >> 16);
    out[offset + 3] = static_cast<uint8_t>(value >> 24);
}

void AppendBytes(std::vector<uint8_t>& out, const void* data, size_t length) {
    if (length == 0)
        return;
    const auto* first = static_cast<const uint8_t*>(data);
    out.insert(out.end(), first, first + length);
}

void AppendString(std::vector<uint8_t>& out, const std::string& value) {
    AppendU32(out, static_cast<uint32_t>(value.size()));
    AppendBytes(out, value.data(), value.size());
}

bool ReadU8(const std::vector<uint8_t>& data, size_t& pos, uint8_t& value) {
    if (pos >= data.size())
        return false;
    value = data[pos++];
    return true;
}

bool ReadU32(const std::vector<uint8_t>& data, size_t& pos, uint32_t& value) {
    if (pos > data.size() || data.size() - pos < 4)
        return false;
    value = static_cast<uint32_t>(data[pos]) |
        (static_cast<uint32_t>(data[pos + 1]) << 8) |
        (static_cast<uint32_t>(data[pos + 2]) << 16) |
        (static_cast<uint32_t>(data[pos + 3]) << 24);
    pos += 4;
    return true;
}

bool ReadU64(const std::vector<uint8_t>& data, size_t& pos, uint64_t& value) {
    if (pos > data.size() || data.size() - pos < 8)
        return false;
    value = 0;
    for (int i = 0; i < 8; ++i)
        value |= static_cast<uint64_t>(data[pos + i]) << (i * 8);
    pos += 8;
    return true;
}

bool ReadString(const std::vector<uint8_t>& data, size_t& pos, std::string& value) {
    uint32_t length = 0;
    if (!ReadU32(data, pos, length) || pos > data.size() || data.size() - pos < length)
        return false;
    value.assign(reinterpret_cast<const char*>(data.data() + pos), length);
    pos += length;
    return true;
}

class Crc32 {
public:
    void Update(const void* data, size_t length) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        const auto& table = Table();
        for (size_t i = 0; i < length; ++i)
            m_value = table[(m_value ^ bytes[i]) & 0xFF] ^ (m_value >> 8);
    }

    uint32_t Finish() const { return m_value ^ 0xFFFFFFFFU; }

    static uint32_t Compute(const void* data, size_t length) {
        Crc32 crc;
        crc.Update(data, length);
        return crc.Finish();
    }

private:
    static const std::array<uint32_t, 256>& Table() {
        static const std::array<uint32_t, 256> table = []() {
            std::array<uint32_t, 256> values{};
            for (uint32_t i = 0; i < values.size(); ++i) {
                uint32_t value = i;
                for (int bit = 0; bit < 8; ++bit)
                    value = (value >> 1) ^ (0xEDB88320U & (0U - (value & 1U)));
                values[i] = value;
            }
            return values;
        }();
        return table;
    }

    uint32_t m_value = 0xFFFFFFFFU;
};

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle = INVALID_HANDLE_VALUE) : m_handle(handle) {}
    ~ScopedHandle() { if (m_handle != INVALID_HANDLE_VALUE) CloseHandle(m_handle); }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    HANDLE Get() const { return m_handle; }
    void Close() {
        if (m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
    }
private:
    HANDLE m_handle;
};

size_t ChooseSmallFileWriterCount(const std::wstring& targetDir) {
    wchar_t volumePath[MAX_PATH] = {};
    if (!GetVolumePathNameW(targetDir.c_str(), volumePath, MAX_PATH))
        return 2;
    if (wcsncmp(volumePath, L"\\\\", 2) == 0)
        return 2;
    if (wcslen(volumePath) < 2 || volumePath[1] != L':')
        return 2;

    const std::wstring devicePath = L"\\\\.\\" + std::wstring(volumePath, 2);
    ScopedHandle device(CreateFileW(devicePath.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));
    if (device.Get() == INVALID_HANDLE_VALUE)
        return 2;
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR descriptor{};
    DWORD returned = 0;
    if (!DeviceIoControl(device.Get(), IOCTL_STORAGE_QUERY_PROPERTY,
            &query, sizeof(query), &descriptor, sizeof(descriptor), &returned, nullptr)) {
        return 2;
    }
    return descriptor.IncursSeekPenalty ? 1 : 4;
}

class SmallFileWriterPool {
public:
    using CompletionCallback = std::function<void(std::vector<uint8_t>)>;

    struct Request {
        std::wstring fullPath;
        std::shared_ptr<std::vector<uint8_t>> payload;
        size_t offset = 0;
        size_t size = 0;
        uint32_t expectedCrc = 0;
    };

    explicit SmallFileWriterPool(size_t threadCount) {
        threadCount = (std::max)(size_t{1}, threadCount);
        for (size_t i = 0; i < threadCount; ++i)
            m_threads.emplace_back(&SmallFileWriterPool::Worker, this);
    }

    ~SmallFileWriterPool() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_cv.notify_all();
        for (auto& thread : m_threads)
            if (thread.joinable()) thread.join();
    }

    void SubmitBatch(const std::vector<Request>& requests, CompletionCallback completion) {
        if (requests.empty()) {
            completion({});
            return;
        }
        auto state = std::make_shared<BatchState>();
        state->remaining = requests.size();
        state->results.assign(requests.size(), 0);
        state->completion = std::move(completion);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (size_t i = 0; i < requests.size(); ++i)
                m_tasks.push_back({requests[i], state, i});
        }
        m_cv.notify_all();
    }

private:
    struct BatchState {
        std::mutex mutex;
        size_t remaining = 0;
        std::vector<uint8_t> results;
        CompletionCallback completion;
    };

    struct Task {
        Request request;
        std::shared_ptr<BatchState> state;
        size_t resultIndex = 0;
    };

    static bool WriteOne(const Request& request) {
        if (Crc32::Compute(request.payload->data() + request.offset, request.size) !=
                request.expectedCrc) {
            return false;
        }
        const size_t separator = request.fullPath.find_last_of(L'\\');
        if (separator == std::wstring::npos ||
            !utils::CreateDirectoryTree(request.fullPath.substr(0, separator))) {
            return false;
        }
        const std::wstring partPath = request.fullPath + L".dtpart";
        ScopedHandle file(CreateFileW(utils::NormalizePath(partPath).c_str(),
            GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file.Get() == INVALID_HANDLE_VALUE)
            return false;
        DWORD written = 0;
        if (request.size > 0 && (!WriteFile(file.Get(),
                request.payload->data() + request.offset,
                static_cast<DWORD>(request.size), &written, nullptr) ||
                written != request.size)) {
            return false;
        }
        file.Close();
        return MoveFileExW(utils::NormalizePath(partPath).c_str(),
            utils::NormalizePath(request.fullPath).c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
    }

    void Worker() {
        for (;;) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [&]() { return m_stopping || !m_tasks.empty(); });
                if (m_stopping && m_tasks.empty())
                    return;
                task = std::move(m_tasks.front());
                m_tasks.pop_front();
            }
            const bool ok = WriteOne(task.request);
            CompletionCallback completion;
            std::vector<uint8_t> results;
            {
                std::lock_guard<std::mutex> lock(task.state->mutex);
                task.state->results[task.resultIndex] = ok ? 1 : 0;
                --task.state->remaining;
                if (task.state->remaining == 0 && task.state->completion) {
                    completion = std::move(task.state->completion);
                    results = task.state->results;
                }
            }
            if (completion)
                completion(std::move(results));
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<Task> m_tasks;
    std::vector<std::thread> m_threads;
    bool m_stopping = false;
};

std::wstring FileTimeToStableString(const FILETIME& time) {
    const uint64_t value = (static_cast<uint64_t>(time.dwHighDateTime) << 32) |
        static_cast<uint64_t>(time.dwLowDateTime);
    wchar_t buffer[32] = {};
    swprintf(buffer, 32, L"%016llx", static_cast<unsigned long long>(value));
    return buffer;
}

bool SourceFileMatches(const std::wstring& fullPath, const FileEntry& entry) {
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(utils::NormalizePath(fullPath).c_str(),
            GetFileExInfoStandard, &attributes)) {
        return false;
    }
    LARGE_INTEGER size{};
    size.LowPart = attributes.nFileSizeLow;
    size.HighPart = attributes.nFileSizeHigh;
    if (size.QuadPart != entry.size)
        return false;
    return entry.lastWriteTime.empty() ||
        FileTimeToStableString(attributes.ftLastWriteTime) == entry.lastWriteTime;
}

bool IsSafeRelativePath(const std::wstring& path) {
    if (path.empty() || path.front() == L'\\' || path.front() == L'/' ||
        (path.size() >= 2 && path[1] == L':')) {
        return false;
    }
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find_first_of(L"\\/", start);
        std::wstring part = path.substr(start,
            end == std::wstring::npos ? std::wstring::npos : end - start);
        if (part.empty() || part == L"." || part == L"..")
            return false;
        if (end == std::wstring::npos)
            break;
        start = end + 1;
    }
    return true;
}

std::wstring MakeTransferId() {
    GUID guid{};
    if (SUCCEEDED(CoCreateGuid(&guid))) {
        wchar_t buffer[64] = {};
        if (StringFromGUID2(guid, buffer, 64) > 0)
            return buffer;
    }
    return std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64());
}

std::wstring BuildHelloPayload(const std::wstring& token) {
    nlohmann::json hello;
    hello["magic"] = "DirectTransfer";
    hello["protocol"] = version::PROTOCOL_VERSION;
    hello["appVersion"] = DT_VERSION_STRING;
    hello["token"] = utils::ToUtf8(token);
    hello["capabilities"] = nlohmann::json::array(
        {"binary-catalog", "batch-window", "session-reconnect", "crc32"});
    return utils::FromUtf8(hello.dump());
}

bool ParseHelloPayload(const std::wstring& payload,
    std::wstring& token, std::wstring& appVersion) {
    try {
        auto hello = nlohmann::json::parse(utils::ToUtf8(payload));
        if (hello.value("magic", std::string()) != "DirectTransfer" ||
            hello.value("protocol", 0) != version::PROTOCOL_VERSION) {
            return false;
        }
        token = utils::FromUtf8(hello.value("token", std::string()));
        appVersion = utils::FromUtf8(hello.value("appVersion", std::string()));
        return !token.empty();
    } catch (...) {
        return false;
    }
}

bool ConfigureSocket(SOCKET socket, DWORD timeoutMs) {
    DWORD timeout = timeoutMs;
    if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
            reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == SOCKET_ERROR)
        return false;
    if (setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
            reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == SOCKET_ERROR)
        return false;
    return true;
}

void ConfigureTransferSocket(SOCKET socket) {
    BOOL noDelay = TRUE;
    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
        reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));

    tcp_keepalive keepAlive{};
    keepAlive.onoff = 1;
    keepAlive.keepalivetime = 30000;
    keepAlive.keepaliveinterval = 5000;
    DWORD returned = 0;
    WSAIoctl(socket, SIO_KEEPALIVE_VALS, &keepAlive, sizeof(keepAlive),
        nullptr, 0, &returned, nullptr, nullptr);
}

void EncodeCatalogEntry(std::vector<uint8_t>& out, uint64_t id,
    const FileEntry& entry) {
    AppendU64(out, id);
    AppendU64(out, static_cast<uint64_t>(entry.size));
    AppendU32(out, entry.attributes);
    AppendString(out, utils::ToUtf8(entry.relativePath));
    AppendString(out, utils::ToUtf8(entry.lastWriteTime));
}

bool DecodeCatalogEntry(const std::vector<uint8_t>& data, size_t& pos,
    uint64_t& id, FileEntry& entry) {
    uint64_t size = 0;
    uint32_t attributes = 0;
    std::string path;
    std::string writeTime;
    if (!ReadU64(data, pos, id) || !ReadU64(data, pos, size) ||
        !ReadU32(data, pos, attributes) || !ReadString(data, pos, path) ||
        !ReadString(data, pos, writeTime) || size > static_cast<uint64_t>(INT64_MAX)) {
        return false;
    }
    entry.relativePath = utils::FromUtf8(path);
    entry.lastWriteTime = utils::FromUtf8(writeTime);
    entry.size = static_cast<int64_t>(size);
    entry.attributes = attributes;
    return IsSafeRelativePath(entry.relativePath);
}

uint32_t ComputeCatalogCrc(const std::vector<FileEntry>& files) {
    Crc32 crc;
    std::vector<uint8_t> encoded;
    for (size_t i = 0; i < files.size(); ++i) {
        encoded.clear();
        EncodeCatalogEntry(encoded, static_cast<uint64_t>(i), files[i]);
        crc.Update(encoded.data(), encoded.size());
    }
    return crc.Finish();
}

TransferResult MakeResult(TransferResultCode code, const std::wstring& message) {
    TransferResult result;
    result.code = code;
    result.message = message;
    return result;
}

TransferResult IoFailure(const IoResult& io, const std::wstring& operation) {
    if (io.status == IoStatus::Cancelled)
        return MakeResult(TransferResultCode::Cancelled, L"传输已取消");
    if (io.status == IoStatus::ProtocolError)
        return MakeResult(TransferResultCode::ProtocolError, operation + L"：协议数据错误");
    if (io.status == IoStatus::IdleTimeout)
        return MakeResult(TransferResultCode::ConnectionLost, operation + L"：90 秒没有网络进展");
    if (io.status == IoStatus::PeerClosed)
        return MakeResult(TransferResultCode::ConnectionLost, operation + L"：对端关闭了连接");
    return MakeResult(TransferResultCode::ConnectionLost,
        operation + L"：网络错误 " + std::to_wstring(io.wsaError));
}

void UpdateStats(const ProgressTracker& progress, TransferStats& stats) {
    stats.transferredBytes = progress.GetTransferred();
    stats.displayTransferredBytes = progress.GetDisplayedTransferred();
    stats.recentSpeedBytesPerSec = progress.GetRecentSpeed();
    stats.averageSpeedBytesPerSec = progress.GetAverageSpeed();
    stats.speedBytesPerSec = progress.GetSpeed();
    stats.estimatedRemainingSeconds = progress.GetEstimatedRemainingSeconds();
    stats.elapsedSeconds = progress.GetElapsedSeconds();
    stats.estimatedTotalSeconds = progress.GetEstimatedTotalSeconds();
    stats.overallWorkTotal = progress.GetWorkTotal();
    stats.overallWorkCompleted = progress.GetDisplayedWorkCompleted();
    stats.waitingForIo = stats.stage == TransferStage::Reconnecting;
}

std::vector<uint8_t> BuildReadyPayload(bool needCatalog,
    const std::vector<bool>& committed) {
    std::vector<uint8_t> payload;
    AppendU8(payload, needCatalog ? 1 : 0);
    const size_t rangeCountOffset = payload.size();
    AppendU32(payload, 0);
    uint32_t rangeCount = 0;
    size_t index = 0;
    while (index < committed.size()) {
        while (index < committed.size() && !committed[index])
            ++index;
        if (index >= committed.size())
            break;
        const size_t start = index;
        while (index < committed.size() && committed[index])
            ++index;
        AppendU64(payload, static_cast<uint64_t>(start));
        AppendU64(payload, static_cast<uint64_t>(index - start));
        ++rangeCount;
    }
    WriteU32(payload, rangeCountOffset, rangeCount);
    return payload;
}

bool ParseReadyPayload(const std::vector<uint8_t>& payload, bool& needCatalog,
    std::vector<std::pair<uint64_t, uint64_t>>& ranges) {
    size_t pos = 0;
    uint8_t need = 0;
    uint32_t count = 0;
    if (!ReadU8(payload, pos, need) || !ReadU32(payload, pos, count))
        return false;
    needCatalog = need != 0;
    ranges.clear();
    ranges.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t start = 0;
        uint64_t length = 0;
        if (!ReadU64(payload, pos, start) || !ReadU64(payload, pos, length) || length == 0)
            return false;
        ranges.emplace_back(start, length);
    }
    return pos == payload.size();
}

} // namespace

struct TransferSession::SenderContext {
    std::vector<FileEntry> files;
    std::wstring transferId;
    uint32_t catalogCrc = 0;
    uint64_t nextBatchId = 1;
    std::unordered_set<uint64_t> committed;
    ProgressTracker progress;
};

struct TransferSession::ReceiverTaskState {
    std::wstring transferId;
    uint32_t catalogCrc = 0;
    uint64_t expectedFiles = 0;
    uint64_t expectedBytes = 0;
    uint32_t nextCatalogSequence = 0;
    std::vector<FileEntry> catalog;
    std::vector<bool> committed;
    bool catalogReady = false;
    bool completed = false;
    ProgressTracker progress;

    void Reset(const std::wstring& id, uint32_t crc, uint64_t files, uint64_t bytes) {
        transferId = id;
        catalogCrc = crc;
        expectedFiles = files;
        expectedBytes = bytes;
        nextCatalogSequence = 0;
        catalog.clear();
        committed.clear();
        catalogReady = false;
        completed = false;
        progress.SetWorkload(static_cast<int64_t>(bytes), static_cast<int>(files));
    }

    size_t CommittedCount() const {
        return static_cast<size_t>(std::count(committed.begin(), committed.end(), true));
    }
};

TransferSession::TransferSession() = default;
TransferSession::~TransferSession() { Stop(); }

void TransferSession::Log(const std::wstring& message) {
    if (m_logCb)
        m_logCb(message);
}

void TransferSession::ReportProgress(bool force) {
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG previous = m_lastProgressReportTick.load();
    if (!force && previous != 0 && now - previous < PROGRESS_REPORT_INTERVAL_MS)
        return;
    m_lastProgressReportTick.store(now);
    if (m_progressCb)
        m_progressCb(m_stats);
}

TransferResult TransferSession::FinalizeResult(TransferResult result) {
    result.role = m_role == SessionRole::SENDER
        ? TransferRole::SENDER : TransferRole::RECEIVER;
    result.socketError = m_lastSocketError.load();
    result.resumable = false;
    result.stats = m_stats;
    return result;
}

bool TransferSession::StartAsSender(const std::wstring& sourceDir,
    const std::wstring& peerIP, int port, const std::wstring& sessionToken) {
    if (m_running)
        return false;
    if (m_workerThread.joinable())
        m_workerThread.join();
    m_stats = TransferStats{};
    m_lastProgressReportTick.store(0);
    m_lastSocketError.store(0);
    m_role = SessionRole::SENDER;
    m_running = true;
    m_workerThread = std::thread(&TransferSession::SenderWorker, this,
        sourceDir, peerIP, port, sessionToken);
    return true;
}

bool TransferSession::StartAsReceiver(const std::wstring& targetDir, int port,
    const std::wstring& sessionToken, TransferMode mode) {
    if (m_running)
        return false;
    if (m_workerThread.joinable())
        m_workerThread.join();
    m_stats = TransferStats{};
    m_lastProgressReportTick.store(0);
    m_lastSocketError.store(0);
    m_role = SessionRole::RECEIVER;
    m_running = true;
    m_workerThread = std::thread(&TransferSession::ReceiverWorker, this,
        targetDir, port, sessionToken, mode);
    return true;
}

void TransferSession::CloseActiveSocket(SOCKET expected) {
    std::lock_guard<std::mutex> lock(m_sockMutex);
    if (m_sock != INVALID_SOCKET &&
        (expected == INVALID_SOCKET || expected == m_sock)) {
        shutdown(m_sock, SD_BOTH);
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
    }
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

IoResult TransferSession::SendAll(SOCKET socket, const uint8_t* data, size_t length) {
    size_t sentTotal = 0;
    ULONGLONG lastProgress = GetTickCount64();
    while (sentTotal < length) {
        if (!m_running)
            return {IoStatus::Cancelled, 0};
        const size_t remaining = length - sentTotal;
        const int request = remaining > static_cast<size_t>(INT_MAX)
            ? INT_MAX : static_cast<int>(remaining);
        const int sent = send(socket,
            reinterpret_cast<const char*>(data + sentTotal), request, 0);
        if (sent > 0) {
            sentTotal += static_cast<size_t>(sent);
            lastProgress = GetTickCount64();
            continue;
        }
        if (sent == 0)
            return {IoStatus::PeerClosed, 0};
        const int error = WSAGetLastError();
        m_lastSocketError.store(error);
        if (!m_running)
            return {IoStatus::Cancelled, error};
        if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK || error == WSAEINTR) {
            if (GetTickCount64() - lastProgress >= CONNECTION_IDLE_MS)
                return {IoStatus::IdleTimeout, error};
            continue;
        }
        return {IoStatus::SocketError, error};
    }
    return {IoStatus::Ok, 0};
}

IoResult TransferSession::RecvExact(SOCKET socket, uint8_t* data, size_t length) {
    size_t receivedTotal = 0;
    ULONGLONG lastProgress = GetTickCount64();
    while (receivedTotal < length) {
        if (!m_running)
            return {IoStatus::Cancelled, 0};
        const size_t remaining = length - receivedTotal;
        const int request = remaining > static_cast<size_t>(INT_MAX)
            ? INT_MAX : static_cast<int>(remaining);
        const int received = recv(socket,
            reinterpret_cast<char*>(data + receivedTotal), request, 0);
        if (received > 0) {
            receivedTotal += static_cast<size_t>(received);
            lastProgress = GetTickCount64();
            continue;
        }
        if (received == 0)
            return {IoStatus::PeerClosed, 0};
        const int error = WSAGetLastError();
        m_lastSocketError.store(error);
        if (!m_running)
            return {IoStatus::Cancelled, error};
        if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK || error == WSAEINTR) {
            if (GetTickCount64() - lastProgress >= CONNECTION_IDLE_MS)
                return {IoStatus::IdleTimeout, error};
            continue;
        }
        return {IoStatus::SocketError, error};
    }
    return {IoStatus::Ok, 0};
}

IoResult TransferSession::SendPacket(SOCKET socket, uint8_t type,
    const uint8_t* payload, size_t size) {
    if (size > MAX_PAYLOAD_BYTES)
        return {IoStatus::ProtocolError, 0};
    std::lock_guard<std::mutex> lock(m_sendMutex);
    uint8_t header[5] = {
        type,
        static_cast<uint8_t>(size),
        static_cast<uint8_t>(size >> 8),
        static_cast<uint8_t>(size >> 16),
        static_cast<uint8_t>(size >> 24)
    };
    IoResult result = SendAll(socket, header, sizeof(header));
    if (result.IsOk() && size > 0)
        result = SendAll(socket, payload, size);
    return result;
}

IoResult TransferSession::SendPacket(SOCKET socket, uint8_t type,
    const std::vector<uint8_t>& payload) {
    return SendPacket(socket, type, payload.empty() ? nullptr : payload.data(), payload.size());
}

IoResult TransferSession::RecvPacket(SOCKET socket, uint8_t& type,
    std::vector<uint8_t>& payload) {
    uint8_t header[5]{};
    IoResult result = RecvExact(socket, header, sizeof(header));
    if (!result.IsOk())
        return result;
    type = header[0];
    const uint32_t length = static_cast<uint32_t>(header[1]) |
        (static_cast<uint32_t>(header[2]) << 8) |
        (static_cast<uint32_t>(header[3]) << 16) |
        (static_cast<uint32_t>(header[4]) << 24);
    if (length > MAX_PAYLOAD_BYTES)
        return {IoStatus::ProtocolError, 0};
    payload.resize(length);
    return length == 0 ? IoResult{} : RecvExact(socket, payload.data(), length);
}

IoResult TransferSession::SendString(SOCKET socket, uint8_t type,
    const std::wstring& text) {
    const std::string utf8 = utils::ToUtf8(text);
    return SendPacket(socket, type,
        reinterpret_cast<const uint8_t*>(utf8.data()), utf8.size());
}

IoResult TransferSession::RecvString(SOCKET socket, uint8_t expectedType,
    std::wstring& text) {
    uint8_t type = 0;
    std::vector<uint8_t> payload;
    IoResult result = RecvPacket(socket, type, payload);
    if (!result.IsOk())
        return result;
    if (type != expectedType)
        return {IoStatus::ProtocolError, 0};
    text = utils::FromUtf8(std::string(
        reinterpret_cast<const char*>(payload.data()), payload.size()));
    return {IoStatus::Ok, 0};
}

void TransferSession::SenderWorker(const std::wstring& sourceDir,
    const std::wstring& peerIP, int port, const std::wstring& sessionToken) {
    TransferResult result;
    try {
        result = RunSender(sourceDir, peerIP, port, sessionToken);
    } catch (...) {
        result = MakeResult(TransferResultCode::InternalError, L"发送线程发生未处理异常");
    }
    CloseActiveSocket();
    m_running = false;
    result = FinalizeResult(std::move(result));
    if (m_doneCb)
        m_doneCb(result);
}

TransferResult TransferSession::RunSender(const std::wstring& sourceDir,
    const std::wstring& peerIP, int port, const std::wstring& sessionToken) {
    SenderContext context;
    context.transferId = MakeTransferId();

    const DWORD rootAttributes = GetFileAttributesW(utils::NormalizePath(sourceDir).c_str());
    if (rootAttributes == INVALID_FILE_ATTRIBUTES ||
        (rootAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return MakeResult(TransferResultCode::FileError, L"源目录不存在或无法访问");
    }

    FileScanner scanner;
    ExcludeRules excludes;
    scanner.SetExcludeCallback([&](const std::wstring& path) {
        return excludes.IsExcluded(path);
    });
    m_stats.scanning = true;
    m_stats.stage = TransferStage::ScanningSource;
    m_stats.stageText = L"正在快速盘点源目录";
    scanner.SetProgressCallback([&](const ScanProgress& scan) {
        m_stats.totalFiles = static_cast<int>((std::min)(
            scan.scannedFiles, static_cast<uint64_t>(INT_MAX)));
        m_stats.scannedDirectories = scan.scannedDirectories;
        m_stats.scannedBytes = static_cast<int64_t>(scan.scannedBytes);
        m_stats.inaccessibleDirectories = scan.inaccessibleDirectories;
        m_stats.currentFile = scan.currentPath;
        ReportProgress();
    });
    context.files = scanner.ScanDirectory(sourceDir);
    m_stats.scanning = false;
    m_stats.currentFile.clear();
    if (!m_running)
        return MakeResult(TransferResultCode::Cancelled, L"传输已取消");
    if (m_stats.inaccessibleDirectories > 0) {
        return MakeResult(TransferResultCode::FileError,
            L"源目录包含无法访问的目录，严格完整性检查未通过");
    }
    if (context.files.size() > static_cast<size_t>(INT_MAX))
        return MakeResult(TransferResultCode::FileError, L"文件数量超过当前版本支持范围");

    int64_t totalBytes = 0;
    for (const auto& file : context.files) {
        if (file.size < 0 || totalBytes > INT64_MAX - file.size)
            return MakeResult(TransferResultCode::FileError, L"源目录总大小溢出");
        totalBytes += file.size;
    }
    context.catalogCrc = ComputeCatalogCrc(context.files);
    context.progress.SetWorkload(totalBytes, static_cast<int>(context.files.size()));
    m_stats.totalBytes = totalBytes;
    m_stats.totalFiles = static_cast<int>(context.files.size());
    m_stats.completedFiles = 0;
    m_stats.stage = TransferStage::WaitingForPlan;
    m_stats.stageText = L"源目录盘点完成，正在建立传输会话";
    m_stats.stageProcessed = 0;
    m_stats.stageTotal = context.files.size();
    UpdateStats(context.progress, m_stats);
    ReportProgress(true);
    Log(L"盘点完成：" + std::to_wstring(context.files.size()) + L" 个文件，" +
        utils::FormatBytes(totalBytes));

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return MakeResult(TransferResultCode::InternalError, L"初始化 Winsock 失败");

    TransferResult result;
    int retryDelaySeconds = 1;
    while (m_running) {
        result = RunSenderConnection(sourceDir, peerIP, port, sessionToken, context);
        CloseActiveSocket();
        if (result.code != TransferResultCode::ConnectionLost)
            break;

        ++m_stats.retryCount;
        ++m_stats.interruptedFiles;
        context.progress.SetInFlight(0);
        m_stats.stage = TransferStage::Reconnecting;
        m_stats.stageText = L"连接中断，正在自动重新连接";
        m_stats.waitingForIo = true;
        UpdateStats(context.progress, m_stats);
        Log(L"连接中断，将在 " + std::to_wstring(retryDelaySeconds) +
            L" 秒后自动重连（已提交文件不会重传）");
        ReportProgress(true);
        for (int i = 0; i < retryDelaySeconds * 10 && m_running; ++i)
            Sleep(100);
        retryDelaySeconds = (std::min)(retryDelaySeconds * 2, 15);
    }
    WSACleanup();
    if (!m_running && result.code == TransferResultCode::ConnectionLost)
        result = MakeResult(TransferResultCode::Cancelled, L"传输已取消");
    UpdateStats(context.progress, m_stats);
    ReportProgress(true);
    if (result.code == TransferResultCode::Success) {
        std::wstring report = ReportGenerator::GenerateReport(m_stats, sourceDir, L"");
        ReportGenerator::SaveReport(report, utils::GetExecutableDir() + L"\\reports");
    }
    return result;
}

TransferResult TransferSession::RunSenderConnection(const std::wstring& sourceDir,
    const std::wstring& peerIP, int port, const std::wstring& sessionToken,
    SenderContext& context) {
    Log(L"正在连接 " + peerIP + L":" + std::to_wstring(port));
    SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET)
        return MakeResult(TransferResultCode::ConnectionLost, L"创建 Socket 失败");
    {
        std::lock_guard<std::mutex> lock(m_sockMutex);
        m_sock = socket;
    }
    if (!ConfigureSocket(socket, HANDSHAKE_TIMEOUT_MS))
        return MakeResult(TransferResultCode::ConnectionLost, L"设置握手超时失败");

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(port));
    const std::string ip = utils::ToUtf8(peerIP);
    if (InetPtonA(AF_INET, ip.c_str(), &address.sin_addr) != 1)
        return MakeResult(TransferResultCode::FileError, L"接收端 IP 地址无效");
    if (connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        m_lastSocketError.store(WSAGetLastError());
        return MakeResult(TransferResultCode::ConnectionLost, L"暂时无法连接接收端");
    }

    std::wstring hello;
    IoResult io = RecvString(socket, static_cast<uint8_t>(PacketType::SERVER_HELLO), hello);
    if (!io.IsOk())
        return IoFailure(io, L"接收服务器握手");
    std::wstring serverToken;
    std::wstring peerVersion;
    if (!ParseHelloPayload(hello, serverToken, peerVersion))
        return MakeResult(TransferResultCode::ProtocolError, L"接收端协议版本不兼容");
    if (!sessionToken.empty() && sessionToken != serverToken)
        return MakeResult(TransferResultCode::ProtocolError, L"接收端会话标识不匹配");
    io = SendString(socket, static_cast<uint8_t>(PacketType::CLIENT_HELLO),
        BuildHelloPayload(serverToken));
    if (!io.IsOk())
        return IoFailure(io, L"发送客户端握手");
    std::wstring helloAck;
    io = RecvString(socket, static_cast<uint8_t>(PacketType::HELLO_ACK), helloAck);
    if (!io.IsOk())
        return IoFailure(io, L"接收握手确认");
    if (helloAck != L"OK")
        return MakeResult(TransferResultCode::ProtocolError, L"接收端拒绝了会话");
    if (!ConfigureSocket(socket, IO_WAIT_TIMEOUT_MS))
        return MakeResult(TransferResultCode::ConnectionLost, L"设置传输超时失败");
    ConfigureTransferSocket(socket);

    std::vector<uint8_t> begin;
    AppendString(begin, utils::ToUtf8(context.transferId));
    AppendU32(begin, context.catalogCrc);
    AppendU64(begin, context.files.size());
    AppendU64(begin, static_cast<uint64_t>(m_stats.totalBytes));
    io = SendPacket(socket, static_cast<uint8_t>(PacketType::SESSION_BEGIN), begin);
    if (!io.IsOk())
        return IoFailure(io, L"发送会话信息");

    auto receiveExpected = [&](PacketType expected, std::vector<uint8_t>& payload) -> IoResult {
        for (;;) {
            uint8_t type = 0;
            IoResult received = RecvPacket(socket, type, payload);
            if (!received.IsOk())
                return received;
            if (type == static_cast<uint8_t>(PacketType::ERROR_MSG))
                return {IoStatus::ProtocolError, 0};
            if (type != static_cast<uint8_t>(expected))
                return {IoStatus::ProtocolError, 0};
            return {IoStatus::Ok, 0};
        }
    };

    bool needCatalog = false;
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    std::vector<uint8_t> ready;
    io = receiveExpected(PacketType::SESSION_READY, ready);
    if (!io.IsOk() || !ParseReadyPayload(ready, needCatalog, ranges))
        return IoFailure(io.IsOk() ? IoResult{IoStatus::ProtocolError, 0} : io,
            L"接收会话恢复状态");

    if (needCatalog) {
        m_stats.stage = TransferStage::BuildingPlan;
        m_stats.stageText = L"正在发送二进制文件目录";
        uint32_t sequence = 0;
        size_t index = 0;
        while (index < context.files.size()) {
            std::vector<uint8_t> chunk;
            AppendU32(chunk, sequence++);
            const size_t countOffset = chunk.size();
            AppendU32(chunk, 0);
            uint32_t count = 0;
            while (index < context.files.size()) {
                std::vector<uint8_t> encoded;
                EncodeCatalogEntry(encoded, index, context.files[index]);
                if (count > 0 && chunk.size() + encoded.size() > CATALOG_CHUNK_BYTES)
                    break;
                AppendBytes(chunk, encoded.data(), encoded.size());
                ++index;
                ++count;
            }
            WriteU32(chunk, countOffset, count);
            io = SendPacket(socket, static_cast<uint8_t>(PacketType::CATALOG_CHUNK), chunk);
            if (!io.IsOk())
                return IoFailure(io, L"发送文件目录");
            m_stats.stageProcessed = index;
            m_stats.stageTotal = context.files.size();
            ReportProgress();
        }
        std::vector<uint8_t> done;
        AppendU32(done, context.catalogCrc);
        io = SendPacket(socket, static_cast<uint8_t>(PacketType::CATALOG_DONE), done);
        if (!io.IsOk())
            return IoFailure(io, L"提交文件目录");
        ready.clear();
        ranges.clear();
        io = receiveExpected(PacketType::SESSION_READY, ready);
        if (!io.IsOk() || !ParseReadyPayload(ready, needCatalog, ranges) || needCatalog)
            return MakeResult(TransferResultCode::ProtocolError, L"接收端未接受文件目录");
    }

    for (const auto& range : ranges) {
        if (range.first > context.files.size() ||
            range.second > context.files.size() - range.first) {
            return MakeResult(TransferResultCode::ProtocolError, L"接收端恢复范围无效");
        }
        for (uint64_t id = range.first; id < range.first + range.second; ++id) {
            if (context.committed.insert(id).second)
                context.progress.AddCommitted(context.files[static_cast<size_t>(id)].size);
        }
    }
    m_stats.completedFiles = static_cast<int>(context.committed.size());
    UpdateStats(context.progress, m_stats);
    m_stats.stage = TransferStage::Transferring;
    m_stats.stageText = L"正在传输文件";
    m_stats.stageProcessed = context.committed.size();
    m_stats.stageTotal = context.files.size();
    m_stats.waitingForIo = false;
    ReportProgress(true);

    struct BatchDescriptor {
        uint64_t id = 0;
        std::vector<uint64_t> files;
        int64_t bytes = 0;
    };

    std::vector<uint64_t> smallFiles;
    for (uint64_t id = 0; id < context.files.size(); ++id) {
        if (context.files[static_cast<size_t>(id)].size <= SMALL_FILE_LIMIT &&
            context.committed.find(id) == context.committed.end()) {
            smallFiles.push_back(id);
        }
    }

    size_t smallCursor = 0;
    std::unordered_map<uint64_t, BatchDescriptor> pendingBatches;
    pendingBatches.reserve(SMALL_BATCH_WINDOW);
    auto sendNextSmallBatch = [&]() -> TransferResult {
        BatchDescriptor descriptor;
        descriptor.id = context.nextBatchId++;
        std::vector<uint8_t> batch;
        AppendU64(batch, descriptor.id);
        const size_t countOffset = batch.size();
        AppendU32(batch, 0);
        uint32_t count = 0;
        while (smallCursor < smallFiles.size() && count < SMALL_BATCH_MAX_FILES) {
            const uint64_t fileId = smallFiles[smallCursor];
            const FileEntry& entry = context.files[static_cast<size_t>(fileId)];
            const size_t recordBytes = 8 + 8 + 4 + static_cast<size_t>(entry.size);
            if (count > 0 && batch.size() + recordBytes > SMALL_BATCH_MAX_BYTES)
                break;
            const std::wstring fullPath = sourceDir + L"\\" + entry.relativePath;
            if (!SourceFileMatches(fullPath, entry))
                return MakeResult(TransferResultCode::FileError,
                    L"源文件在盘点后发生变化：" + entry.relativePath);
            ScopedHandle file(CreateFileW(utils::NormalizePath(fullPath).c_str(),
                GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
            if (file.Get() == INVALID_HANDLE_VALUE)
                return MakeResult(TransferResultCode::FileError,
                    L"无法读取源文件：" + entry.relativePath);
            std::vector<uint8_t> data(static_cast<size_t>(entry.size));
            size_t readTotal = 0;
            while (readTotal < data.size()) {
                DWORD readNow = 0;
                const DWORD request = static_cast<DWORD>(data.size() - readTotal);
                if (!ReadFile(file.Get(), data.data() + readTotal, request, &readNow, nullptr) ||
                    readNow == 0) {
                    return MakeResult(TransferResultCode::FileError,
                        L"读取源文件失败：" + entry.relativePath);
                }
                readTotal += readNow;
            }
            uint8_t extra = 0;
            DWORD extraRead = 0;
            if (!ReadFile(file.Get(), &extra, 1, &extraRead, nullptr) || extraRead != 0 ||
                !SourceFileMatches(fullPath, entry)) {
                return MakeResult(TransferResultCode::FileError,
                    L"源文件在读取期间发生变化：" + entry.relativePath);
            }
            AppendU64(batch, fileId);
            AppendU64(batch, static_cast<uint64_t>(entry.size));
            AppendU32(batch, Crc32::Compute(data.data(), data.size()));
            AppendBytes(batch, data.data(), data.size());
            descriptor.files.push_back(fileId);
            descriptor.bytes += entry.size;
            ++smallCursor;
            ++count;
        }
        WriteU32(batch, countOffset, count);
        io = SendPacket(socket, static_cast<uint8_t>(PacketType::BATCH_DATA), batch);
        if (!io.IsOk())
            return IoFailure(io, L"发送小文件批次");
        pendingBatches.emplace(descriptor.id, std::move(descriptor));
        return MakeResult(TransferResultCode::Success, L"");
    };

    while (smallCursor < smallFiles.size() || !pendingBatches.empty()) {
        while (smallCursor < smallFiles.size() &&
                pendingBatches.size() < SMALL_BATCH_WINDOW) {
            TransferResult sent = sendNextSmallBatch();
            if (sent.code != TransferResultCode::Success)
                return sent;
        }

        std::vector<uint8_t> ack;
        io = receiveExpected(PacketType::BATCH_ACK, ack);
        if (!io.IsOk())
            return IoFailure(io, L"接收小文件批次确认");
        size_t pos = 0;
        uint64_t batchId = 0;
        uint8_t status = 0;
        if (!ReadU64(ack, pos, batchId) || !ReadU8(ack, pos, status) ||
            pos != ack.size()) {
            return MakeResult(TransferResultCode::ProtocolError, L"小文件批次确认不匹配");
        }
        const auto pending = pendingBatches.find(batchId);
        if (pending == pendingBatches.end()) {
            return MakeResult(TransferResultCode::ConnectionLost,
                L"小文件批次确认不属于当前窗口，正在重连恢复");
        }
        if (status != 1)
            return MakeResult(TransferResultCode::FileError, L"接收端提交小文件批次失败");
        BatchDescriptor descriptor = std::move(pending->second);
        pendingBatches.erase(pending);
        int newlyCommitted = 0;
        int64_t newlyCommittedBytes = 0;
        for (uint64_t id : descriptor.files) {
            if (context.committed.insert(id).second) {
                ++newlyCommitted;
                newlyCommittedBytes += context.files[static_cast<size_t>(id)].size;
            }
        }
        context.progress.AddCommitted(newlyCommittedBytes, newlyCommitted);
        m_stats.completedFiles = static_cast<int>(context.committed.size());
        m_stats.stageProcessed = context.committed.size();
        if (!descriptor.files.empty())
            m_stats.currentFile = context.files[static_cast<size_t>(descriptor.files.back())].relativePath;
        UpdateStats(context.progress, m_stats);
        ReportProgress();
    }

    for (uint64_t fileId = 0; fileId < context.files.size(); ++fileId) {
        const FileEntry& entry = context.files[static_cast<size_t>(fileId)];
        if (entry.size <= SMALL_FILE_LIMIT || context.committed.count(fileId) != 0)
            continue;
        const std::wstring fullPath = sourceDir + L"\\" + entry.relativePath;
        if (!SourceFileMatches(fullPath, entry))
            return MakeResult(TransferResultCode::FileError,
                L"源文件在盘点后发生变化：" + entry.relativePath);
        ScopedHandle file(CreateFileW(utils::NormalizePath(fullPath).c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (file.Get() == INVALID_HANDLE_VALUE)
            return MakeResult(TransferResultCode::FileError, L"无法读取源文件：" + entry.relativePath);

        std::vector<uint8_t> beginFile;
        AppendU64(beginFile, fileId);
        AppendU64(beginFile, static_cast<uint64_t>(entry.size));
        io = SendPacket(socket, static_cast<uint8_t>(PacketType::FILE_BEGIN_V5), beginFile);
        if (!io.IsOk())
            return IoFailure(io, L"发送大文件头");
        m_stats.currentFile = entry.relativePath;
        m_stats.stageBytes = 0;
        context.progress.SetInFlight(0);
        ReportProgress();

        constexpr size_t LARGE_FILE_PIPELINE_DEPTH = 4;
        std::mutex pipelineMutex;
        std::condition_variable pipelineReady;
        std::condition_variable pipelineSpace;
        std::deque<std::vector<uint8_t>> readyChunks;
        bool producerDone = false;
        bool producerCancelled = false;
        bool producerError = false;
        uint32_t fileCrc = 0;

        std::thread reader([&]() {
            Crc32 crc;
            int64_t unread = entry.size;
            while (unread > 0) {
                const DWORD request = unread > LARGE_FILE_CHUNK
                    ? LARGE_FILE_CHUNK : static_cast<DWORD>(unread);
                std::vector<uint8_t> data(8 + request);
                for (int i = 0; i < 8; ++i)
                    data[static_cast<size_t>(i)] = static_cast<uint8_t>(fileId >> (i * 8));
                DWORD readNow = 0;
                if (!ReadFile(file.Get(), data.data() + 8, request, &readNow, nullptr) ||
                    readNow == 0) {
                    std::lock_guard<std::mutex> lock(pipelineMutex);
                    producerError = true;
                    producerDone = true;
                    pipelineReady.notify_all();
                    return;
                }
                data.resize(8 + readNow);
                crc.Update(data.data() + 8, readNow);
                unread -= readNow;

                std::unique_lock<std::mutex> lock(pipelineMutex);
                pipelineSpace.wait(lock, [&]() {
                    return producerCancelled || !m_running ||
                        readyChunks.size() < LARGE_FILE_PIPELINE_DEPTH;
                });
                if (producerCancelled || !m_running) {
                    producerDone = true;
                    pipelineReady.notify_all();
                    return;
                }
                readyChunks.push_back(std::move(data));
                lock.unlock();
                pipelineReady.notify_one();
            }

            uint8_t extra = 0;
            DWORD extraRead = 0;
            const bool intact = ReadFile(file.Get(), &extra, 1, &extraRead, nullptr) &&
                extraRead == 0 && SourceFileMatches(fullPath, entry);
            {
                std::lock_guard<std::mutex> lock(pipelineMutex);
                producerError = !intact;
                fileCrc = crc.Finish();
                producerDone = true;
            }
            pipelineReady.notify_all();
        });

        int64_t remaining = entry.size;
        bool firstDataReport = true;
        while (remaining > 0) {
            std::vector<uint8_t> data;
            {
                std::unique_lock<std::mutex> lock(pipelineMutex);
                pipelineReady.wait(lock, [&]() {
                    return !readyChunks.empty() || producerDone || !m_running;
                });
                if (!readyChunks.empty()) {
                    data = std::move(readyChunks.front());
                    readyChunks.pop_front();
                } else {
                    break;
                }
            }
            pipelineSpace.notify_one();
            io = SendPacket(socket, static_cast<uint8_t>(PacketType::FILE_DATA_V5), data);
            if (!io.IsOk()) {
                {
                    std::lock_guard<std::mutex> lock(pipelineMutex);
                    producerCancelled = true;
                }
                pipelineSpace.notify_all();
                reader.join();
                return IoFailure(io, L"发送大文件数据");
            }
            remaining -= static_cast<int64_t>(data.size() - 8);
            m_stats.stageBytes = entry.size - remaining;
            context.progress.SetInFlight(m_stats.stageBytes);
            UpdateStats(context.progress, m_stats);
            ReportProgress(firstDataReport);
            firstDataReport = false;
        }
        reader.join();
        if (producerError || remaining != 0) {
            SendString(socket, static_cast<uint8_t>(PacketType::ERROR_MSG), L"READ_FAILED");
            std::vector<uint8_t> aborted;
            AppendU64(aborted, fileId);
            AppendU32(aborted, 0);
            SendPacket(socket, static_cast<uint8_t>(PacketType::FILE_END_V5), aborted);
            return MakeResult(TransferResultCode::FileError,
                L"源文件在读取期间发生变化：" + entry.relativePath);
        }
        std::vector<uint8_t> endFile;
        AppendU64(endFile, fileId);
        AppendU32(endFile, fileCrc);
        io = SendPacket(socket, static_cast<uint8_t>(PacketType::FILE_END_V5), endFile);
        if (!io.IsOk())
            return IoFailure(io, L"发送大文件完成信息");
        std::vector<uint8_t> ack;
        io = receiveExpected(PacketType::FILE_ACK_V5, ack);
        if (!io.IsOk())
            return IoFailure(io, L"接收大文件确认");
        size_t pos = 0;
        uint64_t ackId = 0;
        uint8_t status = 0;
        if (!ReadU64(ack, pos, ackId) || !ReadU8(ack, pos, status) ||
            pos != ack.size() || ackId != fileId) {
            return MakeResult(TransferResultCode::ProtocolError, L"大文件确认不匹配");
        }
        if (status != 1)
            return MakeResult(TransferResultCode::FileError, L"接收端提交大文件失败");
        if (context.committed.insert(fileId).second)
            context.progress.AddCommitted(entry.size);
        m_stats.completedFiles = static_cast<int>(context.committed.size());
        m_stats.stageProcessed = context.committed.size();
        m_stats.stageBytes = 0;
        UpdateStats(context.progress, m_stats);
        ReportProgress();
    }

    m_stats.stage = TransferStage::Committing;
    m_stats.stageText = L"正在提交传输会话";
    ReportProgress(true);
    std::vector<uint8_t> commit;
    AppendString(commit, utils::ToUtf8(context.transferId));
    io = SendPacket(socket, static_cast<uint8_t>(PacketType::SESSION_COMMIT), commit);
    if (!io.IsOk())
        return IoFailure(io, L"提交传输会话");
    std::vector<uint8_t> commitAck;
    io = receiveExpected(PacketType::SESSION_COMMIT_ACK, commitAck);
    if (!io.IsOk())
        return IoFailure(io, L"接收会话提交确认");
    if (std::string(reinterpret_cast<const char*>(commitAck.data()), commitAck.size()) != "OK")
        return MakeResult(TransferResultCode::FileError, L"接收端拒绝提交传输会话");
    m_stats.currentFile.clear();
    UpdateStats(context.progress, m_stats);
    ReportProgress(true);
    Log(L"所有文件已由接收端提交确认");
    return MakeResult(TransferResultCode::Success, L"传输已完成");
}

void TransferSession::ReceiverWorker(const std::wstring& targetDir, int port,
    const std::wstring& sessionToken, TransferMode mode) {
    try {
        RunReceiver(targetDir, port, sessionToken, mode);
    } catch (...) {
        if (m_doneCb) {
            TransferResult result = FinalizeResult(
                MakeResult(TransferResultCode::InternalError, L"接收线程发生未处理异常"));
            m_doneCb(result);
        }
    }
    CloseActiveSocket();
    m_running = false;
}

void TransferSession::RunReceiver(const std::wstring& targetDir, int port,
    const std::wstring& sessionToken, TransferMode mode) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        if (m_doneCb) {
            TransferResult result = FinalizeResult(
                MakeResult(TransferResultCode::InternalError, L"初始化 Winsock 失败"));
            m_doneCb(result);
        }
        return;
    }
    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) {
        if (m_doneCb) {
            TransferResult result = FinalizeResult(
                MakeResult(TransferResultCode::InternalError, L"创建监听 Socket 失败"));
            m_doneCb(result);
        }
        WSACleanup();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_sockMutex);
        m_listenSock = listenSocket;
    }
    int reuse = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR,
        reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(port));
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listenSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(listenSocket, 2) == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        m_lastSocketError.store(error);
        closesocket(listenSocket);
        {
            std::lock_guard<std::mutex> lock(m_sockMutex);
            m_listenSock = INVALID_SOCKET;
        }
        WSACleanup();
        if (m_doneCb) {
            TransferResult result = FinalizeResult(MakeResult(TransferResultCode::FileError,
                L"监听端口失败，错误码：" + std::to_wstring(error)));
            m_doneCb(result);
        }
        return;
    }

    ReceiverTaskState state;
    Log(L"正在监听端口 " + std::to_wstring(port));
    while (m_running) {
        sockaddr_in peer{};
        int peerLength = sizeof(peer);
        SOCKET socket = accept(listenSocket, reinterpret_cast<sockaddr*>(&peer), &peerLength);
        if (socket == INVALID_SOCKET)
            break;
        {
            std::lock_guard<std::mutex> lock(m_sockMutex);
            m_sock = socket;
        }
        ConfigureSocket(socket, HANDSHAKE_TIMEOUT_MS);
        ReceiverConnectionResult connection = HandleReceiverConnection(
            socket, targetDir, sessionToken, mode, state);
        CloseActiveSocket(socket);
        if (!m_running)
            break;
        if (connection == ReceiverConnectionResult::ConnectionLost) {
            state.progress.SetInFlight(0);
            m_stats.stage = TransferStage::Reconnecting;
            m_stats.stageText = L"连接中断，正在等待发送端自动重连";
            ++m_stats.retryCount;
            UpdateStats(state.progress, m_stats);
            ReportProgress(true);
            Log(L"连接中断，已提交文件保持不变，等待自动重连");
            continue;
        }
        if (connection == ReceiverConnectionResult::Completed) {
            if (m_doneCb) {
                TransferResult result = FinalizeResult(
                    MakeResult(TransferResultCode::Success, L"接收已完成"));
                m_doneCb(result);
            }
            continue;
        }
        if (connection == ReceiverConnectionResult::Failed && m_doneCb) {
            TransferResult result = FinalizeResult(
                MakeResult(TransferResultCode::FileError, L"接收失败"));
            m_doneCb(result);
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_sockMutex);
        if (m_listenSock == listenSocket)
            m_listenSock = INVALID_SOCKET;
    }
    closesocket(listenSocket);
    WSACleanup();
    if (m_running && m_doneCb) {
        TransferResult result = FinalizeResult(
            MakeResult(TransferResultCode::ConnectionLost, L"接收监听意外停止"));
        m_doneCb(result);
    }
}

ReceiverConnectionResult TransferSession::HandleReceiverConnection(SOCKET socket,
    const std::wstring& targetDir, const std::wstring& sessionToken,
    TransferMode mode, ReceiverTaskState& state) {
    IoResult io = SendString(socket, static_cast<uint8_t>(PacketType::SERVER_HELLO),
        BuildHelloPayload(sessionToken));
    if (!io.IsOk())
        return ReceiverConnectionResult::ConnectionLost;
    std::wstring clientHello;
    io = RecvString(socket, static_cast<uint8_t>(PacketType::CLIENT_HELLO), clientHello);
    if (!io.IsOk())
        return ReceiverConnectionResult::ConnectionLost;
    std::wstring clientToken;
    std::wstring peerVersion;
    if (!ParseHelloPayload(clientHello, clientToken, peerVersion) || clientToken != sessionToken) {
        SendString(socket, static_cast<uint8_t>(PacketType::HELLO_ACK), L"FAIL");
        return ReceiverConnectionResult::HandshakeRejected;
    }
    io = SendString(socket, static_cast<uint8_t>(PacketType::HELLO_ACK), L"OK");
    if (!io.IsOk())
        return ReceiverConnectionResult::ConnectionLost;
    if (!ConfigureSocket(socket, IO_WAIT_TIMEOUT_MS))
        return ReceiverConnectionResult::ConnectionLost;
    ConfigureTransferSocket(socket);

    uint8_t type = 0;
    std::vector<uint8_t> payload;
    io = RecvPacket(socket, type, payload);
    if (!io.IsOk())
        return ReceiverConnectionResult::ConnectionLost;
    if (type != static_cast<uint8_t>(PacketType::SESSION_BEGIN))
        return ReceiverConnectionResult::Failed;
    size_t pos = 0;
    std::string transferIdUtf8;
    uint32_t catalogCrc = 0;
    uint64_t expectedFiles = 0;
    uint64_t expectedBytes = 0;
    if (!ReadString(payload, pos, transferIdUtf8) || !ReadU32(payload, pos, catalogCrc) ||
        !ReadU64(payload, pos, expectedFiles) || !ReadU64(payload, pos, expectedBytes) ||
        pos != payload.size() || expectedFiles > static_cast<uint64_t>(INT_MAX) ||
        expectedBytes > static_cast<uint64_t>(INT64_MAX)) {
        return ReceiverConnectionResult::Failed;
    }
    const std::wstring transferId = utils::FromUtf8(transferIdUtf8);
    if (transferId.empty())
        return ReceiverConnectionResult::Failed;
    if (state.transferId != transferId) {
        state.Reset(transferId, catalogCrc, expectedFiles, expectedBytes);
        m_stats = TransferStats{};
        m_stats.totalFiles = static_cast<int>(expectedFiles);
        m_stats.totalBytes = static_cast<int64_t>(expectedBytes);
        m_stats.stage = TransferStage::BuildingPlan;
        m_stats.stageText = L"正在接收文件目录";
        UpdateStats(state.progress, m_stats);
        ReportProgress(true);
    } else if (state.catalogCrc != catalogCrc || state.expectedFiles != expectedFiles ||
        state.expectedBytes != expectedBytes) {
        SendString(socket, static_cast<uint8_t>(PacketType::ERROR_MSG), L"SESSION_MISMATCH");
        return ReceiverConnectionResult::Failed;
    } else if (!state.catalogReady) {
        state.catalog.clear();
        state.nextCatalogSequence = 0;
    }

    std::vector<uint8_t> ready = BuildReadyPayload(!state.catalogReady, state.committed);
    io = SendPacket(socket, static_cast<uint8_t>(PacketType::SESSION_READY), ready);
    if (!io.IsOk())
        return ReceiverConnectionResult::ConnectionLost;

    if (!state.catalogReady) {
        for (;;) {
            payload.clear();
            io = RecvPacket(socket, type, payload);
            if (!io.IsOk())
                return ReceiverConnectionResult::ConnectionLost;
            if (type == static_cast<uint8_t>(PacketType::CATALOG_CHUNK)) {
                size_t chunkPos = 0;
                uint32_t sequence = 0;
                uint32_t count = 0;
                if (!ReadU32(payload, chunkPos, sequence) || !ReadU32(payload, chunkPos, count) ||
                    sequence != state.nextCatalogSequence++) {
                    return ReceiverConnectionResult::Failed;
                }
                for (uint32_t i = 0; i < count; ++i) {
                    uint64_t id = 0;
                    FileEntry entry;
                    if (!DecodeCatalogEntry(payload, chunkPos, id, entry) ||
                        id != state.catalog.size()) {
                        return ReceiverConnectionResult::Failed;
                    }
                    state.catalog.push_back(std::move(entry));
                }
                if (chunkPos != payload.size() || state.catalog.size() > state.expectedFiles)
                    return ReceiverConnectionResult::Failed;
                m_stats.stageProcessed = state.catalog.size();
                m_stats.stageTotal = state.expectedFiles;
                ReportProgress();
                continue;
            }
            if (type != static_cast<uint8_t>(PacketType::CATALOG_DONE))
                return ReceiverConnectionResult::Failed;
            size_t donePos = 0;
            uint32_t sentCrc = 0;
            if (!ReadU32(payload, donePos, sentCrc) || donePos != payload.size() ||
                sentCrc != state.catalogCrc || state.catalog.size() != state.expectedFiles ||
                ComputeCatalogCrc(state.catalog) != state.catalogCrc) {
                return ReceiverConnectionResult::Failed;
            }
            int64_t catalogBytes = 0;
            for (const auto& entry : state.catalog) {
                if (entry.size < 0 || catalogBytes > INT64_MAX - entry.size)
                    return ReceiverConnectionResult::Failed;
                catalogBytes += entry.size;
            }
            if (catalogBytes != static_cast<int64_t>(state.expectedBytes))
                return ReceiverConnectionResult::Failed;
            state.committed.assign(state.catalog.size(), false);
            state.catalogReady = true;
            ready = BuildReadyPayload(false, state.committed);
            io = SendPacket(socket, static_cast<uint8_t>(PacketType::SESSION_READY), ready);
            if (!io.IsOk())
                return ReceiverConnectionResult::ConnectionLost;
            break;
        }
    }

    m_stats.stage = TransferStage::Transferring;
    m_stats.stageText = L"正在接收文件";
    m_stats.stageTotal = state.catalog.size();
    m_stats.stageProcessed = state.CommittedCount();
    m_stats.completedFiles = static_cast<int>(state.CommittedCount());
    UpdateStats(state.progress, m_stats);
    ReportProgress(true);

    std::unordered_set<std::wstring> preparedDirectories;
    auto prepareTarget = [&](const FileEntry& entry, std::wstring& fullPath) -> bool {
        fullPath = targetDir + L"\\" + entry.relativePath;
        const size_t separator = fullPath.find_last_of(L'\\');
        if (separator == std::wstring::npos)
            return false;
        const std::wstring directory = fullPath.substr(0, separator);
        if (preparedDirectories.count(directory) != 0)
            return true;
        if (!utils::CreateDirectoryTree(directory))
            return false;
        preparedDirectories.insert(directory);
        return true;
    };
    const size_t writerCount = ChooseSmallFileWriterCount(targetDir);
    std::mutex smallBatchMutex;
    SmallFileWriterPool smallFileWriters(writerCount);
    Log(L"小文件接收写入线程：" + std::to_wstring(writerCount));

    for (;;) {
        payload.clear();
        io = RecvPacket(socket, type, payload);
        if (!io.IsOk())
            return m_running ? ReceiverConnectionResult::ConnectionLost
                             : ReceiverConnectionResult::Cancelled;

        if (type == static_cast<uint8_t>(PacketType::BATCH_DATA)) {
            auto batchPayload = std::make_shared<std::vector<uint8_t>>(std::move(payload));
            const std::vector<uint8_t>& batchData = *batchPayload;
            size_t batchPos = 0;
            uint64_t batchId = 0;
            uint32_t count = 0;
            bool batchOk = ReadU64(batchData, batchPos, batchId) &&
                ReadU32(batchData, batchPos, count) && count <= SMALL_BATCH_MAX_FILES;
            std::vector<uint64_t> requestIds;
            std::vector<SmallFileWriterPool::Request> requests;
            std::unordered_set<uint64_t> batchIds;
            requestIds.reserve(count);
            requests.reserve(count);
            for (uint32_t i = 0; batchOk && i < count; ++i) {
                uint64_t fileId = 0;
                uint64_t fileSize = 0;
                uint32_t expectedCrc = 0;
                if (!ReadU64(batchData, batchPos, fileId) ||
                    !ReadU64(batchData, batchPos, fileSize) ||
                    !ReadU32(batchData, batchPos, expectedCrc) ||
                    !batchIds.insert(fileId).second || fileId >= state.catalog.size() ||
                    fileSize != static_cast<uint64_t>(state.catalog[static_cast<size_t>(fileId)].size) ||
                    fileSize > static_cast<uint64_t>(SMALL_FILE_LIMIT) ||
                    batchPos > batchData.size() || batchData.size() - batchPos < fileSize) {
                    batchOk = false;
                    break;
                }
                const size_t fileOffset = batchPos;
                batchPos += static_cast<size_t>(fileSize);
                {
                    std::lock_guard<std::mutex> lock(smallBatchMutex);
                    if (state.committed[static_cast<size_t>(fileId)])
                        continue;
                }
                const FileEntry& entry = state.catalog[static_cast<size_t>(fileId)];
                SmallFileWriterPool::Request request;
                request.fullPath = targetDir + L"\\" + entry.relativePath;
                request.payload = batchPayload;
                request.offset = fileOffset;
                request.size = static_cast<size_t>(fileSize);
                request.expectedCrc = expectedCrc;
                requestIds.push_back(fileId);
                requests.push_back(std::move(request));
            }
            if (batchPos != batchData.size())
                batchOk = false;
            if (!batchOk) {
                std::vector<uint8_t> ack;
                AppendU64(ack, batchId);
                AppendU8(ack, 0);
                SendPacket(socket, static_cast<uint8_t>(PacketType::BATCH_ACK), ack);
                ++m_stats.failedFiles;
                return ReceiverConnectionResult::Failed;
            }

            const size_t requestCount = requests.size();
            smallFileWriters.SubmitBatch(requests,
                [&, batchId, requestCount, requestIds = std::move(requestIds)](
                    std::vector<uint8_t> results) mutable {
                    bool completedOk = results.size() == requestCount;
                    int committedFiles = 0;
                    int64_t committedBytes = 0;
                    for (size_t i = 0; i < results.size(); ++i) {
                        if (results[i] != 1) {
                            completedOk = false;
                            continue;
                        }
                        const uint64_t fileId = requestIds[i];
                        {
                            std::lock_guard<std::mutex> lock(smallBatchMutex);
                            if (!state.committed[static_cast<size_t>(fileId)]) {
                                state.committed[static_cast<size_t>(fileId)] = true;
                                ++committedFiles;
                                committedBytes += state.catalog[static_cast<size_t>(fileId)].size;
                                m_stats.currentFile =
                                    state.catalog[static_cast<size_t>(fileId)].relativePath;
                            }
                        }
                    }
                    {
                        std::lock_guard<std::mutex> lock(smallBatchMutex);
                        if (committedFiles > 0)
                            state.progress.AddCommitted(committedBytes, committedFiles);
                        m_stats.completedFiles = static_cast<int>(state.CommittedCount());
                        m_stats.stageProcessed = m_stats.completedFiles;
                        if (!completedOk)
                            ++m_stats.failedFiles;
                        UpdateStats(state.progress, m_stats);
                        ReportProgress();
                    }
                    std::vector<uint8_t> ack;
                    AppendU64(ack, batchId);
                    AppendU8(ack, completedOk ? 1 : 0);
                    IoResult ackIo = SendPacket(
                        socket, static_cast<uint8_t>(PacketType::BATCH_ACK), ack);
                    if (!ackIo.IsOk())
                        CloseActiveSocket(socket);
                });
            continue;
        }

        if (type == static_cast<uint8_t>(PacketType::FILE_BEGIN_V5)) {
            size_t headerPos = 0;
            uint64_t fileId = 0;
            uint64_t fileSize = 0;
            if (!ReadU64(payload, headerPos, fileId) || !ReadU64(payload, headerPos, fileSize) ||
                headerPos != payload.size() || fileId >= state.catalog.size() ||
                fileSize != static_cast<uint64_t>(state.catalog[static_cast<size_t>(fileId)].size) ||
                fileSize <= static_cast<uint64_t>(SMALL_FILE_LIMIT)) {
                return ReceiverConnectionResult::Failed;
            }
            const FileEntry& entry = state.catalog[static_cast<size_t>(fileId)];
            std::wstring fullPath;
            bool fileOk = prepareTarget(entry, fullPath);
            const std::wstring partPath = fullPath + L".dtpart";
            ScopedHandle file(fileOk
                ? CreateFileW(utils::NormalizePath(partPath).c_str(), GENERIC_WRITE, 0,
                    nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)
                : INVALID_HANDLE_VALUE);
            if (file.Get() == INVALID_HANDLE_VALUE)
                fileOk = false;
            constexpr size_t LARGE_FILE_PIPELINE_DEPTH = 4;
            std::mutex writeMutex;
            std::condition_variable writeReady;
            std::condition_variable writeSpace;
            std::deque<std::vector<uint8_t>> pendingWrites;
            bool receiveDone = false;
            uint32_t actualCrc = 0;
            std::thread writer([&]() {
                Crc32 crc;
                for (;;) {
                    std::vector<uint8_t> chunk;
                    {
                        std::unique_lock<std::mutex> lock(writeMutex);
                        writeReady.wait(lock, [&]() {
                            return receiveDone || !pendingWrites.empty();
                        });
                        if (pendingWrites.empty()) {
                            if (receiveDone)
                                break;
                            continue;
                        }
                        chunk = std::move(pendingWrites.front());
                        pendingWrites.pop_front();
                    }
                    writeSpace.notify_one();
                    const DWORD dataSize = static_cast<DWORD>(chunk.size() - 8);
                    crc.Update(chunk.data() + 8, dataSize);
                    DWORD written = 0;
                    if (fileOk && (!WriteFile(file.Get(), chunk.data() + 8,
                            dataSize, &written, nullptr) || written != dataSize)) {
                        fileOk = false;
                    }
                }
                actualCrc = crc.Finish();
            });
            uint64_t remaining = fileSize;
            bool senderAborted = false;
            bool receiveFailed = false;
            bool protocolFailed = false;
            bool firstDataReport = true;
            while (remaining > 0) {
                std::vector<uint8_t> chunk;
                uint8_t chunkType = 0;
                io = RecvPacket(socket, chunkType, chunk);
                if (!io.IsOk()) {
                    receiveFailed = true;
                    break;
                }
                if (chunkType == static_cast<uint8_t>(PacketType::ERROR_MSG)) {
                    senderAborted = true;
                    break;
                }
                if (chunkType != static_cast<uint8_t>(PacketType::FILE_DATA_V5)) {
                    protocolFailed = true;
                    break;
                }
                size_t chunkPos = 0;
                uint64_t chunkId = 0;
                if (!ReadU64(chunk, chunkPos, chunkId) || chunkId != fileId ||
                    chunkPos >= chunk.size() || chunk.size() - chunkPos > remaining) {
                    protocolFailed = true;
                    break;
                }
                const DWORD dataSize = static_cast<DWORD>(chunk.size() - chunkPos);
                {
                    std::unique_lock<std::mutex> lock(writeMutex);
                    writeSpace.wait(lock, [&]() {
                        return pendingWrites.size() < LARGE_FILE_PIPELINE_DEPTH;
                    });
                    pendingWrites.push_back(std::move(chunk));
                }
                writeReady.notify_one();
                remaining -= dataSize;
                m_stats.currentFile = entry.relativePath;
                m_stats.stageBytes = static_cast<int64_t>(fileSize - remaining);
                state.progress.SetInFlight(m_stats.stageBytes);
                UpdateStats(state.progress, m_stats);
                ReportProgress(firstDataReport);
                firstDataReport = false;
            }
            {
                std::lock_guard<std::mutex> lock(writeMutex);
                receiveDone = true;
            }
            writeReady.notify_all();
            writer.join();
            if (receiveFailed)
                return ReceiverConnectionResult::ConnectionLost;
            if (protocolFailed)
                return ReceiverConnectionResult::Failed;
            std::vector<uint8_t> end;
            uint8_t endType = 0;
            io = RecvPacket(socket, endType, end);
            if (!io.IsOk())
                return ReceiverConnectionResult::ConnectionLost;
            size_t endPos = 0;
            uint64_t endId = 0;
            uint32_t expectedCrc = 0;
            if (endType != static_cast<uint8_t>(PacketType::FILE_END_V5) ||
                !ReadU64(end, endPos, endId) || !ReadU32(end, endPos, expectedCrc) ||
                endPos != end.size() || endId != fileId) {
                return ReceiverConnectionResult::Failed;
            }
            if (remaining != 0 || senderAborted || actualCrc != expectedCrc)
                fileOk = false;
            file.Close();
            if (fileOk && !state.committed[static_cast<size_t>(fileId)]) {
                if (!MoveFileExW(utils::NormalizePath(partPath).c_str(),
                        utils::NormalizePath(fullPath).c_str(), MOVEFILE_REPLACE_EXISTING)) {
                    fileOk = false;
                } else {
                    state.committed[static_cast<size_t>(fileId)] = true;
                    state.progress.AddCommitted(entry.size);
                    m_stats.completedFiles = static_cast<int>(state.CommittedCount());
                    m_stats.stageProcessed = m_stats.completedFiles;
                    UpdateStats(state.progress, m_stats);
                }
            }
            std::vector<uint8_t> ack;
            AppendU64(ack, fileId);
            AppendU8(ack, fileOk ? 1 : 0);
            io = SendPacket(socket, static_cast<uint8_t>(PacketType::FILE_ACK_V5), ack);
            if (!io.IsOk())
                return ReceiverConnectionResult::ConnectionLost;
            m_stats.stageBytes = 0;
            ReportProgress(true);
            if (!fileOk) {
                ++m_stats.failedFiles;
                return ReceiverConnectionResult::Failed;
            }
            continue;
        }

        if (type == static_cast<uint8_t>(PacketType::SESSION_COMMIT)) {
            size_t commitPos = 0;
            std::string commitId;
            if (!ReadString(payload, commitPos, commitId) || commitPos != payload.size() ||
                utils::FromUtf8(commitId) != state.transferId ||
                state.CommittedCount() != state.catalog.size()) {
                SendString(socket, static_cast<uint8_t>(PacketType::SESSION_COMMIT_ACK), L"FAIL");
                return ReceiverConnectionResult::Failed;
            }
            m_stats.stage = TransferStage::Committing;
            m_stats.stageText = L"正在提交传输会话";
            ReportProgress(true);
            if (mode == TransferMode::MIRROR) {
                Manifest manifest(state.catalog);
                const int deleted = TransferPlanner::DeleteExtraFiles(manifest, targetDir);
                Log(L"镜像清理完成：删除 " + std::to_wstring(deleted) + L" 个目标端额外文件");
            }
            state.completed = true;
            m_stats.currentFile.clear();
            UpdateStats(state.progress, m_stats);
            io = SendString(socket, static_cast<uint8_t>(PacketType::SESSION_COMMIT_ACK), L"OK");
            if (!io.IsOk())
                return ReceiverConnectionResult::ConnectionLost;
            ReportProgress(true);
            return ReceiverConnectionResult::Completed;
        }

        if (type == static_cast<uint8_t>(PacketType::ERROR_MSG))
            return ReceiverConnectionResult::Failed;
        return ReceiverConnectionResult::Failed;
    }
}
