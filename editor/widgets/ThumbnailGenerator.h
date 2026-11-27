// RavenEditor — CPU-side thumbnail baking (EDITOR_DESIGN §10.2). Runs on JobSystem
// workers; never touches the GPU, ImGui, or the cache's entry map. Results flow back
// as Completion payloads through the ThumbnailCache's mutex-guarded queue.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "raven/core/UUID.h"

namespace raven::editor::thumbnail {

struct BakeResult {
    bool ok = false;
    std::string error;
    uint32_t width = 0;            // fitted dimensions (max side <= 128), aspect preserved
    uint32_t height = 0;
    std::vector<uint8_t> rgba;     // tightly packed RGBA8
    std::vector<raven::UUID> deps;   // texture dependencies (materials); empty for texture bakes
};

// stb_image decode -> aspect-preserving bilinear fit into a 128px box.
// Formats stb cannot decode (DDS/BCn, …) fail with a descriptive error string.
BakeResult BakeTexture(const std::filesystem::path& source);

// Writes rgba to <path> via a temp sibling + rename (atomic on the same filesystem).
bool WritePngAtomic(const std::filesystem::path& path, uint32_t width, uint32_t height,
                    const uint8_t* rgba, std::string* error);

// Loads a previously baked thumbnail PNG. Returns nullptr pixels on failure.
std::vector<uint8_t> LoadPng(const std::filesystem::path& path, uint32_t* outWidth,
                             uint32_t* outHeight);

} // namespace raven::editor::thumbnail
