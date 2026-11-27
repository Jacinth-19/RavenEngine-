// RavenEngine — leveled logger, implemented on top of spdlog.
// Keeps the historical API and output shape ("HH:MM:SS.mmm [LVL] message");
// spdlog provides the sinks, coloring, pattern formatting and thread safety.
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

// spdlog is compiled header-only in this project (SPDLOG_HEADER_ONLY is set on
// raven::spdlog); see cmake/RavenDeps.cmake.

namespace raven {

enum class LogLevel : int { Trace = 0, Debug, Info, Warn, Error, Fatal, Off };

const char* LogLevelToString(LogLevel level);

class Logger {
public:
    using Callback = std::function<void(LogLevel, const std::string&)>;

    static Logger& Instance();

    void SetLevel(LogLevel level);
    LogLevel GetLevel() const;

    void SetFileSink(const std::string& path);          // empty path disables
    void SetEchoToConsole(bool enabled);
    void AddCallback(Callback cb);                      // editor hooks in here
    void ClearCallbacks();

    void Log(LogLevel level, const std::string& message);

    template <typename... Args>
    void Logf(LogLevel level, const char* fmt, Args&&... args) {
        // printf-style formatting, guarded against bad format strings.
        char buf[4096];
        int n = std::snprintf(buf, sizeof(buf), fmt, std::forward<Args>(args)...);
        if (n < 0) {
            Log(level, "<log format error>");
        } else {
            Log(level, std::string(buf, static_cast<size_t>(n) < sizeof(buf) ? static_cast<size_t>(n) : sizeof(buf) - 1));
        }
    }

private:
    Logger();
    ~Logger();
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
