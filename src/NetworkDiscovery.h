#pragma once
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <string>
#include <functional>
#include <thread>
#include <atomic>

struct PeerInfo {
    std::wstring ip;
    std::wstring machineName;
    int port = 0;
};

class NetworkDiscovery {
public:
    NetworkDiscovery();
    ~NetworkDiscovery();

    bool StartSenderDiscovery(HWND hNotifyWnd, int port);
    bool StartReceiverListener(HWND hNotifyWnd, int port);
    void Stop();

    PeerInfo GetLastPeer() const { return m_lastPeer; }
    static std::wstring GetLocalIP();

private:
    SOCKET m_sock = INVALID_SOCKET;
    std::atomic<bool> m_running{false};
    std::thread m_worker;
    HWND m_hNotifyWnd = nullptr;
    PeerInfo m_lastPeer;

    void SenderWorker(int port);
    void ReceiverWorker(int port);
    void Notify(DWORD msg);
    static std::wstring GetMachineName();
};
