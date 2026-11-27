// RavenEditor — ComponentJsonCommand (§6.2 #3/#4 backbone, JSON fragment flavor).
//
// A typed command cannot be built from a runtime clipboard JSON blob, so the
// inspector overflow menu (Copy / Paste Over / Reset / Remove-Add) routes through
// this JSON-snapshot command instead: it stores the component JSON before and
// after. Execute applies the "after" fragment; Uexecute restores the "before"
// fragment (or removes the component entirely when there was none). Works for
// every serialized component type without knowing the type at compile time.
//
// Header-only.
#pragma once

#include <optional>
#include <string>

#include "raven/core/UUID.h"
#include "raven/core/Undo.h"
#include "raven/scene/Scene.h"

namespace raven::editor {

class ComponentJsonCommand final : public raven::ICommand {
public:
    // after: the component fragment to apply (single-component object).
    // name:  history label, e.g. "Paste Component" / "Reset Component".
    ComponentJsonCommand(Scene& scene, const raven::UUID& entity, std::string key,
                         nlohmann::json after, std::string name)
        : m_Scene(scene), m_Entity(entity), m_Key(std::move(key)), m_After(std::move(after)),
          m_Name(std::move(name)) {
        if (auto e = m_Scene.FindByUUID(m_Entity)) {
            m_Before = m_Scene.SerializeComponent(*e, m_Key);
            m_HadBefore = !m_Before.is_null() && !m_Before.empty();
        }
    }

    void Execute() override {
        auto e = m_Scene.FindByUUID(m_Entity);
        if (!e) return;
        m_Scene.ApplyComponent(*e, m_Key, m_After);
    }

    void Uexecute() override {
        auto e = m_Scene.FindByUUID(m_Entity);
        if (!e) return;
        if (m_HadBefore) m_Scene.ApplyComponent(*e, m_Key, m_Before);
        else m_Scene.RemoveComponentKey(*e, m_Key);   // paste onto a missing component — undo removes
    }

    std::string Name() const override { return m_Name; }

private:
    Scene& m_Scene;
    raven::UUID m_Entity;
    std::string m_Key;
    nlohmann::json m_After;
    nlohmann::json m_Before;      // empty object == component was absent at construction
    bool m_HadBefore = false;
    std::string m_Name;
};

} // namespace raven::editor
