#include "raven/core/StackTrace.h"

#include <string>
#include <vector>

#include "raven/core/Logger.h"

#if defined(__unix__) || defined(__APPLE__) || defined(__linux__)
#define RAVEN_STACKTRACE_POSIX 1
#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#endif

namespace raven {

std::vector<std::string> StackTrace::Capture(int skip, int max) {
    std::vector<std::string> frames;
#ifdef RAVEN_STACKTRACE_POSIX
    if (max <= 0) return frames;
    const int kMaxDepth = 64;
    const int want = skip + 1 + max;  // +1 for Capture() itself
    const int depth = want < kMaxDepth ? want : kMaxDepth;
    void* buffer[kMaxDepth] = {};
    const int captured = ::backtrace(buffer, depth);

    for (int i = skip + 1; i < captured; ++i) {
        Dl_info info{};
        const void* addr = buffer[i];
        char line[1024];
        if (::dladdr(addr, &info) != 0 && info.dli_sname) {
            // Demangle when possible; otherwise print the raw symbol name.
            const char* name = info.dli_sname;
            int status = 0;
            char* demangled = abi::__cxa_demangle(name, nullptr, nullptr, &status);
            const char* resolved = (status == 0 && demangled) ? demangled : name;
            const long offset = info.dli_saddr ? static_cast<const char*>(addr) - static_cast<const char*>(info.dli_saddr) : 0;
            std::snprintf(line, sizeof(line), "  %d# %p %s+0x%lx (%s)",
                          frames.size(), addr, resolved, offset,
                          info.dli_fname ? info.dli_fname : "?");
            std::free(demangled);
        } else {
            std::snprintf(line, sizeof(line), "  %d# %p", frames.size(), addr);
        }
        frames.emplace_back(line);
    }
#else
    (void)skip;
    (void)max;
#endif
    return frames;
}

void StackTrace::LogStackTrace(int skip, int max) {
    std::vector<std::string> frames = Capture(skip + 1, max);
    if (frames.empty()) {
        Logger::Instance().Log(LogLevel::Error, "StackTrace: unavailable on this platform");
        return;
    }
    for (const std::string& frame : frames) {
        Logger::Instance().Log(LogLevel::Error, frame);
    }
}

} // namespace raven
