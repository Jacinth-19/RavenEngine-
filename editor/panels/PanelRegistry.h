// RavenEditor — panel registry + error boundary.
//
// Every docked panel is registered here. DrawAll() runs each open panel inside
// try/catch (std::exception and ...), logs with a stack trace, counts consecutive
// failures; at 3 strikes the panel is quarantined ("crashed") and a placeholder window
// with the same ImGui id takes its place — siblings keep running, docking layout is
// preserved. Reset/Close buttons clear the quarantine.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "editor/panels/Panel.h"

namespace raven::editor {

struct PanelEntry {
    std::unique_ptr<Panel> panel;
    int crashStreak = 0;
    bool crashed = false;
    std::string lastError;      // most recent exception text (placeholder shows it)
};

class PanelRegistry {
public:
    void Register(std::unique_ptr<Panel> panel);

    // Draw every open, non-quarantined panel; draw placeholders for quarantined ones.
    // Must be called between ImGui::NewFrame() and ImGui::Render().
    void DrawAll();

    // Snapshot access for settings/diagnostics (name → crashed).
    bool IsCrashed(const char* name) const;

private:
    void DrawGuarded(PanelEntry& entry);
    void DrawCrashedPlaceholder(PanelEntry& entry);
    void ReportCrash(PanelEntry& entry, const char* what);

    std::vector<PanelEntry> m_Entries;
    static constexpr int kCrashStreakLimit = 3;
};

} // namespace raven::editor
