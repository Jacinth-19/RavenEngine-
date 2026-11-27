// RavenEditor — GizmoTransformCommand (§6.2 #5, coalescible).
//
// One drag = one undo entry: GizmoController snapshots t/r/s on ImGuizmo IsUsing
// begin, writes the transform directly during the drag, and pushes this command on
// drag end with the before-snapshot. Execute applies the after-state (idempotent —
// the drag already left the object there, so this is a no-op visually) and Uexecute
// restores the before-state. Implements the quiet-window coalescing contract so a
// programmatic per-frame push stream also collapses into a single entry.
//
// Header-only.
#pragma once

#include <string>
#include <vector>

#include "EditorCommandStack.h"
#include "raven/core/UUID.h"
#include "raven/core/Undo.h"
#include "raven/scene/Components.h"
#include "raven/scene/Scene.h"

namespace raven::editor {

struct TransformSnapshot {
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f};   // radians
    glm::vec3 scale{1.0f};
};

class GizmoTransformCommand final : public raven::ICommand, public ICoalescable {
public:
    GizmoTransformCommand(Scene& scene, const raven::UUID& entity,
                          const TransformSnapshot& before, const TransformSnapshot& after)
        : m_Scene(scene), m_Entity(entity), m_Before(before), m_After(after) {}

    void Execute() override { Apply(m_After); }
    void Uexecute() override { Apply(m_Before); }
    std::string Name() const override { return "Gizmo Transform"; }

    // Keep first-before, last-after.
    void CoalesceWith(const ICoalescable& newer) override {
        if (const auto* g = dynamic_cast<const GizmoTransformCommand*>(&newer))
            m_After = g->m_After;
    }

private:
    void Apply(const TransformSnapshot& s) {
        auto e = m_Scene.FindByUUID(m_Entity);
        if (!e || !m_Scene.World().HasComponent<TransformComponent>(*e)) return;
        auto& t = m_Scene.World().GetComponent<TransformComponent>(*e);
        t.position = s.position;
        t.rotation = s.rotation;
        t.scale = s.scale;
        m_Scene.MarkDirty();
    }

    Scene& m_Scene;
    raven::UUID m_Entity;
    TransformSnapshot m_Before;
    TransformSnapshot m_After;
};

} // namespace raven::editor
