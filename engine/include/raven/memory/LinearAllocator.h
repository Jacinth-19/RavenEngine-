// RavenEngine — bump-pointer linear allocator over a fixed arena.
//
// One allocation zone serves many short-lived allocations (per-frame editor gather
// fragments, asset import scratch, …): allocate fast, then Reset() rewinds the whole
// zone in O(1) (destructors of non-trivially-destructible New<T> objects run first,
// in reverse construction order).
//
// Allocation exhaustion throws std::bad_alloc — callers that cannot tolerate that
// (the editor gather path) pre-size the arena generously; the error boundary in the
// editor catches and quarantines the panel instead of corrupting memory.
//
// Not thread-safe by design; give each worker its own allocator.
#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace raven::memory {

class LinearAllocator {
public:
    explicit LinearAllocator(size_t capacityBytes) : m_Arena(capacityBytes) {}

    LinearAllocator(const LinearAllocator&) = delete;
    LinearAllocator& operator=(const LinearAllocator&) = delete;

    ~LinearAllocator() { Reset(); }

    // Bump-allocate `bytes` with the given alignment (default 8, always a power of two).
    // Throws std::bad_alloc when the arena is exhausted.
    void* Alloc(size_t bytes, size_t alignment = 8) {
        if (alignment == 0 || (alignment & (alignment - 1)) != 0)
            throw std::bad_alloc();  // alignment must be a power of two

        const uintptr_t base = reinterpret_cast<uintptr_t>(m_Arena.data());
        const uintptr_t current = base + m_Offset;
        const uintptr_t aligned = (current + alignment - 1) & ~static_cast<uintptr_t>(alignment - 1);
        const size_t alignedOffset = static_cast<size_t>(aligned - base);
        if (bytes > m_Arena.size() - alignedOffset)
            throw std::bad_alloc();

        m_Offset = alignedOffset + bytes;
        ++m_AllocCount;
        return m_Arena.data() + alignedOffset;
    }

    // Placement-new T into the arena. Non-trivially-destructible T registers a deferred
    // destructor that Reset() (and ~LinearAllocator) runs in reverse order of construction.
    template <typename T, typename... Args>
    T* New(Args&&... args) {
        static_assert(std::is_destructible_v<T>, "LinearAllocator::New requires a destructible T");
        void* mem = Alloc(sizeof(T), alignof(T));
        T* obj = ::new (mem) T(std::forward<Args>(args)...);
        if constexpr (!std::is_trivially_destructible_v<T>) {
            m_DeferredDtors.push_back({obj, [](void* p) { static_cast<T*>(p)->~T(); }});
        }
        return obj;
    }

    // Rewind the arena. Runs deferred destructors (LIFO), then offset = 0.
    // The arena storage itself is retained (no heap traffic per frame).
    void Reset() {
        while (!m_DeferredDtors.empty()) {
            auto [ptr, fn] = m_DeferredDtors.back();
            m_DeferredDtors.pop_back();
            fn(ptr);
        }
        m_Offset = 0;
        m_AllocCount = 0;
    }

    size_t Capacity() const { return m_Arena.size(); }
    size_t Used() const { return m_Offset; }
    size_t Remaining() const { return m_Arena.size() - m_Offset; }
    size_t AllocCount() const { return m_AllocCount; }

private:
    struct DeferredDtor {
        void* ptr;
        void (*run)(void*);
    };

    std::vector<std::byte> m_Arena;
    std::vector<DeferredDtor> m_DeferredDtors;
    size_t m_Offset = 0;
    size_t m_AllocCount = 0;
};

} // namespace raven::memory
