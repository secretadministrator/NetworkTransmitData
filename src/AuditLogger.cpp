#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "AuditLogger.h"
#include "Utils.h"
#include <filesystem>

AuditLogger& AuditLogger::Instance() {
    static AuditLogger logger;
    return logger;
}

void AuditLogger::SetLogDir(const std::wstring& dir) {
    m_logDir = dir;
    CreateDirectoryW(dir.c_str(), NULL);
}

bool AuditLogger::OpenFile() {
    if (m_stream.is_open()) return true;
    if (m_logDir.empty()) {
        m_logDir = utils::GetExecutableDir() + L"\\logs";
        CreateDirectoryW(m_logDir.c_str(), NULL);
    }
    std::wstring path = m_logDir + L"\\transfer_" + utils::GetTimestampForFilename() + L".log";
    m_stream.open(std::filesystem::path(path), std::ios::out | std::ios::app | std::ios::binary);
    return m_stream.is_open();
}

void AuditLogger::Write(const std::wstring& message) {
    if (!OpenFile()) return;
    std::string line = utils::ToUtf8(L"[" + utils::GetTimestamp() + L"] " + message + L"\r\n");
    m_stream.write(line.data(), (std::streamsize)line.size());
    m_stream.flush();
}
