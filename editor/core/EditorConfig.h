// RavenEditor — editor-global configuration persisted as JSON.
//
// Two layers live here:
//   ConfigDir()  — per-user config directory (portable across Win/macOS/Linux)
//   EditorConfig — editor.json inside it: theme, autosave interval, recent projects
//
// Layout is NOT here — that is the workspaces subsystem (Workspaces.h).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace raven::editor {

// Per-user config directory:
//   Windows: %APPDATA%/RavenEditor   macOS: ~/Library/Application Support/RavenEditor
//   else:    $XDG_CONFIG_HOME/raven-editor or ~/.config/raven-editor
// Creates the directory (and parents) on first use; never throws.
std::filesystem::path ConfigDir();

struct RecentProject {
    std::string name;
    std::string path;
};

struct EditorConfig {
    std::string theme = "dark";            // "dark" | "light" — StylePresets maps to the JSON files
    int autosaveSeconds = 60;              // 0 disables; writes to <project>/.raven/autosave/
    float assetRescanSeconds = 3.0f;       // Assets panel auto-rescan cadence
    std::vector<RecentProject> recentProjects;  // MRU order, capped at 10
};

EditorConfig LoadEditorConfig();                      // missing/corrupt file → defaults
void SaveEditorConfig(const EditorConfig& cfg);       // best-effort; logs on failure
void AddRecentProject(EditorConfig& cfg, const std::string& name, const std::string& path);  // MRU, moves to front, cap 10
void SaveLastSessionLayout(const std::string& iniText);   // <config>/last_session.ini (startup restore)
std::string LoadLastSessionLayout();                       // empty when absent
void SaveLastSessionVisibility(const nlohmann::json& panels);  // <config>/last_session.workspace.json
nlohmann::json LoadLastSessionVisibility();             // empty object when absent

} // namespace raven::editor
