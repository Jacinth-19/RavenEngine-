#include <utility>
#include "raven/core/Logger.h"

#include <cstdarg>
#include <ctime>
#include <iostream>

#ifdef _WIN32
#include <io.h>
#define RAVEN_ISATTY _isatty
#define RAVEN_FILENO _fileno
#else
#include <unistd.h>
#define RAVEN_ISATTY isatty
#define RAVEN_FILENO fileno
#endif

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

static const char* LevelColor(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "\033[90m";
        case LogLevel::Debug: return "\033[36m";
        case LogLevel::Info:  return "\033[32m";
        case LogLevel::Warn:  return "\033[33m";
        case LogLevel::Error: return "\033[31m";
        case LogLevel::Fatal: return "\033[1;31m";
        default: return "\033[0m";
    }
}

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

Logger::Logger() {
    // Default level: Info; environment override RAVEN_LOG_LEVEL (TRACE/DEBUG/...).
    if (const char* env = std::getenv("RAVEN_LOG_LEVEL")) {
        std::string s = env;
        for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (s == "TRACE") m_Level = LogLevel::Trace;
        else if (s == "DEBUG") m_Level = LogLevel::Debug;
        else if (s == "INFO")  m_Level = LogLevel::Info;
        else if (s == "WARN")  m_Level = LogLevel::Warn;
        else if (s == "ERROR") m_Level = LogLevel::Error;
        else if (s == "FATAL") m_Level = LogLevel::Fatal;
    }
}

Logger::~Logger() = default;

void Logger::SetFileSink(const std::string& path) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_File.is_open()) m_File.close();
    if (!path.empty()) m_File.open(path, std::ios::app);
}

void Logger::AddCallback(Callback cb) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Callbacks.push_back(std::move(cb));
}

void Logger::ClearCallbacks() {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Callbacks.clear();
}

void Logger::Log(LogLevel level, const std::string& message) {
    if (static_cast<int>(level) < static_cast<int>(m_Level)) return;

    // Timestamp: HH:MM:SS.mmm
    using clock = std::chrono::system_clock;
    auto now = clock::now();
    std::time_t t = clock::to_time_t(now);
    std::tm tmBuf{};
#ifdef _WIN32
    localtime_s(&tmBuf, &t);
#else
    localtime_r(&t, &tmBuf);
#endif
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;

    char line[4600];
    std::snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [%s] %s",
                  tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec,
                  static_cast<int>(ms), LogLevelToString(level), message.c_str());

    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Console) {
        bool tty = RAVEN_ISATTY(RAVEN_FILENO(stderr)) != 0;
        if (tty) {
            std::fprintf(stderr, "%s%s\033[0m\n", LevelColor(level), line);
        } else {
            std::fprintf(stderr, "%s\n", line);
        }
    }
    if (m_File.is_open()) {
        m_File << line << '\n';
        m_File.flush();
    }
    for (auto& cb : m_Callbacks) {
        // Never let a callback exception take down logging.
        try { cb(level, line); } catch (...) {}
    }
}

} // namespace raven
