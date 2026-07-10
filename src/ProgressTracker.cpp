#include "ProgressTracker.h"

ProgressTracker::ProgressTracker() {
    m_lastSampleTime = std::chrono::steady_clock::now();
}

void ProgressTracker::Reset(int64_t totalBytes) {
    m_total = totalBytes;
    m_transferred = 0;
    m_lastBytes = 0;
    m_currentFile.clear();
    m_samples.clear();
    m_lastSampleTime = std::chrono::steady_clock::now();
}

void ProgressTracker::AddTransferred(int64_t bytes) {
    m_transferred += bytes;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastSampleTime).count();

    if (elapsed >= 500) {
        m_samples.push_back({ m_transferred - m_lastBytes, now });
        while (m_samples.size() > 10)
            m_samples.pop_front();
        m_lastBytes = m_transferred;
        m_lastSampleTime = now;
    }
}

void ProgressTracker::SetCurrentFile(const std::wstring& file) {
    m_currentFile = file;
}

void ProgressTracker::SetTotalFiles(int count) {
    m_totalFiles = count;
}

int64_t ProgressTracker::GetSpeed() const {
    if (m_samples.size() < 2) return 0;

    int64_t totalBytes = 0;
    auto duration = std::chrono::steady_clock::duration::zero();

    for (size_t i = 1; i < m_samples.size(); ++i) {
        totalBytes += m_samples[i].bytes;
        duration += m_samples[i].time - m_samples[i - 1].time;
    }

    if (duration.count() == 0) return 0;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    return ms > 0 ? (totalBytes * 1000) / ms : 0;
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
    return remaining / speed;
}
