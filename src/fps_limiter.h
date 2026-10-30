#pragma once
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <immintrin.h>

class FpsLimiter {
public:
    FpsLimiter() 
        : m_targetFps(60.0), m_initialized(false), m_frameCount(0), 
          m_lastFpsCalcTime(0), m_currentFps(0.0) {
        m_qpcFrequency.QuadPart = 0;
        m_lastFrameTime.QuadPart = 0;
        m_frameIntervalTicks = 0;
    }

    void Init(double targetFps = 60.0) {
        m_targetFps = targetFps;
        QueryPerformanceFrequency(&m_qpcFrequency);
        m_frameIntervalTicks = (LONGLONG)((double)m_qpcFrequency.QuadPart / m_targetFps);

        // Request 1ms resolution from Windows system timer
        timeBeginPeriod(1);

        QueryPerformanceCounter(&m_lastFrameTime);
        m_lastFpsCalcTime = m_lastFrameTime.QuadPart;
        m_initialized = true;
    }

    void Shutdown() {
        if (m_initialized) {
            timeEndPeriod(1);
            m_initialized = false;
        }
    }

    void WaitNextFrame() {
        if (!m_initialized) {
            Init(m_targetFps);
        }

        LARGE_INTEGER current;
        QueryPerformanceCounter(&current);

        LONGLONG targetTicks = m_lastFrameTime.QuadPart + m_frameIntervalTicks;

        // If frame is already late
        if (current.QuadPart >= targetTicks) {
            if (current.QuadPart - targetTicks > m_frameIntervalTicks * 2) {
                m_lastFrameTime = current;
            } else {
                m_lastFrameTime.QuadPart = targetTicks;
            }
            UpdateFpsStats(current.QuadPart);
            return;
        }

        // Wait until target time
        while (true) {
            QueryPerformanceCounter(&current);
            LONGLONG remainingTicks = targetTicks - current.QuadPart;
            if (remainingTicks <= 0) break;

            double remainingMs = (double)remainingTicks * 1000.0 / (double)m_qpcFrequency.QuadPart;

            if (remainingMs > 2.0) {
                // Yield CPU if more than 2ms remaining
                Sleep(1);
            } else if (remainingMs > 0.5) {
                // Yield to other threads if 0.5ms - 2ms
                Sleep(0);
            } else {
                // Precision spin for sub-millisecond accuracy
                _mm_pause();
            }
        }

        m_lastFrameTime.QuadPart = targetTicks;
        UpdateFpsStats(current.QuadPart);
    }

    double GetCurrentFps() const {
        return m_currentFps;
    }

private:
    void UpdateFpsStats(LONGLONG currentTicks) {
        m_frameCount++;
        LONGLONG elapsed = currentTicks - m_lastFpsCalcTime;
        if (elapsed >= m_qpcFrequency.QuadPart) { // 1 second elapsed
            m_currentFps = (double)m_frameCount * (double)m_qpcFrequency.QuadPart / (double)elapsed;
            m_frameCount = 0;
            m_lastFpsCalcTime = currentTicks;
        }
    }

    double m_targetFps;
    bool m_initialized;
    LARGE_INTEGER m_qpcFrequency;
    LARGE_INTEGER m_lastFrameTime;
    LONGLONG m_frameIntervalTicks;

    // FPS measurement
    UINT m_frameCount;
    LONGLONG m_lastFpsCalcTime;
    double m_currentFps;
};
