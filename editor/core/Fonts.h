// RavenEditor — font loading (docking ImGui).
//
// Two merged-font copies share the default base (Lato) — Regular-phosphor and
// Fill-phosphor — plus lazily-buildable standalone 64 px icon bakes for grid/thumbnail
// rendering (ProjectPanel, Phase 3).
//
// Resolution: RAVEN_PHOSPHOR_FONT_DIR (compile definition, cmake) → the vendored TTFs.
#pragma once

struct ImGuiIO;
struct ImFont;

namespace raven::editor {

// Build the three fonts (default+phosphor merge, default+fill-phosphor merge) and
// io.Fonts. Call once per ImGui context, before the first NewFrame.
void LoadRavenFonts(ImGuiIO& io);

// The two merged copies (valid after LoadRavenFonts). Never nullptr on success;
// nullptr when the icon font failed to load (default base font still usable).
ImFont* PhosphorRegularFont();
ImFont* PhosphorFillFont();

// Standalone icon-only 64 px bakes (Regular / Fill), built on first use and owned by
// the font atlas. For big glyphs — grid tiles, project thumbnails' glyph, launcher art.
ImFont* PhosphorRegularBake64();
ImFont* PhosphorFillBake64();

} // namespace raven::editor
