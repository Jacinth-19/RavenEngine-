// RavenEditor — ComponentPropertyCommand<T, Member> (§6.2 #1).
//
// Template; stores the member value by pointer-to-member. Every inspector write
// routes through this (or a command derived from the same rule): capture the old
// value at construction, apply on Execute, restore on Uexecute. Entities are always
// resolved by UUID at execute/unexecute time — handles never cross frames, a dead
// entity makes the command a safe no-op.
//
// Header-only.
#pragma once

#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include "raven/core/UUID.h"
#include "raven/core/Undo.h"
#include "raven/scene/Scene.h"
#include "editor/commands/ICoalescable.h"

namespace raven::editor {

template <typename T, typename Member>
class ComponentPropertyCommand final : public raven::ICommand, public ICoalescable {
public:
    using ValueType = std::remove_reference_t<Member>;

    ComponentPropertyCommand(Scene& scene, const raven::UUID& entity, Member T::* member,
                             ValueType newValue, std::string name)
        : m_Scene(scene), m_Entity(entity), m_Member(member), m_New(std::move(newValue)),
          m_Name(std::move(name)) {
        if (auto e = m_Scene.FindByUUID(m_Entity))
            if (m_Scene.World().HasComponent<T>(*e))
                m_Old = m_Scene.World().GetComponent<T>(*e).*m_Member;
    }

    void Execute() override {
        auto e = m_Scene.FindByUUID(m_Entity);
        if (!e || !m_Scene.World().HasComponent<T>(*e)) return;
        m_Scene.World().GetComponent<T>(*e).*m_Member = m_New;
        m_Scene.MarkDirty();
    }

    void Uexecute() override {
        if (!m_Old) return;   // never had T / unknown before-state
        auto e = m_Scene.FindByUUID(m_Entity);
        if (!e || !m_Scene.World().HasComponent<T>(*e)) return;
        m_Scene.World().GetComponent<T>(*e).*m_Member = *m_Old;
        m_Scene.MarkDirty();
    }

    std::string Name() const override { return m_Name; }

    // Burst coalescing (inspector drags, debounced pushes): keep the FIRST before-state,
    // adopt the newest after-state. The undo entry then restores the pre-drag value once.
    void CoalesceWith(const ICoalescable& newerBase) override {
        if (const auto* newer = dynamic_cast<const ComponentPropertyCommand*>(&newerBase))
            m_New = newer->m_New;
    }

private:
    Scene& m_Scene;
    raven::UUID m_Entity;
    Member T::* m_Member;
    ValueType m_New;
    std::optional<ValueType> m_Old;   // captured at construction (pre-mutation value)
    std::string m_Name;
};

// Convenience factory — the inspector calls this from every Property() write site.
template <typename T, typename Member>
std::shared_ptr<ComponentPropertyCommand<T, Member>> MakePropertyCommand(Scene& scene, const raven::UUID& entity,
                                                                         Member T::* member,
                                                                         std::remove_reference_t<Member> value,
                                                                         std::string name) {
    return std::make_shared<ComponentPropertyCommand<T, Member>>(scene, entity, member, std::move(value), std::move(name));
}

} // namespace raven::editor
