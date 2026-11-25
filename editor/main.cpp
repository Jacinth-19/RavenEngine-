// RavenEditor — the standalone RavenEngine editor application.
//
// Modes:
//   windowed (default): GLFW window, swapchain presentation of the docking UI.
//   headless (--headless or automatic fallback when no display/surface exists):
//     the editor renders the scene + UI into the offscreen target and writes
//     the result to a PNG (--capture path). Used on displays-less hosts/CI.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "raven/core/Logger.h"
#include "raven/assets/AssetManager.h"
#include "raven/renderer/Renderer.h"
#include "raven/renderer/Quality.h"
#include "raven/scene/Scene.h"

#include "editor_app.h"

#define GLFW_INCLUDE_NONE
#include "GLFW/glfw3.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"

namespace {

struct EditorArgs {
    bool headless = false;
    uint32_t frames = 5;             // headless: frames to render before the capture
    uint32_t width = 1280;
    uint32_t height = 720;
    std::string capture = "editor_capture.png";
    std::string project = RAVEN_SAMPLE_DIR;
};

void PrintUsage(const char* argv0) {
    std::printf(
        "RavenEditor %s\n"
        "Usage: %s [options]\n"
        "  --headless            run without a window (scene + UI rendered offscreen)\n"
        "  --frames N            headless: frames to render (default 5)\n"
        "  --capture PATH        headless: output PNG path (default editor_capture.png)\n"
        "  --project DIR         project directory with project.ravenproject (default: bundled sample)\n"
        "  --width N  --height N viewport/capture resolution (default 1280x720)\n"
        "  --help\n",
        RAVEN_VERSION, argv0);
}

bool ParseArgs(int argc, char** argv, EditorArgs& args, bool& showHelp) {
    showHelp = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](std::string& out) -> bool {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        if (arg == "--headless") args.headless = true;
        else if (arg == "--frames") { std::string v; if (!next(v)) return false; args.frames = static_cast<uint32_t>(std::stoul(v)); }
        else if (arg == "--capture") { if (!next(args.capture)) return false; }
        else if (arg == "--project") { if (!next(args.project)) return false; }
        else if (arg == "--width") { std::string v; if (!next(v)) return false; args.width = static_cast<uint32_t>(std::stoul(v)); }
        else if (arg == "--height") { std::string v; if (!next(v)) return false; args.height = static_cast<uint32_t>(std::stoul(v)); }
        else if (arg == "--help" || arg == "-h") { showHelp = true; return true; }
        else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return false;
        }
    }
    return true;
}

// Initialize ImGui + Vulkan backend for the given color format.
bool InitImGuiVulkan(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
                     uint32_t queueFamily, VkQueue queue, VkFormat colorFormat,
                     uint32_t minImageCount, uint32_t imageCount) {
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = instance;
    info.PhysicalDevice = physicalDevice;
    info.Device = device;
    info.QueueFamily = queueFamily;
    info.Queue = queue;
    info.DescriptorPool = VK_NULL_HANDLE;
    info.DescriptorPoolSize = 96;   // sampler pool size hint; backend creates its own pool
    info.MinImageCount = minImageCount;
    info.ImageCount = imageCount;
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &colorFormat;
    info.PipelineInfoForViewports = info.PipelineInfoMain;
    return ImGui_ImplVulkan_Init(&info);
}


} // namespace

int main(int argc, char** argv) {
    EditorArgs args;
    bool showHelp = false;
    if (!ParseArgs(argc, argv, args, showHelp)) {
        PrintUsage(argv[0]);
        return 2;
    }
    if (showHelp) {
        PrintUsage(argv[0]);
        return 0;
    }

    raven::Logger::Instance().SetEchoToConsole(true);

    // --- project / assets --------------------------------------------------
    const std::filesystem::path projectDir = args.project;
    std::string initError;
    if (!raven::AssetManager::Instance().Initialize(projectDir / "assets",
                                                    std::filesystem::temp_directory_path() / "raven_editor_library",
                                                    &initError)) {
        RAVEN_LOG_ERROR("RavenEditor: asset manager init failed (%s). Run from the sample project or pass --project DIR.", initError.c_str());
        return 1;
    }

    // --- windowed vs headless ----------------------------------------------
    bool windowed = !args.headless;
    GLFWwindow* window = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    std::vector<const char*> instanceExtensions;

    if (windowed) {
        if (!glfwInit()) {
            RAVEN_LOG_WARN("RavenEditor: glfwInit failed (no windowing platform/display?) — falling back to headless capture");
            windowed = false;
        } else {
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
            window = glfwCreateWindow(static_cast<int>(args.width), static_cast<int>(args.height), "RavenEditor", nullptr, nullptr);
            if (!window) {
                RAVEN_LOG_WARN("RavenEditor: window creation failed — falling back to headless capture");
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

    raven::Renderer renderer;
    raven::RendererConfig rc;
    rc.applicationName = "RavenEditor";
    rc.qualityTier = raven::QualityTier::High;
    rc.allowCpuDeviceFallback = true;
    rc.requiredInstanceExtensions = instanceExtensions;
    if (!renderer.Initialize(rc, &initError)) {
        RAVEN_LOG_ERROR("RavenEditor: Vulkan initialization failed: %s", initError.c_str());
        if (window) { glfwDestroyWindow(window); glfwTerminate(); }
        return 1;
    }
    renderer.RegisterAssetUploadHandler();

    if (windowed) {
        int fbWidth = 0, fbHeight = 0;
        glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
        if (glfwCreateWindowSurface(renderer.Instance(), window, nullptr, &surface) != VK_SUCCESS ||
            !renderer.CreateSwapchain(surface, static_cast<uint32_t>(fbWidth), static_cast<uint32_t>(fbHeight), &initError)) {
            RAVEN_LOG_WARN("RavenEditor: swapchain unavailable (%s) — falling back to headless capture", initError.c_str());
            if (surface != VK_NULL_HANDLE && renderer.Instance() != VK_NULL_HANDLE) {
                vkDestroySurfaceKHR(renderer.Instance(), surface, nullptr);
                surface = VK_NULL_HANDLE;
            }
            glfwDestroyWindow(window);
            glfwTerminate();
            window = nullptr;
            windowed = false;
        }
    }

    // --- editor app + scene ---------------------------------------------------
    raven::EditorApp app(renderer, rc.qualityTier);
    app.RegisterLogCapture();
    std::string projectError;
    if (!app.LoadProject(projectDir, &projectError)) {
        RAVEN_LOG_ERROR("RavenEditor: %s", projectError.c_str());
        // Keep going with an (empty) scene: the editor shell still works.
    }

    // ============================ headless capture ==========================
    if (!windowed) {
        if (!renderer.CreateOffscreenTarget(args.width, args.height, &initError)) {
            RAVEN_LOG_ERROR("RavenEditor: offscreen target failed: %s", initError.c_str());
            renderer.Shutdown();
            return 1;
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        io.IniFilename = nullptr;   // do not write editor state in headless mode

        if (!InitImGuiVulkan(renderer.Instance(), renderer.PhysicalDevice(), renderer.Device(),
                             renderer.GraphicsQueueFamily(), renderer.GraphicsQueue(),
                             renderer.OffscreenFormat(), 2, 2)) {
            RAVEN_LOG_ERROR("RavenEditor: ImGui Vulkan backend initialization failed");
            app.Shutdown();
            ImGui::DestroyContext();
            renderer.Shutdown();
            return 1;
        }

        for (uint32_t frame = 0; frame < args.frames; ++frame) {
            io.DisplaySize = ImVec2(static_cast<float>(args.width), static_cast<float>(args.height));
            io.DeltaTime = 1.0f / 60.0f;
            ImGui_ImplVulkan_NewFrame();
            ImGui::NewFrame();

            app.OnFrame(1.0f / 60.0f);
            ImGui::Render();

            raven::FrameParams params = app.MakeFrameParams(/*uiInScene=*/true);
            renderer.RenderOffscreen(app.GetScene(), app.ViewportCamera(), params);
        }

        std::string captureError;
        const bool captured = renderer.ReadOffscreenToPng(args.capture, &captureError);
        if (!captured) RAVEN_LOG_ERROR("RavenEditor: capture failed: %s", captureError.c_str());
        else RAVEN_LOG_INFO("RavenEditor: captured %u frame(s) to %s", args.frames, args.capture.c_str());

        app.Shutdown();          // drops ImGui descriptor sets before backend shutdown
        ImGui_ImplVulkan_Shutdown();
        ImGui::DestroyContext();
        renderer.Shutdown();
        return captured ? 0 : 1;
    }

    // ============================== windowed UI ==============================
    glfwSetWindowUserPointer(window, &renderer);
    glfwSetFramebufferSizeCallback(window, [](GLFWwindow* w, int width, int height) {
        auto* r = static_cast<raven::Renderer*>(glfwGetWindowUserPointer(w));
        if (r) r->RequestSwapchainResize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    });

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = "raven_editor.ini";

    ImGui_ImplGlfw_InitForVulkan(window, true);
    if (!InitImGuiVulkan(renderer.Instance(), renderer.PhysicalDevice(), renderer.Device(),
                         renderer.GraphicsQueueFamily(), renderer.GraphicsQueue(),
                         renderer.SwapchainColorFormat(), 2, renderer.SwapchainImageCount())) {
        RAVEN_LOG_ERROR("RavenEditor: ImGui Vulkan backend initialization failed");
        app.Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        vkDestroySurfaceKHR(renderer.Instance(), surface, nullptr);
        renderer.Shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    glfwSwapInterval(1);

    double lastTime = glfwGetTime();
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        int fbWidth = 0, fbHeight = 0;
        glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
        if (fbWidth == 0 || fbHeight == 0) { glfwWaitEvents(); continue; }   // minimized

        const double now = glfwGetTime();
        const float deltaTime = static_cast<float>(now - lastTime);
        lastTime = now;

        // One pass: scene renders into the swapchain and the UI overlay draws on top
        // (the central dock node is a transparent hole showing the scene).
        ImGui_ImplGlfw_NewFrame();
        ImGui_ImplVulkan_NewFrame();
        ImGui::NewFrame();
        app.OnFrame(deltaTime);
        ImGui::Render();
        raven::FrameParams params = app.MakeFrameParams(/*uiInScene=*/true);
        params.deltaTime = deltaTime;
        renderer.RenderFrame(app.GetScene(), app.ViewportCamera(), params);
    }

    app.Shutdown();              // drops ImGui descriptor sets before backend shutdown
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    // vkDestroySurfaceKHR does not need the swapchain; must happen before the instance goes away.
    vkDestroySurfaceKHR(renderer.Instance(), surface, nullptr);
    renderer.Shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
