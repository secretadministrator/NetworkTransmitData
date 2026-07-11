#pragma once
#include <cstdint>
#include <chrono>
#include <deque>

class ProgressTracker {
public:
    ProgressTracker();

    void SetWorkload(int64_t totalBytes, int totalFiles);
    void SetInFlight(int64_t bytes);
    void AddCommitted(int64_t bytes, int files = 1);

    int64_t GetTransferred() const { return m_transferred; }
    int64_t GetDisplayedTransferred() const;
    int64_t GetRecentSpeed() const;
    int64_t GetAverageSpeed() const;
    int64_t GetSpeed() const;
    bool HasSpeedSample() const { return m_samples.size() >= 2; }
    int64_t GetEstimatedRemainingSeconds() const;
    int64_t GetElapsedSeconds() const;
    int64_t GetEstimatedTotalSeconds() const;
    int64_t GetWorkTotal() const { return m_totalWork; }
    int64_t GetWorkCompleted() const { return m_completedWork; }
    int64_t GetDisplayedWorkCompleted() const;

private:
    void Reset();
    int64_t m_transferred = 0;
    int64_t m_inFlight = 0;
    int64_t m_totalWork = 0;
    int64_t m_completedWork = 0;
    static constexpr int64_t FILE_EQUIVALENT_BYTES = 64 * 1024;

    struct SpeedSample {
        int64_t bytes;
        int64_t work;
        std::chrono::steady_clock::time_point time;
    };
    static int64_t CalculateSpeed(const SpeedSample& first, const SpeedSample& last);
    std::deque<SpeedSample> m_samples;
    std::chrono::steady_clock::time_point m_startTime;
    std::chrono::steady_clock::time_point m_lastTransferTime;
    std::chrono::steady_clock::time_point m_lastSampleTime;
};
