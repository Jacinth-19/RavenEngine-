// Phase 2: Logger channels (§7.1) — structured LogEntry + channel propagation.
#include "test_framework.h"

#include <string>
#include <vector>

#include "raven/core/Logger.h"

using namespace raven;

RAVEN_TEST("logger: plain RAVEN_LOG calls carry an empty channel and the raw text") {
    std::vector<LogEntry> entries;
    Logger::Instance().AddCallback([&](const LogEntry& e) { entries.push_back(e); });
    Logger::Instance().SetLevel(LogLevel::Trace);

    Logger::Instance().Log(LogLevel::Info, "hello-plain");

    Logger::Instance().ClearCallbacks();
    CHECK(!entries.empty());
    const LogEntry& e = entries.back();
    CHECK(e.channel.empty());
    CHECK(e.text == "hello-plain");
    CHECK(e.level == LogLevel::Info);
}

RAVEN_TEST("logger: LogChannel threads the channel through to the callback entry") {
    std::vector<LogEntry> entries;
    Logger::Instance().AddCallback([&](const LogEntry& e) { entries.push_back(e); });
    Logger::Instance().SetLevel(LogLevel::Trace);

    RAVEN_LOG_INFO_C("Physics", "body %d spawned", 7);
    RAVEN_LOG_WARN_C("Scripts", "late tick");
    RAVEN_LOG_C(LogLevel::Debug, "Assets", "rescan done");

    Logger::Instance().ClearCallbacks();
    CHECK(entries.size() >= 3);
    const LogEntry& a = entries[entries.size() - 3];
    const LogEntry& b = entries[entries.size() - 2];
    const LogEntry& c = entries[entries.size() - 1];
    CHECK(a.channel == "Physics");
    CHECK(a.text == "body 7 spawned");
    CHECK(a.level == LogLevel::Info);
    CHECK(b.channel == "Scripts");
    CHECK(b.text == "late tick");
    CHECK(c.channel == "Assets");
    CHECK(c.level == LogLevel::Debug);
}

RAVEN_TEST("logger: level filtering still applies to channelled calls") {
    std::vector<LogEntry> entries;
    Logger::Instance().AddCallback([&](const LogEntry& e) { entries.push_back(e); });
    Logger::Instance().SetLevel(LogLevel::Warn);   // Info should be dropped

    RAVEN_LOG_INFO_C("Physics", "must-not-appear");
    RAVEN_LOG_ERROR_C("Physics", "must-appear");

    Logger::Instance().ClearCallbacks();
    Logger::Instance().SetLevel(LogLevel::Info);
    bool sawFiltered = false, sawError = false;
    for (const auto& e : entries) {
        if (e.text == "must-not-appear") sawFiltered = true;
        if (e.text == "must-appear") sawError = true;
    }
    CHECK(!sawFiltered);
    CHECK(sawError);
}
