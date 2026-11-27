// LinearAllocator: bump allocation, alignment, reset semantics, deferred destructors.
#include "test_framework.h"

#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "raven/memory/LinearAllocator.h"

using namespace raven::memory;

RAVEN_TEST("linear allocator: allocations are aligned and non-overlapping") {
    LinearAllocator a(1024);
    void* p1 = a.Alloc(3, 16);
    void* p2 = a.Alloc(7, 16);
    CHECK(reinterpret_cast<uintptr_t>(p1) % 16 == 0);
    CHECK(reinterpret_cast<uintptr_t>(p2) % 16 == 0);
    CHECK(p2 != p1);
    CHECK(reinterpret_cast<uintptr_t>(p2) > reinterpret_cast<uintptr_t>(p1));
}

RAVEN_TEST("linear allocator: tracks Used/Remaining/Capacity") {
    LinearAllocator a(256);
    CHECK_EQ(a.Capacity(), size_t(256));
    CHECK_EQ(a.Used(), size_t(0));
    CHECK_EQ(a.Remaining(), size_t(256));
    a.Alloc(100, 8);
    CHECK_EQ(a.Used(), size_t(100));
    CHECK_EQ(a.Remaining(), size_t(156));
    CHECK_EQ(a.AllocCount(), size_t(1));
}

RAVEN_TEST("linear allocator: New constructs objects in place") {
    LinearAllocator a(4096);
    struct Point { float x, y; Point(float x_, float y_) : x(x_), y(y_) {} };
    Point* p = a.New<Point>(3.0f, 4.0f);
    CHECK(p != nullptr);
    CHECK(p->x == 3.0f && p->y == 4.0f);
}

RAVEN_TEST("linear allocator: non-trivial New<T> runs destructors on Reset (LIFO)") {
    static std::vector<int> destructionOrder;
    destructionOrder.clear();

    struct Widget {
        int id;
        explicit Widget(int id_) : id(id_) {}
        ~Widget() { destructionOrder.push_back(id); }
    };

    {
        LinearAllocator a(4096);
        a.New<Widget>(1);
        a.New<Widget>(2);
        a.New<Widget>(3);
        CHECK_EQ(destructionOrder.size(), size_t(0));
        a.Reset();
        CHECK_EQ(destructionOrder.size(), size_t(3));
        CHECK_EQ(destructionOrder[0], 3);  // LIFO
        CHECK_EQ(destructionOrder[1], 2);
        CHECK_EQ(destructionOrder[2], 1);
    }
}

RAVEN_TEST("linear allocator: trivial types skip the deferred destructor list") {
    LinearAllocator a(1024);
    a.New<int>(42);
    a.New<double>(2.5);
    a.Reset();  // must not run anything, must not crash
    a.Reset();  // double reset is a no-op
}

RAVEN_TEST("linear allocator: Reset rewinds so the arena can be reused") {
    LinearAllocator a(128);
    void* first = a.Alloc(64, 8);
    a.Alloc(32, 8);
    a.Reset();
    CHECK_EQ(a.Used(), size_t(0));
    CHECK_EQ(a.AllocCount(), size_t(0));
    void* second = a.Alloc(64, 8);
    CHECK_EQ(first, second);  // same base address reused
}

RAVEN_TEST("linear allocator: exhaustion throws std::bad_alloc") {
    LinearAllocator a(64);
    a.Alloc(48, 8);
    bool threw = false;
    try {
        a.Alloc(48, 8);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    CHECK(threw);
}

RAVEN_TEST("linear allocator: rejects non-power-of-two alignment") {
    LinearAllocator a(64);
    bool threw = false;
    try {
        a.Alloc(8, 3);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    CHECK(threw);
}

RAVEN_TEST("linear allocator: shared_ptr payloads inside New<T> are destroyed on Reset") {
    auto* tracker = new bool(false);
    auto deleter = [tracker](bool* p) { *tracker = true; delete p; };
    {
        LinearAllocator a(1024);
        auto* sp = a.New<std::shared_ptr<bool>>(new bool(true), deleter);
        CHECK(**sp);
        CHECK(sp->use_count() == 1);
        a.Reset();
    }
    CHECK(*tracker);
    delete tracker;
}
