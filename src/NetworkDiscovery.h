#pragma once
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

struct PeerInfo {
    std::wstring ip;
    std::wstring machineName;
    std::wstring sessionToken;
    int protocolVersion = 0;
    int port = 0;
};

class NetworkDiscovery {
public:
    NetworkDiscovery();
    ~NetworkDiscovery();

    bool StartSenderDiscovery(HWND hNotifyWnd, int port,
        const std::wstring& directIP = L"");
    bool StartReceiverListener(HWND hNotifyWnd, int port,
        const std::wstring& sessionToken);
    void Stop();

    PeerInfo GetLastPeer() const;
    static std::wstring GetLocalIP();

private:
    SOCKET m_sock = INVALID_SOCKET;
    std::atomic<bool> m_running{false};
    std::thread m_worker;
    HWND m_hNotifyWnd = nullptr;
    PeerInfo m_lastPeer;
    mutable std::mutex m_peerMutex;
    std::wstring m_sessionToken;
    std::wstring m_directIP;

    void SenderWorker(int port, std::wstring directIP);
    void ReceiverWorker(int port);
    void Notify(DWORD msg);
    static std::wstring GetMachineName();
};
