#include "raven/core/FileWatcher.h"
#include "raven/core/FileSystem.h"
#include "raven/core/Logger.h"

namespace raven {

void FileWatcher::AddWatch(const std::filesystem::path& directory, bool recursive) {
    m_Watches.emplace_back(directory, recursive);
}

void FileWatcher::ClearWatches() { m_Watches.clear(); }

void FileWatcher::SetCallback(std::function<void(const std::filesystem::path&, FileEvent)> callback) {
    m_Callback = std::move(callback);
}

void FileWatcher::Start(uint32_t pollIntervalMs) {
    if (m_Running.exchange(true)) return;
    m_PollIntervalMs = pollIntervalMs;
    // Prime the snapshot so existing files don't all fire as "Created".
    Scan(m_Snapshot);
    m_Thread = std::thread([this]() { PollLoop(); });
}

void FileWatcher::Stop() {
    if (!m_Running.exchange(false)) return;
    if (m_Thread.joinable()) m_Thread.join();
}

void FileWatcher::Scan(std::unordered_map<std::string, FileState>& out) const {
    out.clear();
    for (const auto& [dir, recursive] : m_Watches) {
        std::vector<fs::Path> files;
        if (recursive) {
            files = fs::ListFilesRecursive(dir);
        } else {
            std::error_code ec;
            if (fs::IsDirectory(dir)) {
                for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
                    if (e.is_regular_file(ec)) files.push_back(e.path());
                }
            }
        }
        for (const auto& file : files) {
            FileState st;
            st.mtime = fs::LastWriteTime(file);
            st.size = fs::FileSize(file);
            out.emplace(fs::NormalizeSlashes(file).string(), st);
        }
    }
}

void FileWatcher::PollLoop() {
    while (m_Running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(m_PollIntervalMs));
        if (!m_Running.load(std::memory_order_relaxed)) break;

        std::unordered_map<std::string, FileState> current;
        Scan(current);

        std::vector<std::pair<std::string, FileEvent>> events;
        for (const auto& [path, state] : current) {
            auto it = m_Snapshot.find(path);
            if (it == m_Snapshot.end()) {
                events.emplace_back(path, FileEvent::Created);
            } else if (it->second.mtime != state.mtime || it->second.size != state.size) {
                events.emplace_back(path, FileEvent::Modified);
            }
        }
        for (const auto& [path, state] : m_Snapshot) {
            if (current.find(path) == current.end()) events.emplace_back(path, FileEvent::Deleted);
        }
        m_Snapshot = std::move(current);

        for (const auto& [path, event] : events) {
            if (m_Callback) {
                try {
                    m_Callback(fs::NormalizeSlashes(fs::Path(path)), event);
                } catch (const std::exception& e) {
                    RAVEN_LOG_ERROR("FileWatcher callback error for %s: %s", path.c_str(), e.what());
                } catch (...) {
                    RAVEN_LOG_ERROR("FileWatcher callback error for %s", path.c_str());
                }
            }
        }
    }
}

} // namespace raven
