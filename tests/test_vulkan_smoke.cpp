// Vulkan smoke test — only built when RAVEN_VULKAN_FOUND.
// Renders the sample scene OFFSCREEN (works on Lavapipe/CI, no window needed),
// writes a PNG and verifies real pixels came out (crate is lit against a dark clear).
#include "test_framework.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>
#include <cstdio>

#include <glm/glm.hpp>
#include <stb_image.h>

#include "raven/assets/AssetManager.h"
#include "raven/renderer/Camera.h"
#include "raven/renderer/Renderer.h"
#include "raven/scene/Scene.h"

using namespace raven;

namespace fs = std::filesystem;

#ifndef RAVEN_SAMPLE_DIR
#define RAVEN_SAMPLE_DIR "sample"
#endif
#ifndef RAVEN_ENGINE_ASSETS_DIR
#define RAVEN_ENGINE_ASSETS_DIR "assets/engine"
#endif

namespace {

fs::path CreateTempProjectCopy() {
    const fs::path dst = fs::temp_directory_path() / "raven_test_sample_project";
    fs::remove_all(dst);
    fs::create_directories(dst);
    const fs::path src = fs::path(RAVEN_SAMPLE_DIR);
    for (const auto& entry : fs::recursive_directory_iterator(src)) {
        const fs::path rel = fs::relative(entry.path(), src);
        const fs::path target = dst / rel;
        if (entry.is_directory()) {
            fs::create_directories(target);
        } else {
            fs::create_directories(target.parent_path());
            fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing);
        }
    }
    return dst;
}

} // namespace

RAVEN_TEST("vulkan: offscreen render of the sample scene produces lit pixels") {
    // Shaders are looked up via RAVEN_ENGINE_ASSETS_DIR (env wins over compile-time default).
    setenv("RAVEN_ENGINE_ASSETS_DIR", RAVEN_ENGINE_ASSETS_DIR, /*overwrite*/ 0);

    const fs::path project = CreateTempProjectCopy();

    AssetManager& mgr = AssetManager::Instance();
    std::string error;
    REQUIRE(mgr.Initialize(project / "assets", project / "Library", &error));

    Renderer renderer;
    RendererConfig config;
    config.applicationName = "RavenEngine_Test";
    config.enableValidation = false;
    config.allowCpuDeviceFallback = true;   // Lavapipe on CI
#if defined(__APPLE__)
    config.enablePortabilityEnumeration = true;  // MoltenVK requires this flag
#endif
    if (!renderer.Initialize(config, &error)) {
        // A GPU-less environment (e.g. a build-only CI runner) may lack any Vulkan device.
        // Skipping is only acceptable when the runner is not GPU-mandatory; Linux CI sets
        // RAVEN_REQUIRE_GPU=1 so device-creation regressions on Lavapipe still fail hard.
        const char* require = std::getenv("RAVEN_REQUIRE_GPU");
        if (require && require[0] == '1') {
            FAIL("RAVEN_REQUIRE_GPU=1 but Vulkan device creation failed: " + (error.empty() ? std::string("unknown error") : error));
        }
        std::fprintf(stderr, "[ SKIP ] no Vulkan device available — smoke test skipped (%s)\n", error.c_str());
        return;
    }
    renderer.RegisterAssetUploadHandler();
    REQUIRE(renderer.CreateOffscreenTarget(320, 240, &error));

    Scene scene("Sample");
    REQUIRE(scene.Load(project / "scenes" / "sample_scene.ravenscene", &error));

    // Warm the GPU caches by loading every mesh/material asset referenced by the scene.
    std::string shaderDirWarning;
    (void)shaderDirWarning;

    Camera camera;
    camera.position = {4.2f, 3.1f, 6.3f};
    camera.LookAt(camera.position, glm::vec3(0.0f, 1.0f, 0.0f));
    camera.fovDegrees = 55.0f;

    FrameParams params;
    params.drawGrid = false;
    FrameStats stats{};
    REQUIRE(renderer.RenderOffscreen(scene, camera, params));
    stats = renderer.GetFrameStats();

    const fs::path pngPath = fs::temp_directory_path() / "raven_test_offscreen.png";
    REQUIRE(renderer.SaveOffscreenToPng(scene, camera, params, pngPath, &error));
    CHECK(fs::exists(pngPath));
    CHECK(fs::file_size(pngPath) > 800);  // not a flat clear-color image

    // Read it back and require a good fraction of non-background pixels.
    int width = 0, height = 0, channels = 0;
    unsigned char* pixels = stbi_load(pngPath.string().c_str(), &width, &height, &channels, 4);
    REQUIRE(pixels != nullptr);
    CHECK_EQ(width, 320);
    CHECK_EQ(height, 240);
    int litPixels = 0;
    for (int i = 0; i < width * height; ++i) {
        const unsigned char r = pixels[i * 4 + 0], g = pixels[i * 4 + 1], b = pixels[i * 4 + 2];
        // Background clear is ~(6,7,12). Anything clearly brighter is scene content.
        if (int(r) + int(g) + int(b) > 90) ++litPixels;
    }
    stbi_image_free(pixels);
    const float litFraction = static_cast<float>(litPixels) / static_cast<float>(width * height);
    CHECK(litFraction > 0.05f);  // crate + ground cover a real chunk of frame

    // The frame recorded actual work: two mesh draws (crate + ground), 24+ triangles.
    CHECK(stats.meshesDrawn >= 2u);
    CHECK(stats.triangles >= 24u);

    renderer.Shutdown();
}
