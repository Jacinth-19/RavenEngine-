// RavenEditor — entity lifecycle commands (§6.2 #5–12).
//
// All resolve entities via Scene::FindByUUID at Execute/Uexecute time — handles never
// cross frames; a dead entity makes the command a safe no-op. Snapshots use the
// subtree serialization (Scene::SerializeSubtree / InstantiateSubtree) so undo/redo
// restores original UUIDs and hierarchy shape.
//
// Header-only.
#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "raven/core/Logger.h"
#include "raven/core/UUID.h"
#include "raven/core/Undo.h"
#include "raven/scene/Components.h"
#include "raven/scene/Scene.h"

namespace raven::editor {

// §6.2 #6 EntityCreateCommand — pre-minted UUID so undo→redo keeps identity stable.
class EntityCreateCommand final : public raven::ICommand {
public:
    EntityCreateCommand(Scene& scene, std::string name, raven::UUID parentUuid)
        : m_Scene(scene), m_Name(std::move(name)), m_Parent(parentUuid), m_NewUuid(raven::UUID::Generate()) {}

    void Execute() override {
        ecs::Entity parent = ecs::NullEntity;
        if (!m_Parent.IsNil()) {
            if (auto p = m_Scene.FindByUUID(m_Parent)) parent = *p;
        }
        nlohmann::json snap = BuildSnapshot(m_NewUuid, m_Name);
        m_Created = m_Scene.InstantiateSubtree(snap, parent, /*preserveUuids=*/true);
    }

    void Uexecute() override {
        if (auto e = m_Scene.FindByUUID(m_NewUuid)) m_Scene.DestroyEntity(*e);
        m_Created = ecs::NullEntity;
    }

    std::string Name() const override { return "Create Entity"; }
    const raven::UUID& NewUuid() const { return m_NewUuid; }

private:
    static nlohmann::json BuildSnapshot(const raven::UUID& uuid, const std::string& name) {
        nlohmann::json entity = {
            {"id", 0},
            {"components", {
                {"name", {{"name", name}}},
                {"uuid", {{"id", uuid.ToString()}}},
                {"transform", {{"position", {0.0f, 0.0f, 0.0f}},
                                {"rotation", {0.0f, 0.0f, 0.0f}},
                                {"scale", {1.0f, 1.0f, 1.0f}},
                                {"parent", -1}}},
            }},
        };
        return nlohmann::json{{"schemaVersion", Scene::kSchemaVersion},
                              {"name", "(created)"},
                              {"entities", nlohmann::json::array({entity})}};
    }

    Scene& m_Scene;
    std::string m_Name;
    raven::UUID m_Parent;
    raven::UUID m_NewUuid;
    ecs::Entity m_Created = ecs::NullEntity;
};

// §6.2 #7/#8 EntityDestroyCommand — the whole subtree as one undo entry.
// Multi-selection deletes push N of these into a compound ("Delete Selection").
class EntityDestroyCommand final : public raven::ICommand {
public:
    struct Entry {
        nlohmann::json snapshot;   // full subtree (root first)
        raven::UUID parentUuid;    // original parent (nil = scene root)
        size_t childIndex = 0;     // index within the parent's children list
    };

    explicit EntityDestroyCommand(Scene& scene, const raven::UUID& rootUuid) : m_Scene(scene), m_Root(rootUuid) {}

    void Execute() override {
        auto root = m_Scene.FindByUUID(m_Root);
        if (!root) return;
        Entry entry;
        entry.snapshot = m_Scene.SerializeSubtree(*root);
        if (const auto& t = m_Scene.World().GetComponent<TransformComponent>(*root);
            t.parent != ecs::NullEntity) {
            if (m_Scene.World().HasComponent<UuidComponent>(t.parent))
                entry.parentUuid = m_Scene.World().GetComponent<UuidComponent>(t.parent).id;
        }
        ecs::Entity parent = ecs::NullEntity;
        if (const auto& t = m_Scene.World().GetComponent<TransformComponent>(*root);
            t.parent != ecs::NullEntity && m_Scene.World().IsAlive(t.parent))
            parent = t.parent;
        if (parent != ecs::NullEntity) {
            const auto& children = m_Scene.World().GetComponent<TransformComponent>(parent).children;
            const auto it = std::find(children.begin(), children.end(), *root);
            if (it != children.end()) entry.childIndex = static_cast<size_t>(it - children.begin());
        }
        m_Entries.push_back(std::move(entry));
        m_Scene.DestroyEntity(*root);
    }

    void Uexecute() override {
        // Restore in reverse order of destruction; clamped insertion index.
        for (auto it = m_Entries.rbegin(); it != m_Entries.rend(); ++it) {
            ecs::Entity parent = ecs::NullEntity;
            if (!it->parentUuid.IsNil())
                if (auto p = m_Scene.FindByUUID(it->parentUuid)) parent = *p;
            ecs::Entity root = m_Scene.InstantiateSubtree(it->snapshot, parent, /*preserveUuids=*/true);
            if (root != ecs::NullEntity) m_Scene.MoveChildToIndex(root, it->childIndex);
        }
    }

    std::string Name() const override { return "Delete Entity"; }
    size_t EntryCount() const { return m_Entries.size(); }

private:
    Scene& m_Scene;
    raven::UUID m_Root;
    std::vector<Entry> m_Entries;
};

// §6.2 #9 EntityRenameCommand.
class EntityRenameCommand final : public raven::ICommand {
public:
    EntityRenameCommand(Scene& scene, const raven::UUID& entity, std::string newName)
        : m_Scene(scene), m_Entity(entity), m_New(std::move(newName)) {
        if (auto e = m_Scene.FindByUUID(m_Entity))
            if (m_Scene.World().HasComponent<NameComponent>(*e))
                m_Old = m_Scene.World().GetComponent<NameComponent>(*e).name;
    }

    void Execute() override {
        if (auto e = m_Scene.FindByUUID(m_Entity)) {
            if (!m_Scene.World().HasComponent<NameComponent>(*e)) return;
            m_Scene.World().GetComponent<NameComponent>(*e).name = m_New;
            m_Scene.MarkDirty();
        }
    }

    void Uexecute() override {
        if (!m_Old) return;
        if (auto e = m_Scene.FindByUUID(m_Entity)) {
            if (!m_Scene.World().HasComponent<NameComponent>(*e)) return;
            m_Scene.World().GetComponent<NameComponent>(*e).name = *m_Old;
            m_Scene.MarkDirty();
        }
    }

    std::string Name() const override { return "Rename Entity"; }

private:
    Scene& m_Scene;
    raven::UUID m_Entity;
    std::string m_New;
    std::optional<std::string> m_Old;
};

// §6.2 #10 EntityReparentCommand — validates against cycles; a rejected re-parent
// records nothing and undoes to a no-op.
class EntityReparentCommand final : public raven::ICommand {
public:
    // newIndex == SIZE_MAX → append (hierarchy drop semantics).
    EntityReparentCommand(Scene& scene, const raven::UUID& child, const raven::UUID& newParent,
                          size_t newIndex = std::numeric_limits<size_t>::max())
        : m_Scene(scene), m_Child(child), m_NewParent(newParent), m_NewIndex(newIndex) {}

    void Execute() override {
        auto child = m_Scene.FindByUUID(m_Child);
        if (!child) return;
        ecs::Entity childE = *child;

        ecs::Entity newParent = ecs::NullEntity;
        if (!m_NewParent.IsNil()) {
            auto p = m_Scene.FindByUUID(m_NewParent);
            if (!p) { RAVEN_LOG_WARN("EntityReparentCommand: new parent %s not found", m_NewParent.ToString().c_str()); return; }
            newParent = *p;
        }
        if (newParent == childE || (newParent != ecs::NullEntity && m_Scene.IsDescendantOf(newParent, childE))) {
            RAVEN_LOG_WARN("EntityReparentCommand: reparent rejected — would create a hierarchy cycle");
            m_Applied = false;
            return;
        }

        TransformComponent& t = m_Scene.World().GetComponent<TransformComponent>(childE);
        if (m_OldParentSet = t.parent != ecs::NullEntity; m_OldParentSet) {
            if (m_Scene.World().HasComponent<UuidComponent>(t.parent))
                m_OldParent = m_Scene.World().GetComponent<UuidComponent>(t.parent).id;
            const auto& children = m_Scene.World().GetComponent<TransformComponent>(t.parent).children;
            const auto it = std::find(children.begin(), children.end(), childE);
            if (it != children.end()) m_OldIndex = static_cast<size_t>(it - children.begin());
        }
        m_Scene.SetParent(childE, newParent);
        if (m_NewIndex != std::numeric_limits<size_t>::max()) m_Scene.MoveChildToIndex(childE, m_NewIndex);
        m_Applied = true;
    }

    void Uexecute() override {
        if (!m_Applied) return;
        auto child = m_Scene.FindByUUID(m_Child);
        if (!child) return;
        ecs::Entity oldParent = ecs::NullEntity;
        if (m_OldParentSet && !m_OldParent.IsNil())
            if (auto p = m_Scene.FindByUUID(m_OldParent)) oldParent = *p;
        m_Scene.SetParent(*child, oldParent);
        m_Scene.MoveChildToIndex(*child, m_OldIndex);
    }

    std::string Name() const override { return "Reparent Entity"; }

private:
    Scene& m_Scene;
    raven::UUID m_Child;
    raven::UUID m_NewParent;
    size_t m_NewIndex;
    raven::UUID m_OldParent;
    size_t m_OldIndex = 0;
    bool m_OldParentSet = false;
    bool m_Applied = false;
};

// §6.2 #11 EntityReorderCommand — sibling index move within one parent.
class EntityReorderCommand final : public raven::ICommand {
public:
    EntityReorderCommand(Scene& scene, const raven::UUID& child, size_t newIndex)
        : m_Scene(scene), m_Child(child), m_NewIndex(newIndex) {
        if (auto e = m_Scene.FindByUUID(m_Child)) {
            TransformComponent& t = m_Scene.World().GetComponent<TransformComponent>(*e);
            if (t.parent != ecs::NullEntity && m_Scene.World().IsAlive(t.parent)) {
                const auto& children = m_Scene.World().GetComponent<TransformComponent>(t.parent).children;
                const auto it = std::find(children.begin(), children.end(), *e);
                if (it != children.end()) m_OldIndex = static_cast<size_t>(it - children.begin());
            }
        }
    }

    void Execute() override {
        if (auto e = m_Scene.FindByUUID(m_Child)) m_Scene.MoveChildToIndex(*e, m_NewIndex);
    }

    void Uexecute() override {
        if (auto e = m_Scene.FindByUUID(m_Child)) m_Scene.MoveChildToIndex(*e, m_OldIndex);
    }

    std::string Name() const override { return "Reorder Entity"; }

private:
    Scene& m_Scene;
    raven::UUID m_Child;
    size_t m_NewIndex;
    size_t m_OldIndex = 0;
};

// §6.2 #12 EntityDuplicateCommand — deep copy with FRESH uuids; redo of undo
// re-materializes the exact same entity set (UUIDs recorded on first Execute).
class EntityDuplicateCommand final : public raven::ICommand {
public:
    explicit EntityDuplicateCommand(Scene& scene, const raven::UUID& source) : m_Scene(scene), m_Source(source) {}

    void Execute() override {
        auto src = m_Scene.FindByUUID(m_Source);
        if (!src) return;
        ecs::Entity srcE = *src;

        nlohmann::json snap = m_Scene.SerializeSubtree(srcE);
        StripUuidsAndRenameCopy(snap);

        ecs::Entity parent = ecs::NullEntity;
        if (const auto& t = m_Scene.World().GetComponent<TransformComponent>(srcE);
            t.parent != ecs::NullEntity && m_Scene.World().IsAlive(t.parent))
            parent = t.parent;

        if (m_MintedUuids.empty()) {
            m_Created = m_Scene.InstantiateSubtree(snap, parent, /*preserveUuids=*/false);
            if (m_Created != ecs::NullEntity)
                for (ecs::Entity e : m_Scene.SubtreeEntities(m_Created))
                    if (m_Scene.World().HasComponent<UuidComponent>(e))
                        m_MintedUuids.push_back(m_Scene.World().GetComponent<UuidComponent>(e).id);
        } else {
            // Redo: restore the exact entity set minted on the first Execute.
            RewriteUuids(snap, m_MintedUuids);
            m_Created = m_Scene.InstantiateSubtree(snap, parent, /*preserveUuids=*/true);
        }
    }

    void Uexecute() override {
        if (auto e = m_Scene.FindByUUID(CreatedRootUuid())) m_Scene.DestroyEntity(*e);
        m_Created = ecs::NullEntity;
    }

    std::string Name() const override { return "Duplicate Entity"; }
    raven::UUID CreatedRootUuid() const { return m_MintedUuids.empty() ? raven::UUID::Nil() : m_MintedUuids.front(); }

private:
    static void StripUuidsAndRenameCopy(nlohmann::json& snap) {
        auto& entities = snap["entities"];
        bool first = true;
        for (auto& entry : entities) {
            auto& components = entry["components"];
            components.erase("uuid");
            if (first && components.contains("name")) {
                components["name"]["name"] = components["name"].value("name", std::string("Entity")) + " (Copy)";
                first = false;
            }
        }
    }

    static void RewriteUuids(nlohmann::json& snap, const std::vector<raven::UUID>& uuids) {
        auto& entities = snap["entities"];
        for (size_t i = 0; i < entities.size() && i < uuids.size(); ++i)
            entities[i]["components"]["uuid"] = {{"id", uuids[i].ToString()}};
    }

    Scene& m_Scene;
    raven::UUID m_Source;
    ecs::Entity m_Created = ecs::NullEntity;
    std::vector<raven::UUID> m_MintedUuids;
};

} // namespace raven::editor
