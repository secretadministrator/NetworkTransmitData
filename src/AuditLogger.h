#pragma once
#include <string>
#include <fstream>

class AuditLogger {
public:
    static AuditLogger& Instance();
    void Write(const std::wstring& message);
    void SetLogDir(const std::wstring& dir);

private:
    AuditLogger() = default;
    std::wstring m_logDir;
    std::ofstream m_stream;
    bool OpenFile();
};
