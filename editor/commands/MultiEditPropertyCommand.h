// RavenEditor — MultiEditPropertyCommand (§6.3, MultiEdit fan-out).
//
// One edit applied to every selected entity holding component T. The before-state
// of every target is captured at construction; Execute applies the shared after
// value; Uexecute restores each target's own before value. Coalescable so a drag
// gesture across N entities collapses into a single history entry ("Multi Edit (3)").
//
// Header-only.
#pragma once

#include <cstdio>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "raven/core/UUID.h"
#include "raven/core/Undo.h"
#include "raven/scene/Scene.h"

#include "editor/commands/ICoalescable.h"

namespace raven::editor {

template <typename T, typename Member>
class MultiEditPropertyCommand final : public raven::ICommand, public ICoalescable {
public:
    using ValueType = std::remove_reference_t<Member>;

    MultiEditPropertyCommand(Scene& scene, std::vector<raven::UUID> targets, Member T::* member,
                             ValueType after, std::string name)
        : m_Scene(scene), m_Targets(std::move(targets)), m_Member(member),
          m_After(std::move(after)), m_Name(std::move(name)) {
        for (const raven::UUID& id : m_Targets) {
            auto e = m_Scene.FindByUUID(id);
            if (!e || !m_Scene.World().HasComponent<T>(*e)) continue;
            m_Before.emplace_back(id, m_Scene.World().GetComponent<T>(*e).*m_Member);
        }
    }

    void Execute() override {
        for (const auto& [id, before] : m_Before) {
            (void)before;
            Apply(id, m_After);
        }
    }

    void Uexecute() override {
        for (const auto& [id, before] : m_Before) Apply(id, before);
    }

    // Burst coalescing: keep every target's first before; adopt the newest after.
    void CoalesceWith(const ICoalescable& newerBase) override {
        if (const auto* newer = dynamic_cast<const MultiEditPropertyCommand*>(&newerBase))
            m_After = newer->m_After;
    }

    std::string Name() const override {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s (%zu)", m_Name.c_str(), m_Before.size());
        return buf;
    }

    size_t TargetCount() const { return m_Before.size(); }

private:
    void Apply(const raven::UUID& id, const ValueType& value) {
        auto e = m_Scene.FindByUUID(id);
        if (!e || !m_Scene.World().HasComponent<T>(*e)) return;
        m_Scene.World().GetComponent<T>(*e).*m_Member = value;
        m_Scene.MarkDirty();
    }

    Scene& m_Scene;
    std::vector<raven::UUID> m_Targets;
    Member T::* m_Member;
    ValueType m_After;
    std::vector<std::pair<raven::UUID, ValueType>> m_Before;   // per-target before-states (only targets with T)
    std::string m_Name;
};

// Factory — the inspector calls this when >=2 entities are selected and all hold T.
template <typename T, typename Member>
std::shared_ptr<MultiEditPropertyCommand<T, Member>> MakeMultiEditCommand(
    Scene& scene, std::vector<raven::UUID> targets, Member T::* member,
    std::remove_reference_t<Member> value, const std::string& name) {
    return std::make_shared<MultiEditPropertyCommand<T, Member>>(scene, std::move(targets), member, std::move(value), name);
}

} // namespace raven::editor
