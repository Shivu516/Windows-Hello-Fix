#pragma once
#include <windows.h>

class PerfTimer {
private:
    LARGE_INTEGER m_start;
    LARGE_INTEGER m_freq;
public:
    PerfTimer() {
        QueryPerformanceFrequency(&m_freq);
        QueryPerformanceCounter(&m_start);
    }
    double ElapsedMs() const {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return (static_cast<double>(now.QuadPart - m_start.QuadPart) * 1000.0) / static_cast<double>(m_freq.QuadPart);
    }
};
