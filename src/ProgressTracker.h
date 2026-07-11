#pragma once
#include <cstdint>
#include <string>
#include <chrono>
#include <deque>

class ProgressTracker {
public:
    ProgressTracker();

    void Reset(int64_t totalBytes);
    void AddTransferred(int64_t bytes);
    void SetCurrentFile(const std::wstring& file);
    void SetTotalFiles(int count);

    int64_t GetTransferred() const { return m_transferred; }
    int64_t GetTotal() const { return m_total; }
    void SetTotal(int64_t totalBytes) { m_total = totalBytes; }
    int64_t GetRecentSpeed() const;
    int64_t GetAverageSpeed() const;
    int64_t GetSpeed() const;
    bool HasSpeedSample() const { return m_samples.size() >= 2; }
    std::wstring GetCurrentFile() const { return m_currentFile; }
    double GetPercent() const;
    int64_t GetEstimatedRemainingSeconds() const;

private:
    int64_t m_total = 0;
    int64_t m_transferred = 0;
    std::wstring m_currentFile;
    int m_totalFiles = 0;
    int m_completedFiles = 0;

    struct SpeedSample {
        int64_t bytes;
        std::chrono::steady_clock::time_point time;
    };
    static int64_t CalculateSpeed(const SpeedSample& first, const SpeedSample& last);
    std::deque<SpeedSample> m_samples;
    std::chrono::steady_clock::time_point m_startTime;
    std::chrono::steady_clock::time_point m_lastTransferTime;
    std::chrono::steady_clock::time_point m_lastSampleTime;
};
