#include "raven/core/JobSystem.h"
#include "raven/core/Logger.h"
#include <algorithm>

namespace raven {

JobSystem& JobSystem::Instance() {
    static JobSystem instance;
    return instance;
}

JobSystem::~JobSystem() {
    Shutdown();
}

void JobSystem::Initialize(uint32_t numThreads) {
    if (m_Initialized) return;
    uint32_t hw = std::thread::hardware_concurrency();
    uint32_t workers = numThreads ? numThreads : (hw > 1 ? hw - 1 : 1);
    workers = std::max(1u, workers);
    m_Running = true;
    m_Workers.reserve(workers);
    for (uint32_t i = 0; i < workers; ++i) {
        m_Workers.emplace_back([this]() { WorkerLoop(); });
    }
    m_Initialized = true;
    RAVEN_LOG_INFO("JobSystem: %u worker thread(s) online", workers);
}

void JobSystem::Shutdown() {
    if (!m_Initialized) return;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Running = false;
    }
    m_CV.notify_all();
    for (auto& t : m_Workers) {
        if (t.joinable()) t.join();
    }
    m_Workers.clear();
    m_Initialized = false;
}

void JobSystem::WorkerLoop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            m_CV.wait(lock, [this]() { return !m_Running || !m_Queue.empty(); });
            if (!m_Running && m_Queue.empty()) return;
            job = std::move(m_Queue.front());
            m_Queue.pop();
        }
        job();
        --m_Pending;
    }
}

} // namespace raven
