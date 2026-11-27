// RavenEditor — AssetMetaSnapshotCommand (§6.2, sibling of MaterialSnapshotCommand).
//
// Captures the `importSettings` JSON object of an asset's .meta sidecar
// before/after a burst of edits. The texture import-settings panel uses this so
// undo restores exactly the import settings (unrelated sidecar members, e.g.
// uuid / importVersion / textureRemap, are never touched — the applier replaces
// only the importSettings object). Without an applier this is a bookkeeping
// entry, same contract as MaterialSnapshotCommand.
//
// PushCoalesced-compatible (ICoalescable): first before-state wins, last
// after-state wins.
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

class AssetMetaSnapshotCommand final : public raven::ICommand, public ICoalescable {
public:
    AssetMetaSnapshotCommand(raven::UUID asset, std::string beforeJson, std::string afterJson,
                             std::function<void(const std::string& snapshot)> applier = {})
        : m_Asset(asset), m_Before(std::move(beforeJson)), m_After(std::move(afterJson)), m_Applier(std::move(applier)) {}

    void Execute() override { if (m_Applier) m_Applier(m_After); }
    void Uexecute() override { if (m_Applier) m_Applier(m_Before); }
    std::string Name() const override { return "Import Settings Snapshot"; }

    void CoalesceWith(const ICoalescable& newer) override {
        if (const auto* m = dynamic_cast<const AssetMetaSnapshotCommand*>(&newer)) {
            m_After = m->m_After;
            if (!m_Applier && m->m_Applier) m_Applier = m->m_Applier;
        }
    }

    const raven::UUID& Asset() const { return m_Asset; }

private:
    raven::UUID m_Asset;
    std::string m_Before;
    std::string m_After;
    std::function<void(const std::string& snapshot)> m_Applier;
};

} // namespace raven::editor
