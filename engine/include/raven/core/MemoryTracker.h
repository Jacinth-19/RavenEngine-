// RavenEngine — per-category runtime memory accounting.
//
// STL and third-party heap traffic is intentionally NOT tracked (that is Tracy's job);
// this tracker covers engine-owned allocations tagged at the call site (renderer GPU
// CPU-side blocks, asset load buffers, editor-side scratch, …). Adoption is incremental —
// call sites opt in with Add/Sub. Reads from the UI/profiler thread are relaxed and
// racy-by-design (they are for HUD display, not for control flow).
//
// Porting notes: mirrors Luth MemoryTracker.h (MIT), re-namespaced (raven:: vs
// Luth::Memory) and re-categorised (Raven's MemCategory set), with the doc's
// Add/Sub + LeakReport surface (docs/EDITOR_DESIGN.md §11.1).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace raven {

enum class MemCategory : uint8_t {
    Core = 0,
    Renderer,
    Assets,
    Editor,
    Physics,
    Audio,
    Script,
    Other,
    kCount,
};

// Lock-free per-category counters. Signed so underflow bugs surface instead of wrapping.
struct MemCategoryStats {
    std::atomic<int64_t> current{0};
    std::atomic<int64_t> peak{0};
    std::atomic<int64_t> totalAlloc{0};  // cumulative bytes added
    std::atomic<int64_t> totalFree{0};   // cumulative bytes subtracted
};

// Plain-data snapshot safe to hand to a UI thread.
struct MemCategorySnapshot {
    MemCategory category = MemCategory::Other;
    int64_t current = 0;
    int64_t peak = 0;
    int64_t totalAlloc = 0;
    int64_t totalFree = 0;
    int64_t budget = 0;  // 0 = no budget set
};

class MemoryTracker {
public:
    static MemoryTracker& Instance();

    void Add(MemCategory category, size_t bytes);
    void Sub(MemCategory category, size_t bytes);

    // Optional per-category budget (0 clears). Snapshot reports it; nothing enforces it —
    // enforcement policy belongs to the profiler/leak reporting layer.
    void SetBudget(MemCategory category, int64_t bytes);
    int64_t Budget(MemCategory category) const;

    MemCategorySnapshot Snapshot(MemCategory category) const;
    std::vector<MemCategorySnapshot> SnapshotAll() const;
    int64_t TotalCurrent() const;
    int64_t TotalPeak() const;

    // Multi-line human-readable report of every category whose current != 0 (plus totals).
    // Returns an empty string when everything is balanced — i.e. "no leaks detected".
    std::string LeakReport() const;

    // Zero every counter and budget. For tests and full engine shutdown.
    void Reset();

    static const char* CategoryName(MemCategory category);

private:
    MemoryTracker() = default;

    MemCategoryStats m_Stats[static_cast<size_t>(MemCategory::kCount)];
    std::atomic<int64_t> m_Budgets[static_cast<size_t>(MemCategory::kCount)]{};
    std::atomic<int64_t> m_TotalCurrent{0};
    std::atomic<int64_t> m_TotalPeak{0};
};

} // namespace raven
