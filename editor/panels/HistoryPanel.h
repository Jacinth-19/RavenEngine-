// RavenEditor — HistoryPanel (§6.2): the undo/redo timeline.
//
// Renders the command stack top-down: applied entries (newest first, with type icons,
// compounds expandable) above the greyed redo tail. Clicking an entry travels history
// to just after it (replaying undo/redo primitives — no special stack support needed).
// Footer carries Undo N · Redo M · Clear and the current undo/redo labels.
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "imgui.h"

#include "raven/core/Undo.h"

#include "editor/Icons.h"
#include "editor/commands/EditorCommandStack.h"
#include "editor/panels/Panel.h"

namespace raven::editor {

class HistoryPanel final : public Panel {
public:
    // openFlag: external visibility shared with menus/workspaces (like CallablePanel).
    // onTraveled: optional hook after a click-to-travel sequence (editor marks the scene dirty).
    explicit HistoryPanel(bool* openFlag = nullptr, std::function<void()> onTraveled = nullptr)
        : m_OpenFlag(openFlag), m_OnTraveled(std::move(onTraveled)) {}

    const char* Name() const override { return "History"; }
    bool IsOpen() const override { return m_OpenFlag ? *m_OpenFlag : true; }
    void SetOpen(bool open) override { if (m_OpenFlag) *m_OpenFlag = open; }

    void Draw() override {
        if (!ImGui::Begin("History", m_OpenFlag)) { ImGui::End(); return; }

        raven::CommandStack& stack = Commands().Raw();
        const auto& undoEntries = stack.UndoEntries();
        const auto& redoEntries = stack.RedoEntries();
        const size_t undoDepth = undoEntries.size();
        const size_t redoDepth = redoEntries.size();

        // Toolbar row — undo/redo buttons (disabled states) + entry counts.
        ImGui::BeginDisabled(undoDepth == 0);
        if (ImGui::Button(ICON_UNDO "##undoTop")) Travel([&stack] { stack.Undo(); });
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && undoDepth > 0) ImGui::SetTooltip("Undo: %s", stack.UndoName().c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(redoDepth == 0);
        if (ImGui::Button(ICON_REDO "##redoTop")) Travel([&stack] { stack.Redo(); });
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && redoDepth > 0) ImGui::SetTooltip("Redo: %s", stack.RedoName().c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%zu applied · %zu redoable", undoDepth, redoDepth);
        ImGui::Separator();

        ImGui::BeginChild("historyScroll", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 4.0f), 0);

        if (undoDepth == 0 && redoDepth == 0) {
            ImGui::TextDisabled("No history yet — every edit lands here.");
        }

        // Applied entries: newest on top. Clicking entry i rewinds to just after it.
        for (size_t i = undoDepth; i-- > 0;) {
            const auto& cmd = undoEntries[i];
            DrawEntryRow(*cmd, /*inRedoTail=*/false, [&stack, i] {
                const size_t appliedTarget = i + 1;   // entries 0..i stay applied
                while (stack.Depth() > appliedTarget) stack.Undo();
            });
        }
        // Greyed "now" divider separating applied from the redo tail.
        if (redoDepth > 0) ImGui::SeparatorText("now");
        // Redo tail: the next entry to redo sits directly below the divider.
        for (size_t j = redoDepth; j-- > 0;) {
            const auto& cmd = redoEntries[j];
            DrawEntryRow(*cmd, /*inRedoTail=*/true, [&stack, redoDepth, j] {
                const size_t count = redoDepth - j;   // re-apply from the back down to j
                for (size_t k = 0; k < count; ++k) stack.Redo();
            });
        }

        ImGui::EndChild();

        // Footer: counts + clear, and the labels the status line would show.
        ImGui::TextDisabled("Undo %zu · Redo %zu", undoDepth, redoDepth);
        if (!stack.UndoName().empty() || !stack.RedoName().empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            if (!stack.UndoName().empty()) { ImGui::SameLine(); ImGui::TextDisabled("Undo: %s", stack.UndoName().c_str()); }
            else if (!stack.RedoName().empty()) { ImGui::SameLine(); ImGui::TextDisabled("Redo: %s", stack.RedoName().c_str()); }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) Commands().Raw().Clear();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drop the whole history (cannot be undone)");
        ImGui::End();
    }

private:
    void Travel(const std::function<void()>& trip) {
        trip();
        if (m_OnTraveled) m_OnTraveled();
    }

    static const char* EntryIcon(const std::string& name, bool compound) {
        if (compound) return ICON_HISTORY_COMPOUND;
        const auto has = [&name](const char* needle) { return name.find(needle) != std::string::npos; };
        if (has("Create") || has("Add")) return ICON_ADD;
        if (has("Delete") || has("Remove") || has("Destroy")) return ICON_DELETE;
        if (has("Duplicate")) return ICON_COPY;
        if (has("Gizmo") || has("Transform")) return ICON_GIZMO_MOVE;
        if (has("Rename") || has("Name")) return ICON_RENAME;
        if (has("Visibility") || has("Toggle")) return ICON_ROW_EYE;
        if (has("Paste") || has("Copy")) return ICON_COPY;
        if (has("Reparent") || has("Parent") || has("Move")) return ICON_GIZMO_MOVE;
        if (has("Reorder")) return ICON_HISTORY_COMMAND;
        if (has("Material")) return ICON_ASSET_MATERIAL;
        if (has("Property") || has("Edit") || has("Scale") || has("Rotation") || has("Position")) return ICON_LIST;
        return ICON_HISTORY_COMMAND;
    }

    void DrawEntryRow(const raven::ICommand& cmd, bool inRedoTail,
                      const std::function<void()>& travel) {
        const bool compound = cmd.IsCompound();
        const std::string name = cmd.Name();
        const char* icon = EntryIcon(name, compound);

        ImGui::PushID(static_cast<const void*>(&cmd));
        ImGui::PushStyleColor(ImGuiCol_Text,
                              inRedoTail ? ImVec4(0.55f, 0.55f, 0.58f, 1.0f) : ImVec4(0.92f, 0.92f, 0.94f, 1.0f));
        const std::string rowLabel = std::string(icon) + "  " + name;
        // Compound rows leave room on the right for the expand/collapse button.
        if (ImGui::Selectable(rowLabel.c_str(), false, ImGuiSelectableFlags_None, ImVec2(compound ? -30.0f : 0.0f, 0.0f)))
            Travel(travel);
        ImGui::PopStyleColor();

        if (compound) {
            const auto* compoundCmd = dynamic_cast<const raven::CompoundCommand*>(&cmd);
            if (compoundCmd) {
                ImGui::SameLine();
                const bool expanded = m_Expanded.find(&cmd) != m_Expanded.end();
                if (ImGui::SmallButton(expanded ? "-" : "+")) {
                    if (expanded) m_Expanded.erase(&cmd);
                    else m_Expanded.insert(&cmd);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%zu command(s) in this entry", compoundCmd->ChildCount());
                if (m_Expanded.find(&cmd) != m_Expanded.end()) {
                    ImGui::Indent(18.0f);
                    size_t childIndex = 0;
                    for (const auto& child : compoundCmd->Children()) {
                        ImGui::PushID(static_cast<int>(childIndex++));
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.70f, 0.74f, 1.0f));
                        ImGui::TextUnformatted((std::string("   ") + EntryIcon(child->Name(), child->IsCompound()) + "  " + child->Name()).c_str());
                        ImGui::PopStyleColor();
                        ImGui::PopID();
                    }
                    ImGui::Unindent(18.0f);
                }
            }
        }
        ImGui::PopID();
    }

    bool* m_OpenFlag = nullptr;
    std::function<void()> m_OnTraveled;
    std::unordered_set<const void*> m_Expanded;   // compound identity for expand/collapse
};

} // namespace raven::editor
