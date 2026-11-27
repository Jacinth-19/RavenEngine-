#include "editor/core/Workspaces.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include "imgui.h"

#include "editor/core/EditorConfig.h"
#include "raven/core/Logger.h"

namespace raven::editor {

static std::filesystem::path WorkspacesDir() {
    namespace fs = std::filesystem;
    fs::path dir = ConfigDir() / "workspaces";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

std::vector<std::string> WorkspaceManager::List() const {
    namespace fs = std::filesystem;
    std::vector<std::string> names{kBuiltinDefault};
    std::error_code ec;
    const fs::path dir = WorkspacesDir();
    if (fs::is_directory(dir, ec)) {
        std::vector<std::string> user;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (entry.path().extension() != ".ini") continue;
            user.push_back(entry.path().stem().string());
        }
        std::sort(user.begin(), user.end());
        for (auto& n : user) names.push_back(std::move(n));
    }
    return names;
}

bool WorkspaceManager::Exists(const std::string& name) const {
    if (name == kBuiltinDefault) return true;
    return std::filesystem::exists(WorkspacesDir() / (SanitizeName(name) + ".ini"));
}

std::string WorkspaceManager::SanitizeName(const std::string& name) {
    // Keep the file stem path-safe and the UI name recognisable.
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (out.size() >= 64) break;
        if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_' || c == '.') out.push_back(c);
        else out.push_back('_');
    }
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::map<std::string, bool> WorkspaceManager::DefaultVisibility() {
    return {{"Hierarchy", true},  {"Inspector", true}, {"Assets", true},
            {"Profiler", true},  {"Console", true},   {"Viewport Tools", true}};
}

bool WorkspaceManager::SaveAs(const std::string& name, const std::map<std::string, bool>& panelVisibility, std::string* error) {
    const std::string stem = SanitizeName(name);
    if (stem.empty()) {
        if (error) *error = "workspace name is empty or invalid";
        return false;
    }
    if (stem == kBuiltinDefault) {
        if (error) *error = "the builtin 'Default' workspace cannot be overwritten";
        return false;
    }

    // ImGui ini — captured now; caller must ensure we're not inside a Begin/End imbalance.
    size_t iniSize = 0;
    const char* iniData = ImGui::SaveIniSettingsToMemory(&iniSize);

    nlohmann::json sidecar;
    sidecar["version"] = 1;
    nlohmann::json panels = nlohmann::json::object();
    for (const auto& [k, v] : panelVisibility) panels[k] = v;
    sidecar["panels"] = std::move(panels);

    const std::filesystem::path iniPath = WorkspacesDir() / (stem + ".ini");
    const std::filesystem::path jsonPath = WorkspacesDir() / (stem + ".workspace.json");
    {
        std::ofstream out(iniPath, std::ios::trunc);
        if (!out) { if (error) *error = "cannot write " + iniPath.string(); return false; }
        out.write(iniData, static_cast<std::streamsize>(iniSize));
    }
    {
        std::ofstream out(jsonPath, std::ios::trunc);
        if (!out) { if (error) *error = "cannot write " + jsonPath.string(); return false; }
        out << sidecar.dump(2);
    }
    m_Current = stem;
    return true;
}

bool WorkspaceManager::SaveCurrent(const std::map<std::string, bool>& panelVisibility, std::string* error) {
    if (m_Current.empty() || m_Current == kBuiltinDefault) return true;  // builtin: nothing to persist
    return SaveAs(m_Current, panelVisibility, error);
}

WorkspaceManager::LoadedWorkspace WorkspaceManager::Load(const std::string& name, std::string* error) {
    LoadedWorkspace result;

    if (name == kBuiltinDefault) {
        result.ok = true;
        result.iniText.clear();  // empty ini = ImGui falls back to default settings + our DockBuilder layout
        result.panelVisibility = DefaultVisibility();
        m_Current = kBuiltinDefault;
        return result;
    }

    const std::string stem = SanitizeName(name);
    const std::filesystem::path iniPath = WorkspacesDir() / (stem + ".ini");
    const std::filesystem::path jsonPath = WorkspacesDir() / (stem + ".workspace.json");

    {
        std::ifstream in(iniPath, std::ios::binary);
        if (!in) {
            if (error) *error = "workspace not found: " + name;
            return result;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        result.iniText = ss.str();
    }

    std::ifstream sidecarIn(jsonPath);
    if (sidecarIn) {
        try {
            nlohmann::json j;
            sidecarIn >> j;
            if (j.contains("panels") && j["panels"].is_object()) {
                for (const auto& [k, v] : j["panels"].items()) {
                    if (v.is_boolean()) result.panelVisibility[k] = v.get<bool>();
                }
            }
        } catch (const std::exception& e) {
            RAVEN_LOG_WARN("Editor: workspace '%s' sidecar unreadable (%s) — panel visibility defaults kept", name.c_str(), e.what());
        }
    }
    result.ok = true;
    m_Current = stem;
    return result;
}

bool WorkspaceManager::Delete(const std::string& name, std::string* error) {
    const std::string stem = SanitizeName(name);
    if (stem.empty() || stem == kBuiltinDefault) {
        if (error) *error = "the builtin 'Default' workspace cannot be deleted";
        return false;
    }
    std::error_code ec;
    std::filesystem::remove(WorkspacesDir() / (stem + ".ini"), ec);
    std::filesystem::remove(WorkspacesDir() / (stem + ".workspace.json"), ec);
    if (m_Current == stem) m_Current.clear();
    return true;
}

} // namespace raven::editor
