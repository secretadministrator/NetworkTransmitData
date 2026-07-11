#include "ProgressTracker.h"
#include <algorithm>
#include <climits>

ProgressTracker::ProgressTracker() {
    Reset();
}

void ProgressTracker::Reset() {
    m_transferred = 0;
    m_inFlight = 0;
    m_totalWork = 0;
    m_completedWork = 0;
    m_samples.clear();
    m_startTime = std::chrono::steady_clock::now();
    m_lastTransferTime = m_startTime;
    m_lastSampleTime = m_startTime;
    m_samples.push_back({0, 0, m_startTime});
}

void ProgressTracker::SetInFlight(int64_t bytes) {
    if (bytes < 0)
        return;
    m_inFlight = bytes;

    const auto now = std::chrono::steady_clock::now();
    m_lastTransferTime = now;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_lastSampleTime).count();
    if (elapsed >= 500) {
        m_samples.push_back({GetDisplayedTransferred(), GetDisplayedWorkCompleted(), now});
        while (m_samples.size() > 11)
            m_samples.pop_front();
        m_lastSampleTime = now;
    }
}

void ProgressTracker::SetWorkload(int64_t totalBytes, int totalFiles) {
    Reset();
    const int64_t fileWork = static_cast<int64_t>(totalFiles) * FILE_EQUIVALENT_BYTES;
    m_totalWork = totalBytes > INT64_MAX - fileWork ? INT64_MAX : totalBytes + fileWork;
}

void ProgressTracker::AddCommitted(int64_t bytes, int files) {
    if (bytes < 0 || files < 0)
        return;
    m_inFlight = 0;
    m_transferred += bytes;
    const int64_t fileWork = static_cast<int64_t>(files) * FILE_EQUIVALENT_BYTES;
    const int64_t addedWork = bytes > INT64_MAX - fileWork
        ? INT64_MAX : bytes + fileWork;
    m_completedWork = m_completedWork > INT64_MAX - addedWork
        ? INT64_MAX : m_completedWork + addedWork;

    auto now = std::chrono::steady_clock::now();
    m_lastTransferTime = now;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_lastSampleTime).count();
    if (elapsed >= 500 || m_completedWork >= m_totalWork) {
        m_samples.push_back({m_transferred, m_completedWork, now});
        while (m_samples.size() > 11)
            m_samples.pop_front();
        m_lastSampleTime = now;
    }
}

int64_t ProgressTracker::CalculateSpeed(const SpeedSample& first, const SpeedSample& last)
{
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        last.time - first.time).count();
    if (ms <= 0 || last.bytes < first.bytes)
        return 0;
    return ((last.bytes - first.bytes) * 1000) / ms;
}

int64_t ProgressTracker::GetDisplayedTransferred() const {
    if (m_inFlight > INT64_MAX - m_transferred)
        return INT64_MAX;
    return m_transferred + m_inFlight;
}

int64_t ProgressTracker::GetDisplayedWorkCompleted() const {
    if (m_inFlight > INT64_MAX - m_completedWork)
        return INT64_MAX;
    const int64_t displayed = m_completedWork + m_inFlight;
    return m_totalWork > 0 ? (std::min)(displayed, m_totalWork) : displayed;
}

int64_t ProgressTracker::GetRecentSpeed() const {
    if (m_samples.size() < 2) return 0;
    return CalculateSpeed(m_samples[m_samples.size() - 2], m_samples.back());
}

int64_t ProgressTracker::GetAverageSpeed() const {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        m_lastTransferTime - m_startTime).count();
    return ms > 0 ? (GetDisplayedTransferred() * 1000) / ms : 0;
}

int64_t ProgressTracker::GetSpeed() const {
    if (m_samples.size() < 2) return 0;
    return CalculateSpeed(m_samples.front(), m_samples.back());
}

int64_t ProgressTracker::GetEstimatedRemainingSeconds() const {
    if (m_samples.size() < 2) return -1;
    const SpeedSample& first = m_samples.front();
    const SpeedSample& last = m_samples.back();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        last.time - first.time).count();
    if (ms <= 0 || last.work <= first.work) return -1;
    int64_t workSpeed = ((last.work - first.work) * 1000) / ms;
    if (workSpeed <= 0) return -1;
    int64_t remaining = m_totalWork - GetDisplayedWorkCompleted();
    if (remaining <= 0) return 0;
    return (remaining + workSpeed - 1) / workSpeed;
}

int64_t ProgressTracker::GetElapsedSeconds() const {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - m_startTime).count();
}

int64_t ProgressTracker::GetEstimatedTotalSeconds() const {
    int64_t remaining = GetEstimatedRemainingSeconds();
    return remaining < 0 ? -1 : GetElapsedSeconds() + remaining;
}
