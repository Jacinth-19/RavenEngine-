// Phase 3 (§10.2): CPU-side thumbnail baking — ThumbnailGenerator.cpp is compiled
// straight into the test binary (it is GPU/ImGui-free).
#include "test_framework.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "stb_image.h"
#include "stb_image_write.h"

#include "editor/widgets/ThumbnailGenerator.h"

using namespace raven::editor;

namespace {

struct TempDir {
    std::filesystem::path root;
    TempDir() {
        root = std::filesystem::temp_directory_path() / ("raven_thumbs_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

// Writes a solid-color w*h RGBA PNG via stb_image_write and returns its path.
std::filesystem::path WriteSyntheticPng(const std::filesystem::path& path, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < pixels.size(); i += 4) { pixels[i] = r; pixels[i + 1] = g; pixels[i + 2] = b; pixels[i + 3] = 255; }
    stbi_write_png(path.string().c_str(), w, h, 4, pixels.data(), w * 4);
    return path;
}

} // namespace

RAVEN_TEST("thumbnails: texture within the box keeps its size (no upscale)") {
    TempDir dir;
    const auto src = WriteSyntheticPng(dir.root / "small.png", 40, 24, 200, 30, 30);
    const thumbnail::BakeResult baked = thumbnail::BakeTexture(src);
    CHECK(baked.ok);
    CHECK(baked.width == 40 && baked.height == 24);
    CHECK(baked.rgba.size() == 40u * 24u * 4u);
    CHECK(baked.rgba[0] == 200 && baked.rgba[1] == 30 && baked.rgba[2] == 30 && baked.rgba[3] == 255);
}

RAVEN_TEST("thumbnails: aspect-preserving fit caps the long side at 128") {
    TempDir dir;
    const auto src = WriteSyntheticPng(dir.root / "wide.png", 333, 111, 20, 220, 40);
    const thumbnail::BakeResult baked = thumbnail::BakeTexture(src);
    CHECK(baked.ok);
    CHECK(baked.width == 128);
    CHECK(baked.height >= 41 && baked.height <= 44);   // 111 * 128/333 = 42.6
    const auto src2 = WriteSyntheticPng(dir.root / "tall.png", 111, 333, 20, 40, 220);
    const thumbnail::BakeResult baked2 = thumbnail::BakeTexture(src2);
    CHECK(baked2.ok);
    CHECK(baked2.height == 128);
    CHECK(baked2.width >= 41 && baked2.width <= 44);
}

RAVEN_TEST("thumbnails: odd-size downscale does not read out of bounds") {
    // Regression: bilinear taps must clamp to the edge — previously floor() of a
    // slightly negative coordinate truncated to a giant index.
    TempDir dir;
    const auto src = WriteSyntheticPng(dir.root / "odd.png", 517, 129, 90, 90, 200);
    const thumbnail::BakeResult baked = thumbnail::BakeTexture(src);
    CHECK(baked.ok);
    CHECK(baked.width == 128 && baked.height > 0);
    bool sawTint = false;
    for (size_t i = 0; i + 2 < baked.rgba.size(); i += 4) if (baked.rgba[i + 2] > 150) sawTint = true;
    CHECK(sawTint);
}

RAVEN_TEST("thumbnails: corrupt source fails cleanly with an error") {
    TempDir dir;
    const auto src = dir.root / "not_an_image.png";
    { std::ofstream out(src); out << "definitely not a png"; }
    const thumbnail::BakeResult baked = thumbnail::BakeTexture(src);
    CHECK(!baked.ok);
    CHECK(!baked.error.empty());
}

RAVEN_TEST("thumbnails: PNG write/LoadPng round trip is lossless at bake size") {
    TempDir dir;
    const auto src = WriteSyntheticPng(dir.root / "roundtrip_src.png", 200, 90, 17, 201, 111);
    const thumbnail::BakeResult baked = thumbnail::BakeTexture(src);
    CHECK(baked.ok);
    const auto out = dir.root / "roundtrip.png";
    std::string writeError;
    CHECK(thumbnail::WritePngAtomic(out, baked.width, baked.height, baked.rgba.data(), &writeError));
    CHECK(std::filesystem::exists(out));
    uint32_t w = 0, h = 0;
    const std::vector<uint8_t> loaded = thumbnail::LoadPng(out, &w, &h);
    CHECK(w == baked.width && h == baked.height);
    CHECK(loaded == baked.rgba);   // PNG is lossless — pixels must be identical
}

RAVEN_TEST("thumbnails: sample crate texture bakes to a bounded size") {
    const std::filesystem::path src = std::filesystem::path(RAVEN_SAMPLE_DIR) / "assets" / "textures" / "crate_albedo.png";
    if (!std::filesystem::exists(src)) return;   // sample not available on this runner
    const thumbnail::BakeResult baked = thumbnail::BakeTexture(src);
    CHECK(baked.ok);
    CHECK(baked.width <= 128 && baked.height <= 128);
    CHECK(baked.width > 0 && baked.height > 0);
}
