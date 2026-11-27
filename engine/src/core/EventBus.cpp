#include "raven/core/EventBus.h"

#include <exception>
#include <utility>

#include "raven/core/Logger.h"

namespace raven {

EventBus& EventBus::Instance() {
    static EventBus kInstance;
    return kInstance;
}

bool EventBus::Unsubscribe(SubscriptionHandle handle) {
    if (handle == kInvalidSubscription) return false;
    for (auto it = m_Handlers.begin(); it != m_Handlers.end(); ++it) {
        if (it->id == handle) {
            m_Handlers.erase(it);
            return true;
        }
    }
    return false;
}

void EventBus::Drain() {
    if (m_DrainDepth >= kMaxDrainDepth) {
        Logger::Instance().Log(LogLevel::Error, "EventBus: Drain() depth cap (8) exceeded — nested drain request dropped");
        return;
    }

    // Swap the queue out up front: signals published during dispatch land in the next
    // batch (next frame, or an enclosing nested Drain) — never re-entering this batch.
    std::deque<std::unique_ptr<EditorSignal>> batch;
    {
        std::lock_guard<std::mutex> lock(m_InboxMutex);
        batch.swap(m_Inbox);
    }

    ++m_DrainDepth;
    struct DepthGuard {
        int& depth;
        ~DepthGuard() { --depth; }
    } guard{m_DrainDepth};

    while (!batch.empty()) {
        std::unique_ptr<EditorSignal> signal = std::move(batch.front());
        batch.pop_front();
        Dispatch(*signal);
    }
}

void EventBus::Reset() {
    std::lock_guard<std::mutex> lock(m_InboxMutex);
    m_Inbox.clear();
    m_Handlers.clear();
    m_NextSubscriptionId = 0;
    m_DrainDepth = 0;
}

size_t EventBus::PendingSignalCount() const {
    std::lock_guard<std::mutex> lock(m_InboxMutex);
    return m_Inbox.size();
}

void EventBus::Dispatch(const EditorSignal& signal) {
    // Snapshot matching handlers: a handler that unsubscribes itself (or anyone else)
    // mid-dispatch must not mutate the list we iterate.
    const std::type_index type = std::type_index(typeid(signal));
    std::vector<std::function<void(const EditorSignal&)>> snapshot;
    for (const HandlerEntry& entry : m_Handlers) {
        if (entry.type == type) snapshot.push_back(entry.fn);
    }
    for (const auto& handler : snapshot) {
        try {
            handler(signal);
        } catch (const std::exception& e) {
            Logger::Instance().Log(LogLevel::Error, std::string("EventBus: handler threw: ") + e.what());
        } catch (...) {
            Logger::Instance().Log(LogLevel::Error, "EventBus: handler threw a non-std exception");
        }
    }
}

} // namespace raven
