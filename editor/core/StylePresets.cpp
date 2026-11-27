#include "editor/core/StylePresets.h"

#include <fstream>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "imgui.h"

#include "raven/core/Logger.h"

namespace raven::editor {

namespace {

const char* StyleDir() { return RAVEN_EDITOR_STYLE_DIR; }

std::string StyleFileFor(const std::string& themeId) {
    if (themeId == "light") return std::string(StyleDir()) + "/DefaultLight.json";
    return std::string(StyleDir()) + "/DefaultDark.json";
}

struct ColEntry {
    ImGuiCol col;
    const char* jsonName;
};

// The supported override surface — extend as needed; unknown keys warn and are ignored.
const ColEntry kColorTable[] = {
    {ImGuiCol_Text, "Text"}, {ImGuiCol_TextDisabled, "TextDisabled"},
    {ImGuiCol_WindowBg, "WindowBg"}, {ImGuiCol_ChildBg, "ChildBg"}, {ImGuiCol_PopupBg, "PopupBg"},
    {ImGuiCol_Border, "Border"},
    {ImGuiCol_FrameBg, "FrameBg"}, {ImGuiCol_FrameBgHovered, "FrameBgHovered"}, {ImGuiCol_FrameBgActive, "FrameBgActive"},
    {ImGuiCol_TitleBg, "TitleBg"}, {ImGuiCol_TitleBgActive, "TitleBgActive"}, {ImGuiCol_MenuBarBg, "MenuBarBg"},
    {ImGuiCol_ScrollbarBg, "ScrollbarBg"}, {ImGuiCol_ScrollbarGrab, "ScrollbarGrab"},
    {ImGuiCol_ScrollbarGrabHovered, "ScrollbarGrabHovered"}, {ImGuiCol_ScrollbarGrabActive, "ScrollbarGrabActive"},
    {ImGuiCol_CheckMark, "CheckMark"}, {ImGuiCol_SliderGrab, "SliderGrab"}, {ImGuiCol_SliderGrabActive, "SliderGrabActive"},
    {ImGuiCol_Button, "Button"}, {ImGuiCol_ButtonHovered, "ButtonHovered"}, {ImGuiCol_ButtonActive, "ButtonActive"},
    {ImGuiCol_Header, "Header"}, {ImGuiCol_HeaderHovered, "HeaderHovered"}, {ImGuiCol_HeaderActive, "HeaderActive"},
    {ImGuiCol_Separator, "Separator"}, {ImGuiCol_SeparatorHovered, "SeparatorHovered"}, {ImGuiCol_SeparatorActive, "SeparatorActive"},
    {ImGuiCol_Tab, "Tab"}, {ImGuiCol_TabHovered, "TabHovered"}, {ImGuiCol_TabSelected, "TabSelected"},
    {ImGuiCol_TabDimmed, "TabDimmed"}, {ImGuiCol_TabDimmedSelected, "TabDimmedSelected"},
    {ImGuiCol_PlotLines, "PlotLines"},
    {ImGuiCol_TableHeaderBg, "TableHeaderBg"}, {ImGuiCol_TableBorderStrong, "TableBorderStrong"}, {ImGuiCol_TableBorderLight, "TableBorderLight"},
    {ImGuiCol_TableRowBg, "TableRowBg"}, {ImGuiCol_TableRowBgAlt, "TableRowBgAlt"},
    {ImGuiCol_TextSelectedBg, "TextSelectedBg"}, {ImGuiCol_DragDropTarget, "DragDropTarget"}, {ImGuiCol_NavHighlight, "NavHighlight"},
};

bool ReadFloat4(const nlohmann::json& j, float out[4]) {
    if (!j.is_array() || j.size() != 4) return false;
    for (int i = 0; i < 4; ++i) {
        if (!j[i].is_number()) return false;
        out[i] = j[i].get<float>();
    }
    return true;
}

} // namespace

void ApplyTheme(const std::string& themeId) {
    ApplyThemeFromFile(themeId);
}

void ApplyThemeFromFile(const std::string& themeId) {
    ImGuiStyle& style = ImGui::GetStyle();
    const bool light = (themeId == "light");
    if (light) ImGui::StyleColorsLight(); else ImGui::StyleColorsDark();

    const std::string file = StyleFileFor(themeId);
    std::ifstream in(file);
    if (!in) {
        RAVEN_LOG_WARN("Editor: style file %s missing — using built-in %s colors", file.c_str(), light ? "light" : "dark");
        return;
    }

    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        RAVEN_LOG_WARN("Editor: style file %s unreadable (%s) — using built-in %s colors", file.c_str(), e.what(), light ? "light" : "dark");
        return;
    }

    auto applyFloat = [&style, &j](const char* key, auto&& assign) {
        if (j.contains(key) && j[key].is_number()) assign(j[key].get<float>());
    };
    applyFloat("windowRounding", [&style](float v) { style.WindowRounding = v; });
    applyFloat("frameRounding", [&style](float v) { style.FrameRounding = v; });
    applyFloat("grabRounding", [&style](float v) { style.GrabRounding = v; });
    applyFloat("tabRounding", [&style](float v) { style.TabRounding = v; });

    if (j.contains("colors") && j["colors"].is_object()) {
        const nlohmann::json& colors = j["colors"];
        std::unordered_map<std::string, ImGuiCol> lookup;
        for (const ColEntry& entry : kColorTable) lookup.emplace(entry.jsonName, entry.col);
        for (const auto& [key, value] : colors.items()) {
            auto it = lookup.find(key);
            float rgba[4];
            if (it == lookup.end()) {
                RAVEN_LOG_WARN("Editor: style file %s — unknown color key '%s' (ignored)", file.c_str(), key.c_str());
                continue;
            }
            if (!ReadFloat4(value, rgba)) {
                RAVEN_LOG_WARN("Editor: style file %s — color '%s' must be [r,g,b,a] numbers", file.c_str(), key.c_str());
                continue;
            }
            style.Colors[it->second] = ImVec4(rgba[0], rgba[1], rgba[2], rgba[3]);
        }
    }
    RAVEN_LOG_INFO("Editor: theme '%s' applied from %s", themeId.c_str(), file.c_str());
}

std::string ThemeDisplayName(const std::string& themeId) {
    // Cheap path: read the JSON "name" if present; fall back to a static label.
    const std::string file = StyleFileFor(themeId);
    std::ifstream in(file);
    if (in) {
        try {
            nlohmann::json j;
            in >> j;
            if (j.contains("name") && j["name"].is_string()) return j["name"].get<std::string>();
        } catch (const std::exception&) {}
    }
    return themeId == "light" ? "Default Light" : "Default Dark";
}

} // namespace raven::editor
