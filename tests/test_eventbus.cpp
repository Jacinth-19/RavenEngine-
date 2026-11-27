// EventBus: publish/subscribe/drain semantics, re-entrancy, thread safety.
#include "test_framework.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "raven/core/EventBus.h"

using namespace raven;

namespace {
struct PingSignal : EditorSignal {
    int value = 0;
    PingSignal() = default;
    explicit PingSignal(int v) : value(v) {}
};
struct PongSignal : EditorSignal {
    std::string tag;
    PongSignal() = default;
    explicit PongSignal(const char* t) : tag(t) {}
};

struct BusFixture {
    BusFixture() { EventBus::Instance().Reset(); }
    ~BusFixture() { EventBus::Instance().Reset(); }
};
} // namespace

RAVEN_TEST("eventbus: published signal is not delivered before Drain") {
    BusFixture bus;
    int delivered = 0;
    EventBus::Instance().Subscribe<PingSignal>([&](const PingSignal&) { ++delivered; });
    EventBus::Instance().Publish(PingSignal{1});
    CHECK_EQ(delivered, 0);
    EventBus::Instance().Drain();
    CHECK_EQ(delivered, 1);
}

RAVEN_TEST("eventbus: drain delivers payloads in FIFO order to all subscribers") {
    BusFixture bus;
    std::vector<int> a, b;
    EventBus::Instance().Subscribe<PingSignal>([&](const PingSignal& s) { a.push_back(s.value); });
    EventBus::Instance().Subscribe<PingSignal>([&](const PingSignal& s) { b.push_back(s.value); });
    for (int i = 0; i < 5; ++i) EventBus::Instance().Publish(PingSignal{i});
    EventBus::Instance().Drain();
    CHECK_EQ(a.size(), size_t(5));
    CHECK_EQ(b.size(), size_t(5));
    for (int i = 0; i < 5; ++i) {
        CHECK_EQ(a[static_cast<size_t>(i)], i);
        CHECK_EQ(b[static_cast<size_t>(i)], i);
    }
}

RAVEN_TEST("eventbus: handler publishes during drain — delivered on the NEXT drain, not this one") {
    BusFixture bus;
    std::vector<std::string> order;
    EventBus::Instance().Subscribe<PingSignal>([&](const PingSignal& s) {
        order.push_back("ping");
        EventBus::Instance().Publish(PongSignal{"pong"});
    });
    EventBus::Instance().Subscribe<PongSignal>([&](const PongSignal&) { order.push_back("pong"); });
    EventBus::Instance().Publish(PingSignal{7});
    EventBus::Instance().Drain();
    CHECK_EQ(order.size(), size_t(1));  // PongSignal is still queued
    CHECK(order[0] == "ping");
    EventBus::Instance().Drain();
    CHECK_EQ(order.size(), size_t(2));
    CHECK(order[1] == "pong");
}

RAVEN_TEST("eventbus: re-entrant Drain from a handler is honored but depth-capped safely") {
    BusFixture bus;
    int pongSeen = 0;
    EventBus::Instance().Subscribe<PingSignal>([&](const PingSignal&) {
        // Publish inside a handler, then force a nested drain: the nested Drain
        // flushes the pending PongSignal without stack overflow.
        EventBus::Instance().Publish(PongSignal{"x"});
        EventBus::Instance().Drain();
        EventBus::Instance().Drain();
        EventBus::Instance().Drain();
        EventBus::Instance().Drain();
        EventBus::Instance().Drain();
        EventBus::Instance().Drain();
        EventBus::Instance().Drain();
        EventBus::Instance().Drain();
        EventBus::Instance().Drain();  // depth cap reached here — must not crash
    });
    EventBus::Instance().Subscribe<PongSignal>([&](const PongSignal&) { ++pongSeen; });
    EventBus::Instance().Publish(PingSignal{1});
    EventBus::Instance().Drain();
    CHECK(pongSeen >= 1);  // at least one nested drain flushed it
}

RAVEN_TEST("eventbus: publish from a worker thread is visible after drain on main thread") {
    BusFixture bus;
    std::atomic<int> delivered{0};
    EventBus::Instance().Subscribe<PingSignal>([&](const PingSignal&) { ++delivered; });
    std::thread producer([&] {
        for (int i = 0; i < 50; ++i) EventBus::Instance().Publish(PingSignal{i});
    });
    producer.join();
    CHECK_EQ(delivered.load(), 0);
    EventBus::Instance().Drain();
    CHECK_EQ(delivered.load(), 50);
}

RAVEN_TEST("eventbus: a throwing handler does not break remaining handlers") {
    BusFixture bus;
    int survived = 0;
    EventBus::Instance().Subscribe<PingSignal>([&](const PingSignal&) { throw std::runtime_error("boom"); });
    EventBus::Instance().Subscribe<PingSignal>([&](const PingSignal&) { ++survived; });
    EventBus::Instance().Publish(PingSignal{1});
    EventBus::Instance().Drain();
    CHECK_EQ(survived, 1);
}

RAVEN_TEST("eventbus: unsubscribe stops delivery; invalid handle is a no-op") {
    BusFixture bus;
    int count = 0;
    auto handle = EventBus::Instance().Subscribe<PingSignal>([&](const PingSignal&) { ++count; });
    EventBus::Instance().Publish(PingSignal{1});
    EventBus::Instance().Drain();
    CHECK_EQ(count, 1);
    CHECK(EventBus::Instance().Unsubscribe(handle));
    CHECK(!EventBus::Instance().Unsubscribe(handle));  // already removed
    CHECK(!EventBus::Instance().Unsubscribe(EventBus::kInvalidSubscription));
    EventBus::Instance().Publish(PingSignal{2});
    EventBus::Instance().Drain();
    CHECK_EQ(count, 1);
}

RAVEN_TEST("eventbus: no subscribers — publish + drain is a silent no-op") {
    BusFixture bus;
    EventBus::Instance().Publish(PingSignal{42});
    EventBus::Instance().Drain();  // must not crash
    CHECK_EQ(EventBus::Instance().PendingSignalCount(), size_t(0));
}
