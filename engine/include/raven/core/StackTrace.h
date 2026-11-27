// RavenEngine — best-effort stack trace capture for diagnostics (error boundary,
// crash reporting). POSIX: backtrace() + dladdr + __cxa_demangle. Other platforms
// return an empty frame list until a Windows backend lands (docs/EDITOR_DESIGN.md §3.2).
#pragma once

#include <string>
#include <vector>

namespace raven {

class StackTrace {
public:
    // Capture the current call stack as formatted frames, e.g.
    //   "  0# 0x000055... raven::Scene::SerializeToString()+0x1f2 (/path/a.out)"
    // skip drops the top `skip` frames above Capture itself (use 1 from a helper).
    // max caps the result length. Returns an empty vector on unsupported platforms.
    // Frame *contents* are not guaranteed stable — treat them as human diagnostics.
    static std::vector<std::string> Capture(int skip = 0, int max = 32);

    // Capture and emit each frame at LogLevel::Error. Never throws; never dereferences
    // a bad program counter (unwinding stops at the first unresolvable frame).
    static void LogStackTrace(int skip = 0, int max = 32);
};

} // namespace raven
