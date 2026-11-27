// RavenEngine — logger implementation on top of spdlog.
// Public API (raven::Logger + RAVEN_LOG_* macros) is unchanged from the
// previous hand-rolled logger; spdlog provides sinks, colors, pattern
// formatting and thread safety. The message line keeps the historical shape:
//   HH:MM:SS.mmm [LEVEL] message
#include "raven/core/Logger.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
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

// Forwards the formatted line to registered callbacks (editor console panel).
// Never lets a callback exception take down logging.
class CallbackSink : public spdlog::sinks::base_sink<std::mutex> {
public:
    void AddCallback(Logger::Callback cb) { m_Callbacks.push_back(std::move(cb)); }
    void ClearCallbacks() { m_Callbacks.clear(); }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        this->formatter_->format(msg, formatted);
        const std::string line(formatted.data(), formatted.size());
        const LogLevel level = FromSpdlogLevel(msg.level);
        for (auto& cb : m_Callbacks) {
            try { cb(level, line); } catch (...) {}
        }
    }

    void flush_() override {}

private:
    std::vector<Logger::Callback> m_Callbacks;
};

struct Logger::Impl {
    std::shared_ptr<spdlog::logger> logger;
    std::shared_ptr<spdlog::sinks::stderr_color_sink_mt> stderrSink;
    std::shared_ptr<CallbackSink> callbackSink;
    std::shared_ptr<spdlog::sinks::basic_file_sink_mt> fileSink;
    std::string fileSinkPath;
    LogLevel level = LogLevel::Info;
    bool console = true;
};

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

Logger::Logger() : m_Impl(std::make_unique<Impl>()) {
    m_Impl->stderrSink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    m_Impl->callbackSink = std::make_shared<CallbackSink>();
    m_Impl->logger = std::make_shared<spdlog::logger>("raven",
        spdlog::sinks_init_list{m_Impl->stderrSink, m_Impl->callbackSink});
    // "12:34:56.789 [INFO ] message" — pad level names for alignment.
    m_Impl->logger->set_pattern("%H:%M:%S.%e [%^%l%$] %v");
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
    try {
        m_Impl->fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path, /*truncate=*/false);
        m_Impl->fileSink->set_level(spdlog::level::trace);
        m_Impl->fileSink->set_pattern("%H:%M:%S.%e [%l] %v");
        m_Impl->logger->sinks().push_back(m_Impl->fileSink);
        m_Impl->fileSinkPath = path;
    } catch (const spdlog::spdlog_ex& e) {
        // Never take down the process because a log file could not be opened.
        fprintf(stderr, "raven: failed to open log file %s: %s\n", path.c_str(), e.what());
    }
}

void Logger::AddCallback(Callback cb) { m_Impl->callbackSink->AddCallback(std::move(cb)); }
void Logger::ClearCallbacks() { m_Impl->callbackSink->ClearCallbacks(); }

void Logger::Log(LogLevel level, const std::string& message) {
    if (static_cast<int>(level) < static_cast<int>(m_Impl->level)) return;
    m_Impl->logger->log(ToSpdlogLevel(level), "{}", message);
}

} // namespace raven
