// RavenEngine — small fiber-like task system: one worker thread per core,
// lightweight enqueued jobs, futures for results. This is where async asset
// imports and other background work run. (Fibers/async-yield come later; the
// public API is already future-based so that upgrade won't leak into callers.)
#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <vector>
#include <atomic>

namespace raven {

class JobSystem {
public:
    static JobSystem& Instance();

    // Spawns workers. numThreads == 0 selects hardware_concurrency() - 1 (min 1).
    void Initialize(uint32_t numThreads = 0);
    void Shutdown();              // drains the queue, then joins workers
    bool IsInitialized() const { return m_Initialized; }

    template <typename F>
    auto Submit(F&& fn) -> std::future<std::invoke_result_t<F>> {
        using R = std::invoke_result_t<F>;
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn));
        std::future<R> future = task->get_future();
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Queue.push([task]() { (*task)(); });
            ++m_Pending;
        }
        m_CV.notify_one();
        return future;
    }

    // Runs `fn(begin, end)` on all workers over [0, count) cooperatively.
    template <typename F>
    void ParallelFor(size_t count, F&& fn) {
        if (count == 0) return;
        const uint32_t workers = WorkerCount();
        if (count < 4 || workers <= 1) {
            fn(size_t{0}, count);
            return;
        }
        const size_t chunk = (count + workers - 1) / workers;
        std::vector<std::future<void>> futures;
        futures.reserve(workers);
        for (uint32_t w = 0; w < workers; ++w) {
            size_t begin = w * chunk;
            if (begin >= count) break;
            size_t end = (begin + chunk) < count ? (begin + chunk) : count;
            futures.push_back(Submit([&fn, begin, end]() { fn(begin, end); }));
        }
        for (auto& f : futures) f.get();
    }

    uint32_t WorkerCount() const { return static_cast<uint32_t>(m_Workers.size()); }
    size_t PendingJobs() const { return m_Pending.load(std::memory_order_relaxed); }

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

private:
    JobSystem() = default;
    ~JobSystem();

    void WorkerLoop();

    std::vector<std::thread> m_Workers;
    std::queue<std::function<void()>> m_Queue;
    std::mutex m_Mutex;
    std::condition_variable m_CV;
    std::atomic<size_t> m_Pending{0};
    std::atomic<bool> m_Running{false};
    bool m_Initialized = false;
};

} // namespace raven
