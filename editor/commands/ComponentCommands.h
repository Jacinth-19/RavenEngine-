// RavenEditor — component add/remove/paste commands (§6.2 #2–4).
//
// T is the component POD (all scene components are trivially copyable). Entities are
// resolved by UUID at execute/unexecute time; a dead entity makes the command a no-op.
//
// Header-only.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include "raven/core/Logger.h"
#include "raven/core/UUID.h"
#include "raven/core/Undo.h"
#include "raven/scene/Scene.h"

namespace raven::editor {

// §6.2 #2 ComponentAddCommand<T> — attaches a value (default T{} unless provided).
// Undo removes the component again; if it already existed the command is a no-op and
// does not clobber it.
template <typename T>
class ComponentAddCommand final : public raven::ICommand {
public:
    ComponentAddCommand(Scene& scene, const raven::UUID& entity, T value = T{}, std::string name = {})
        : m_Scene(scene), m_Entity(entity), m_Value(std::move(value)),
          m_Name(name.empty() ? std::string("Add ") + "Component" : std::move(name)) {}

    void Execute() override {
        auto e = m_Scene.FindByUUID(m_Entity);
        if (!e) return;
        ecs::World& world = m_Scene.World();
        if (world.HasComponent<T>(*e)) { m_Added = false; return; }   // already present — nothing to do
        world.AddComponent<T>(*e, m_Value);
        m_Added = true;
        m_Scene.MarkDirty();
    }

    void Uexecute() override {
        if (!m_Added) return;
        auto e = m_Scene.FindByUUID(m_Entity);
        if (!e) return;
        m_Scene.World().RemoveComponent<T>(*e);
        m_Added = false;
        m_Scene.MarkDirty();
    }

    std::string Name() const override { return m_Name; }

private:
    Scene& m_Scene;
    raven::UUID m_Entity;
    T m_Value;
    std::string m_Name;
    bool m_Added = false;
};

// §6.2 #3 ComponentRemoveCommand<T> — stores the removed component POD.
// Undo re-attaches it verbatim. Removing an absent component is a no-op.
template <typename T>
class ComponentRemoveCommand final : public raven::ICommand {
public:
    ComponentRemoveCommand(Scene& scene, const raven::UUID& entity, std::string name = {})
        : m_Scene(scene), m_Entity(entity),
          m_Name(name.empty() ? std::string("Remove ") + "Component" : std::move(name)) {}

    void Execute() override {
        auto e = m_Scene.FindByUUID(m_Entity);
        if (!e) return;
        ecs::World& world = m_Scene.World();
        if (!world.HasComponent<T>(*e)) { m_Removed.reset(); return; }
        m_Removed = world.GetComponent<T>(*e);
        world.RemoveComponent<T>(*e);
        m_Scene.MarkDirty();
    }

    void Uexecute() override {
        if (!m_Removed) return;
        auto e = m_Scene.FindByUUID(m_Entity);
        if (!e) return;
        m_Scene.World().AddComponent<T>(*e, *m_Removed);
        m_Scene.MarkDirty();
    }

    std::string Name() const override { return m_Name; }

private:
    Scene& m_Scene;
    raven::UUID m_Entity;
    std::string m_Name;
    std::optional<T> m_Removed;
};

// §6.2 #4 paste-half of copy/paste — T delivered from the editor clipboard.
// Execute overwrites any existing T (stashing the previous value); undo restores it.
template <typename T>
class ComponentPasteCommand final : public raven::ICommand {
public:
    ComponentPasteCommand(Scene& scene, const raven::UUID& entity, T payload, std::string name = {})
        : m_Scene(scene), m_Entity(entity), m_Payload(std::move(payload)),
          m_Name(name.empty() ? std::string("Paste ") + "Component" : std::move(name)) {}

    void Execute() override {
        auto e = m_Scene.FindByUUID(m_Entity);
        if (!e) return;
        ecs::World& world = m_Scene.World();
        if (world.HasComponent<T>(*e)) m_Prev = world.GetComponent<T>(*e);
        else m_Prev.reset();
        world.AddComponent<T>(*e, m_Payload);
        m_Scene.MarkDirty();
    }

    void Uexecute() override {
        auto e = m_Scene.FindByUUID(m_Entity);
        if (!e) return;
        ecs::World& world = m_Scene.World();
        if (m_Prev) world.AddComponent<T>(*e, *m_Prev);
        else world.RemoveComponent<T>(*e);
        m_Scene.MarkDirty();
    }

    std::string Name() const override { return m_Name; }

private:
    Scene& m_Scene;
    raven::UUID m_Entity;
    T m_Payload;
    std::string m_Name;
    std::optional<T> m_Prev;
};

} // namespace raven::editor
