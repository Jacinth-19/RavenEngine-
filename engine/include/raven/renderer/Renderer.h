// RavenEngine — high-level renderer. Owns the Vulkan context, swapchain (windowed),
// offscreen viewport targets (editor / headless screenshots), pipelines, and the
// GPU resource caches for meshes/textures/materials.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "raven/renderer/Quality.h"

namespace raven {

class Scene;
class Camera;
struct Asset;

struct RendererConfig {
    std::string applicationName = "RavenEngine";
    bool enableValidation = false;
    bool allowCpuDeviceFallback = true;    // use lavapipe/SwiftShader-class devices when no GPU exists
    bool enablePortabilityEnumeration = false;  // macOS/iOS (MoltenVK)
    std::vector<const char*> requiredInstanceExtensions;  // from the windowing layer (glfwGetRequiredInstanceExtensions)
    QualityTier qualityTier = QualityTier::Medium;        // picks shadows/MSAA/texture budget (see Quality.h)
    // Internal render scale in (0.5, 1.0]: the scene is rendered at scale * target
    // extent and expanded to the target with a Catmull-Rom resolve.
    // 0 = auto (use the quality tier's preset: Low 0.5, Medium 0.67, High/Ultra 1.0).
    float resolutionScale = 0.0f;
};

// Passed to FrameParams::uiOverlay: describes the final color target the UI draws into.
// The renderer begins a dynamic-rendering instance on that view (COLOR_ATTACHMENT_OPTIMAL,
// loadOp=LOAD, storeOp=STORE) — the overlay callback only emits draw commands.
struct FrameOverlayTarget {
    VkImageView colorView = VK_NULL_HANDLE;
    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct FrameParams {
    float deltaTime = 0.0f;
    bool drawGrid = false;        // editor viewport reference grid
    // Optional: editor UI overlay drawn after the scene into the final image (windowed
    // swapchain or offscreen target). Called once per frame. Null = no overlay.
    std::function<void(VkCommandBuffer, const FrameOverlayTarget&)> uiOverlay;
};

struct PassTiming {
    std::string name;
    float milliseconds = 0.0f;
};

struct FrameStats {
    float cpuFrameMs = 0.0f;
    float gpuFrameMs = 0.0f;
    std::vector<PassTiming> passTimes;   // last completed frame
    uint32_t drawCalls = 0;
    uint32_t triangles = 0;
    uint32_t meshesDrawn = 0;
    uint32_t culledPasses = 0;
    uint32_t lodCulled = 0;          // entities rejected by the LOD distance cutoff this frame
    std::string gpuName;
    uint64_t vramBytes = 0;
    bool dynamicRendering = true;
    bool timelineSemaphores = false;
    bool cpuDevice = false;
};

class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool Initialize(const RendererConfig& config, std::string* error = nullptr);
    void Shutdown();
    bool IsInitialized() const;
    std::string GpuName() const;

    // --- Window presentation -------------------------------------------------
    // The caller creates the VkSurfaceKHR (e.g. via glfwCreateWindowSurface).
    bool CreateSwapchain(VkSurfaceKHR surface, uint32_t width, uint32_t height, std::string* error = nullptr);
    void RequestSwapchainResize(uint32_t width, uint32_t height);
    // Acquire, record, submit, present. Returns false when the frame was skipped (surface gone).
    bool RenderFrame(Scene& scene, const Camera& camera, const FrameParams& params);

    // --- Offscreen target (editor viewport / headless screenshots) -----------
    bool CreateOffscreenTarget(uint32_t width, uint32_t height, std::string* error = nullptr);
    void ResizeOffscreenTarget(uint32_t width, uint32_t height);
    bool RenderOffscreen(Scene& scene, const Camera& camera, const FrameParams& params);
    // Handles for the ImGui Vulkan backend: register the color view with
    // ImGui_ImplVulkan_AddTexture() to get an ImTextureID (void*).
    VkImageView OffscreenColorView() const;
    VkSampler OffscreenSampler() const;
    VkFormat OffscreenFormat() const;
    uint32_t OffscreenWidth() const;
    uint32_t OffscreenHeight() const;

    // Renders one frame offscreen then writes a PNG. Used by CI smoke tests.
    bool SaveOffscreenToPng(Scene& scene, const Camera& camera, const FrameParams& params,
                            const std::filesystem::path& path, std::string* error = nullptr);
    // Writes the CURRENT offscreen contents to a PNG (no render). Call after RenderOffscreen.
    bool ReadOffscreenToPng(const std::filesystem::path& path, std::string* error = nullptr);

    // Swapchain color format (undefined when no swapchain exists). For ImGui/backends setup.
    VkFormat SwapchainColorFormat() const;
    uint32_t SwapchainImageCount() const;

    // Renders ONLY the given overlay into the next swapchain image and presents it
    // (no scene pass). Editor UI path: render the scene offscreen, build the ImGui
    // frame, then hand its draw data to this call. Returns false when the frame was
    // skipped (swapchain stale/minimized).
    bool RenderUiFrame(const std::function<void(VkCommandBuffer, const FrameOverlayTarget&)>& overlay);

    FrameStats GetFrameStats() const;

    // --- Vulkan handles for ImGui backend ------------------------------------
    VkInstance Instance() const;
    VkPhysicalDevice PhysicalDevice() const;
    VkDevice Device() const;
    VkQueue GraphicsQueue() const;
    uint32_t GraphicsQueueFamily() const;

    // --- Asset pipeline hookup ------------------------------------------------
    // Registers with AssetManager so uploaded assets become GPU resources.
    void RegisterAssetUploadHandler();

    struct Impl;

private:
    std::unique_ptr<Impl> m_Impl;
};

} // namespace raven
