#include "NetworkDiscovery.h"
#include "Resource.h"
#include "Utils.h"
#include "Version.h"
#include <ws2tcpip.h>
#include <vector>

NetworkDiscovery::NetworkDiscovery() {}
NetworkDiscovery::~NetworkDiscovery() { Stop(); }

void NetworkDiscovery::Notify(DWORD msg) {
    if (m_hNotifyWnd)
        PostMessageW(m_hNotifyWnd, msg, 0, 0);
}

bool NetworkDiscovery::StartSenderDiscovery(HWND hNotifyWnd, int port,
    const std::wstring& directIP) {
    Stop();
    m_hNotifyWnd = hNotifyWnd;
    m_running = true;
    m_worker = std::thread(&NetworkDiscovery::SenderWorker, this, port, directIP);
    return true;
}

bool NetworkDiscovery::StartReceiverListener(HWND hNotifyWnd, int port,
    const std::wstring& sessionToken) {
    Stop();
    m_hNotifyWnd = hNotifyWnd;
    m_running = true;
    m_sessionToken = sessionToken;
    m_worker = std::thread(&NetworkDiscovery::ReceiverWorker, this, port);
    return true;
}

PeerInfo NetworkDiscovery::GetLastPeer() const {
    std::lock_guard<std::mutex> lock(m_peerMutex);
    return m_lastPeer;
}

void NetworkDiscovery::Stop() {
    m_running = false;
    if (m_sock != INVALID_SOCKET) {
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
    }
    if (m_worker.joinable())
        m_worker.join();
}

std::wstring NetworkDiscovery::GetMachineName() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD len = MAX_COMPUTERNAME_LENGTH + 1;
    GetComputerNameW(name, &len);
    return name;
}

static std::vector<sockaddr_in> GetBroadcastTargets(int port) {
    std::vector<sockaddr_in> targets;
    sockaddr_in bc = {};
    bc.sin_family = AF_INET;
    bc.sin_port = htons((u_short)port);
    bc.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    targets.push_back(bc);
    return targets;
}

void NetworkDiscovery::SenderWorker(int port, std::wstring directIP) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        Notify(WM_DISCOVERY_FAILED);
        WSACleanup();
        return;
    }

    m_sock = sock;
    BOOL broadcast = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (char*)&broadcast, sizeof(broadcast));

    u_long nonblock = 1;
    ioctlsocket(sock, FIONBIO, &nonblock);

    auto targets = GetBroadcastTargets(port);
    if (!directIP.empty()) {
        sockaddr_in direct = {};
        direct.sin_family = AF_INET;
        direct.sin_port = htons((u_short)port);
        std::string directA = utils::ToUtf8(directIP);
        if (InetPtonA(AF_INET, directA.c_str(), &direct.sin_addr) == 1)
            targets.push_back(direct);
    }

    std::wstring wPacket = L"DirectTransfer:DISCOVER:"
        + std::to_wstring(version::PROTOCOL_VERSION) + L":" + GetMachineName();
    std::string packet = utils::ToUtf8(wPacket);

    sockaddr_in from = {};
    char recvBuf[1024] = {};

    int ticks = 0;
    while (m_running) {
        if (ticks % 4 == 0) {
            for (const auto& t : targets) {
                int sent = sendto(sock, packet.c_str(), (int)packet.size(), 0,
                    (sockaddr*)&t, sizeof(t));
                if (sent == SOCKET_ERROR) {
                    int err = WSAGetLastError();
                    if (err != WSAEWOULDBLOCK && err != WSAENETUNREACH && err != WSAEADDRNOTAVAIL)
                        break;
                }
            }
        }
        ++ticks;

        int fromLen = sizeof(from);
        int received = recvfrom(sock, recvBuf, sizeof(recvBuf) - 1, 0,
            (sockaddr*)&from, &fromLen);
        if (received > 0) {
            recvBuf[received] = '\0';
            std::string resp(recvBuf);
            if (resp.find("DirectTransfer:RESPONSE:") == 0) {
                std::string fields = resp.substr(24);
                size_t first = fields.find(':');
                size_t second = first == std::string::npos
                    ? std::string::npos : fields.find(':', first + 1);
                if (first == std::string::npos || second == std::string::npos)
                    continue;
                int protocolVersion = atoi(fields.substr(0, first).c_str());
                std::string token = fields.substr(first + 1, second - first - 1);
                std::string remoteMachine = fields.substr(second + 1);
                char ipBuf[64] = {};
                InetNtopA(AF_INET, &from.sin_addr, ipBuf, sizeof(ipBuf));
                {
                    std::lock_guard<std::mutex> lock(m_peerMutex);
                    m_lastPeer.ip = utils::FromUtf8(ipBuf);
                    m_lastPeer.machineName = utils::FromUtf8(remoteMachine);
                    m_lastPeer.sessionToken = utils::FromUtf8(token);
                    m_lastPeer.protocolVersion = protocolVersion;
                    m_lastPeer.port = port;
                }

                Notify(WM_DISCOVERY_FOUND);
                break;
            }
        }
        Sleep(500);
    }

    closesocket(sock);
    m_sock = INVALID_SOCKET;
    WSACleanup();
}

void NetworkDiscovery::ReceiverWorker(int port) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) { WSACleanup(); return; }

    m_sock = sock;
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(sock); m_sock = INVALID_SOCKET; WSACleanup(); return;
    }

    u_long nonblock = 1;
    ioctlsocket(sock, FIONBIO, &nonblock);

    std::wstring wResp = L"DirectTransfer:RESPONSE:"
        + std::to_wstring(version::PROTOCOL_VERSION) + L":"
        + m_sessionToken + L":" + GetMachineName();
    std::string response = utils::ToUtf8(wResp);

    sockaddr_in from = {};
    char recvBuf[1024] = {};

    while (m_running) {
        int fromLen = sizeof(from);
        int received = recvfrom(sock, recvBuf, sizeof(recvBuf) - 1, 0,
            (sockaddr*)&from, &fromLen);
        if (received > 0) {
            recvBuf[received] = '\0';
            std::string req(recvBuf);
            if (req.find("DirectTransfer:DISCOVER:") == 0) {
                sendto(sock, response.c_str(), (int)response.size(), 0,
                    (sockaddr*)&from, sizeof(from));

                std::string remoteMachine = req.substr(23);
                size_t separator = remoteMachine.find(':');
                if (separator != std::string::npos)
                    remoteMachine = remoteMachine.substr(separator + 1);
                char ipBuf[64] = {};
                InetNtopA(AF_INET, &from.sin_addr, ipBuf, sizeof(ipBuf));

                {
                    std::lock_guard<std::mutex> lock(m_peerMutex);
                    m_lastPeer.machineName = utils::FromUtf8(remoteMachine);
                    m_lastPeer.ip = utils::FromUtf8(ipBuf);
                    m_lastPeer.protocolVersion = version::PROTOCOL_VERSION;
                    m_lastPeer.port = port;
                }

                Notify(WM_DISCOVERY_FOUND);
            }
        }
        Sleep(200);
    }

    closesocket(sock);
    m_sock = INVALID_SOCKET;
    WSACleanup();
}

std::wstring NetworkDiscovery::GetLocalIP() {
    return utils::GetLocalIPString();
}
