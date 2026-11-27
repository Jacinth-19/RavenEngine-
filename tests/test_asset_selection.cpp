// Resource-mode asset selection (§6.3): the ProjectPanel mirrors its primary
// selection into a singleton the Inspector consults before entity mode.
// Header-only — no editor lib link, same pattern as the selection/command tests.
#include "test_framework.h"

#include <memory>
#include <vector>

#include "editor/events/EditorSignals.h"
#include "editor/selection/EditorSelection.h"
#include "raven/core/EventBus.h"
#include "raven/core/UUID.h"

using namespace raven;
using namespace raven::editor;

namespace {
struct BusFixture {
    BusFixture() {
        EventBus::Instance().Reset();
        ClearAssetSelection();
    }
    ~BusFixture() {
        ClearAssetSelection();
        EventBus::Instance().Reset();
    }
};
} // namespace

RAVEN_TEST("asset selection: SelectAsset publishes the new UUID; SelectedAsset/HasAssetSelection agree") {
    BusFixture bus;
    std::vector<raven::UUID> delivered;
    EventBus::Instance().Subscribe<AssetSelectionChangedSignal>(
        [&](const AssetSelectionChangedSignal& s) { delivered.push_back(s.asset); });

    const raven::UUID id = raven::UUID::Generate();
    SelectAsset(id);
    CHECK(HasAssetSelection());
    CHECK(SelectedAsset() == id);
    EventBus::Instance().Drain();
    CHECK_EQ(delivered.size(), size_t(1));
    if (!delivered.empty()) CHECK(delivered[0] == id);
}

RAVEN_TEST("asset selection: selecting the same UUID twice publishes only once") {
    BusFixture bus;
    std::vector<raven::UUID> delivered;
    EventBus::Instance().Subscribe<AssetSelectionChangedSignal>(
        [&](const AssetSelectionChangedSignal& s) { delivered.push_back(s.asset); });

    const raven::UUID id = raven::UUID::Generate();
    SelectAsset(id);
    SelectAsset(id);
    EventBus::Instance().Drain();
    CHECK_EQ(delivered.size(), size_t(1));
}

RAVEN_TEST("asset selection: ClearAssetSelection publishes nil and is idempotent") {
    BusFixture bus;
    std::vector<raven::UUID> delivered;
    EventBus::Instance().Subscribe<AssetSelectionChangedSignal>(
        [&](const AssetSelectionChangedSignal& s) { delivered.push_back(s.asset); });

    const raven::UUID id = raven::UUID::Generate();
    SelectAsset(id);
    ClearAssetSelection();
    ClearAssetSelection();   // no selection left — must not publish again
    CHECK(!HasAssetSelection());
    CHECK(SelectedAsset().IsNil());
    EventBus::Instance().Drain();
    CHECK_EQ(delivered.size(), size_t(2));   // select + clear, nothing else
    if (delivered.size() == 2) CHECK(delivered[1].IsNil());
}

RAVEN_TEST("asset selection: replacing the selection publishes the new UUID") {
    BusFixture bus;
    std::vector<raven::UUID> delivered;
    EventBus::Instance().Subscribe<AssetSelectionChangedSignal>(
        [&](const AssetSelectionChangedSignal& s) { delivered.push_back(s.asset); });

    const raven::UUID a = raven::UUID::Generate();
    const raven::UUID b = raven::UUID::Generate();
    SelectAsset(a);
    SelectAsset(b);
    CHECK(SelectedAsset() == b);
    EventBus::Instance().Drain();
    CHECK_EQ(delivered.size(), size_t(2));
    if (delivered.size() == 2) CHECK(delivered[1] == b);
}

RAVEN_TEST("asset selection: entity selection version is not bumped and entity selection survives") {
    BusFixture bus;
    const raven::UUID entity = raven::UUID::Generate();
    SelectOnly(entity);
    const uint64_t version = SelectionVersion();

    const raven::UUID asset = raven::UUID::Generate();
    SelectAsset(asset);
    CHECK_EQ(SelectionVersion(), version);
    CHECK(IsSelected(entity));
    ClearAssetSelection();
    CHECK(IsSelected(entity));
    CHECK_EQ(SelectionVersion(), version);
    ClearSelection();
}
