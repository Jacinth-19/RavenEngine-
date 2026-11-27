// MemoryTracker: counters, peak tracking, snapshots, leak report.
#include "test_framework.h"

#include <string>

#include "raven/core/MemoryTracker.h"

using namespace raven;

namespace {
struct TrackerFixture {
    TrackerFixture() { MemoryTracker::Instance().Reset(); }
    ~TrackerFixture() { MemoryTracker::Instance().Reset(); }
};
} // namespace

RAVEN_TEST("memory tracker: Add/Sub maintain current bytes and alloc/free totals") {
    TrackerFixture fx;
    auto& t = MemoryTracker::Instance();
    t.Add(MemCategory::Assets, 100);
    t.Add(MemCategory::Assets, 50);
    MemCategorySnapshot s1 = t.Snapshot(MemCategory::Assets);
    CHECK_EQ(s1.current, 150);
    CHECK_EQ(s1.totalAlloc, 150);
    CHECK_EQ(s1.peak, 150);

    t.Sub(MemCategory::Assets, 70);
    MemCategorySnapshot s2 = t.Snapshot(MemCategory::Assets);
    CHECK_EQ(s2.current, 80);
    CHECK_EQ(s2.totalFree, 70);
    CHECK_EQ(s2.peak, 150);  // peak does not shrink
}

RAVEN_TEST("memory tracker: peak is the high-water mark across sub/add cycles") {
    TrackerFixture fx;
    auto& t = MemoryTracker::Instance();
    t.Add(MemCategory::Renderer, 1000);
    t.Sub(MemCategory::Renderer, 1000);
    t.Add(MemCategory::Renderer, 400);
    CHECK_EQ(t.Snapshot(MemCategory::Renderer).peak, 1000);
    CHECK_EQ(t.Snapshot(MemCategory::Renderer).current, 400);
}

RAVEN_TEST("memory tracker: categories are independent and totals aggregate") {
    TrackerFixture fx;
    auto& t = MemoryTracker::Instance();
    t.Add(MemCategory::Editor, 10);
    t.Add(MemCategory::Physics, 30);
    t.Add(MemCategory::Script, 60);
    CHECK_EQ(t.TotalCurrent(), 100);
    CHECK(t.TotalPeak() >= 100);
    CHECK_EQ(t.Snapshot(MemCategory::Editor).current, 10);
    CHECK_EQ(t.Snapshot(MemCategory::Physics).current, 30);
}

RAVEN_TEST("memory tracker: budget is reported and configurable") {
    TrackerFixture fx;
    auto& t = MemoryTracker::Instance();
    CHECK_EQ(t.Budget(MemCategory::Editor), 0);
    t.SetBudget(MemCategory::Editor, 4096);
    CHECK_EQ(t.Snapshot(MemCategory::Editor).budget, 4096);
    t.SetBudget(MemCategory::Editor, 0);  // clears
    CHECK_EQ(t.Budget(MemCategory::Editor), 0);
}

RAVEN_TEST("memory tracker: LeakReport flags imbalanced categories, empty when balanced") {
    TrackerFixture fx;
    auto& t = MemoryTracker::Instance();
    CHECK(t.LeakReport().empty());

    t.Add(MemCategory::Editor, 2048);
    std::string report = t.LeakReport();
    CHECK(report.find("Editor") != std::string::npos);
    CHECK(report.find("2048") != std::string::npos);

    t.Sub(MemCategory::Editor, 2048);
    CHECK(t.LeakReport().empty());
}

RAVEN_TEST("memory tracker: snapshot covers every category, names are non-null") {
    TrackerFixture fx;
    auto& t = MemoryTracker::Instance();
    auto all = t.SnapshotAll();
    CHECK_EQ(all.size(), size_t(MemCategory::kCount));
    for (const auto& s : all) {
        CHECK(MemoryTracker::CategoryName(s.category) != nullptr);
    }
}
