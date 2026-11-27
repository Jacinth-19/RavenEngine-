// RavenEngine — Scene: an ecs::World plus name, dirty tracking, JSON I/O and
// hierarchy helpers. File format: *.ravenscene (see docs/ASSET_PIPELINE.md).
#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "raven/ecs/World.h"
#include "raven/scene/Components.h"

namespace raven {

class Scene {
public:
    // Schema history: 1 = initial; 2 adds the "lod" component (LOD chain) on entities;
    // 3 adds the "uuid" component (stable entity identity for editor/undo/prefabs).
    // Readers at v1/v2 mint fresh entity UUIDs on load (that is the migration); writers
    // always emit the current version.
    static constexpr int kSchemaVersion = 3;

    Scene() = default;
    explicit Scene(std::string name) : m_Name(std::move(name)) {}

    // --- identity -----------------------------------------------------------
    const std::string& Name() const { return m_Name; }
    void SetName(std::string name) { m_Name = std::move(name); m_Dirty = true; }

    const std::filesystem::path& Path() const { return m_Path; }
    void SetPath(const std::filesystem::path& path) { m_Path = path; }

    // --- dirty tracking -----------------------------------------------------
    bool IsDirty() const { return m_Dirty; }
    void MarkDirty() { m_Dirty = true; }
    void ClearDirty() { m_Dirty = false; }

    // --- world access --------------------------------------------------------
    ecs::World& World() { return m_World; }
    const ecs::World& World() const { return m_World; }

    ecs::Entity CreateEntity(const std::string& name = "Entity");
    void DestroyEntity(ecs::Entity entity);          // recursive (children too)
    ecs::Entity DuplicateEntity(ecs::Entity entity); // copies one level (children too)
    void Clear();

    // --- hierarchy -----------------------------------------------------------
    void SetParent(ecs::Entity child, ecs::Entity newParent);  // NullEntity detaches
    bool IsDescendantOf(ecs::Entity entity, ecs::Entity potentialAncestor) const;
    glm::mat4 GetWorldMatrix(ecs::Entity entity) const;
    glm::mat4 GetLocalMatrix(const TransformComponent& t) const;
    std::vector<ecs::Entity> RootEntities() const;
    const std::vector<ecs::Entity>& ChildrenOf(ecs::Entity entity) const;

    // --- persistence ----------------------------------------------------------
    bool Save(const std::filesystem::path& output, std::string* error = nullptr);
    bool Save(std::string* error = nullptr);          // to m_Path
    bool Load(const std::filesystem::path& path, std::string* error = nullptr);

    // Whole-scene text round-trip (used for play-mode snapshot & tests).
    std::string SerializeToString() const;
    bool LoadFromString(const std::string& text, std::string* error = nullptr);

    // Find entities by predicate / name / UUID.
    std::optional<ecs::Entity> FindByName(const std::string& name) const;
    std::optional<ecs::Entity> FindByUUID(const UUID& id) const;

private:
    ecs::Entity CreateEntityInternal(const std::string& name, bool markDirty);
    void RebuildChildLists();

    ecs::World m_World;
    std::string m_Name = "Untitled";
    std::filesystem::path m_Path;
    bool m_Dirty = false;
};

} // namespace raven
