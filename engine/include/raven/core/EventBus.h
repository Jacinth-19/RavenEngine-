// RavenEngine — in-process publish/subscribe event bus.
//
// Low-frequency, edge-triggered notifications (selection changed, asset reloaded, …).
// Hot per-frame data must NOT flow through here; use direct calls or the renderer's
// frame snapshots. One bus per process; events are queued and dispatched on the main
// thread via Drain(), from any number of producer threads.
//
// Porting notes: semantics mirror Luth events/EventBus.h (MIT), reduced to a single
// engine-global bus with per-frame drain. Differences: no per-type MemoryTracker
// accounting, no Handled flag, no dual bus types.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <typeindex>
#include <vector>

namespace raven {

// Base for every payload that crosses the bus. Deliberately minimal — derived signals
// in editor code (EditorSignals.h) are plain data structs.
struct EditorSignal {
    virtual ~EditorSignal() = default;
};

class EventBus {
public:
    // Opaque subscription token; 0 = invalid. Pass to Unsubscribe to revoke.
    using SubscriptionHandle = uint64_t;
    static constexpr SubscriptionHandle kInvalidSubscription = 0;

    // One per process (engine-global). Thread-safe for Publish; Drain is main-thread only.
    static EventBus& Instance();

    // Enqueue a signal from any thread. The payload is moved into the queue; the signal
    // is visible to subscribers at the next Drain() (never synchronously).
    template <typename T>
    void Publish(T payload) {
        static_assert(std::is_base_of_v<EditorSignal, T>,
                      "EventBus payloads must derive from raven::EditorSignal");
        auto boxed = std::make_unique<T>(std::move(payload));
        std::lock_guard<std::mutex> lock(m_InboxMutex);
        m_Inbox.emplace_back(std::move(boxed));
    }

    // Register a handler (main thread). Handlers run in subscription order. Handlers
    // are process-lifetime unless Unsubscribe is called — panels must unsubscribe in
    // teardown or outlive the bus.
    template <typename T>
    SubscriptionHandle Subscribe(std::function<void(const T&)> fn) {
        static_assert(std::is_base_of_v<EditorSignal, T>,
                      "EventBus handlers must subscribe to types derived from raven::EditorSignal");
        const SubscriptionHandle id = ++m_NextSubscriptionId;  // 0 stays invalid
        m_Handlers.push_back(HandlerEntry{std::type_index(typeid(T)),
                                          [fn = std::move(fn)](const EditorSignal& s) {
                                              fn(static_cast<const T&>(s));
                                          },
                                          id});
        return id;
    }

    // Revoke a subscription returned by Subscribe. Safe on invalid handles and on
    // handles subscribed on another signal type. Returns true when something was removed.
    bool Unsubscribe(SubscriptionHandle handle);

    // Dispatch every queued signal to matching handlers. Main thread only.
    // Re-entrancy: handlers may Publish (deferred to the next Drain call, or to the
    // enclosing nested Drain if any) and may call Drain() themselves; nested drains are
    // honoured up to kMaxDrainDepth levels deep — beyond that the request is logged and dropped.
    // A throwing handler is logged and skipped; remaining handlers still run.
    void Drain();

    // Drop every queued signal and every handler. Intended for tests and for full
    // engine shutdown; not used by the frame loop.
    void Reset();

    size_t PendingSignalCount() const;

private:
    struct HandlerEntry {
        std::type_index type;
        std::function<void(const EditorSignal&)> fn;
        SubscriptionHandle id;
    };

    void Dispatch(const EditorSignal& signal);

    // Hard cap on nested Drain() calls originating inside handlers. Overflow logs an error.
    static constexpr int kMaxDrainDepth = 8;

    mutable std::mutex m_InboxMutex;                     // guards m_Inbox only
    std::deque<std::unique_ptr<EditorSignal>> m_Inbox;   // producer side (any thread)
    std::vector<HandlerEntry> m_Handlers;                // main thread affinity
    SubscriptionHandle m_NextSubscriptionId = 0;         // main thread affinity
    int m_DrainDepth = 0;                                // main thread affinity
};

} // namespace raven
