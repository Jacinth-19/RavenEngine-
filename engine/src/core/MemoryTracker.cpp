#include "raven/core/MemoryTracker.h"

#include <sstream>

namespace raven {

MemoryTracker& MemoryTracker::Instance() {
    static MemoryTracker kInstance;
    return kInstance;
}

void MemoryTracker::Add(MemCategory category, size_t bytes) {
    auto& s = m_Stats[static_cast<size_t>(category)];
    const int64_t delta = static_cast<int64_t>(bytes);

    const int64_t newVal = s.current.fetch_add(delta, std::memory_order_relaxed) + delta;
    s.totalAlloc.fetch_add(delta, std::memory_order_relaxed);

    int64_t peak = s.peak.load(std::memory_order_relaxed);
    while (newVal > peak) {
        if (s.peak.compare_exchange_weak(peak, newVal, std::memory_order_relaxed)) break;
    }

    const int64_t globalNew = m_TotalCurrent.fetch_add(delta, std::memory_order_relaxed) + delta;
    int64_t globalPeak = m_TotalPeak.load(std::memory_order_relaxed);
    while (globalNew > globalPeak) {
        if (m_TotalPeak.compare_exchange_weak(globalPeak, globalNew, std::memory_order_relaxed)) break;
    }
}

void MemoryTracker::Sub(MemCategory category, size_t bytes) {
    auto& s = m_Stats[static_cast<size_t>(category)];
    const int64_t delta = static_cast<int64_t>(bytes);

    s.current.fetch_sub(delta, std::memory_order_relaxed);
    s.totalFree.fetch_add(delta, std::memory_order_relaxed);
    m_TotalCurrent.fetch_sub(delta, std::memory_order_relaxed);
}

void MemoryTracker::SetBudget(MemCategory category, int64_t bytes) {
    m_Budgets[static_cast<size_t>(category)].store(bytes, std::memory_order_relaxed);
}

int64_t MemoryTracker::Budget(MemCategory category) const {
    return m_Budgets[static_cast<size_t>(category)].load(std::memory_order_relaxed);
}

MemCategorySnapshot MemoryTracker::Snapshot(MemCategory category) const {
    const auto& s = m_Stats[static_cast<size_t>(category)];
    MemCategorySnapshot snap;
    snap.category = category;
    snap.current = s.current.load(std::memory_order_relaxed);
    snap.peak = s.peak.load(std::memory_order_relaxed);
    snap.totalAlloc = s.totalAlloc.load(std::memory_order_relaxed);
    snap.totalFree = s.totalFree.load(std::memory_order_relaxed);
    snap.budget = m_Budgets[static_cast<size_t>(category)].load(std::memory_order_relaxed);
    return snap;
}

std::vector<MemCategorySnapshot> MemoryTracker::SnapshotAll() const {
    std::vector<MemCategorySnapshot> out;
    out.reserve(static_cast<size_t>(MemCategory::kCount));
    for (uint8_t i = 0; i < static_cast<uint8_t>(MemCategory::kCount); ++i)
        out.push_back(Snapshot(static_cast<MemCategory>(i)));
    return out;
}

int64_t MemoryTracker::TotalCurrent() const {
    return m_TotalCurrent.load(std::memory_order_relaxed);
}

int64_t MemoryTracker::TotalPeak() const {
    return m_TotalPeak.load(std::memory_order_relaxed);
}

std::string MemoryTracker::LeakReport() const {
    std::ostringstream out;
    bool any = false;
    for (uint8_t i = 0; i < static_cast<uint8_t>(MemCategory::kCount); ++i) {
        const auto snap = Snapshot(static_cast<MemCategory>(i));
        if (snap.current == 0) continue;
        any = true;
        out << "LEAK [" << CategoryName(snap.category) << "]: " << snap.current
            << " bytes remaining (peak " << snap.peak << ", total allocated " << snap.totalAlloc
            << ", total freed " << snap.totalFree << ")\n";
    }
    if (any) {
        out << "TOTAL: " << TotalCurrent() << " bytes unaccounted at exit (peak " << TotalPeak() << ")";
    }
    return out.str();
}

void MemoryTracker::Reset() {
    for (auto& s : m_Stats) {
        s.current.store(0, std::memory_order_relaxed);
        s.peak.store(0, std::memory_order_relaxed);
        s.totalAlloc.store(0, std::memory_order_relaxed);
        s.totalFree.store(0, std::memory_order_relaxed);
    }
    for (auto& b : m_Budgets) b.store(0, std::memory_order_relaxed);
    m_TotalCurrent.store(0, std::memory_order_relaxed);
    m_TotalPeak.store(0, std::memory_order_relaxed);
}

const char* MemoryTracker::CategoryName(MemCategory category) {
    switch (category) {
        case MemCategory::Core:     return "Core";
        case MemCategory::Renderer: return "Renderer";
        case MemCategory::Assets:   return "Assets";
        case MemCategory::Editor:   return "Editor";
        case MemCategory::Physics:  return "Physics";
        case MemCategory::Audio:    return "Audio";
        case MemCategory::Script:   return "Script";
        case MemCategory::Other:    return "Other";
        default:                    return "Unknown";
    }
}

} // namespace raven
