// RavenEngine — directory watcher. Polls mtimes on a background thread and
// reports Created/Modified/Deleted for any file under the watched roots.
// (Polling is the portable baseline; native backends can come later behind
// this same interface.)
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace raven {

enum class FileEvent { Created, Modified, Deleted };

class FileWatcher {
public:
    FileWatcher() = default;
    ~FileWatcher() { Stop(); }

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    void AddWatch(const std::filesystem::path& directory, bool recursive = true);
    void ClearWatches();
    void SetCallback(std::function<void(const std::filesystem::path&, FileEvent)> callback);
    void Start(uint32_t pollIntervalMs = 500);
    void Stop();
    bool IsRunning() const { return m_Running; }

private:
    struct FileState {
        std::filesystem::file_time_type mtime{};
        uint64_t size = 0;
    };

    void PollLoop();
    void Scan(std::unordered_map<std::string, FileState>& out) const;

    std::vector<std::pair<std::filesystem::path, bool>> m_Watches;
    std::function<void(const std::filesystem::path&, FileEvent)> m_Callback;
    std::thread m_Thread;
    std::atomic<bool> m_Running{false};
    uint32_t m_PollIntervalMs = 500;
    std::unordered_map<std::string, FileState> m_Snapshot;
};

} // namespace raven
