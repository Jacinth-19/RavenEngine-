#include "editor/core/Fonts.h"

#include <string>

#include "imgui.h"

#include "raven/core/Logger.h"

namespace raven::editor {

namespace {
ImFont* s_Regular = nullptr;   // Copy A — merged Regular (used everywhere)
ImFont* s_Fill = nullptr;      // Copy B — merged Fill (active/toggled accents)
ImFont* s_BakeRegular64 = nullptr;
ImFont* s_BakeFill64 = nullptr;

const ImWchar kPhosphorGlyphRanges[] = { 0xE000, 0xF8FF, 0 };

std::string FontDir() { return RAVEN_PHOSPHOR_FONT_DIR; }
} // namespace

void LoadRavenFonts(ImGuiIO& io) {
    // Oversampling tuned for editor-crisp text on mixed-DPI displays.
    ImFontConfig baseConfig;
    baseConfig.OversampleH = 3;
    baseConfig.OversampleV = 2;
    baseConfig.PixelSnapH = true;

    io.Fonts->AddFontDefault(&baseConfig);

    const std::string regular = FontDir() + "/Phosphor.ttf";
    {
        ImFontConfig config;
        config.MergeMode = true;
        config.PixelSnapH = true;
        config.OversampleH = 2;
        s_Regular = io.Fonts->AddFontFromFileTTF(regular.c_str(), 13.0f, &config, kPhosphorGlyphRanges);
        if (!s_Regular) RAVEN_LOG_WARN("Editor: icon font %s failed to load (icons will render as boxes)", regular.c_str());
    }

    // Copy B gets its OWN default base (ImGui forbids merging into the returned merge font twice).
    io.Fonts->AddFontDefault(&baseConfig);
    const std::string fill = FontDir() + "/Phosphor-Fill.ttf";
    {
        ImFontConfig config;
        config.MergeMode = true;
        config.PixelSnapH = true;
        config.OversampleH = 2;
        s_Fill = io.Fonts->AddFontFromFileTTF(fill.c_str(), 13.0f, &config, kPhosphorGlyphRanges);
        if (!s_Fill) RAVEN_LOG_WARN("Editor: fill icon font %s failed to load (accent icons will render as boxes)", fill.c_str());
    }
}

ImFont* PhosphorRegularFont() { return s_Regular; }
ImFont* PhosphorFillFont() { return s_Fill; }

static ImFont* BuildBake(const char* file, ImFont*& cache) {
    if (cache) return cache;
    const std::string path = FontDir() + "/" + file;
    ImFontConfig config;
    config.PixelSnapH = true;
    cache = ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), 64.0f, &config, kPhosphorGlyphRanges);
    if (!cache) RAVEN_LOG_WARN("Editor: 64px bake %s failed to load", path.c_str());
    return cache;
}

ImFont* PhosphorRegularBake64() { return BuildBake("Phosphor.ttf", s_BakeRegular64); }
ImFont* PhosphorFillBake64() { return BuildBake("Phosphor-Fill.ttf", s_BakeFill64); }

} // namespace raven::editor
