// RavenEngine — monotonic stopwatch for CPU-side profiling.
#pragma once

#include <chrono>

namespace raven {

class Stopwatch {
public:
    Stopwatch() { Restart(); }

    void Restart() { m_Start = clock::now(); m_Running = true; }
    void Stop() { m_Accumulated += clock::now() - m_Start; m_Running = false; }

    double ElapsedMilliseconds() const {
        auto total = m_Accumulated;
        if (m_Running) total += clock::now() - m_Start;
        return std::chrono::duration<double, std::milli>(total).count();
    }
    double ElapsedSeconds() const { return ElapsedMilliseconds() / 1000.0; }

private:
    using clock = std::chrono::steady_clock;
    clock::time_point m_Start = clock::now();
    std::chrono::steady_clock::duration m_Accumulated{};
    bool m_Running = true;
};

} // namespace raven
