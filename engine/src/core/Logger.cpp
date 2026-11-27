// RavenEngine — logger implementation on top of spdlog.
// Sinks receive a structured view: every call builds one raven::LogEntry and
// hands it to the custom sinks below, which format their own line:
//   stderr: HH:MM:SS.mmm [LEVEL][channel] text   (ANSI-colored by level)
//   file:   HH:MM:SS.mmm [LEVEL][channel] text   (plain)
//   callback: the LogEntry struct itself (editor console → bus)
// Channelled logging (LogChannel/logfChannel) threads the channel from the
// call site to the sinks with a thread_local — spdlog runs sinks synchronously
// on the calling thread, so this stays correct (and cheap) under concurrency.
#include "raven/core/Logger.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/basic_file_sink.h>

namespace raven {

const char* LogLevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
        default: return "OFF  ";
    }
}

static spdlog::level::level_enum ToSpdlogLevel(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return spdlog::level::trace;
        case LogLevel::Debug: return spdlog::level::debug;
        case LogLevel::Info:  return spdlog::level::info;
        case LogLevel::Warn:  return spdlog::level::warn;
        case LogLevel::Error: return spdlog::level::err;
        case LogLevel::Fatal: return spdlog::level::critical;
        default:              return spdlog::level::off;
    }
}

static LogLevel FromSpdlogLevel(spdlog::level::level_enum level) {
    switch (level) {
        case spdlog::level::trace:    return LogLevel::Trace;
        case spdlog::level::debug:    return LogLevel::Debug;
        case spdlog::level::info:     return LogLevel::Info;
        case spdlog::level::warn:     return LogLevel::Warn;
        case spdlog::level::err:      return LogLevel::Error;
        case spdlog::level::critical: return LogLevel::Fatal;
        default:                      return LogLevel::Off;
    }
}

// Channel of the in-flight log call. Set for the synchronous duration of
// spdlog::logger::log on this thread and read by the sinks in sink_it_.
static thread_local std::string t_Channel;

static std::string ChannelTag(const std::string& channel) {
    return channel.empty() ? std::string() : "[" + channel + "]";
}

static std::string FormatLine(const std::tm& tmNow, int millis, LogLevel level,
                              const std::string& channel, const std::string& text) {
    char buf[4100];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d [%s]%s %s",
                  tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec, millis,
                  LogLevelToString(level), ChannelTag(channel).c_str(), text.c_str());
    return std::string(buf);
}

// Shared formatting for the two line sinks — local time via system_clock.
static std::string FormatLogLine(LogLevel level, const std::string& text) {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tmNow{};
#if defined(_WIN32)
    localtime_s(&tmNow, &now);
#else
    localtime_r(&now, &tmNow);
#endif
    const auto nowMs = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
    const int millis = static_cast<int>(nowMs.time_since_epoch().count() % 1000);
    return FormatLine(tmNow, millis, level, t_Channel, text);
}

// ANSI color per level for the stderr sink.
static const char* LevelAnsi(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "\033[37m";
        case LogLevel::Debug: return "\033[36m";
        case LogLevel::Info:  return "\033[32m";
        case LogLevel::Warn:  return "\033[33m";
        case LogLevel::Error: return "\033[31m";
        case LogLevel::Fatal: return "\033[1;31m";
        default:              return "\033[0m";
    }
}

// Sinks read msg.payload directly — they format the whole line themselves, so no
// pattern is involved in their output (the logger runs with "%v" only for spdlog's
// internal payload plumbing).
class StderrSink : public spdlog::sinks::base_sink<std::mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        const LogLevel level = FromSpdlogLevel(msg.level);
        const std::string text(msg.payload.data(), msg.payload.size());
        const std::string line = FormatLogLine(level, text);
        std::fwrite(LevelAnsi(level), 1, std::strlen(LevelAnsi(level)), stderr);
        std::fwrite(line.data(), 1, line.size(), stderr);
        std::fwrite("\033[0m\n", 1, 5, stderr);
    }
    void flush_() override { std::fflush(stderr); }
};

class PlainFileSink : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit PlainFileSink(const std::string& path) {
        m_Stream.open(path, std::ios::app);
    }
    bool IsOpen() const { return m_Stream.is_open(); }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        const LogLevel level = FromSpdlogLevel(msg.level);
        const std::string text(msg.payload.data(), msg.payload.size());
        m_Stream << FormatLogLine(level, text) << '\n';
    }
    void flush_() override { m_Stream.flush(); }

private:
    std::ofstream m_Stream;
};

// Forwards the structured entry to registered callbacks (editor console panel).
// Never lets a callback exception take down logging.
class CallbackSink : public spdlog::sinks::base_sink<std::mutex> {
public:
    void AddCallback(Logger::Callback cb) { m_Callbacks.push_back(std::move(cb)); }
    void ClearCallbacks() { m_Callbacks.clear(); }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        LogEntry entry;
        entry.time = std::chrono::system_clock::now();
        entry.level = FromSpdlogLevel(msg.level);
        entry.channel = t_Channel;
        entry.text.assign(msg.payload.data(), msg.payload.size());
        for (auto& cb : m_Callbacks) {
            try { cb(entry); } catch (...) {}
        }
    }
    void flush_() override {}

private:
    std::vector<Logger::Callback> m_Callbacks;
};

struct Logger::Impl {
    std::shared_ptr<spdlog::logger> logger;
    std::shared_ptr<StderrSink> stderrSink;
    std::shared_ptr<CallbackSink> callbackSink;
    std::shared_ptr<PlainFileSink> fileSink;
    std::string fileSinkPath;
    LogLevel level = LogLevel::Info;
    bool console = true;
};

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

Logger::Logger() : m_Impl(std::make_unique<Impl>()) {
    m_Impl->stderrSink = std::make_shared<StderrSink>();
    m_Impl->callbackSink = std::make_shared<CallbackSink>();
    m_Impl->logger = std::make_shared<spdlog::logger>("raven",
        spdlog::sinks_init_list{m_Impl->stderrSink, m_Impl->callbackSink});
    // Raw message only — each sink formats its own "HH:MM:SS.mmm [LEVEL]... msg" line.
    m_Impl->logger->set_pattern("%v");
    m_Impl->logger->set_level(spdlog::level::info);
    m_Impl->logger->flush_on(spdlog::level::warn);

    // Default level: Info; environment override RAVEN_LOG_LEVEL (TRACE/DEBUG/...).
    if (const char* env = std::getenv("RAVEN_LOG_LEVEL")) {
        std::string s = env;
        for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (s == "TRACE") SetLevel(LogLevel::Trace);
        else if (s == "DEBUG") SetLevel(LogLevel::Debug);
        else if (s == "INFO")  SetLevel(LogLevel::Info);
        else if (s == "WARN")  SetLevel(LogLevel::Warn);
        else if (s == "ERROR") SetLevel(LogLevel::Error);
        else if (s == "FATAL") SetLevel(LogLevel::Fatal);
    }
}

Logger::~Logger() = default;

void Logger::SetLevel(LogLevel level) {
    m_Impl->level = level;
    m_Impl->logger->set_level(ToSpdlogLevel(level));
}

LogLevel Logger::GetLevel() const { return m_Impl->level; }

void Logger::SetEchoToConsole(bool enabled) {
    m_Impl->console = enabled;
    m_Impl->stderrSink->set_level(enabled ? ToSpdlogLevel(m_Impl->level) : spdlog::level::off);
}

void Logger::SetFileSink(const std::string& path) {
    if (m_Impl->fileSink) {
        m_Impl->logger->sinks().erase(
            std::remove(m_Impl->logger->sinks().begin(), m_Impl->logger->sinks().end(), m_Impl->fileSink),
            m_Impl->logger->sinks().end());
        m_Impl->fileSink.reset();
        m_Impl->fileSinkPath.clear();
    }
    if (path.empty()) return;
    auto sink = std::make_shared<PlainFileSink>(path);
    if (!sink->IsOpen()) {
        fprintf(stderr, "raven: failed to open log file %s\n", path.c_str());
        return;
    }
    sink->set_level(spdlog::level::trace);
    m_Impl->fileSink = std::move(sink);
    m_Impl->logger->sinks().push_back(m_Impl->fileSink);
    m_Impl->fileSinkPath = path;
}

void Logger::AddCallback(Callback cb) { m_Impl->callbackSink->AddCallback(std::move(cb)); }
void Logger::ClearCallbacks() { m_Impl->callbackSink->ClearCallbacks(); }

void Logger::Log(LogLevel level, const std::string& message) {
    LogChannel(level, std::string_view(), message);
}

void Logger::LogChannel(LogLevel level, std::string_view channel, const std::string& message) {
    if (static_cast<int>(level) < static_cast<int>(m_Impl->level)) return;
    t_Channel.assign(channel.data(), channel.size());
    m_Impl->logger->log(ToSpdlogLevel(level), "{}", message);
    t_Channel.clear();
}

} // namespace raven
