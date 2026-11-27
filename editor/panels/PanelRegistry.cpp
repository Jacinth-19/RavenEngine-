#include "editor/panels/PanelRegistry.h"

#include <exception>
#include <string>

#include "imgui.h"

#include "editor/Icons.h"
#include "raven/core/Logger.h"
#include "raven/core/StackTrace.h"

namespace raven::editor {

void PanelRegistry::Register(std::unique_ptr<Panel> panel) {
    if (panel) m_Entries.push_back(PanelEntry{std::move(panel), 0, false, ""});
}

bool PanelRegistry::IsCrashed(const char* name) const {
    for (const PanelEntry& e : m_Entries)
        if (e.crashed && e.panel->Name() == std::string_view(name)) return true;
    return false;
}

void PanelRegistry::DrawAll() {
    for (PanelEntry& entry : m_Entries) {
        if (entry.crashed) { DrawCrashedPlaceholder(entry); continue; }
        if (!entry.panel->IsOpen()) continue;   // closed panels draw nothing
        DrawGuarded(entry);
    }
}

void PanelRegistry::DrawGuarded(PanelEntry& entry) {
    try {
        entry.panel->Draw();
        entry.crashStreak = 0;
    } catch (const std::exception& e) {
        ReportCrash(entry, e.what());
    } catch (...) {
        ReportCrash(entry, "unknown (non-std) exception");
    }
}

void PanelRegistry::ReportCrash(PanelEntry& entry, const char* what) {
    ++entry.crashStreak;
    entry.lastError = what ? what : "(no message)";
    RAVEN_LOG_ERROR("Editor: panel '%s' threw (%d/%d): %s", entry.panel->Name(), entry.crashStreak, kCrashStreakLimit, entry.lastError.c_str());
    StackTrace::LogStackTrace(1, 24);
    if (entry.crashStreak >= kCrashStreakLimit) {
        entry.crashed = true;
        RAVEN_LOG_ERROR("Editor: panel '%s' quarantined after %d consecutive failures — placeholder shown; other panels unaffected",
                        entry.panel->Name(), kCrashStreakLimit);
    }
}

void PanelRegistry::DrawCrashedPlaceholder(PanelEntry& entry) {
    bool open = entry.panel->IsOpen();
    ImGui::Begin(entry.panel->Name(), &open, ImGuiWindowFlags_NoCollapse);
    if (!open) {
        // User closed the crashed panel: quarantine lifts, streak resets.
        entry.panel->SetOpen(false);
        entry.crashed = false;
        entry.crashStreak = 0;
        ImGui::End();
        return;
    }
    ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), ICON_LOG_ERROR "  Panel crashed");
    ImGui::TextWrapped("The '%s' panel hit %d consecutive errors and was quarantined so the rest of the editor keeps working.",
                       entry.panel->Name(), kCrashStreakLimit);
    if (!entry.lastError.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("Last error");
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.60f, 0.45f, 1.0f));
        ImGui::TextWrapped("%s", entry.lastError.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Separator();
    if (ImGui::Button(ICON_REFRESH "  Reset panel", ImVec2(180.0f, 0.0f))) {
        entry.crashStreak = 0;
        entry.crashed = false;
        entry.lastError.clear();
        RAVEN_LOG_INFO("Editor: panel '%s' reset by user", entry.panel->Name());
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear the quarantine and draw the panel again on the next frame");
    ImGui::SameLine();
    if (ImGui::Button(ICON_DELETE "  Close panel", ImVec2(180.0f, 0.0f))) {
        entry.panel->SetOpen(false);
        entry.crashed = false;
        entry.crashStreak = 0;
    }
    ImGui::End();
}

} // namespace raven::editor
