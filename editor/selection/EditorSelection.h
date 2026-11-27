// RavenEditor — selection singleton (§5.3, 1:1 with Luth EditorSelection).
//
// One process-wide selection of entity UUIDs. The primary selection is the back of
// the vector. s_Version bumps on every mutation; consumers (hierarchy highlight,
// inspector, viewport outline) key off Version() to know when to resync. Publishes
// SelectionChangedSignal on the bus — drained once per frame by the editor loop.
//
// Header-only: included by the editor app AND by tests (no editor lib link).
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>

#include "editor/events/EditorSignals.h"
#include "raven/core/EventBus.h"
#include "raven/core/UUID.h"

namespace raven::editor {

namespace detail {
inline std::vector<raven::UUID>& SelectedStore() {
    static std::vector<raven::UUID> s_Selected;   // process-lifetime
    return s_Selected;
}
inline uint64_t& VersionStore() {
    static uint64_t s_Version = 0;
    return s_Version;
}
inline void PublishSelectionChanged() {
    const std::vector<raven::UUID>& sel = SelectedStore();
    SelectionChangedSignal signal;
    signal.version = VersionStore();
    signal.primary = sel.empty() ? raven::UUID::Nil() : sel.back();
    raven::EventBus::Instance().Publish(std::move(signal));
}
inline void BumpAndPublish() { ++VersionStore(); PublishSelectionChanged(); }
} // namespace detail

// Monotonic selection version — bumped on every mutation (0 = pristine).
inline uint64_t SelectionVersion() { return detail::VersionStore(); }
// The selected entity UUIDs, primary LAST.
inline const std::vector<raven::UUID>& Selection() { return detail::SelectedStore(); }
// Primary selection (last added), or nil UUID when nothing is selected.
inline raven::UUID PrimarySelection() {
    const auto& sel = detail::SelectedStore();
    return sel.empty() ? raven::UUID::Nil() : sel.back();
}
inline bool HasSelection() { return !detail::SelectedStore().empty(); }
inline size_t SelectionCount() { return detail::SelectedStore().size(); }
inline bool IsSelected(const raven::UUID& id) {
    const auto& sel = detail::SelectedStore();
    return std::find(sel.begin(), sel.end(), id) != sel.end();
}

// Replace the selection with exactly this entity.
inline void SelectOnly(const raven::UUID& id) {
    auto& sel = detail::SelectedStore();
    if (sel.size() == 1 && sel.front() == id) return;
    sel.assign(1, id);
    detail::BumpAndPublish();
}

// Add to the selection (no-op if already present); keeps existing members, appends.
inline void SelectAdd(const raven::UUID& id) {
    auto& sel = detail::SelectedStore();
    if (std::find(sel.begin(), sel.end(), id) != sel.end()) return;
    sel.push_back(id);
    detail::BumpAndPublish();
}

// Toggle membership (Ctrl-click semantics).
inline void SelectToggle(const raven::UUID& id) {
    auto& sel = detail::SelectedStore();
    if (auto it = std::find(sel.begin(), sel.end(), id); it != sel.end()) sel.erase(it);
    else sel.push_back(id);
    detail::BumpAndPublish();
}

// Remove one member (no-op if absent).
inline void SelectRemove(const raven::UUID& id) {
    auto& sel = detail::SelectedStore();
    if (auto it = std::find(sel.begin(), sel.end(), id); it != sel.end()) {
        sel.erase(it);
        detail::BumpAndPublish();
    }
}

// --- Asset (resource-mode) selection, independent of the entity set ------------
// "Resource mode wins when both set" (LUTH behavior): the Inspector shows the asset
// viewer when an asset is selected, entity inspector otherwise.
namespace detail {
inline raven::UUID& SelectedAssetStore() {
    static raven::UUID s_Asset = raven::UUID::Nil();
    return s_Asset;
}
inline void PublishAssetSelectionChanged() {
    AssetSelectionChangedSignal signal;
    signal.asset = SelectedAssetStore();
    raven::EventBus::Instance().Publish(std::move(signal));
}
} // namespace detail

// Nil UUID when no asset is selected.
inline raven::UUID SelectedAsset() { return detail::SelectedAssetStore(); }
inline bool HasAssetSelection() { return !detail::SelectedAssetStore().IsNil(); }
// Select exactly this asset (replaces the previous asset selection).
inline void SelectAsset(const raven::UUID& id) {
    raven::UUID& current = detail::SelectedAssetStore();
    if (current == id) return;
    current = id;
    detail::PublishAssetSelectionChanged();
}
// Clear the asset selection (entity selection is untouched).
inline void ClearAssetSelection() {
    raven::UUID& current = detail::SelectedAssetStore();
    if (current.IsNil()) return;
    current = raven::UUID::Nil();
    detail::PublishAssetSelectionChanged();
}

// Empty the selection.
inline void ClearSelection() {
    auto& sel = detail::SelectedStore();
    if (sel.empty()) return;
    sel.clear();
    detail::BumpAndPublish();
}

// Drop members that no longer exist (post-delete reconciliation).
inline void PruneSelection(const std::function<bool(const raven::UUID&)>& alive) {
    auto& sel = detail::SelectedStore();
    const size_t before = sel.size();
    sel.erase(std::remove_if(sel.begin(), sel.end(), [&](const raven::UUID& id) { return !alive(id); }), sel.end());
    if (sel.size() != before) detail::BumpAndPublish();
}

// Replace the whole selection with a list (range-select result).
inline void SetSelection(std::vector<raven::UUID> ids) {
    auto& sel = detail::SelectedStore();
    if (sel == ids) return;
    sel = std::move(ids);
    detail::BumpAndPublish();
}

} // namespace raven::editor
