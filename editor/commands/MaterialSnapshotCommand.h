// RavenEditor — MaterialSnapshotCommand (§6.2 #13, stubbed).
//
// Captures a JSON snapshot of a material asset before/after a burst of edits. The
// material system lands in Phase 6; until then this is the debounce contract the
// command stack is tested against: `applier` writes the snapshot back when provided
// (undo/redo); without one it is a bookkeeping entry.
//
// Debounce semantics (EditorCommandStack::PushDebounced): pushes with the same tag
// inside the quiet window merge — first before-state wins, last after-state wins.
//
// Header-only.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "EditorCommandStack.h"
#include "raven/core/UUID.h"
#include "raven/core/Undo.h"

namespace raven::editor {

class MaterialSnapshotCommand final : public raven::ICommand, public ICoalescable {
public:
    MaterialSnapshotCommand(raven::UUID asset, std::string beforeJson, std::string afterJson,
                            std::function<void(const std::string& snapshot)> applier = {})
        : m_Asset(asset), m_Before(std::move(beforeJson)), m_After(std::move(afterJson)), m_Applier(std::move(applier)) {}

    void Execute() override { if (m_Applier) m_Applier(m_After); }
    void Uexecute() override { if (m_Applier) m_Applier(m_Before); }
    std::string Name() const override { return "Material Snapshot"; }

    // First before-state wins; last after-state wins; applier is identical either way.
    void CoalesceWith(const ICoalescable& newer) override {
        if (const auto* m = dynamic_cast<const MaterialSnapshotCommand*>(&newer)) {
            m_After = m->m_After;
            if (!m_Applier && m->m_Applier) m_Applier = m->m_Applier;
        }
    }

    const raven::UUID& Asset() const { return m_Asset; }

private:
    raven::UUID m_Asset;
    std::string m_Before;
    std::string m_After;
    std::function<void(const std::string&)> m_Applier;
};

} // namespace raven::editor
