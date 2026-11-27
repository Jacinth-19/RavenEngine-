// raven-sandbox — minimal runnable RavenEngine app.
//
// Windowed mode: opens a window and renders the sample scene with an orbit camera
// (ESC quits, drag orbits). When no display/swapchain is available (CI, null-platform
// GLFW), it renders a fixed number of frames offscreen and prints average frame times,
// then exits 0 — usable as a headless smoke run of the full engine stack.
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "raven/core/Logger.h"
#include "raven/assets/AssetManager.h"
#include "raven/renderer/Camera.h"
#include "raven/renderer/Renderer.h"
#include "raven/renderer/Quality.h"
#include "raven/scene/Scene.h"

#define GLFW_INCLUDE_NONE
#include "GLFW/glfw3.h"

using namespace raven;

namespace {

struct SandboxArgs {
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t frames = 120;         // headless mode only
    QualityTier tier = QualityTier::Medium;
    bool offscreen = false;        // force headless
};

void Usage(const char* argv0) {
    std::printf(
        "raven-sandbox — minimal RavenEngine runner\n"
        "Usage: %s [--width N] [--height N] [--frames N] [--tier low|medium|high|ultra] [--offscreen]\n"
        "  --offscreen renders N frames offscreen and prints timings instead of opening a window\n",
        argv0);
}

} // namespace

int main(int argc, char** argv) {
    SandboxArgs args;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) { RAVEN_LOG_ERROR("missing value for %s", name); std::exit(2); }
            return argv[++i];
        };
        if (arg == "--width") args.width = static_cast<uint32_t>(std::stoul(next("--width")));
        else if (arg == "--height") args.height = static_cast<uint32_t>(std::stoul(next("--height")));
        else if (arg == "--frames") args.frames = static_cast<uint32_t>(std::stoul(next("--frames")));
        else if (arg == "--tier") {
            const std::string t = next("--tier");
            if (t == "low") args.tier = QualityTier::Low;
            else if (t == "medium") args.tier = QualityTier::Medium;
            else if (t == "high") args.tier = QualityTier::High;
            else if (t == "ultra") args.tier = QualityTier::Ultra;
            else { RAVEN_LOG_ERROR("unknown tier: %s", t.c_str()); return 2; }
        }
        else if (arg == "--offscreen") args.offscreen = true;
        else if (arg == "--help" || arg == "-h") { Usage(argv[0]); return 0; }
        else { RAVEN_LOG_ERROR("unknown argument: %s", arg.c_str()); Usage(argv[0]); return 2; }
    }

    const std::filesystem::path projectDir = RAVEN_SAMPLE_DIR;
    std::string error;
    if (!AssetManager::Instance().Initialize(projectDir / "assets",
                                             std::filesystem::temp_directory_path() / "raven_sandbox_library",
                                             &error)) {
        RAVEN_LOG_ERROR("raven-sandbox: asset manager init failed: %s", error.c_str());
        return 1;
    }

    // Try the windowed path first (unless forced offscreen).
    bool windowed = !args.offscreen;
    GLFWwindow* window = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    std::vector<const char*> instanceExtensions;

    if (windowed) {
        if (!glfwInit()) {
            RAVEN_LOG_WARN("raven-sandbox: glfwInit failed — switching to offscreen mode");
            windowed = false;
        } else {
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            window = glfwCreateWindow(static_cast<int>(args.width), static_cast<int>(args.height), "RavenEngine Sandbox", nullptr, nullptr);
            if (!window) {
                RAVEN_LOG_WARN("raven-sandbox: window creation failed — switching to offscreen mode");
                glfwTerminate();
                windowed = false;
            }
        }
    }
    if (windowed) {
        uint32_t extensionCount = 0;
        const char** extensions = glfwGetRequiredInstanceExtensions(&extensionCount);
        instanceExtensions.assign(extensions, extensions + extensionCount);
    }

    Renderer renderer;
    RendererConfig config;
    config.applicationName = "RavenEngine Sandbox";
    config.qualityTier = args.tier;
    config.allowCpuDeviceFallback = true;
    config.requiredInstanceExtensions = instanceExtensions;
    if (!renderer.Initialize(config, &error)) {
        RAVEN_LOG_ERROR("raven-sandbox: Vulkan initialization failed: %s", error.c_str());
        if (window) { glfwDestroyWindow(window); glfwTerminate(); }
        return 1;
    }
    renderer.RegisterAssetUploadHandler();

    if (windowed) {
        int fbWidth = 0, fbHeight = 0;
        glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
        if (glfwCreateWindowSurface(renderer.Instance(), window, nullptr, &surface) != VK_SUCCESS ||
            !renderer.CreateSwapchain(surface, static_cast<uint32_t>(fbWidth), static_cast<uint32_t>(fbHeight), &error)) {
            RAVEN_LOG_WARN("raven-sandbox: swapchain unavailable (%s) — switching to offscreen mode", error.c_str());
            if (surface != VK_NULL_HANDLE) vkDestroySurfaceKHR(renderer.Instance(), surface, nullptr);
            glfwDestroyWindow(window);
            glfwTerminate();
            window = nullptr;
            surface = VK_NULL_HANDLE;
            windowed = false;
        }
    }

    Scene scene("Sandbox");
    if (!scene.Load(projectDir / "scenes" / "sample_scene.ravenscene", &error)) {
        RAVEN_LOG_ERROR("raven-sandbox: scene load failed: %s", error.c_str());
        renderer.Shutdown();
        if (window) { glfwDestroyWindow(window); glfwTerminate(); }
        return 1;
    }

    Camera camera;
    camera.position = {4.2f, 3.1f, 6.3f};
    camera.LookAt(camera.position, glm::vec3(0.0f, 1.0f, 0.0f));
    camera.fovDegrees = 55.0f;

    FrameParams params;
    params.drawGrid = false;

    if (!windowed) {
        if (!renderer.CreateOffscreenTarget(args.width, args.height, &error)) {
            RAVEN_LOG_ERROR("raven-sandbox: offscreen target failed: %s", error.c_str());
            renderer.Shutdown();
            return 1;
        }
        float totalMs = 0.0f;
        for (uint32_t i = 0; i < args.frames; ++i) {
            if (!renderer.RenderOffscreen(scene, camera, params)) {
                RAVEN_LOG_ERROR("raven-sandbox: render failed on frame %u", i);
                renderer.Shutdown();
                return 1;
            }
            totalMs += renderer.GetFrameStats().cpuFrameMs;
        }
        const FrameStats stats = renderer.GetFrameStats();
        std::printf("raven-sandbox offscreen: %u frames, avg cpu %.2f ms, draws %u, triangles %u (tier %s)\n",
                    args.frames, totalMs / static_cast<float>(args.frames), stats.drawCalls, stats.triangles,
                    QualityPresetFor(args.tier).name);
        renderer.Shutdown();
        return 0;
    }

    double lastTime = glfwGetTime();
    double lastPrint = lastTime;
    uint32_t frames = 0;
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        int fbWidth = 0, fbHeight = 0;
        glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
        if (fbWidth == 0 || fbHeight == 0) { glfwWaitEvents(); continue; }

        const double now = glfwGetTime();
        lastTime = now;
        (void)lastTime;

        if (!renderer.RenderFrame(scene, camera, params)) {
            // swapchain stale — request resize happens internally; try again next tick
            continue;
        }
        ++frames;
        if (now - lastPrint > 2.0) {
            const FrameStats stats = renderer.GetFrameStats();
            RAVEN_LOG_INFO("sandbox: %u frames, last frame cpu %.2f ms", frames, stats.cpuFrameMs);
            lastPrint = now;
        }
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) glfwSetWindowShouldClose(window, GLFW_TRUE);
    }

    vkDestroySurfaceKHR(renderer.Instance(), surface, nullptr);
    renderer.Shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
