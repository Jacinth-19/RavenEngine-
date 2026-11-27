// RavenEditor — named workspaces: ImGui ini + panel-visibility sidecar.
//
// A workspace is two files in <config>/workspaces/<name>.{ini,workspace.json}:
//   <name>.ini              — ImGui docking/window ini (SaveIniSettingsToMemory)
//   <name>.workspace.json   — { "version": 1, "panels": { "<window>": bool, ... } }
//
// Builtin "Default" always exists (it is just "no ini + default panels") and cannot be
// overwritten or deleted. "last_session" (in the config dir root) is the crash/quit
// fallback: it captures the exact layout+visibility on graceful shutdown and is restored
// on startup so an edited Default layout survives a restart without an explicit Save As.
//
// SaveLocalIni=false style usage: io.IniFilename must be nullptr — named workspaces
// own layout persistence; ImGui's automatic ini would fight them.
#pragma once

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace raven::editor {

class WorkspaceManager {
public:
    static constexpr const char* kBuiltinDefault = "Default";

    // All workspace names: builtin Default first, then user workspaces (sorted).
    std::vector<std::string> List() const;
    bool Exists(const std::string& name) const;

    // Currently loaded/named workspace; "" until the user saves/loads one.
    const std::string& Current() const { return m_Current; }

    // Serialize ImGui state and write <name>.{ini,workspace.json}.
    // Refuses to overwrite the builtin Default. Returns false + error on failure.
    bool SaveAs(const std::string& name, const std::map<std::string, bool>& panelVisibility, std::string* error);
    bool SaveCurrent(const std::map<std::string, bool>& panelVisibility, std::string* error);  // no-op (true) when Current() is empty/builtin

    // Read a workspace. Returns ImGui ini text + parsed panel visibility; the caller
    // (EditorApp) feeds them to ImGui::LoadIniSettingsFromMemory / applies visibility.
    // On success the manager's current-workspace marker moves to `name`.
    struct LoadedWorkspace {
        bool ok = false;
        std::string iniText;
        std::map<std::string, bool> panelVisibility;
    };
    LoadedWorkspace Load(const std::string& name, std::string* error);

    // Back to unsaved/default (used by Reset Layout).
    void SetCurrent(const std::string& name) { m_Current = name; }

    // Builtin Default: the built-in dock layout + default panel visibility.
    static std::map<std::string, bool> DefaultVisibility();

    // Delete a user workspace (builtin refused).
    bool Delete(const std::string& name, std::string* error);

private:
    static std::string SanitizeName(const std::string& name);  // path-safe file stem
    std::string m_Current;
};

} // namespace raven::editor
