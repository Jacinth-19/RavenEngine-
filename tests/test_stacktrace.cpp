// StackTrace: capture sanity on POSIX; must never crash, whatever the platform.
#include "test_framework.h"

#include <string>
#include <vector>

#include "raven/core/StackTrace.h"

using namespace raven;

RAVEN_TEST("stacktrace: Capture returns well-formed frame strings without crashing") {
    std::vector<std::string> frames = StackTrace::Capture(0, 16);
#if defined(__unix__) || defined(__APPLE__) || defined(__linux__)
    CHECK(!frames.empty());
    CHECK(frames.size() <= 16);
    for (const std::string& f : frames) {
        CHECK(!f.empty());
        CHECK(f.find("#") != std::string::npos);
    }
#else
    CHECK(frames.empty());  // unsupported platforms: empty, no crash
#endif
}

RAVEN_TEST("stacktrace: max caps the frame count and skip advances the start frame") {
    std::vector<std::string> all = StackTrace::Capture(0, 8);
    std::vector<std::string> capped = StackTrace::Capture(0, 1);
    std::vector<std::string> skipped = StackTrace::Capture(1, 8);
#if defined(__unix__) || defined(__APPLE__) || defined(__linux__)
    CHECK(capped.size() <= 1);
    CHECK(all.size() >= skipped.size());
    for (const std::string& f : capped) CHECK(f.find("0#") == 0 || f.find("  0#") == 0);
#else
    (void)all; (void)capped; (void)skipped;
#endif
}

RAVEN_TEST("stacktrace: LogStackTrace does not throw or terminate") {
    StackTrace::LogStackTrace(0, 4);
}
