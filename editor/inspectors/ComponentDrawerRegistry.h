// RavenEditor — inspector component-drawer registry (§6.3).
//
// Replaces the fixed if-chain in DrawInspectorPanel. Each entry knows:
//   * the scene-format JSON key (clipboard / Reset / paste round-trips)
//   * how to test presence, add (default value), remove (command) — all command-routed
//   * a draw() body rendering only the fields
// The registry header-drawing (CollapsingHeader + overflow context menu — Copy / Paste /
// Reset / Remove / Copy As JSON) is shared for every component. MultiEdit fan-out is
// built in: DrawerContext carries the OTHER selected entities holding the same
// component, and the PropertyRow/ColorRow helpers push one MultiEditPropertyCommand
// over all of them (one compound entry per gesture).
//
// Header-only. The editor app calls EnsureBuiltinDrawersRegistered() once at startup.
#pragma once

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "imgui.h"

#include "raven/core/UUID.h"
#include "raven/core/Undo.h"
#include "raven/scene/Components.h"
#include "raven/scene/Scene.h"

#include "editor/commands/ComponentCommands.h"
#include "editor/commands/ComponentJsonCommand.h"
#include "editor/commands/EditorCommandStack.h"
#include "editor/commands/MultiEditPropertyCommand.h"
#include "editor/commands/PropertyCommand.h"
#include "editor/selection/EditorSelection.h"

namespace raven::editor {

struct DrawerContext {
    Scene* scene = nullptr;
    ecs::Entity entity = 0;
    raven::UUID uuid;             // primary-selected entity UUID
    std::vector<raven::UUID> others;   // OTHER selected entities also holding this component (multi-edit)
};

struct ComponentDrawerEntry {
    std::string key;              // scene-format key ("transform", "rigidbody", ...)
    std::string name;             // display name
    std::string category;         // Add Component popup grouping
    bool canAdd = true;
    bool canRemove = true;
    bool canReset = true;
    std::function<bool(const ecs::World&, ecs::Entity)> present;
    std::function<std::shared_ptr<raven::ICommand>(Scene&, const raven::UUID&)> addCommand;      // null → not addable
    std::function<std::shared_ptr<raven::ICommand>(Scene&, const raven::UUID&)> removeCommand;   // null → not removable
    std::function<nlohmann::json()> defaultJson;                                                  // Reset payload
    std::function<void(DrawerContext&)> draw;
};

class ComponentDrawerRegistry {
public:
    static ComponentDrawerRegistry& Instance() {
        static ComponentDrawerRegistry registry;
        return registry;
    }

    void Register(ComponentDrawerEntry entry) { m_Entries.push_back(std::move(entry)); }
    const std::vector<ComponentDrawerEntry>& Entries() const { return m_Entries; }
    const ComponentDrawerEntry* Find(const std::string& key) const {
        for (const auto& entry : m_Entries) if (entry.key == key) return &entry;
        return nullptr;
    }

private:
    std::vector<ComponentDrawerEntry> m_Entries;
};

// ---------------------------------------------------------------------------
// Widget helpers — float / vec3 / bool / int / color rows with MultiEdit fan-out
// ---------------------------------------------------------------------------

template <typename T, typename Member>
bool PropertyRow(DrawerContext& ctx, const char* label, Member T::* member, const char* commandName,
                 float speed, float vmin, float vmax) {
    using ValueType = std::remove_reference_t<Member>;
    ecs::World& world = ctx.scene->World();
    if (!world.HasComponent<T>(ctx.entity)) return false;
    const ValueType& current = world.GetComponent<T>(ctx.entity).*member;

    // Mixed when any other target disagrees — cleared on the next uniform write.
    bool mixed = false;
    for (const raven::UUID& otherId : ctx.others) {
        auto other = ctx.scene->FindByUUID(otherId);
        if (!other || !world.HasComponent<T>(*other)) continue;
        const ValueType& otherValue = world.GetComponent<T>(*other).*member;
        if (!(otherValue == current)) { mixed = true; break; }
    }

    std::string displayLabel = mixed ? (std::string(label) + " [mixed]") : std::string(label);
    ValueType edit = current;
    bool changed = false;
    if constexpr (std::is_same_v<ValueType, float>) {
        changed = ImGui::DragFloat(displayLabel.c_str(), &edit, speed, vmin, vmax);
    } else if constexpr (std::is_same_v<ValueType, glm::vec3>) {
        changed = ImGui::DragFloat3(displayLabel.c_str(), &edit.x, speed, vmin, vmax);
    } else if constexpr (std::is_same_v<ValueType, bool>) {
        changed = ImGui::Checkbox(displayLabel.c_str(), &edit);
    } else if constexpr (std::is_same_v<ValueType, int>) {
        changed = ImGui::DragInt(displayLabel.c_str(), &edit, speed, static_cast<int>(vmin), static_cast<int>(vmax));
    } else {
        ImGui::TextDisabled("%s", displayLabel.c_str());
        return false;
    }
    if (!changed) return false;

    std::vector<raven::UUID> targets;
    targets.push_back(ctx.uuid);
    targets.insert(targets.end(), ctx.others.begin(), ctx.others.end());
    const std::string tag = "prop:" + ctx.uuid.ToString() + ":" + commandName;
    if (targets.size() == 1) {
        Commands().PushCoalesced(tag,
            MakePropertyCommand(*ctx.scene, ctx.uuid, member, std::move(edit), commandName));
    } else {
        Commands().PushCoalesced(tag,
            MakeMultiEditCommand(*ctx.scene, std::move(targets), member, std::move(edit), commandName));
    }
    return true;
}

template <typename T, typename Member>
bool ColorRow(DrawerContext& ctx, const char* label, Member T::* member, const char* commandName) {
    using ValueType = std::remove_reference_t<Member>;
    static_assert(std::is_same_v<ValueType, glm::vec3>);
    ecs::World& world = ctx.scene->World();
    if (!world.HasComponent<T>(ctx.entity)) return false;
    const glm::vec3& current = world.GetComponent<T>(ctx.entity).*member;
    glm::vec3 edit = current;
    if (!ImGui::ColorEdit3(label, &edit.x)) return false;

    std::vector<raven::UUID> targets;
    targets.push_back(ctx.uuid);
    targets.insert(targets.end(), ctx.others.begin(), ctx.others.end());
    const std::string tag = "prop:" + ctx.uuid.ToString() + ":" + commandName;
    if (targets.size() == 1) {
        Commands().Push(MakePropertyCommand(*ctx.scene, ctx.uuid, member, std::move(edit), commandName));
    } else {
        Commands().Push(MakeMultiEditCommand(*ctx.scene, std::move(targets), member, std::move(edit), commandName));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Component section header + overflow menu (Copy / Paste / Reset / Remove / Copy As JSON)
// ---------------------------------------------------------------------------

// Editor-wide clipboard for component fragments (key + JSON body). Session-local.
struct ComponentClipboard {
    bool valid = false;
    std::string key;
    nlohmann::json json;
};
ComponentClipboard& InspectorClipboard();              // defined in ComponentDrawerRegistry.cpp

// Draws the section header (CollapsingHeader) + overflow menu; calls entry.draw when
// expanded. `multiTargets` is computed here from the live selection.
void DrawComponentSection(Scene& scene, ecs::Entity entity, const raven::UUID& uuid,
                          const ComponentDrawerEntry& entry);

// The Add Component button + searchable category-grouped popup (call at the end of DrawInspectorPanel).
void DrawAddComponentPopup(Scene& scene, ecs::Entity entity, const raven::UUID& uuid);

// One-shot built-in registration (idempotent).
void EnsureBuiltinDrawersRegistered();

} // namespace raven::editor
