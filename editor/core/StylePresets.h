// RavenEditor — JSON style presets (docs/EDITOR_DESIGN.md §2.3).
//
// Presets live in editor/assets/styles/*.json. Each file:
//   { "name": "Default Dark", "base": "dark", "windowRounding": 6,
//     "colors": { "<ImGuiCol name>": [r,g,b,a], ... } }
// Base "dark"/"light" seeds ImGui::StyleColorsDark/Light; known keys override.
// Unknown keys are ignored with a warning (forwards compatibility). Missing files or
// parse errors fall back to the built-in base — the editor always comes up styled.
#pragma once

#include <string>

struct ImGuiStyle;

namespace raven::editor {

struct ThemeInfo {
    std::string id;          // config value: "dark" | "light"
    std::string displayName; // from the JSON "name" (fallback: id)
};

// Apply "dark" or "light" (anything else → "dark") to ImGui::GetStyle().
void ApplyTheme(const std::string& themeId);

// Load+apply the preset JSON for an id; falls back to built-in colors on any error.
void ApplyThemeFromFile(const std::string& themeId);

// Display names for menus ("Default Dark" / "Default Light").
std::string ThemeDisplayName(const std::string& themeId);

} // namespace raven::editor
