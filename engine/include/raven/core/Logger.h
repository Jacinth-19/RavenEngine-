// RavenEngine — leveled logger (§7.1), implemented on top of spdlog sinks.
// Keeps the historical API and output shape ("HH:MM:SS.mmm [LVL] message") and
// adds channels: LogChannel/LogfChannel overloads plus RAVEN_LOG_*_C macros.
// Every log call builds one LogEntry { time, level, channel, text }; sinks
// format that entry (stderr/file) and registered callbacks receive the struct
// itself — the editor console republishes it onto the bus without re-parsing.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

// spdlog is compiled header-only in this project (SPDLOG_HEADER_ONLY is set on
// raven::spdlog); see cmake/RavenDeps.cmake.

namespace raven {

enum class LogLevel : int { Trace = 0, Debug, Info, Warn, Error, Fatal, Off };

const char* LogLevelToString(LogLevel level);

// One structured log record. Sinks format locally; callbacks forward the struct.
struct LogEntry {
    std::chrono::system_clock::time_point time;
    LogLevel level = LogLevel::Info;
    std::string channel;             // "" == unchannelled (historical RAVEN_LOG_* calls)
    std::string text;                // formatted, without a channel prefix
};

class Logger {
public:
    using Callback = std::function<void(const LogEntry&)>;

    static Logger& Instance();

    void SetLevel(LogLevel level);
    LogLevel GetLevel() const;

    void SetFileSink(const std::string& path);          // empty path disables
    void SetEchoToConsole(bool enabled);
    void AddCallback(Callback cb);                      // editor hooks in here
    void ClearCallbacks();

    void Log(LogLevel level, const std::string& message);
    void LogChannel(LogLevel level, std::string_view channel, const std::string& message);

    template <typename... Args>
    void Logf(LogLevel level, const char* fmt, Args&&... args) {
        LogChannel(level, std::string_view(), FormatVarargs(fmt, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void LogfChannel(LogLevel level, std::string_view channel, const char* fmt, Args&&... args) {
        LogChannel(level, channel, FormatVarargs(fmt, std::forward<Args>(args)...));
    }

private:
    Logger();
    ~Logger();

    template <typename... Args>
    static std::string FormatVarargs(const char* fmt, Args&&... args) {
        // printf-style formatting, guarded against bad format strings.
        char buf[4096];
        int n = std::snprintf(buf, sizeof(buf), fmt, std::forward<Args>(args)...);
        if (n < 0) return "<log format error>";
        return std::string(buf, static_cast<size_t>(n) < sizeof(buf) ? static_cast<size_t>(n) : sizeof(buf) - 1);
    }

    struct Impl;                 // hides the spdlog sink types from this header
    std::unique_ptr<Impl> m_Impl;
};

} // namespace raven

#define RAVEN_LOG_TRACE(...) ::raven::Logger::Instance().Logf(::raven::LogLevel::Trace, __VA_ARGS__)
#define RAVEN_LOG_DEBUG(...) ::raven::Logger::Instance().Logf(::raven::LogLevel::Debug, __VA_ARGS__)
#define RAVEN_LOG_INFO(...)  ::raven::Logger::Instance().Logf(::raven::LogLevel::Info, __VA_ARGS__)
#define RAVEN_LOG_WARN(...)  ::raven::Logger::Instance().Logf(::raven::LogLevel::Warn, __VA_ARGS__)
#define RAVEN_LOG_ERROR(...) ::raven::Logger::Instance().Logf(::raven::LogLevel::Error, __VA_ARGS__)
#define RAVEN_LOG_FATAL(...) ::raven::Logger::Instance().Logf(::raven::LogLevel::Fatal, __VA_ARGS__)
// Channelled variants: RAVEN_LOG_INFO_C("Physics", "body %u spawned", id)
#define RAVEN_LOG_TRACE_C(tag, ...) ::raven::Logger::Instance().LogfChannel(::raven::LogLevel::Trace, tag, __VA_ARGS__)
#define RAVEN_LOG_DEBUG_C(tag, ...) ::raven::Logger::Instance().LogfChannel(::raven::LogLevel::Debug, tag, __VA_ARGS__)
#define RAVEN_LOG_INFO_C(tag, ...)  ::raven::Logger::Instance().LogfChannel(::raven::LogLevel::Info, tag, __VA_ARGS__)
#define RAVEN_LOG_WARN_C(tag, ...)  ::raven::Logger::Instance().LogfChannel(::raven::LogLevel::Warn, tag, __VA_ARGS__)
#define RAVEN_LOG_ERROR_C(tag, ...) ::raven::Logger::Instance().LogfChannel(::raven::LogLevel::Error, tag, __VA_ARGS__)
#define RAVEN_LOG_FATAL_C(tag, ...) ::raven::Logger::Instance().LogfChannel(::raven::LogLevel::Fatal, tag, __VA_ARGS__)
// Generic: RAVEN_LOG_C(LogLevel::Info, "Scripts", "...")
#define RAVEN_LOG_C(level, tag, ...) ::raven::Logger::Instance().LogfChannel(level, tag, __VA_ARGS__)
