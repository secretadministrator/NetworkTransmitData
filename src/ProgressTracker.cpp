#include "ProgressTracker.h"

ProgressTracker::ProgressTracker() {
    Reset(0);
}

void ProgressTracker::Reset(int64_t totalBytes) {
    m_total = totalBytes;
    m_transferred = 0;
    m_currentFile.clear();
    m_samples.clear();
    m_startTime = std::chrono::steady_clock::now();
    m_lastTransferTime = m_startTime;
    m_lastSampleTime = m_startTime;
    m_samples.push_back({0, m_startTime});
}

void ProgressTracker::AddTransferred(int64_t bytes) {
    m_transferred += bytes;

    auto now = std::chrono::steady_clock::now();
    m_lastTransferTime = now;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastSampleTime).count();

    if (elapsed >= 500) {
        m_samples.push_back({ m_transferred, now });
        // Ten intervals form an approximately five-second sliding window.
        while (m_samples.size() > 11)
            m_samples.pop_front();
        m_lastSampleTime = now;
    }
}

void ProgressTracker::SetCurrentFile(const std::wstring& file) {
    m_currentFile = file;
}

void ProgressTracker::SetTotalFiles(int count) {
    m_totalFiles = count;
}

int64_t ProgressTracker::CalculateSpeed(const SpeedSample& first, const SpeedSample& last)
{
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        last.time - first.time).count();
    if (ms <= 0 || last.bytes < first.bytes)
        return 0;
    return ((last.bytes - first.bytes) * 1000) / ms;
}

int64_t ProgressTracker::GetRecentSpeed() const {
    if (m_samples.size() < 2) return 0;
    return CalculateSpeed(m_samples[m_samples.size() - 2], m_samples.back());
}

int64_t ProgressTracker::GetAverageSpeed() const {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        m_lastTransferTime - m_startTime).count();
    return ms > 0 ? (m_transferred * 1000) / ms : 0;
}

int64_t ProgressTracker::GetSpeed() const {
    if (m_samples.size() < 2) return 0;
    return CalculateSpeed(m_samples.front(), m_samples.back());
}

double ProgressTracker::GetPercent() const {
    if (m_total == 0) return 0.0;
    return (double)m_transferred / (double)m_total * 100.0;
}

int64_t ProgressTracker::GetEstimatedRemainingSeconds() const {
    int64_t speed = GetSpeed();
    if (speed <= 0) return -1;
    int64_t remaining = m_total - m_transferred;
    if (remaining <= 0) return 0;
    return (remaining + speed - 1) / speed;
}
