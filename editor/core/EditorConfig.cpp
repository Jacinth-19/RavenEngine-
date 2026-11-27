#include "editor/core/EditorConfig.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "raven/core/Logger.h"

namespace raven::editor {

std::filesystem::path ConfigDir() {
    namespace fs = std::filesystem;
    fs::path dir;
#if defined(_WIN32)
    if (const char* appdata = std::getenv("APPDATA")) dir = fs::path(appdata) / "RavenEditor";
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME")) dir = fs::path(home) / "Library" / "Application Support" / "RavenEditor";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) dir = fs::path(xdg) / "raven-editor";
    else if (const char* home = std::getenv("HOME")) dir = fs::path(home) / ".config" / "raven-editor";
#endif
    if (dir.empty()) dir = fs::temp_directory_path() / "raven-editor-config";

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        RAVEN_LOG_ERROR("Editor: cannot create config dir %s: %s", dir.string().c_str(), ec.message().c_str());
        return fs::temp_directory_path() / "raven-editor-config";
    }
    return dir;
}

static std::filesystem::path ConfigFile() { return ConfigDir() / "editor.json"; }

EditorConfig LoadEditorConfig() {
    EditorConfig cfg;
    std::ifstream in(ConfigFile());
    if (!in) return cfg;
    try {
        nlohmann::json j;
        in >> j;
        cfg.theme = j.value("theme", cfg.theme);
        cfg.autosaveSeconds = j.value("autosaveSeconds", cfg.autosaveSeconds);
        cfg.assetRescanSeconds = j.value("assetRescanSeconds", cfg.assetRescanSeconds);
        if (j.contains("recentProjects") && j["recentProjects"].is_array()) {
            for (const auto& entry : j["recentProjects"]) {
                RecentProject p;
                p.name = entry.value("name", std::string());
                p.path = entry.value("path", std::string());
                if (!p.path.empty()) cfg.recentProjects.push_back(std::move(p));
            }
        }
    } catch (const std::exception& e) {
        RAVEN_LOG_WARN("Editor: ignoring corrupt %s (%s) — using defaults", ConfigFile().string().c_str(), e.what());
    }
    return cfg;
}

void SaveEditorConfig(const EditorConfig& cfg) {
    nlohmann::json j;
    j["theme"] = cfg.theme;
    j["autosaveSeconds"] = cfg.autosaveSeconds;
    j["assetRescanSeconds"] = cfg.assetRescanSeconds;
    nlohmann::json recent = nlohmann::json::array();
    for (const RecentProject& p : cfg.recentProjects) recent.push_back({{"name", p.name}, {"path", p.path}});
    j["recentProjects"] = std::move(recent);

    std::ofstream out(ConfigFile(), std::ios::trunc);
    if (!out) {
        RAVEN_LOG_ERROR("Editor: cannot write %s", ConfigFile().string().c_str());
        return;
    }
    out << j.dump(2);
}

void AddRecentProject(EditorConfig& cfg, const std::string& name, const std::string& path) {
    cfg.recentProjects.erase(std::remove_if(cfg.recentProjects.begin(), cfg.recentProjects.end(),
                                            [&path](const RecentProject& p) { return p.path == path; }),
                             cfg.recentProjects.end());
    cfg.recentProjects.insert(cfg.recentProjects.begin(), RecentProject{name, path});
    if (cfg.recentProjects.size() > 10) cfg.recentProjects.resize(10);
    SaveEditorConfig(cfg);
}

void SaveLastSessionLayout(const std::string& iniText) {
    std::ofstream out(ConfigDir() / "last_session.ini", std::ios::trunc);
    if (!out) RAVEN_LOG_WARN("Editor: could not persist last_session.ini");
    else out << iniText;
}

std::string LoadLastSessionLayout() {
    std::ifstream in(ConfigDir() / "last_session.ini");
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void SaveLastSessionVisibility(const nlohmann::json& panels) {
    std::ofstream out(ConfigDir() / "last_session.workspace.json", std::ios::trunc);
    if (!out) RAVEN_LOG_WARN("Editor: could not persist last_session.workspace.json");
    else out << panels.dump(2);
}

nlohmann::json LoadLastSessionVisibility() {
    std::ifstream in(ConfigDir() / "last_session.workspace.json");
    if (!in) return nlohmann::json::object();
    try {
        nlohmann::json j;
        in >> j;
        return j;
    } catch (const std::exception&) {
        return nlohmann::json::object();
    }
}

} // namespace raven::editor
