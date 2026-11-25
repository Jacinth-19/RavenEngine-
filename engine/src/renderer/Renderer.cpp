// RavenEngine — high-level renderer implementation (Vulkan 1.3, dynamic rendering).
#include <utility>
#include "raven/renderer/Renderer.h"
#include "raven/renderer/Camera.h"
#include "raven/renderer/RenderGraph.h"

#include "vk/VulkanContext.h"
#include "vk/Swapchain.h"
#include "vk/GpuResources.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/quaternion.hpp>
#include <stb_image_write.h>

#include <cstring>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdarg>
#include <chrono>
#include <cfloat>
#include <cstdio>

#include "raven/assets/AssetManager.h"
#include "raven/assets/Asset.h"
#include "raven/core/FileSystem.h"
#include "raven/core/Logger.h"
#include "raven/core/Timer.h"
#include "raven/scene/Scene.h"
#include "raven/scene/Components.h"

namespace raven {

namespace {

constexpr uint32_t kFramesInFlight = 2;
constexpr uint32_t kTotalSlots = 3;       // 2 frames in flight + 1 dedicated offscreen slot
constexpr uint32_t kOffscreenSlot = 2;
const char* kEngineAssetsDirEnv = "RAVEN_ENGINE_ASSETS_DIR";

std::string EngineAssetsDir() {
    if (const char* env = std::getenv(kEngineAssetsDirEnv); env && *env) return env;
    return "assets/engine";
}

struct FrameUboData {
    glm::mat4 view;
    glm::mat4 proj;
    glm::mat4 lightViewProj;
    glm::vec4 lightDirIntensity;      // xyz = direction, w = intensity
    glm::vec4 lightColorAmbient;      // rgb = light color, w = ambient strength
    glm::vec4 cameraPosTime;          // xyz = camera pos, w = time (seconds)
    glm::vec4 pointPos[4];            // xyz = position, w = radius
    glm::vec4 pointColorIntensity[4]; // rgb = color, w = intensity
    glm::ivec4 counts;                // x = point light count
};

// std140 uniform block shared by all post-process shaders (keep in sync with the .frag files).
struct PostUboData {
    glm::mat4 viewProj;      // current frame (incl. TAA jitter)
    glm::mat4 invViewProj;   // inverse of viewProj — depth reconstruction
    glm::mat4 prevViewProj;  // previous frame — TAA reprojection
    glm::vec4 cameraPos;     // xyz = camera world position
    glm::vec4 params;        // x = bloom strength, y = TAA retention (0=off), z = history valid, w = SSR strength
    glm::vec4 misc;          // x = camera-motion radial blur strength
    glm::vec4 internalInfo;  // x = internal W, y = internal H, z = 1/W, w = 1/H
    glm::vec4 targetSize;    // x = output W, y = output H
};

struct MeshPushConstants {
    glm::mat4 model;
    glm::vec4 albedo;
    glm::vec4 mrParams;               // metallic, roughness, emissiveStrength, shadowOn
};

struct ShadowPushConstants {
    glm::mat4 lightViewProj;
    glm::mat4 model;
};

struct GridPushConstants {
    glm::mat4 viewProj;
};

VkImageLayout GraphStateToLayout(GraphResourceState state) {
    switch (state) {
        case GraphResourceState::Undefined: return VK_IMAGE_LAYOUT_UNDEFINED;
        case GraphResourceState::General: return VK_IMAGE_LAYOUT_GENERAL;
        case GraphResourceState::ColorAttachmentWrite: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case GraphResourceState::DepthAttachmentWrite: return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        case GraphResourceState::SampledRead: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case GraphResourceState::TransferSrc: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case GraphResourceState::TransferDst: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        case GraphResourceState::Present: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        default: return VK_IMAGE_LAYOUT_GENERAL;
    }
}

VkImageAspectFlags AspectForResource(GraphResourceType type) {
    return type == GraphResourceType::DepthImage ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}

bool LoadSpvFile(const std::string& name, std::vector<uint8_t>& out) {
    std::filesystem::path path = std::filesystem::path(EngineAssetsDir()) / "shaders" / name;
    auto bytes = fs::ReadBinary(path);
    if (!bytes) {
        RAVEN_LOG_ERROR("Renderer: cannot load SPIR-V '%s' (set %s to the engine assets directory)",
                        path.string().c_str(), kEngineAssetsDirEnv);
        return false;
    }
    if (bytes->size() % 4 != 0 || bytes->size() < 20) {
        RAVEN_LOG_ERROR("Renderer: '%s' is not valid SPIR-V", path.string().c_str());
        return false;
    }
    out = std::move(*bytes);
    return true;
}

} // namespace

// =====================================================================================
// Renderer::Impl
// =====================================================================================

struct Renderer::Impl {
    vk::VulkanContext context;

    // --- window presentation ---
    std::unique_ptr<vk::Swapchain> swapchain;
    VkSurfaceKHR surface = VK_NULL_HANDLE;  // borrowed, not owned
    std::vector<VkFence> imagesInFlight;
    uint32_t requestedWidth = 0;
    uint32_t requestedHeight = 0;
    bool resizeRequested = false;

    // --- offscreen target ---
    vk::ImageResource offscreenColor;
    uint32_t shadowMapSize = 1024;
    vk::ImageResource shadowMap;
    vk::ImageResource windowDepth;
    uint32_t windowDepthWidth = 0;
    uint32_t windowDepthHeight = 0;
    vk::ImageResource offscreenDepth;
    uint32_t offscreenDepthWidth = 0;
    uint32_t offscreenDepthHeight = 0;
    uint32_t offscreenWidth = 0;
    uint32_t offscreenHeight = 0;
    VkSampler offscreenSampler = VK_NULL_HANDLE;

    // --- post-processing chain ---
    // One internal-resolution chain per slot (offscreen uses kOffscreenSlot; windowed
    // alternates 0/1). Passes: forward (with in-pass MSAA resolves) -> grid ->
    // ssao? -> ssr? -> bloom bright/blur/blur? -> composite (+TAA) -> upscale.
    struct PostChainImages {
        vk::ImageResource sceneColor;    // UNORM internal (MSAA resolve target / direct)
        vk::ImageResource sceneDepth;    // D32_SFLOAT 1x internal
        vk::ImageResource sceneMaterial; // R8G8B8A8: r=metallic g=roughness b=emissive
        vk::ImageResource litColor;      // scene * (1 - 0.75*(1-ao))  [only when SSAO on]
        vk::ImageResource ssrColor;      // lit + reflections          [only when SSR on]
        vk::ImageResource bloomA, bloomB;// half internal res
        vk::ImageResource history[2];    // ping-pong, full internal res (TAA / composite output)
        vk::ImageResource sceneColorMS, sceneDepthMS, sceneMaterialMS;  // samples>1
        uint32_t width = 0, height = 0, msaaSamples = 1;
        bool historyValid = false;
    };
    PostChainImages postChains[kTotalSlots];
    VkSampler postSamplerLinear = VK_NULL_HANDLE;
    VkSampler postSamplerNearest = VK_NULL_HANDLE;
    VkDescriptorSetLayout postSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout postPipelineLayout = VK_NULL_HANDLE;
    VkPipeline ssaoPipeline = VK_NULL_HANDLE;
    VkPipeline ssrPipeline = VK_NULL_HANDLE;
    VkPipeline bloomBrightPipeline = VK_NULL_HANDLE;
    VkPipeline bloomBlurPipeline = VK_NULL_HANDLE;
    VkPipeline compositePipeline = VK_NULL_HANDLE;
    VkPipeline upscalePipeline = VK_NULL_HANDLE;
    VkFormat upscaleFormat = VK_FORMAT_UNDEFINED;
    bool postChainPipelinesReady = false;
    // Seven per-slot descriptor sets (ssao, ssr, bright, blurH, blurV, composite, upscale),
    // all sharing one mapped UBO per slot.
    VkDescriptorSet postSets[kTotalSlots][7] = {};
    vk::BufferResource postUboBuffers[kTotalSlots];
    void* postUboMapped[kTotalSlots] = {};
    glm::mat4 prevViewProj{1.0f};
    glm::vec3 prevCameraPos{0.0f};
    bool prevCameraValid = false;
    uint32_t postFrameCounter = 0;
    // Tier-driven feature flags (cached from the quality preset at Initialize).
    uint32_t msaaSamples = 1;
    bool fxSsao = false, fxBloom = false, fxSsr = false, fxTaa = false, fxMotionBlur = false;
    float resolutionScaleEff = 1.0f;
    float lodBiasEff = 0.0f, lodMaxDistance = 250.0f;

    // --- pipelines / descriptors ---
    VkDescriptorSetLayout frameSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout materialSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout forwardPipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout gridPipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout depthPipelineLayout = VK_NULL_HANDLE;
    VkPipeline forwardPipeline = VK_NULL_HANDLE;
    VkPipeline gridPipeline = VK_NULL_HANDLE;
    VkPipeline depthPipeline = VK_NULL_HANDLE;
    VkFormat pipelineColorFormat = VK_FORMAT_UNDEFINED;
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet frameSets[kTotalSlots] = {};
    vk::BufferResource frameUboBuffers[kTotalSlots];
    void* frameUboMapped[kTotalSlots] = {};
    uint32_t frameIndex = 0;
    std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();

    vk::BufferResource gridVertexBuffer;
    uint32_t gridVertexCount = 0;

    // --- asset GPU resources ---
    struct MeshGpu {
        vk::BufferResource vertexBuffer;
        vk::BufferResource indexBuffer;
        uint32_t indexCount = 0;
        uint32_t triangleCount = 0;
        glm::vec3 boundsMin{0.0f};
        glm::vec3 boundsMax{0.0f};
    };
    struct TextureGpu {
        vk::ImageResource image;
    };
    std::unordered_map<UUID, MeshGpu, UUIDHash> meshes;
    std::unordered_map<UUID, TextureGpu, UUIDHash> textures;
    std::unordered_map<UUID, VkDescriptorSet, UUIDHash> materialSets;
    VkSampler defaultSampler = VK_NULL_HANDLE;
    VkSampler shadowSampler = VK_NULL_HANDLE;
    TextureGpu defaultWhiteTexture;
    TextureGpu defaultNormalTexture;
    VkDescriptorSet defaultMaterialSet = VK_NULL_HANDLE;

    // --- per-frame command/sync ---
    VkCommandPool commandPools[kTotalSlots] = {};
    VkCommandBuffer commandBuffers[kTotalSlots] = {};
    VkSemaphore imageAvailable[kTotalSlots] = {};
    VkSemaphore renderFinished[kTotalSlots] = {};
    VkFence inFlightFences[kTotalSlots] = {};

    // --- timestamps ---
    VkQueryPool queryPool = VK_NULL_HANDLE;
    bool queriesPending = false;
    uint32_t m_QueryExecutedMask = 0;  // bit i: pair (2i, 2i+1) was written this frame

    // --- bookkeeping ---
    FrameStats stats;
    Stopwatch frameStopwatch;
    std::unordered_set<std::string> warnedOnce;
    bool registeredUploadHandler = false;

    // ============================================================ helpers

    void WarnOnce(const std::string& key, const char* fmt, ...) {
        if (!warnedOnce.insert(key).second) return;
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        RAVEN_LOG_WARN("%s", buf);
    }

    void CreateBaseResources() {
        // Command pools for the two frames in flight.
        for (auto& pool : commandPools) {
            pool = context.CreateCommandPool(context.QueueFamilies().graphics, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
        }
        // Fences/semaphores (imageAvailable/renderFinished only used by the windowed path).
        for (uint32_t i = 0; i < kTotalSlots; ++i) {
            VkSemaphoreCreateInfo semInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            vkCreateSemaphore(context.Device(), &semInfo, nullptr, &imageAvailable[i]);
            vkCreateSemaphore(context.Device(), &semInfo, nullptr, &renderFinished[i]);
            VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
            vkCreateFence(context.Device(), &fenceInfo, nullptr, &inFlightFences[i]);

            VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocInfo.commandPool = commandPools[i];
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = 1;
            vkAllocateCommandBuffers(context.Device(), &allocInfo, &commandBuffers[i]);
        }

        // Timestamp query pool: 7 pairs (shadow, forward, ssao, ssr, bloom, composite, upscale).
        if (context.TimestampPeriod() > 0.0f) {
            VkQueryPoolCreateInfo queryInfo{};
            queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queryInfo.queryCount = 14;
            vkCreateQueryPool(context.Device(), &queryInfo, nullptr, &queryPool);
        }
    }

    void DestroyBaseResources() {
        VkDevice device = context.Device();
        for (uint32_t i = 0; i < kTotalSlots; ++i) {
            if (commandPools[i] != VK_NULL_HANDLE) vkDestroyCommandPool(device, commandPools[i], nullptr);
            if (imageAvailable[i] != VK_NULL_HANDLE) vkDestroySemaphore(device, imageAvailable[i], nullptr);
            if (renderFinished[i] != VK_NULL_HANDLE) vkDestroySemaphore(device, renderFinished[i], nullptr);
            if (inFlightFences[i] != VK_NULL_HANDLE) vkDestroyFence(device, inFlightFences[i], nullptr);
            commandPools[i] = VK_NULL_HANDLE;
            commandBuffers[i] = VK_NULL_HANDLE;
        }
        if (queryPool != VK_NULL_HANDLE) vkDestroyQueryPool(device, queryPool, nullptr);
        queryPool = VK_NULL_HANDLE;
    }

    void CreateSamplers() {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.anisotropyEnable = VK_FALSE;  // keep low-end friendly
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        vkCreateSampler(context.Device(), &samplerInfo, nullptr, &defaultSampler);
        offscreenSampler = defaultSampler;  // same filtering for the viewport

        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        vkCreateSampler(context.Device(), &samplerInfo, nullptr, &shadowSampler);

        // Post-chain samplers: clamp-to-edge; linear for color, nearest for depth.
        VkSamplerCreateInfo postInfo{};
        postInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        postInfo.magFilter = VK_FILTER_LINEAR;
        postInfo.minFilter = VK_FILTER_LINEAR;
        postInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        postInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        postInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        postInfo.maxLod = VK_LOD_CLAMP_NONE;
        vkCreateSampler(context.Device(), &postInfo, nullptr, &postSamplerLinear);
        postInfo.magFilter = VK_FILTER_NEAREST;
        postInfo.minFilter = VK_FILTER_NEAREST;
        vkCreateSampler(context.Device(), &postInfo, nullptr, &postSamplerNearest);
    }

    void CreateDefaultTextures() {
        // 4x4 white albedo
        std::vector<uint8_t> white(4 * 4 * 4, 255);
        std::vector<std::pair<uint32_t, uint32_t>> sizes{{4, 4}};
        std::vector<std::vector<uint8_t>> pixels{white};
        defaultWhiteTexture.image = vk::UploadImage(context, sizes, pixels, VK_FORMAT_R8G8B8A8_SRGB);

        // 4x4 flat normal (128,128,255)
        std::vector<uint8_t> flatNormal(4 * 4 * 4);
        for (size_t i = 0; i < 16; ++i) {
            flatNormal[i * 4 + 0] = 128;
            flatNormal[i * 4 + 1] = 128;
            flatNormal[i * 4 + 2] = 255;
            flatNormal[i * 4 + 3] = 255;
        }
        pixels = {flatNormal};
        defaultNormalTexture.image = vk::UploadImage(context, sizes, pixels, VK_FORMAT_R8G8B8A8_UNORM);
    }

    void CreateDescriptorSetLayouts() {
        // set 0: frame UBO (binding 0) + shadow map (binding 1)
        VkDescriptorSetLayoutBinding uboBinding{};
        uboBinding.binding = 0;
        uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        uboBinding.descriptorCount = 1;
        uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutBinding shadowBinding{};
        shadowBinding.binding = 1;
        shadowBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        shadowBinding.descriptorCount = 1;
        shadowBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutBinding frameBindings[] = {uboBinding, shadowBinding};
        VkDescriptorSetLayoutCreateInfo frameInfo{};
        frameInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        frameInfo.bindingCount = 2;
        frameInfo.pBindings = frameBindings;
        vkCreateDescriptorSetLayout(context.Device(), &frameInfo, nullptr, &frameSetLayout);

        // set 1: material (albedo + normal)
        VkDescriptorSetLayoutBinding albedoBinding{};
        albedoBinding.binding = 0;
        albedoBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        albedoBinding.descriptorCount = 1;
        albedoBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutBinding normalBinding{};
        normalBinding.binding = 1;
        normalBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        normalBinding.descriptorCount = 1;
        normalBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutBinding materialBindings[] = {albedoBinding, normalBinding};
        VkDescriptorSetLayoutCreateInfo materialInfo{};
        materialInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        materialInfo.bindingCount = 2;
        materialInfo.pBindings = materialBindings;
        vkCreateDescriptorSetLayout(context.Device(), &materialInfo, nullptr, &materialSetLayout);

        // Pipeline layouts
        VkPushConstantRange meshRange{};
        meshRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        meshRange.offset = 0;
        meshRange.size = sizeof(MeshPushConstants);
        VkDescriptorSetLayout sets[] = {frameSetLayout, materialSetLayout};
        VkPipelineLayoutCreateInfo forwardLayoutInfo{};
        forwardLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        forwardLayoutInfo.setLayoutCount = 2;
        forwardLayoutInfo.pSetLayouts = sets;
        forwardLayoutInfo.pushConstantRangeCount = 1;
        forwardLayoutInfo.pPushConstantRanges = &meshRange;
        vkCreatePipelineLayout(context.Device(), &forwardLayoutInfo, nullptr, &forwardPipelineLayout);

        VkPushConstantRange gridRange{};
        gridRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        gridRange.offset = 0;
        gridRange.size = sizeof(GridPushConstants);
        VkPipelineLayoutCreateInfo gridLayoutInfo{};
        gridLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        gridLayoutInfo.pushConstantRangeCount = 1;
        gridLayoutInfo.pPushConstantRanges = &gridRange;
        vkCreatePipelineLayout(context.Device(), &gridLayoutInfo, nullptr, &gridPipelineLayout);

        VkPushConstantRange shadowRange{};
        shadowRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        shadowRange.offset = 0;
        shadowRange.size = sizeof(ShadowPushConstants);
        VkPipelineLayoutCreateInfo depthLayoutInfo{};
        depthLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        depthLayoutInfo.pushConstantRangeCount = 1;
        depthLayoutInfo.pPushConstantRanges = &shadowRange;
        vkCreatePipelineLayout(context.Device(), &depthLayoutInfo, nullptr, &depthPipelineLayout);

        // set 0 (post chain): binding 0 = PostUbo, 1 = src color, 2 = depth (nearest),
        // 3 = bloom, 4 = history, 5 = material buffer.
        VkDescriptorSetLayoutBinding postUboBinding{};
        postUboBinding.binding = 0;
        postUboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        postUboBinding.descriptorCount = 1;
        postUboBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutBinding postImageBindings[5]{};
        for (uint32_t i = 0; i < 5; ++i) {
            postImageBindings[i].binding = 1 + i;
            postImageBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            postImageBindings[i].descriptorCount = 1;
            postImageBindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutBinding postBindings[6] = {postUboBinding,
            postImageBindings[0], postImageBindings[1], postImageBindings[2], postImageBindings[3], postImageBindings[4]};
        VkDescriptorSetLayoutCreateInfo postLayoutInfo{};
        postLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        postLayoutInfo.bindingCount = 6;
        postLayoutInfo.pBindings = postBindings;
        vkCreateDescriptorSetLayout(context.Device(), &postLayoutInfo, nullptr, &postSetLayout);

        // Shared post pipeline layout: 8-byte fragment push constant carries the
        // blur direction (unused by passes other than the gaussian blur).
        VkPushConstantRange postRange{};
        postRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        postRange.offset = 0;
        postRange.size = sizeof(float) * 2;
        VkPipelineLayoutCreateInfo postPipelineLayoutInfo{};
        postPipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        postPipelineLayoutInfo.setLayoutCount = 1;
        postPipelineLayoutInfo.pSetLayouts = &postSetLayout;
        postPipelineLayoutInfo.pushConstantRangeCount = 1;
        postPipelineLayoutInfo.pPushConstantRanges = &postRange;
        vkCreatePipelineLayout(context.Device(), &postPipelineLayoutInfo, nullptr, &postPipelineLayout);
    }

    void CreateDescriptorPoolAndSets() {
        VkDescriptorPoolSize poolSizes[2] = {};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[0].descriptorCount = 8 + 3 * 7;          // frame UBOs + post UBOs (7 per slot)
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[1].descriptorCount = 1024 + 3 * 7 * 5;   // material sets + post sets
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 512 + 3 * 7;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = poolSizes;
        vkCreateDescriptorPool(context.Device(), &poolInfo, nullptr, &descriptorPool);

        // Frame UBOs (one per in-flight frame), persistently mapped.
        for (uint32_t i = 0; i < kTotalSlots; ++i) {
            frameUboBuffers[i] = vk::CreateHostVisibleBuffer(context, sizeof(FrameUboData),
                                                              VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            vkMapMemory(context.Device(), frameUboBuffers[i].memory, 0, sizeof(FrameUboData), 0, &frameUboMapped[i]);
        }

        // Shadow map (depth-only image) created up front.
        CreateShadowMap(shadowMapSize);

        // Frame descriptor sets: bind UBO + shadow map.
        for (uint32_t i = 0; i < kTotalSlots; ++i) {
            VkDescriptorSetAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocInfo.descriptorPool = descriptorPool;
            allocInfo.descriptorSetCount = 1;
            allocInfo.pSetLayouts = &frameSetLayout;
            vkAllocateDescriptorSets(context.Device(), &allocInfo, &frameSets[i]);

            VkDescriptorBufferInfo bufferInfo{};
            bufferInfo.buffer = frameUboBuffers[i].buffer;
            bufferInfo.offset = 0;
            bufferInfo.range = sizeof(FrameUboData);

            VkDescriptorImageInfo shadowInfo{};
            shadowInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            shadowInfo.imageView = shadowMap.view;
            shadowInfo.sampler = shadowSampler;

            VkWriteDescriptorSet writes[2]{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = frameSets[i];
            writes[0].dstBinding = 0;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].descriptorCount = 1;
            writes[0].pBufferInfo = &bufferInfo;
            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = frameSets[i];
            writes[1].dstBinding = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].descriptorCount = 1;
            writes[1].pImageInfo = &shadowInfo;
            vkUpdateDescriptorSets(context.Device(), 2, writes, 0, nullptr);
        }

        // Post-chain UBOs (one per slot, persistently mapped) + 7 sets per slot.
        for (uint32_t i = 0; i < kTotalSlots; ++i) {
            postUboBuffers[i] = vk::CreateHostVisibleBuffer(context, sizeof(PostUboData),
                                                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            vkMapMemory(context.Device(), postUboBuffers[i].memory, 0, sizeof(PostUboData), 0, &postUboMapped[i]);

            for (uint32_t p = 0; p < 7; ++p) {
                VkDescriptorSetAllocateInfo allocInfo{};
                allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                allocInfo.descriptorPool = descriptorPool;
                allocInfo.descriptorSetCount = 1;
                allocInfo.pSetLayouts = &postSetLayout;
                vkAllocateDescriptorSets(context.Device(), &allocInfo, &postSets[i][p]);

                VkDescriptorBufferInfo bufferInfo{};
                bufferInfo.buffer = postUboBuffers[i].buffer;
                bufferInfo.offset = 0;
                bufferInfo.range = sizeof(PostUboData);
                VkWriteDescriptorSet uboWrite{};
                uboWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                uboWrite.dstSet = postSets[i][p];
                uboWrite.dstBinding = 0;
                uboWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                uboWrite.descriptorCount = 1;
                uboWrite.pBufferInfo = &bufferInfo;
                vkUpdateDescriptorSets(context.Device(), 1, &uboWrite, 0, nullptr);
            }
        }

        // Default material set: white albedo + flat normal.
        defaultMaterialSet = AllocateMaterialSet(VK_NULL_HANDLE, VK_NULL_HANDLE);
    }

    void CreateShadowMap(uint32_t size) {
        shadowMap.format = VK_FORMAT_D32_SFLOAT;
        shadowMap.width = size;
        shadowMap.height = size;
        shadowMap.mipLevels = 1;
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = {size, size, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = shadowMap.format;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        vkCreateImage(context.Device(), &imageInfo, nullptr, &shadowMap.image);

        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(context.Device(), shadowMap.image, &req);
        VkPhysicalDeviceMemoryProperties memProps{};
        vkGetPhysicalDeviceMemoryProperties(context.PhysicalDevice(), &memProps);
        uint32_t typeIndex = vk::FindMemoryType(memProps, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = req.size;
        allocInfo.memoryTypeIndex = typeIndex;
        vkAllocateMemory(context.Device(), &allocInfo, nullptr, &shadowMap.memory);
        vkBindImageMemory(context.Device(), shadowMap.image, shadowMap.memory, 0);
        shadowMap.view = vk::CreateImageView(context, shadowMap.image, shadowMap.format, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1);
    }

    VkDescriptorSet AllocateMaterialSet(VkDescriptorSet /*unused*/, VkDescriptorSet /*unused2*/) {
        // Allocates a material set bound to the default textures; texture-specific
        // sets are built by EnsureMaterialSet(materialUuid).
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = descriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &materialSetLayout;
        vkAllocateDescriptorSets(context.Device(), &allocInfo, &set);

        VkDescriptorImageInfo albedoInfo{};
        albedoInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        albedoInfo.imageView = defaultWhiteTexture.image.view;
        albedoInfo.sampler = defaultSampler;
        VkDescriptorImageInfo normalInfo{};
        normalInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        normalInfo.imageView = defaultNormalTexture.image.view;
        normalInfo.sampler = defaultSampler;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = set;
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo = &albedoInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = set;
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &normalInfo;
        vkUpdateDescriptorSets(context.Device(), 2, writes, 0, nullptr);
        return set;
    }

    VkDescriptorSet EnsureMaterialSet(const UUID& materialUuid) {
        auto it = materialSets.find(materialUuid);
        if (it != materialSets.end()) return it->second;

        // Resolve material asset (blocking is acceptable here: editor only).
        auto material = AssetManager::Instance().LoadImmediate(materialUuid);
        auto materialAsset = std::dynamic_pointer_cast<MaterialAsset>(material.asset);
        UUID albedoId = materialAsset ? materialAsset->albedoTexture : UUID::Nil();
        UUID normalId = materialAsset ? materialAsset->normalTexture : UUID::Nil();

        const vk::ImageResource* albedoRes = nullptr;
        const vk::ImageResource* normalRes = nullptr;
        if (!albedoId.IsNil()) {
            auto textureAsset = AssetManager::Instance().LoadImmediate(albedoId);
            if (textureAsset.asset) CreateTextureGpuIfMissing(albedoId, *std::dynamic_pointer_cast<TextureAsset>(textureAsset.asset));
            auto tit = textures.find(albedoId);
            if (tit != textures.end()) albedoRes = &tit->second.image;
        }
        if (!normalId.IsNil()) {
            auto textureAsset = AssetManager::Instance().LoadImmediate(normalId);
            if (textureAsset.asset) CreateTextureGpuIfMissing(normalId, *std::dynamic_pointer_cast<TextureAsset>(textureAsset.asset));
            auto tit = textures.find(normalId);
            if (tit != textures.end()) normalRes = &tit->second.image;
        }
        if (!albedoRes) albedoRes = &defaultWhiteTexture.image;
        if (!normalRes) normalRes = &defaultNormalTexture.image;

        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = descriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &materialSetLayout;
        vkAllocateDescriptorSets(context.Device(), &allocInfo, &set);

        VkDescriptorImageInfo albedoInfo{};
        albedoInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        albedoInfo.imageView = albedoRes->view;
        albedoInfo.sampler = defaultSampler;
        VkDescriptorImageInfo normalInfo{};
        normalInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        normalInfo.imageView = normalRes->view;
        normalInfo.sampler = defaultSampler;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = set;
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo = &albedoInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = set;
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &normalInfo;
        vkUpdateDescriptorSets(context.Device(), 2, writes, 0, nullptr);
        materialSets.emplace(materialUuid, set);
        return set;
    }

    void InvalidateMaterialSets() {
        if (!materialSets.empty()) vkDeviceWaitIdle(context.Device());
        for (auto& [uuid, set] : materialSets) {
            vkFreeDescriptorSets(context.Device(), descriptorPool, 1, &set);
        }
        materialSets.clear();
    }

    void CreateTextureGpuIfMissing(const UUID& uuid, const TextureAsset& asset) {
        if (textures.find(uuid) != textures.end()) return;
        TextureGpu gpu;
        if (CreateTextureGpu(asset, gpu)) textures.emplace(uuid, std::move(gpu));
    }

    bool CreateTextureGpu(const TextureAsset& asset, TextureGpu& out) {
        if (asset.mips.empty()) return false;
        std::vector<std::pair<uint32_t, uint32_t>> sizes;
        std::vector<std::vector<uint8_t>> pixels;
        sizes.reserve(asset.mips.size());
        pixels.reserve(asset.mips.size());
        for (const auto& mip : asset.mips) {
            sizes.emplace_back(mip.width, mip.height);
            pixels.push_back(mip.pixels);
        }
        VkFormat format = asset.isSrgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        out.image = vk::UploadImage(context, sizes, pixels, format);
        if (out.image.image == VK_NULL_HANDLE) {
            RAVEN_LOG_ERROR("Renderer: failed uploading texture '%s'", asset.uuid.ToString().c_str());
            return false;
        }
        return true;
    }

    bool CreateMeshGpu(const MeshAsset& asset, MeshGpu& out) {
        if (asset.vertices.empty() || asset.indices.empty()) return false;
        out.vertexBuffer = vk::UploadBuffer(context, asset.vertices.data(),
                                            asset.vertices.size() * sizeof(MeshVertex),
                                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        out.indexBuffer = vk::UploadBuffer(context, asset.indices.data(),
                                           asset.indices.size() * sizeof(uint32_t),
                                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        if (out.vertexBuffer.buffer == VK_NULL_HANDLE || out.indexBuffer.buffer == VK_NULL_HANDLE) return false;
        out.indexCount = static_cast<uint32_t>(asset.indices.size());
        out.triangleCount = asset.triangleCount;
        out.boundsMin = asset.boundsMin;
        out.boundsMax = asset.boundsMax;
        return true;
    }

    void OnAssetUploaded(const std::shared_ptr<Asset>& asset) {
        if (!asset) return;
        switch (asset->type) {
            case AssetType::Texture: {
                auto* tex = static_cast<TextureAsset*>(asset.get());
                // Replace existing resource on hot reload.
                auto it = textures.find(tex->uuid);
                if (it != textures.end()) {
                    it->second.image.Destroy(context.Device());
                    textures.erase(it);
                }
                TextureGpu gpu;
                if (CreateTextureGpu(*tex, gpu)) textures.emplace(tex->uuid, std::move(gpu));
                InvalidateMaterialSets();
                break;
            }
            case AssetType::Mesh: {
                auto* mesh = static_cast<MeshAsset*>(asset.get());
                auto it = meshes.find(mesh->uuid);
                if (it != meshes.end()) {
                    it->second.vertexBuffer.Destroy(context.Device());
                    it->second.indexBuffer.Destroy(context.Device());
                    meshes.erase(it);
                }
                MeshGpu gpu;
                if (CreateMeshGpu(*mesh, gpu)) meshes.emplace(mesh->uuid, std::move(gpu));
                break;
            }
            case AssetType::Shader:
                // Pipeline hot-reload: force recreation on next frame.
                if (forwardPipeline != VK_NULL_HANDLE) {
                    vkDeviceWaitIdle(context.Device());
                    InvalidatePipelines();
                }
                break;
            case AssetType::Material:
                InvalidateMaterialSets();
                break;
            default:
                break;
        }
    }

    void InvalidatePipelines() {
        VkDevice device = context.Device();
        if (forwardPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, forwardPipeline, nullptr);
        if (gridPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, gridPipeline, nullptr);
        if (depthPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, depthPipeline, nullptr);
        if (ssaoPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, ssaoPipeline, nullptr);
        if (ssrPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, ssrPipeline, nullptr);
        if (bloomBrightPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, bloomBrightPipeline, nullptr);
        if (bloomBlurPipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, bloomBlurPipeline, nullptr);
        if (compositePipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, compositePipeline, nullptr);
        if (upscalePipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, upscalePipeline, nullptr);
        forwardPipeline = VK_NULL_HANDLE;
        gridPipeline = VK_NULL_HANDLE;
        depthPipeline = VK_NULL_HANDLE;
        ssaoPipeline = VK_NULL_HANDLE;
        ssrPipeline = VK_NULL_HANDLE;
        bloomBrightPipeline = VK_NULL_HANDLE;
        bloomBlurPipeline = VK_NULL_HANDLE;
        compositePipeline = VK_NULL_HANDLE;
        upscalePipeline = VK_NULL_HANDLE;
        postChainPipelinesReady = false;
        pipelineColorFormat = VK_FORMAT_UNDEFINED;
    }

    // --- frame recording (defined below) ---
    struct FrameTargets {
        VkImage colorImage = VK_NULL_HANDLE;
        VkImageView colorView = VK_NULL_HANDLE;
        VkImage depthImage = VK_NULL_HANDLE;   // or offscreenDepth/windowDepth
        VkImageView depthView = VK_NULL_HANDLE;
        uint32_t width = 0, height = 0;
        bool toPresent = false;                // final barrier: PRESENT_SRC vs SHADER_READ_ONLY
        VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    };
    void EnsurePipelines(VkFormat colorFormat);
    void EnsurePostChain(uint32_t slot, uint32_t width, uint32_t height);
    void DestroyPostChain(uint32_t slot);
    void ReadQueryResults();
    void EnsureOffscreenTarget(uint32_t width, uint32_t height);
    void EnsureWindowDepth(uint32_t width, uint32_t height);
    void ApplyBarrier(VkCommandBuffer cb, const BarrierOp& op, VkImage image);
    void RecordFrame(VkCommandBuffer cb, const FrameTargets& targets, Scene& scene, const Camera& camera,
                     const FrameParams& params, uint32_t slot, FrameStats& outStats);
    VkCommandBuffer BeginSlot(uint32_t slot, bool waitFence);
    void SubmitSlot(uint32_t slot, VkSemaphore waitSemaphore, VkSemaphore signalSemaphore);
    MeshGpu& GetOrCreateMeshGpu(const UUID& uuid);

    void CreateGridMesh() {
        // 41x2 lines (every 1 unit over [-20,20] on X and Z), axes highlighted.
        const int half = 20;
        const int step = 1;
        std::vector<float> vertices;
        auto pushVertex = [&](float x, float y, float z, float r, float g, float b) {
            vertices.insert(vertices.end(), {x, y, z, r, g, b});
        };
        for (int i = -half; i <= half; i += step) {
            float f = static_cast<float>(i);
            bool axis = (i == 0);
            // X-parallel line (varying x at fixed z=f)
            float r = axis ? 0.75f : 0.28f, g = axis ? 0.3f : 0.28f, b = axis ? 0.3f : 0.28f;
            pushVertex(-half, 0.0f, f, r, g, b);
            pushVertex(half, 0.0f, f, r, g, b);
            // Z-parallel line
            r = axis ? 0.3f : 0.28f; g = axis ? 0.3f : 0.28f; b = axis ? 0.75f : 0.28f;
            pushVertex(f, 0.0f, -half, r, g, b);
            pushVertex(f, 0.0f, half, r, g, b);
        }
        gridVertexCount = static_cast<uint32_t>(vertices.size() / 6);
        gridVertexBuffer = vk::UploadBuffer(context, vertices.data(), vertices.size() * sizeof(float),
                                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    }
};

} // namespace raven

namespace raven {

// =====================================================================================
// Renderer::Impl — pipeline building + frame recording
// =====================================================================================

namespace {

VkShaderModule CreateShaderModule(const vk::VulkanContext& context, const std::vector<uint8_t>& spirv) {
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = spirv.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(spirv.data());
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(context.Device(), &createInfo, nullptr, &module) != VK_SUCCESS) return VK_NULL_HANDLE;
    return module;
}

} // namespace

void Renderer::Impl::EnsurePipelines(VkFormat colorFormat) {
    const VkSampleCountFlagBits forwardSamples = static_cast<VkSampleCountFlagBits>(msaaSamples);
    const VkFormat kPostFormat = VK_FORMAT_B8G8R8A8_UNORM;
    const VkFormat kMaterialFormat = VK_FORMAT_R8G8B8A8_UNORM;

    VkPipelineDepthStencilStateCreateInfo noDepth{};
    noDepth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    noDepth.depthTestEnable = VK_FALSE;
    noDepth.depthWriteEnable = VK_FALSE;
    noDepth.depthCompareOp = VK_COMPARE_OP_ALWAYS;

    auto stageInfo = [](VkShaderModule module, VkShaderStageFlagBits stage) {
        VkPipelineShaderStageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage = stage;
        info.module = module;
        info.pName = "main";
        return info;
    };
    auto dynamicViewportScissor = [] {
        VkPipelineDynamicStateCreateInfo dynamicState{};
        static VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = 2;
        dynamicState.pDynamicStates = dynamicStates;
        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        return std::pair{dynamicState, viewportState};
    };
    auto triangleListAssembly = [] {
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        return inputAssembly;
    };
    auto fillRaster = [] {
        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.cullMode = VK_CULL_MODE_NONE;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.lineWidth = 1.0f;
        return rasterizer;
    };

    // Builds a fullscreen-triangle post pipeline with a single color attachment.
    auto makePostPipeline = [&](const char* fragName, VkFormat format, VkPipeline* out) {
        std::vector<uint8_t> vertSpv, fragSpv;
        if (!LoadSpvFile("post.vert.spv", vertSpv) || !LoadSpvFile(fragName, fragSpv)) {
            RAVEN_LOG_ERROR("Renderer: post shader '%s' missing — pass disabled", fragName);
            return false;
        }
        VkShaderModule vertModule = CreateShaderModule(context, vertSpv);
        VkShaderModule fragModule = CreateShaderModule(context, fragSpv);

        VkPipelineShaderStageCreateInfo stages[2] = {stageInfo(vertModule, VK_SHADER_STAGE_VERTEX_BIT),
                                                     stageInfo(fragModule, VK_SHADER_STAGE_FRAGMENT_BIT)};
        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo inputAssembly = triangleListAssembly();
        auto [dynamicState, viewportState] = dynamicViewportScissor();
        VkPipelineRasterizationStateCreateInfo rasterizer = fillRaster();
        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments = &blendAttachment;

        VkPipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachmentFormats = &format;

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.pNext = &renderingInfo;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &noDepth;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = postPipelineLayout;

        VkResult result = vkCreateGraphicsPipelines(context.Device(), pipelineCache, 1, &pipelineInfo, nullptr, out);
        vkDestroyShaderModule(context.Device(), vertModule, nullptr);
        vkDestroyShaderModule(context.Device(), fragModule, nullptr);
        if (result != VK_SUCCESS) {
            RAVEN_LOG_ERROR("Renderer: failed creating post pipeline for '%s': %s", fragName, vk::VkResultToString(result));
            return false;
        }
        return true;
    };

    // --- forward pipeline: MRT (color + material), fixed internal UNORM formats, optional MSAA ---
    if (forwardPipeline == VK_NULL_HANDLE) {
        std::vector<uint8_t> vertSpv, fragSpv;
        if (!LoadSpvFile("forward.vert.spv", vertSpv) || !LoadSpvFile("forward.frag.spv", fragSpv)) {
            RAVEN_LOG_ERROR("Renderer: forward shader SPIR-V missing — pipeline not created");
            return;
        }
        VkShaderModule vertModule = CreateShaderModule(context, vertSpv);
        VkShaderModule fragModule = CreateShaderModule(context, fragSpv);

        VkPipelineShaderStageCreateInfo stages[2] = {stageInfo(vertModule, VK_SHADER_STAGE_VERTEX_BIT),
                                                     stageInfo(fragModule, VK_SHADER_STAGE_FRAGMENT_BIT)};

        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(MeshVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription attributes[3]{};
        attributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, position)};
        attributes[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, normal)};
        attributes[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(MeshVertex, uv)};

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 3;
        vertexInput.pVertexAttributeDescriptions = attributes;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly = triangleListAssembly();
        auto [dynamicState, viewportState] = dynamicViewportScissor();

        VkPipelineRasterizationStateCreateInfo rasterizer = fillRaster();
        rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;

        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.rasterizationSamples = forwardSamples;

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

        VkPipelineColorBlendAttachmentState blendAttachments[2]{};
        for (auto& a : blendAttachments) {
            a.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        }
        VkPipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = 2;
        colorBlend.pAttachments = blendAttachments;

        const VkFormat formats[2] = {kPostFormat, kMaterialFormat};
        VkPipelineRenderingCreateInfo renderingInfo{};
        renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        renderingInfo.colorAttachmentCount = 2;
        renderingInfo.pColorAttachmentFormats = formats;
        renderingInfo.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.pNext = &renderingInfo;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = forwardPipelineLayout;
        pipelineInfo.renderPass = VK_NULL_HANDLE;

        VkResult result = vkCreateGraphicsPipelines(context.Device(), pipelineCache, 1, &pipelineInfo, nullptr, &forwardPipeline);
        vkDestroyShaderModule(context.Device(), vertModule, nullptr);
        vkDestroyShaderModule(context.Device(), fragModule, nullptr);
        if (result != VK_SUCCESS) {
            RAVEN_LOG_ERROR("Renderer: failed creating forward pipeline: %s", vk::VkResultToString(result));
            return;
        }
    }

    // --- grid pipeline: line list into the resolved 1x internal target (LOAD both) ---
    if (gridPipeline == VK_NULL_HANDLE) {
        std::vector<uint8_t> vertSpv, fragSpv;
        if (LoadSpvFile("grid.vert.spv", vertSpv) && LoadSpvFile("grid.frag.spv", fragSpv)) {
            VkShaderModule vertModule = CreateShaderModule(context, vertSpv);
            VkShaderModule fragModule = CreateShaderModule(context, fragSpv);

            VkPipelineShaderStageCreateInfo stages[2] = {stageInfo(vertModule, VK_SHADER_STAGE_VERTEX_BIT),
                                                         stageInfo(fragModule, VK_SHADER_STAGE_FRAGMENT_BIT)};

            VkVertexInputBindingDescription binding{};
            binding.binding = 0;
            binding.stride = 6 * sizeof(float);
            binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            VkVertexInputAttributeDescription attributes[2]{};
            attributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
            attributes[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 3 * sizeof(float)};

            VkPipelineVertexInputStateCreateInfo vertexInput{};
            vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            vertexInput.vertexBindingDescriptionCount = 1;
            vertexInput.pVertexBindingDescriptions = &binding;
            vertexInput.vertexAttributeDescriptionCount = 2;
            vertexInput.pVertexAttributeDescriptions = attributes;

            VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
            inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

            auto [dynamicState, viewportState] = dynamicViewportScissor();
            VkPipelineRasterizationStateCreateInfo rasterizer = fillRaster();

            VkPipelineMultisampleStateCreateInfo multisampling{};
            multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

            VkPipelineDepthStencilStateCreateInfo depthStencil{};
            depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            depthStencil.depthTestEnable = VK_TRUE;
            depthStencil.depthWriteEnable = VK_FALSE;
            depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

            VkPipelineColorBlendAttachmentState blendAttachment{};
            blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo colorBlend{};
            colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            colorBlend.attachmentCount = 1;
            colorBlend.pAttachments = &blendAttachment;

            VkPipelineRenderingCreateInfo renderingInfo{};
            renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
            renderingInfo.colorAttachmentCount = 1;
            renderingInfo.pColorAttachmentFormats = &kPostFormat;
            renderingInfo.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

            VkGraphicsPipelineCreateInfo pipelineInfo{};
            pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            pipelineInfo.pNext = &renderingInfo;
            pipelineInfo.stageCount = 2;
            pipelineInfo.pStages = stages;
            pipelineInfo.pVertexInputState = &vertexInput;
            pipelineInfo.pInputAssemblyState = &inputAssembly;
            pipelineInfo.pViewportState = &viewportState;
            pipelineInfo.pRasterizationState = &rasterizer;
            pipelineInfo.pMultisampleState = &multisampling;
            pipelineInfo.pDepthStencilState = &depthStencil;
            pipelineInfo.pColorBlendState = &colorBlend;
            pipelineInfo.pDynamicState = &dynamicState;
            pipelineInfo.layout = gridPipelineLayout;
            vkCreateGraphicsPipelines(context.Device(), pipelineCache, 1, &pipelineInfo, nullptr, &gridPipeline);
            vkDestroyShaderModule(context.Device(), vertModule, nullptr);
            vkDestroyShaderModule(context.Device(), fragModule, nullptr);
        }
    }

    // --- depth-only pipeline (shadow map) ---
    if (depthPipeline == VK_NULL_HANDLE) {
        std::vector<uint8_t> vertSpv;
        if (LoadSpvFile("depth.vert.spv", vertSpv)) {
            VkShaderModule vertModule = CreateShaderModule(context, vertSpv);

            VkPipelineShaderStageCreateInfo stage = stageInfo(vertModule, VK_SHADER_STAGE_VERTEX_BIT);

            VkVertexInputBindingDescription binding{};
            binding.binding = 0;
            binding.stride = sizeof(MeshVertex);
            binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            VkVertexInputAttributeDescription attribute{};
            attribute = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, position)};

            VkPipelineVertexInputStateCreateInfo vertexInput{};
            vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            vertexInput.vertexBindingDescriptionCount = 1;
            vertexInput.pVertexBindingDescriptions = &binding;
            vertexInput.vertexAttributeDescriptionCount = 1;
            vertexInput.pVertexAttributeDescriptions = &attribute;

            VkPipelineInputAssemblyStateCreateInfo inputAssembly = triangleListAssembly();
            auto [dynamicState, viewportState] = dynamicViewportScissor();

            VkPipelineRasterizationStateCreateInfo rasterizer = fillRaster();
            rasterizer.depthBiasEnable = VK_TRUE;
            rasterizer.depthBiasConstantFactor = 1.25f;
            rasterizer.depthBiasSlopeFactor = 1.75f;

            VkPipelineMultisampleStateCreateInfo multisampling{};
            multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

            VkPipelineDepthStencilStateCreateInfo depthStencil{};
            depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            depthStencil.depthTestEnable = VK_TRUE;
            depthStencil.depthWriteEnable = VK_TRUE;
            depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

            VkPipelineRenderingCreateInfo renderingInfo{};
            renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
            renderingInfo.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

            VkGraphicsPipelineCreateInfo pipelineInfo{};
            pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            pipelineInfo.pNext = &renderingInfo;
            pipelineInfo.stageCount = 1;
            pipelineInfo.pStages = &stage;
            pipelineInfo.pVertexInputState = &vertexInput;
            pipelineInfo.pInputAssemblyState = &inputAssembly;
            pipelineInfo.pViewportState = &viewportState;
            pipelineInfo.pRasterizationState = &rasterizer;
            pipelineInfo.pMultisampleState = &multisampling;
            pipelineInfo.pDepthStencilState = &depthStencil;
            pipelineInfo.pDynamicState = &dynamicState;
            pipelineInfo.layout = depthPipelineLayout;
            vkCreateGraphicsPipelines(context.Device(), pipelineCache, 1, &pipelineInfo, nullptr, &depthPipeline);
            vkDestroyShaderModule(context.Device(), vertModule, nullptr);
        }
    }

    // --- post chain (internal UNORM formats; created once) ---
    if (!postChainPipelinesReady) {
        const bool ssaoOk = makePostPipeline("ssao.frag.spv", kPostFormat, &ssaoPipeline);
        const bool ssrOk = makePostPipeline("ssr.frag.spv", kPostFormat, &ssrPipeline);
        const bool brightOk = makePostPipeline("bloom_bright.frag.spv", kPostFormat, &bloomBrightPipeline);
        const bool blurOk = makePostPipeline("bloom_blur.frag.spv", kPostFormat, &bloomBlurPipeline);
        const bool compositeOk = makePostPipeline("composite.frag.spv", kPostFormat, &compositePipeline);
        if (ssaoOk && ssrOk && brightOk && blurOk && compositeOk) {
            postChainPipelinesReady = true;
        } else {
            WarnOnce("postfx-incomplete", "Renderer: one or more post pipelines failed — affected passes will be skipped this frame");
        }
    }

    // --- upscale pipeline (depends on the target format: offscreen SRGB / swapchain) ---
    if (upscalePipeline == VK_NULL_HANDLE || upscaleFormat != colorFormat) {
        if (upscalePipeline != VK_NULL_HANDLE) vkDestroyPipeline(context.Device(), upscalePipeline, nullptr);
        upscalePipeline = VK_NULL_HANDLE;
        if (makePostPipeline("upscale.frag.spv", colorFormat, &upscalePipeline)) {
            upscaleFormat = colorFormat;
        } else {
            upscaleFormat = VK_FORMAT_UNDEFINED;
        }
    }

    pipelineColorFormat = colorFormat;
    RAVEN_LOG_INFO("Renderer: pipelines ready (forward MSAA=%ux, upscale format=%d)", msaaSamples, static_cast<int>(colorFormat));
}

void Renderer::Impl::ReadQueryResults() {
    if (queryPool == VK_NULL_HANDLE || !queriesPending) return;
    // Read back only the pairs that were written last frame; unwritten queries never
    // become available and would stall VK_QUERY_RESULT_WAIT_BIT forever.
    uint64_t results[14] = {};
    // Timestamp pairs: 0 shadow, 1 forward, 2 ssao, 3 ssr, 4 bloom, 5 composite, 6 upscale.
    static const char* kNames[7] = {"shadow", "forward", "ssao", "ssr", "bloom", "composite", "upscale"};
    stats.passTimes.clear();
    float total = 0.0f;
    for (uint32_t pass = 0; pass < 7; ++pass) {
        if (!(m_QueryExecutedMask & (1u << pass))) continue;
        VkResult res = vkGetQueryPoolResults(context.Device(), queryPool, pass * 2, 2,
                                             sizeof(uint64_t) * 2, &results[pass * 2], sizeof(uint64_t),
                                             VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        if (res != VK_SUCCESS) continue;  // keep previous value for this pair
        const uint64_t dt = results[pass * 2 + 1] - results[pass * 2];
        const float ms = static_cast<float>(static_cast<double>(dt) * context.TimestampPeriod() / 1e6);
        stats.passTimes.push_back({kNames[pass], ms});
        total += ms;
    }
    stats.gpuFrameMs = total;
    queriesPending = false;
}

} // namespace raven

namespace raven {

// =====================================================================================
// Renderer::Impl — dynamic rendering barriers + frame recording
// =====================================================================================

namespace {

VkPipelineStageFlags2 StageMaskFor(GraphResourceState state, bool isDst) {
    switch (state) {
        case GraphResourceState::ColorAttachmentWrite:
            return isDst ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        case GraphResourceState::DepthAttachmentWrite:
            return VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        case GraphResourceState::SampledRead:
            return VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        case GraphResourceState::TransferDst:
        case GraphResourceState::TransferSrc:
            return VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        case GraphResourceState::General:
            return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        case GraphResourceState::Present:
            return VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
        default:
            return isDst ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
    }
}

VkAccessFlags2 AccessMaskFor(GraphResourceState state, bool isDst) {
    switch (state) {
        case GraphResourceState::ColorAttachmentWrite:
            return VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        case GraphResourceState::DepthAttachmentWrite:
            return VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        case GraphResourceState::SampledRead:
            return VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        case GraphResourceState::TransferDst:
            return VK_ACCESS_2_TRANSFER_WRITE_BIT;
        case GraphResourceState::TransferSrc:
            return VK_ACCESS_2_TRANSFER_READ_BIT;
        case GraphResourceState::General:
            return VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        default:
            return 0;
    }
}

} // namespace

vk::ImageResource CreateColorImage2D(vk::VulkanContext& context, uint32_t width, uint32_t height, VkFormat format,
                                     VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT,
                                     VkImageUsageFlags extraUsage = 0) {
    vk::ImageResource res;
    res.format = format;
    res.width = width;
    res.height = height;
    res.mipLevels = 1;
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | extraUsage;
    if (samples == VK_SAMPLE_COUNT_1_BIT) imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = samples;
    vkCreateImage(context.Device(), &imageInfo, nullptr, &res.image);
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(context.Device(), res.image, &req);
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(context.PhysicalDevice(), &memProps);
    uint32_t typeIndex = vk::FindMemoryType(memProps, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = req.size;
    allocInfo.memoryTypeIndex = typeIndex;
    vkAllocateMemory(context.Device(), &allocInfo, nullptr, &res.memory);
    vkBindImageMemory(context.Device(), res.image, res.memory, 0);
    res.view = vk::CreateImageView(context, res.image, format, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);
    return res;
}

vk::ImageResource CreateDepthImage2D(vk::VulkanContext& context, uint32_t width, uint32_t height,
                                     VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT,
                                     VkImageUsageFlags extraUsage = 0) {
    vk::ImageResource res;
    res.format = VK_FORMAT_D32_SFLOAT;
    res.width = width;
    res.height = height;
    res.mipLevels = 1;
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = res.format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | extraUsage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = samples;
    vkCreateImage(context.Device(), &imageInfo, nullptr, &res.image);
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(context.Device(), res.image, &req);
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(context.PhysicalDevice(), &memProps);
    uint32_t typeIndex = vk::FindMemoryType(memProps, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = req.size;
    allocInfo.memoryTypeIndex = typeIndex;
    vkAllocateMemory(context.Device(), &allocInfo, nullptr, &res.memory);
    vkBindImageMemory(context.Device(), res.image, res.memory, 0);
    res.view = vk::CreateImageView(context, res.image, res.format, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1);
    return res;
}

void Renderer::Impl::DestroyPostChain(uint32_t slot) {
    PostChainImages& chain = postChains[slot];
    VkDevice device = context.Device();
    auto destroyOne = [&](vk::ImageResource& res) { if (res.image != VK_NULL_HANDLE) res.Destroy(device); };
    destroyOne(chain.sceneColor); destroyOne(chain.sceneDepth); destroyOne(chain.sceneMaterial);
    destroyOne(chain.litColor); destroyOne(chain.ssrColor);
    destroyOne(chain.bloomA); destroyOne(chain.bloomB);
    destroyOne(chain.history[0]); destroyOne(chain.history[1]);
    destroyOne(chain.sceneColorMS); destroyOne(chain.sceneDepthMS); destroyOne(chain.sceneMaterialMS);
    chain = PostChainImages{};
}

void Renderer::Impl::EnsurePostChain(uint32_t slot, uint32_t width, uint32_t height) {
    PostChainImages& chain = postChains[slot];
    const uint32_t wantMsaa = msaaSamples;
    if (chain.sceneColor.image != VK_NULL_HANDLE && chain.width == width && chain.height == height &&
        chain.msaaSamples == wantMsaa) {
        return;
    }
    vkDeviceWaitIdle(context.Device());
    DestroyPostChain(slot);
    auto msaaBit = static_cast<VkSampleCountFlagBits>(wantMsaa);
    const bool msaa = wantMsaa > 1;

    chain.width = width;
    chain.height = height;
    chain.msaaSamples = wantMsaa;

    if (msaa) {
        // MSAA color/material render targets (resolve attachments hold the 1x results).
        chain.sceneColorMS = CreateColorImage2D(context, width, height, VK_FORMAT_B8G8R8A8_UNORM, msaaBit, 0);
        chain.sceneMaterialMS = CreateColorImage2D(context, width, height, VK_FORMAT_R8G8B8A8_UNORM, msaaBit, 0);
        // Depth MS target is resolved in-pass (SAMPLE_ZERO) into the 1x depth sampled by SSAO/SSR/TAA.
        // Support is guaranteed here — EnsurePostChain is only reached when the device
        // exposes VK_KHR_depth_stencil_resolve (MSAA is forced off otherwise at Initialize).
        chain.sceneDepthMS = CreateDepthImage2D(context, width, height, msaaBit, 0);
    }
    chain.sceneColor = CreateColorImage2D(context, width, height, VK_FORMAT_B8G8R8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, 0);
    chain.sceneDepth = CreateDepthImage2D(context, width, height, VK_SAMPLE_COUNT_1_BIT, 0);
    chain.sceneMaterial = CreateColorImage2D(context, width, height, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, 0);
    chain.litColor = CreateColorImage2D(context, width, height, VK_FORMAT_B8G8R8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    chain.ssrColor = CreateColorImage2D(context, width, height, VK_FORMAT_B8G8R8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    const uint32_t halfW = std::max(1u, width / 2), halfH = std::max(1u, height / 2);
    chain.bloomA = CreateColorImage2D(context, halfW, halfH, VK_FORMAT_B8G8R8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, 0);
    chain.bloomB = CreateColorImage2D(context, halfW, halfH, VK_FORMAT_B8G8R8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, 0);
    chain.history[0] = CreateColorImage2D(context, width, height, VK_FORMAT_B8G8R8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, 0);
    chain.history[1] = CreateColorImage2D(context, width, height, VK_FORMAT_B8G8R8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, 0);
    chain.historyValid = false;
}

void Renderer::Impl::EnsureOffscreenTarget(uint32_t width, uint32_t height) {
    bool colorOk = offscreenColor.image != VK_NULL_HANDLE && offscreenWidth == width && offscreenHeight == height;
    bool depthOk = offscreenDepth.image != VK_NULL_HANDLE && offscreenDepthWidth == width && offscreenDepthHeight == height;
    if (colorOk && depthOk) return;
    vkDeviceWaitIdle(context.Device());
    if (!colorOk) {
        offscreenColor.Destroy(context.Device());
        offscreenColor = CreateColorImage2D(context, width, height, VK_FORMAT_B8G8R8A8_SRGB);
        offscreenWidth = width;
        offscreenHeight = height;
    }
    if (!depthOk) {
        offscreenDepth.Destroy(context.Device());
        offscreenDepth = CreateDepthImage2D(context, width, height);
        offscreenDepthWidth = width;
        offscreenDepthHeight = height;
    }
    EnsurePipelines(offscreenColor.format);
}

void Renderer::Impl::EnsureWindowDepth(uint32_t width, uint32_t height) {
    if (windowDepth.image != VK_NULL_HANDLE && windowDepthWidth == width && windowDepthHeight == height) return;
    vkDeviceWaitIdle(context.Device());
    windowDepth.Destroy(context.Device());
    windowDepth = CreateDepthImage2D(context, width, height);
    windowDepthWidth = width;
    windowDepthHeight = height;
}

void Renderer::Impl::ApplyBarrier(VkCommandBuffer cb, const BarrierOp& op, VkImage image) {
    VkImageAspectFlags aspect = AspectForResource(op.type);
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = StageMaskFor(op.from, /*isDst*/false);
    barrier.srcAccessMask = AccessMaskFor(op.from, /*isDst*/false);
    barrier.dstStageMask = StageMaskFor(op.to, /*isDst*/true);
    barrier.dstAccessMask = AccessMaskFor(op.to, /*isDst*/true);
    barrier.oldLayout = GraphStateToLayout(op.from);
    barrier.newLayout = GraphStateToLayout(op.to);
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {aspect, 0, 1, 0, 1};

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cb, &dependency);
}


} // namespace raven

namespace raven {

// =====================================================================================
// Renderer::Impl — slot acquire / submit helpers
// =====================================================================================

VkCommandBuffer Renderer::Impl::BeginSlot(uint32_t slot, bool waitFence) {
    VkDevice device = context.Device();
    if (waitFence) {
        vkWaitForFences(device, 1, &inFlightFences[slot], VK_TRUE, UINT64_MAX);
        ReadQueryResults();  // previous work in this slot is complete; results are ready
        vkResetFences(device, 1, &inFlightFences[slot]);
    }
    vkResetCommandPool(device, commandPools[slot], 0);
    VkCommandBuffer cb = commandBuffers[slot];
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &beginInfo);
    return cb;
}

void Renderer::Impl::SubmitSlot(uint32_t slot, VkSemaphore waitSemaphore, VkSemaphore signalSemaphore) {
    VkCommandBuffer cb = commandBuffers[slot];
    vkEndCommandBuffer(cb);

    VkPipelineStageFlags waitStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    if (waitSemaphore != VK_NULL_HANDLE) {
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &waitSemaphore;
        submit.pWaitDstStageMask = &waitStages;
    }
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    if (signalSemaphore != VK_NULL_HANDLE) {
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &signalSemaphore;
    }
    vkQueueSubmit(context.GraphicsQueue(), 1, &submit, inFlightFences[slot]);
}

Renderer::Impl::MeshGpu& Renderer::Impl::GetOrCreateMeshGpu(const UUID& uuid) {
    auto it = meshes.find(uuid);
    if (it != meshes.end()) return it->second;

    MeshGpu fresh;
    auto result = AssetManager::Instance().LoadImmediate(uuid);
    auto mesh = std::dynamic_pointer_cast<MeshAsset>(result.asset);
    if (mesh && CreateMeshGpu(*mesh, fresh)) {
        return meshes.emplace(uuid, std::move(fresh)).first->second;
    }
    WarnOnce("mesh-load:" + uuid.ToString(),
             "Renderer: mesh '%s' failed to load (%s); drawing nothing for it",
             uuid.ToString().c_str(), result.error.empty() ? "unknown error" : result.error.c_str());
    return meshes.emplace(uuid, MeshGpu{}).first->second;  // negative-cache an empty mesh
}

} // namespace raven

namespace raven {

// =====================================================================================
// Renderer::Impl — RecordFrame
// =====================================================================================

void Renderer::Impl::RecordFrame(VkCommandBuffer cb, const FrameTargets& targets, Scene& scene,
                                 const Camera& camera, const FrameParams& params, uint32_t slot,
                                 FrameStats& outStats) {
    // --- internal render extent (render-scale upscaler) + per-slot chain ---
    const float scale = SanitizeResolutionScale(resolutionScaleEff);
    const uint32_t intW = InternalExtent(targets.width, scale);
    const uint32_t intH = InternalExtent(targets.height, scale);
    EnsurePostChain(slot, intW, intH);
    PostChainImages& chain = postChains[slot];
    const uint32_t halfW = std::max(1u, intW / 2), halfH = std::max(1u, intH / 2);
    const uint32_t compositeParity = postFrameCounter & 1u;
    const char* historyReadName = compositeParity == 0 ? "historyA" : "historyB";
    const char* historyWriteName = compositeParity == 0 ? "historyB" : "historyA";

    const float aspect = intH > 0 ? static_cast<float>(intW) / static_cast<float>(intH) : 1.0f;
    const glm::mat4 view = camera.ViewMatrix();
    glm::mat4 proj = camera.ProjectionMatrix(aspect);
    const float timeSeconds = static_cast<float>(
        std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count());

    // TAA sub-pixel jitter: Halton(2,3), applied to the projection so the sample
    // pattern rotates frame to frame.
    if (fxTaa) {
        auto halton = [](uint32_t index, uint32_t base) {
            float f = 1.0f, r = 0.0f;
            uint32_t i = index;
            while (i > 0) { f /= static_cast<float>(base); r += f * static_cast<float>(i % base); i /= base; }
            return r;
        };
        const uint32_t idx = postFrameCounter + 1;
        const glm::vec2 jitterPx((halton(idx, 2) - 0.5f) * 0.9f, (halton(idx, 3) - 0.5f) * 0.9f);
        proj[2][0] += jitterPx.x * 2.0f / static_cast<float>(intW);
        proj[2][1] += jitterPx.y * 2.0f / static_cast<float>(intH);
    }
    const glm::mat4 viewProjCur = proj * view;

    // --- gather directional + point lights (first directional wins) ---
    DirectionalLightComponent sun;
    bool hasSun = false;
    scene.World().ForEach<DirectionalLightComponent>([&](ecs::Entity, const DirectionalLightComponent& light) {
        if (!hasSun) { sun = light; hasSun = true; }
    });
    glm::vec3 lightDir = glm::normalize(hasSun ? sun.direction : glm::vec3(0.4472f, -0.8944f, 0.1789f));

    struct PointLightData { glm::vec3 pos; glm::vec3 color; float intensity; float radius; };
    std::vector<PointLightData> pointLights;
    scene.World().ForEach<PointLightComponent>([&](ecs::Entity entity, const PointLightComponent& light) {
        if (pointLights.size() >= 4) return;
        TransformComponent transformComponent{};
        if (scene.World().HasComponent<TransformComponent>(entity)) {
            transformComponent = scene.World().GetComponent<TransformComponent>(entity);
        }
        glm::mat4 world = scene.GetWorldMatrix(entity);
        pointLights.push_back({glm::vec3(world[3]), light.color, light.intensity, light.radius});
    });

    // --- scene bounds for the shadow camera (from mesh entity positions) ---
    glm::vec3 boundsMin(FLT_MAX), boundsMax(-FLT_MAX);
    bool hasBounds = false;
    scene.World().ForEach<MeshRendererComponent>([&](ecs::Entity entity, const MeshRendererComponent& mr) {
        if (mr.mesh.IsNil()) return;
        glm::mat4 world = scene.GetWorldMatrix(entity);
        glm::vec3 p(world[3]);
        boundsMin = glm::min(boundsMin, p - glm::vec3(1.0f));
        boundsMax = glm::max(boundsMax, p + glm::vec3(1.0f));
        hasBounds = true;
    });
    if (!hasBounds) { boundsMin = glm::vec3(-4.0f); boundsMax = glm::vec3(4.0f); }
    glm::vec3 boundsCenter = 0.5f * (boundsMin + boundsMax);
    float boundsRadius = 0.5f * glm::length(boundsMax - boundsMin) + 4.0f;
    boundsRadius = glm::clamp(boundsRadius, 10.0f, 60.0f);

    // --- frame UBO ---
    FrameUboData ubo{};
    ubo.view = view;
    ubo.proj = proj;
    glm::vec3 eye = boundsCenter - lightDir * 80.0f;
    glm::mat4 lightView = glm::lookAtRH(eye, boundsCenter,
        std::abs(lightDir.y) > 0.95f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 lightProj = glm::ortho(-boundsRadius, boundsRadius, -boundsRadius, boundsRadius, 0.1f, 200.0f);
    ubo.lightViewProj = lightProj * lightView;
    ubo.lightDirIntensity = glm::vec4(lightDir, hasSun ? sun.intensity : 3.0f);
    ubo.lightColorAmbient = glm::vec4(hasSun ? sun.color : glm::vec3(1.0f), 0.35f);
    ubo.cameraPosTime = glm::vec4(camera.position, timeSeconds);
    ubo.counts = glm::ivec4(static_cast<int>(pointLights.size()), 0, 0, 0);
    for (size_t i = 0; i < pointLights.size(); ++i) {
        ubo.pointPos[i] = glm::vec4(pointLights[i].pos, pointLights[i].radius);
        ubo.pointColorIntensity[i] = glm::vec4(pointLights[i].color, pointLights[i].intensity);
    }
    std::memcpy(frameUboMapped[slot], &ubo, sizeof(ubo));

    // --- source selection for the post chain (first enabled effect owns the chain input) ---
    const bool ssaoOn = fxSsao && ssaoPipeline != VK_NULL_HANDLE;
    const bool ssrOn = fxSsr && ssrPipeline != VK_NULL_HANDLE;
    const bool bloomOn = fxBloom && bloomBrightPipeline != VK_NULL_HANDLE && bloomBlurPipeline != VK_NULL_HANDLE;
    const char* baseName = "sceneColor";
    VkImage baseImage = chain.sceneColor.image;
    VkImageView baseView = chain.sceneColor.view;
    if (ssaoOn) { baseName = "litColor"; baseImage = chain.litColor.image; baseView = chain.litColor.view; }
    if (ssrOn)  { baseName = "ssrColor"; baseImage = chain.ssrColor.image; baseView = chain.ssrColor.view; }

    // --- post UBO + per-pass descriptor updates (layouts are SHADER_READ_ONLY after barriers) ---
    PostUboData post{};
    post.viewProj = viewProjCur;
    post.invViewProj = glm::inverse(viewProjCur);
    post.prevViewProj = prevCameraValid ? prevViewProj : viewProjCur;
    post.cameraPos = glm::vec4(camera.position, 0.0f);
    float mbStrength = 0.0f;
    if (fxMotionBlur && prevCameraValid && params.deltaTime > 1e-4f) {
        const float speed = glm::length(camera.position - prevCameraPos) / params.deltaTime;
        mbStrength = glm::clamp((speed - 1.5f) * 0.055f, 0.0f, 0.5f);
    }
    const bool historyValidNow = fxTaa && chain.historyValid && prevCameraValid;
    post.params = glm::vec4(bloomOn ? 0.55f : 0.0f, fxTaa ? 0.85f : 0.0f, historyValidNow ? 1.0f : 0.0f,
                            ssrOn ? 0.9f : 0.0f);
    post.misc = glm::vec4(mbStrength, 0.0f, 0.0f, 0.0f);
    post.internalInfo = glm::vec4(static_cast<float>(intW), static_cast<float>(intH),
                                  1.0f / static_cast<float>(intW), 1.0f / static_cast<float>(intH));
    post.targetSize = glm::vec4(static_cast<float>(targets.width), static_cast<float>(targets.height), 0.0f, 0.0f);
    std::memcpy(postUboMapped[slot], &post, sizeof(post));

    const VkImageView whiteView = defaultWhiteTexture.image.view;
    const VkImageView histReadView = fxTaa ? (compositeParity == 0 ? chain.history[0].view : chain.history[1].view) : whiteView;
    const VkImageView histWriteView = compositeParity == 0 ? chain.history[1].view : chain.history[0].view;
    {
        struct Binding { uint32_t set; uint32_t binding; VkImageView view; bool nearest; };
        const Binding bindings[] = {
            {0, 1, chain.sceneColor.view, false},
            {0, 2, chain.sceneDepth.view, true},
            {0, 3, whiteView, false}, {0, 4, whiteView, false}, {0, 5, whiteView, false},
            {1, 1, ssaoOn ? chain.litColor.view : chain.sceneColor.view, false},
            {1, 2, chain.sceneDepth.view, true},
            {1, 3, whiteView, false}, {1, 4, whiteView, false}, {1, 5, chain.sceneMaterial.view, false},
            {2, 1, baseView, false},
            {2, 2, whiteView, true}, {2, 3, whiteView, false}, {2, 4, whiteView, false}, {2, 5, whiteView, false},
            {3, 1, chain.bloomA.view, false},
            {3, 2, whiteView, true}, {3, 3, whiteView, false}, {3, 4, whiteView, false}, {3, 5, whiteView, false},
            {4, 1, chain.bloomB.view, false},
            {4, 2, whiteView, true}, {4, 3, whiteView, false}, {4, 4, whiteView, false}, {4, 5, whiteView, false},
            {5, 1, baseView, false},
            {5, 2, chain.sceneDepth.view, true},
            {5, 3, bloomOn ? chain.bloomA.view : whiteView, false},
            {5, 4, histReadView, false},
            {5, 5, whiteView, false},
            {6, 1, histWriteView, false},
            {6, 2, whiteView, true}, {6, 3, whiteView, false}, {6, 4, whiteView, false}, {6, 5, whiteView, false},
        };
        for (const Binding& b : bindings) {
            VkDescriptorImageInfo info{};
            info.sampler = b.nearest ? postSamplerNearest : postSamplerLinear;
            info.imageView = b.view;
            info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = postSets[slot][b.set];
            write.dstBinding = b.binding;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.descriptorCount = 1;
            write.pImageInfo = &info;
            vkUpdateDescriptorSets(context.Device(), 1, &write, 0, nullptr);
        }
    }

    if (queryPool != VK_NULL_HANDLE) vkCmdResetQueryPool(cb, queryPool, 0, 14);

    // --- render graph: pass declarations + barrier solve ---
    const bool shadowPass = hasSun && sun.castShadow;
    const bool msaa = chain.msaaSamples > 1;
    RenderGraph graph;
    graph.AddResource({"shadow", GraphResourceType::DepthImage, shadowMapSize, shadowMapSize});
    graph.AddResource({"sceneColor", GraphResourceType::ColorImage, intW, intH});
    graph.AddResource({"sceneDepth", GraphResourceType::DepthImage, intW, intH});
    graph.AddResource({"sceneMaterial", GraphResourceType::ColorImage, intW, intH});
    if (msaa) {
        graph.AddResource({"sceneColorMS", GraphResourceType::ColorImage, intW, intH});
        graph.AddResource({"sceneDepthMS", GraphResourceType::DepthImage, intW, intH});
        graph.AddResource({"sceneMaterialMS", GraphResourceType::ColorImage, intW, intH});
    }
    if (ssaoOn) graph.AddResource({"litColor", GraphResourceType::ColorImage, intW, intH});
    if (ssrOn) graph.AddResource({"ssrColor", GraphResourceType::ColorImage, intW, intH});
    if (bloomOn) {
        graph.AddResource({"bloomA", GraphResourceType::ColorImage, halfW, halfH});
        graph.AddResource({"bloomB", GraphResourceType::ColorImage, halfW, halfH});
    }
    graph.AddResource({"historyA", GraphResourceType::ColorImage, intW, intH});
    graph.AddResource({"historyB", GraphResourceType::ColorImage, intW, intH});
    graph.AddResource({"targetColor", GraphResourceType::ColorImage, targets.width, targets.height});

    GraphPassDesc shadowDesc;
    shadowDesc.name = "shadow";
    shadowDesc.enabled = shadowPass;
    shadowDesc.writes = {{ "shadow", GraphResourceState::DepthAttachmentWrite }};
    graph.AddPass(shadowDesc);

    GraphPassDesc forwardDesc;
    forwardDesc.name = "forward";
    if (shadowPass) forwardDesc.reads = {{ "shadow", GraphResourceState::SampledRead }};
    if (msaa) {
        // MSAA attachments carry the store, resolves target the 1x images — the
        // graph treats the 1x images as written by this pass so consumers barrier correctly.
        forwardDesc.writes = {
            { "sceneColorMS", GraphResourceState::ColorAttachmentWrite },
            { "sceneMaterialMS", GraphResourceState::ColorAttachmentWrite },
            { "sceneDepthMS", GraphResourceState::DepthAttachmentWrite },
            { "sceneColor", GraphResourceState::ColorAttachmentWrite },
            { "sceneMaterial", GraphResourceState::ColorAttachmentWrite },
            { "sceneDepth", GraphResourceState::DepthAttachmentWrite },
        };
    } else {
        forwardDesc.writes = {
            { "sceneColor", GraphResourceState::ColorAttachmentWrite },
            { "sceneMaterial", GraphResourceState::ColorAttachmentWrite },
            { "sceneDepth", GraphResourceState::DepthAttachmentWrite },
        };
    }
    graph.AddPass(forwardDesc);

    if (params.drawGrid) {
        GraphPassDesc gridDesc;
        gridDesc.name = "grid";
        gridDesc.sideEffect = true;  // draws UI-ish content with no consumers; never cull
        gridDesc.reads = { { "sceneColor", GraphResourceState::ColorAttachmentWrite },  // LOAD existing color
                           { "sceneDepth", GraphResourceState::DepthAttachmentWrite } };
        gridDesc.writes = { { "sceneColor", GraphResourceState::ColorAttachmentWrite },
                            { "sceneDepth", GraphResourceState::DepthAttachmentWrite } };
        graph.AddPass(gridDesc);
    }

    // Post chain. Source of each pass is the running `baseName` (sceneColor, possibly
    // replaced by litColor/ssrColor when those passes are enabled).
    if (ssaoOn) {
        GraphPassDesc desc;
        desc.name = "ssao";
        desc.reads = { { "sceneColor", GraphResourceState::SampledRead },
                       { "sceneDepth", GraphResourceState::SampledRead } };
        desc.writes = {{ "litColor", GraphResourceState::ColorAttachmentWrite }};
        graph.AddPass(desc);
    }
    if (ssrOn) {
        GraphPassDesc desc;
        desc.name = "ssr";
        desc.reads = { { ssaoOn ? "litColor" : "sceneColor", GraphResourceState::SampledRead },
                       { "sceneDepth", GraphResourceState::SampledRead },
                       { "sceneMaterial", GraphResourceState::SampledRead } };
        desc.writes = {{ "ssrColor", GraphResourceState::ColorAttachmentWrite }};
        graph.AddPass(desc);
    }
    if (bloomOn) {
        GraphPassDesc bright;
        bright.name = "bloom-bright";
        bright.reads = {{ baseName, GraphResourceState::SampledRead }};
        bright.writes = {{ "bloomA", GraphResourceState::ColorAttachmentWrite }};
        graph.AddPass(bright);
        GraphPassDesc blurH;
        blurH.name = "bloom-blur-h";
        blurH.reads = {{ "bloomA", GraphResourceState::SampledRead }};
        blurH.writes = {{ "bloomB", GraphResourceState::ColorAttachmentWrite }};
        graph.AddPass(blurH);
        GraphPassDesc blurV;
        blurV.name = "bloom-blur-v";
        blurV.reads = {{ "bloomB", GraphResourceState::SampledRead }};
        blurV.writes = {{ "bloomA", GraphResourceState::ColorAttachmentWrite }};
        graph.AddPass(blurV);
    }

    {
        GraphPassDesc composite;
        composite.name = "composite";
        composite.reads = {{ baseName, GraphResourceState::SampledRead }};
        if (bloomOn) composite.reads.push_back({ "bloomA", GraphResourceState::SampledRead });
        if (fxTaa) {
            composite.reads.push_back({ "sceneDepth", GraphResourceState::SampledRead });
            composite.reads.push_back({ historyReadName, GraphResourceState::SampledRead });
        }
        composite.writes = {{ historyWriteName, GraphResourceState::ColorAttachmentWrite }};
        graph.AddPass(composite);
    }
    {
        GraphPassDesc upscale;
        upscale.name = "upscale";
        upscale.reads = {{ historyWriteName, GraphResourceState::SampledRead }};
        upscale.writes = {{ "targetColor", GraphResourceState::ColorAttachmentWrite }};
        graph.AddPass(upscale);
    }

    GraphPassDesc finalDesc;
    finalDesc.name = targets.toPresent ? "present" : "to-host";
    finalDesc.sideEffect = true;
    finalDesc.reads = {{ "targetColor", targets.toPresent ? GraphResourceState::Present : GraphResourceState::SampledRead }};
    finalDesc.writes = {{ "targetColor", targets.toPresent ? GraphResourceState::Present : GraphResourceState::SampledRead }};
    graph.AddPass(finalDesc);
    graph.SetExternalOutput("targetColor");

    const CompiledGraph compiled = graph.Compile();
    if (!compiled.ok) {
        RAVEN_LOG_ERROR("Renderer: render graph failed: %s", compiled.error.c_str());
    }

    auto imageFor = [&](const std::string& name) -> VkImage {
        if (name == "shadow") return shadowMap.image;
        if (name == "sceneColor") return chain.sceneColor.image;
        if (name == "sceneDepth") return chain.sceneDepth.image;
        if (name == "sceneMaterial") return chain.sceneMaterial.image;
        if (name == "sceneColorMS") return chain.sceneColorMS.image;
        if (name == "sceneDepthMS") return chain.sceneDepthMS.image;
        if (name == "sceneMaterialMS") return chain.sceneMaterialMS.image;
        if (name == "litColor") return chain.litColor.image;
        if (name == "ssrColor") return chain.ssrColor.image;
        if (name == "bloomA") return chain.bloomA.image;
        if (name == "bloomB") return chain.bloomB.image;
        if (name == "historyA") return chain.history[0].image;
        if (name == "historyB") return chain.history[1].image;
        if (name == "targetColor") return targets.colorImage;
        return VK_NULL_HANDLE;
    };
    auto applyBarriersFor = [&](const std::string& passName) {
        for (const BarrierOp& op : compiled.barriers) {
            if (op.beforePass != passName) continue;
            VkImage image = imageFor(op.resource);
            if (image != VK_NULL_HANDLE) ApplyBarrier(cb, op, image);
        }
    };

    // --- shared draw list ---
    struct DrawItem {
        const MeshGpu* gpu;
        glm::mat4 model;
        VkDescriptorSet material;
        glm::vec4 albedo;
        glm::vec4 mrParams;
        bool castShadow;
    };
    std::vector<DrawItem> draws;
    draws.reserve(64);
    uint32_t lodCulled = 0;
    auto& worldRef = scene.World();
    const float lodScreenHeight = static_cast<float>(intH);
    scene.World().ForEach<MeshRendererComponent>([&](ecs::Entity entity, const MeshRendererComponent& mr) {
        if (!mr.visible || mr.mesh.IsNil()) return;
        UUID meshUuid = mr.mesh;
        const glm::mat4 worldMat = scene.GetWorldMatrix(entity);
        if (worldRef.HasComponent<LodComponent>(entity)) {
            const auto& lod = worldRef.GetComponent<LodComponent>(entity);
            const float dist = glm::length(glm::vec3(worldMat[3]) - camera.position);
            if (dist > lodMaxDistance) {
                ++lodCulled;  // beyond the tier's LOD horizon — skip the entity entirely
                return;
            }
            if (!lod.levels.empty()) {
                LodConfig lodConfig;
                lodConfig.lodBias = lodBiasEff + lod.lodBias;
                const int maxLod = static_cast<int>(lod.levels.size()) - 1;
                const int selected = SelectLod(dist, lod.boundingRadius, lodConfig,
                                               static_cast<uint32_t>(lodScreenHeight), camera.fovDegrees, maxLod);
                if (selected > 0 && selected < static_cast<int>(lod.levels.size()) &&
                    !lod.levels[selected].IsNil()) {
                    meshUuid = lod.levels[selected];
                }
            }
        }
        MeshGpu& gpu = GetOrCreateMeshGpu(meshUuid);
        if (gpu.indexCount == 0) return;

        glm::vec4 albedo(0.8f, 0.8f, 0.8f, 1.0f);
        glm::vec4 mrParams(0.0f, 0.85f, 0.0f, mr.castShadow ? 1.0f : 0.0f);
        VkDescriptorSet materialSet = defaultMaterialSet;
        if (!mr.material.IsNil()) {
            auto result = AssetManager::Instance().LoadImmediate(mr.material);
            auto material = std::dynamic_pointer_cast<MaterialAsset>(result.asset);
            if (material) {
                albedo = material->albedo;
                mrParams = glm::vec4(material->metallic, material->roughness, glm::length(material->emissive),
                                     mr.castShadow ? 1.0f : 0.0f);
            }
            materialSet = EnsureMaterialSet(mr.material);
        }
        draws.push_back({&gpu, worldMat, materialSet, albedo, mrParams, mr.castShadow});
    });

    uint32_t drawCalls = 0, triangles = 0, meshesDrawn = 0;
    m_QueryExecutedMask = 0;

    const VkClearValue clearColor{{{0.022f, 0.028f, 0.048f, 1.0f}}};
    const VkClearValue depthClear{{1.0f, 0}};
    const VkClearValue shadowDepthClear{{1.0f, 0}};

    for (const GraphPassDesc* pass : compiled.executionOrder) {
        applyBarriersFor(pass->name);

        if (pass->name == "shadow") {
            if (queryPool != VK_NULL_HANDLE) { vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 0); m_QueryExecutedMask |= 1u << 0; }

            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, depthPipeline);
            VkViewport viewport{0.0f, 0.0f, static_cast<float>(shadowMap.width), static_cast<float>(shadowMap.height), 0.0f, 1.0f};
            VkRect2D scissor{{0, 0}, {shadowMap.width, shadowMap.height}};
            vkCmdSetViewport(cb, 0, 1, &viewport);
            vkCmdSetScissor(cb, 0, 1, &scissor);

            VkRenderingAttachmentInfo depthAttachment{};
            depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depthAttachment.imageView = shadowMap.view;
            depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depthAttachment.clearValue = shadowDepthClear;

            VkRenderingInfo renderingInfo{};
            renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            renderingInfo.renderArea = scissor;
            renderingInfo.layerCount = 1;
            renderingInfo.pDepthAttachment = &depthAttachment;
            vkCmdBeginRendering(cb, &renderingInfo);

            for (const DrawItem& draw : draws) {
                if (!draw.castShadow) continue;
                ShadowPushConstants push{ubo.lightViewProj, draw.model};
                vkCmdPushConstants(cb, depthPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
                VkBuffer vertexBuffers[] = {draw.gpu->vertexBuffer.buffer};
                VkDeviceSize offsets[] = {0};
                vkCmdBindVertexBuffers(cb, 0, 1, vertexBuffers, offsets);
                vkCmdBindIndexBuffer(cb, draw.gpu->indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cb, draw.gpu->indexCount, 1, 0, 0, 0);
            }
            vkCmdEndRendering(cb);

            if (queryPool != VK_NULL_HANDLE) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 1);

        } else if (pass->name == "forward") {
            if (queryPool != VK_NULL_HANDLE) { vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 2); m_QueryExecutedMask |= 1u << 1; }

            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, forwardPipeline);
            VkViewport viewport{0.0f, 0.0f, static_cast<float>(intW), static_cast<float>(intH), 0.0f, 1.0f};
            VkRect2D scissor{{0, 0}, {intW, intH}};
            vkCmdSetViewport(cb, 0, 1, &viewport);
            vkCmdSetScissor(cb, 0, 1, &scissor);

            VkClearValue materialClear{};
            materialClear.color = {{0.0f, 0.8f, 0.0f, 1.0f}};  // metallic=0, roughness=0.8 default
            VkRenderingAttachmentInfo colorAttachment{};
            colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            if (msaa) {
                colorAttachment.imageView = chain.sceneColorMS.view;
                colorAttachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
                colorAttachment.resolveImageView = chain.sceneColor.view;
                colorAttachment.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            } else {
                colorAttachment.imageView = chain.sceneColor.view;
                colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            }
            colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAttachment.clearValue = clearColor;

            VkRenderingAttachmentInfo materialAttachment{};
            materialAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            if (msaa) {
                materialAttachment.imageView = chain.sceneMaterialMS.view;
                materialAttachment.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
                materialAttachment.resolveImageView = chain.sceneMaterial.view;
                materialAttachment.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                materialAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            } else {
                materialAttachment.imageView = chain.sceneMaterial.view;
                materialAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            }
            materialAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            materialAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            materialAttachment.clearValue = materialClear;

            VkRenderingAttachmentInfo depthAttachment{};
            depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            if (msaa) {
                depthAttachment.imageView = chain.sceneDepthMS.view;
                depthAttachment.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
                depthAttachment.resolveImageView = chain.sceneDepth.view;
                depthAttachment.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            } else {
                depthAttachment.imageView = chain.sceneDepth.view;
                depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            }
            depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthAttachment.clearValue = depthClear;

            VkRenderingAttachmentInfo colorAttachments[2] = {colorAttachment, materialAttachment};
            VkRenderingInfo renderingInfo{};
            renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            renderingInfo.renderArea = scissor;
            renderingInfo.layerCount = 1;
            renderingInfo.colorAttachmentCount = 2;
            renderingInfo.pColorAttachments = colorAttachments;
            renderingInfo.pDepthAttachment = &depthAttachment;
            vkCmdBeginRendering(cb, &renderingInfo);

            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, forwardPipelineLayout, 0, 1, &frameSets[slot], 0, nullptr);
            VkDescriptorSet boundMaterial = VK_NULL_HANDLE;
            for (const DrawItem& draw : draws) {
                if (draw.material != boundMaterial) {
                    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, forwardPipelineLayout, 1, 1, &draw.material, 0, nullptr);
                    boundMaterial = draw.material;
                }
                MeshPushConstants push{};
                push.model = draw.model;
                push.albedo = draw.albedo;
                push.mrParams = draw.mrParams;
                vkCmdPushConstants(cb, forwardPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(push), &push);
                VkBuffer vertexBuffers[] = {draw.gpu->vertexBuffer.buffer};
                VkDeviceSize offsets[] = {0};
                vkCmdBindVertexBuffers(cb, 0, 1, vertexBuffers, offsets);
                vkCmdBindIndexBuffer(cb, draw.gpu->indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cb, draw.gpu->indexCount, 1, 0, 0, 0);
                ++drawCalls;
                triangles += draw.gpu->indexCount / 3;
                ++meshesDrawn;
            }
            vkCmdEndRendering(cb);

            if (queryPool != VK_NULL_HANDLE) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 3);

        } else if (pass->name == "grid" && gridPipeline != VK_NULL_HANDLE && gridVertexCount > 0) {
            VkViewport viewport{0.0f, 0.0f, static_cast<float>(intW), static_cast<float>(intH), 0.0f, 1.0f};
            VkRect2D scissor{{0, 0}, {intW, intH}};
            vkCmdSetViewport(cb, 0, 1, &viewport);
            vkCmdSetScissor(cb, 0, 1, &scissor);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gridPipeline);

            VkRenderingAttachmentInfo colorAttachment{};
            colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAttachment.imageView = chain.sceneColor.view;
            colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingAttachmentInfo depthAttachment{};
            depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depthAttachment.imageView = chain.sceneDepth.view;
            depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;   // keep depth from the forward pass
            depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depthAttachment.clearValue = depthClear;

            VkRenderingInfo renderingInfo{};
            renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            renderingInfo.renderArea = scissor;
            renderingInfo.layerCount = 1;
            renderingInfo.colorAttachmentCount = 1;
            renderingInfo.pColorAttachments = &colorAttachment;
            renderingInfo.pDepthAttachment = &depthAttachment;
            vkCmdBeginRendering(cb, &renderingInfo);

            GridPushConstants push{viewProjCur};
            vkCmdPushConstants(cb, gridPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
            VkBuffer vertexBuffers[] = {gridVertexBuffer.buffer};
            VkDeviceSize offsets[] = {0};
            vkCmdBindVertexBuffers(cb, 0, 1, vertexBuffers, offsets);
            vkCmdDraw(cb, gridVertexCount, 1, 0, 0);
            vkCmdEndRendering(cb);

        } else if (pass->name == "ssao" && ssaoPipeline != VK_NULL_HANDLE) {
            if (queryPool != VK_NULL_HANDLE) { vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 4); }
            m_QueryExecutedMask |= 1u << 2;
            VkViewport viewport{0.0f, 0.0f, static_cast<float>(intW), static_cast<float>(intH), 0.0f, 1.0f};
            VkRect2D scissor{{0, 0}, {intW, intH}};
            vkCmdSetViewport(cb, 0, 1, &viewport);
            vkCmdSetScissor(cb, 0, 1, &scissor);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, ssaoPipeline);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, postPipelineLayout, 0, 1, &postSets[slot][0], 0, nullptr);

            VkRenderingAttachmentInfo colorAttachment{};
            colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAttachment.imageView = chain.litColor.view;
            colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingInfo renderingInfo{};
            renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            renderingInfo.renderArea = scissor;
            renderingInfo.layerCount = 1;
            renderingInfo.colorAttachmentCount = 1;
            renderingInfo.pColorAttachments = &colorAttachment;
            vkCmdBeginRendering(cb, &renderingInfo);
            vkCmdDraw(cb, 3, 1, 0, 0);
            vkCmdEndRendering(cb);
            if (queryPool != VK_NULL_HANDLE) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 5);

        } else if (pass->name == "ssr" && ssrPipeline != VK_NULL_HANDLE) {
            if (queryPool != VK_NULL_HANDLE) { vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 6); }
            m_QueryExecutedMask |= 1u << 3;
            VkViewport viewport{0.0f, 0.0f, static_cast<float>(intW), static_cast<float>(intH), 0.0f, 1.0f};
            VkRect2D scissor{{0, 0}, {intW, intH}};
            vkCmdSetViewport(cb, 0, 1, &viewport);
            vkCmdSetScissor(cb, 0, 1, &scissor);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, ssrPipeline);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, postPipelineLayout, 0, 1, &postSets[slot][1], 0, nullptr);

            VkRenderingAttachmentInfo colorAttachment{};
            colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAttachment.imageView = chain.ssrColor.view;
            colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingInfo renderingInfo{};
            renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            renderingInfo.renderArea = scissor;
            renderingInfo.layerCount = 1;
            renderingInfo.colorAttachmentCount = 1;
            renderingInfo.pColorAttachments = &colorAttachment;
            vkCmdBeginRendering(cb, &renderingInfo);
            vkCmdDraw(cb, 3, 1, 0, 0);
            vkCmdEndRendering(cb);
            if (queryPool != VK_NULL_HANDLE) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 7);

        } else if (pass->name == "bloom-bright" && bloomBrightPipeline != VK_NULL_HANDLE) {
            if (queryPool != VK_NULL_HANDLE) { vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 8); }
            m_QueryExecutedMask |= 1u << 4;
            VkViewport viewport{0.0f, 0.0f, static_cast<float>(halfW), static_cast<float>(halfH), 0.0f, 1.0f};
            VkRect2D scissor{{0, 0}, {halfW, halfH}};
            vkCmdSetViewport(cb, 0, 1, &viewport);
            vkCmdSetScissor(cb, 0, 1, &scissor);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, bloomBrightPipeline);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, postPipelineLayout, 0, 1, &postSets[slot][2], 0, nullptr);

            VkRenderingAttachmentInfo colorAttachment{};
            colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAttachment.imageView = chain.bloomA.view;
            colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingInfo renderingInfo{};
            renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            renderingInfo.renderArea = scissor;
            renderingInfo.layerCount = 1;
            renderingInfo.colorAttachmentCount = 1;
            renderingInfo.pColorAttachments = &colorAttachment;
            vkCmdBeginRendering(cb, &renderingInfo);
            vkCmdDraw(cb, 3, 1, 0, 0);
            vkCmdEndRendering(cb);

        } else if ((pass->name == "bloom-blur-h" || pass->name == "bloom-blur-v") && bloomBlurPipeline != VK_NULL_HANDLE) {
            const bool horizontal = pass->name == "bloom-blur-h";
            VkViewport viewport{0.0f, 0.0f, static_cast<float>(halfW), static_cast<float>(halfH), 0.0f, 1.0f};
            VkRect2D scissor{{0, 0}, {halfW, halfH}};
            vkCmdSetViewport(cb, 0, 1, &viewport);
            vkCmdSetScissor(cb, 0, 1, &scissor);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, bloomBlurPipeline);
            const uint32_t setIndex = horizontal ? 3u : 4u;
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, postPipelineLayout, 0, 1, &postSets[slot][setIndex], 0, nullptr);
            const float dirH[2] = {1.4f, 0.0f};
            const float dirV[2] = {0.0f, 1.4f};
            const float* direction = horizontal ? dirH : dirV;
            vkCmdPushConstants(cb, postPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 2, direction);

            VkRenderingAttachmentInfo colorAttachment{};
            colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAttachment.imageView = horizontal ? chain.bloomB.view : chain.bloomA.view;
            colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingInfo renderingInfo{};
            renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            renderingInfo.renderArea = scissor;
            renderingInfo.layerCount = 1;
            renderingInfo.colorAttachmentCount = 1;
            renderingInfo.pColorAttachments = &colorAttachment;
            vkCmdBeginRendering(cb, &renderingInfo);
            vkCmdDraw(cb, 3, 1, 0, 0);
            vkCmdEndRendering(cb);
            if (!horizontal && queryPool != VK_NULL_HANDLE) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 9);

        } else if (pass->name == "composite" && compositePipeline != VK_NULL_HANDLE) {
            if (queryPool != VK_NULL_HANDLE) { vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 10); }
            m_QueryExecutedMask |= 1u << 5;
            VkViewport viewport{0.0f, 0.0f, static_cast<float>(intW), static_cast<float>(intH), 0.0f, 1.0f};
            VkRect2D scissor{{0, 0}, {intW, intH}};
            vkCmdSetViewport(cb, 0, 1, &viewport);
            vkCmdSetScissor(cb, 0, 1, &scissor);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, compositePipeline);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, postPipelineLayout, 0, 1, &postSets[slot][5], 0, nullptr);

            VkRenderingAttachmentInfo colorAttachment{};
            colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAttachment.imageView = histWriteView;
            colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingInfo renderingInfo{};
            renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            renderingInfo.renderArea = scissor;
            renderingInfo.layerCount = 1;
            renderingInfo.colorAttachmentCount = 1;
            renderingInfo.pColorAttachments = &colorAttachment;
            vkCmdBeginRendering(cb, &renderingInfo);
            vkCmdDraw(cb, 3, 1, 0, 0);
            vkCmdEndRendering(cb);
            if (queryPool != VK_NULL_HANDLE) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 11);

        } else if (pass->name == "upscale" && upscalePipeline != VK_NULL_HANDLE) {
            if (queryPool != VK_NULL_HANDLE) { vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 12); }
            m_QueryExecutedMask |= 1u << 6;
            VkViewport viewport{0.0f, 0.0f, static_cast<float>(targets.width), static_cast<float>(targets.height), 0.0f, 1.0f};
            VkRect2D scissor{{0, 0}, {targets.width, targets.height}};
            vkCmdSetViewport(cb, 0, 1, &viewport);
            vkCmdSetScissor(cb, 0, 1, &scissor);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, upscalePipeline);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, postPipelineLayout, 0, 1, &postSets[slot][6], 0, nullptr);

            VkRenderingAttachmentInfo colorAttachment{};
            colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAttachment.imageView = targets.colorView;
            colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingInfo renderingInfo{};
            renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            renderingInfo.renderArea = scissor;
            renderingInfo.layerCount = 1;
            renderingInfo.colorAttachmentCount = 1;
            renderingInfo.pColorAttachments = &colorAttachment;
            vkCmdBeginRendering(cb, &renderingInfo);
            vkCmdDraw(cb, 3, 1, 0, 0);
            vkCmdEndRendering(cb);
            if (queryPool != VK_NULL_HANDLE) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 13);
        }
        // "present" / "to-host" passes need no commands; the graph emitted the barrier.
    }

    outStats.drawCalls = drawCalls;
    outStats.lodCulled = lodCulled;
    outStats.triangles = triangles;
    outStats.meshesDrawn = meshesDrawn;
    outStats.culledPasses = static_cast<uint32_t>(compiled.culledPasses.size());
    // Previous-frame matrices for TAA reprojection / motion blur.
    prevViewProj = viewProjCur;
    prevCameraPos = camera.position;
    prevCameraValid = true;
    if (fxTaa) chain.historyValid = true;
    ++postFrameCounter;
    queriesPending = (queryPool != VK_NULL_HANDLE);
}

} // namespace raven

namespace raven {

// =====================================================================================
// Renderer — public API
// =====================================================================================

Renderer::Renderer() = default;
Renderer::~Renderer() { Shutdown(); }

bool Renderer::Initialize(const RendererConfig& config, std::string* error) {
    Shutdown();
    m_Impl = std::make_unique<Impl>();
    Impl& impl = *m_Impl;

    vk::VulkanContextConfig contextConfig;
    contextConfig.applicationName = config.applicationName.c_str();
    contextConfig.enableValidation = config.enableValidation;
    contextConfig.allowCpuDeviceFallback = config.allowCpuDeviceFallback;
    contextConfig.requireSwapchain = !config.requiredInstanceExtensions.empty();
    contextConfig.requiredInstanceExtensions = config.requiredInstanceExtensions;
    contextConfig.enablePortabilityEnumeration = config.enablePortabilityEnumeration;

    if (!impl.context.Initialize(contextConfig, error)) {
        m_Impl.reset();
        return false;
    }

    // Quality tier drives shadow map size, MSAA and the post-effect set before base resources are built.
    const QualityPreset& preset = QualityPresetFor(config.qualityTier);
    impl.shadowMapSize = preset.shadowMapSize;
    {
        VkPhysicalDeviceProperties deviceProps{};
        vkGetPhysicalDeviceProperties(impl.context.PhysicalDevice(), &deviceProps);
        const VkSampleCountFlags supported = deviceProps.limits.framebufferColorSampleCounts &
                                              deviceProps.limits.framebufferDepthSampleCounts;
        const uint32_t requested = preset.msaaSamples > 0 ? preset.msaaSamples : 1;
        uint32_t chosen = 1;
        bool found = false;
        for (const uint32_t candidate : {1u, 2u, 4u, 8u, 16u}) {
            if ((supported & candidate) && candidate >= requested) { chosen = candidate; found = true; break; }
        }
        if (!found) {
            for (const uint32_t candidate : {16u, 8u, 4u, 2u, 1u}) {
                if (supported & candidate) { chosen = candidate; break; }
            }
        }
        if (chosen != requested) {
            RAVEN_LOG_INFO("Renderer: MSAA %ux unsupported by device (sample-count mask 0x%x) — using %ux",
                           requested, static_cast<uint32_t>(supported), chosen);
        }
        impl.msaaSamples = chosen;
        if (impl.msaaSamples > 1 && !impl.context.SupportsDepthResolve()) {
            RAVEN_LOG_WARN("Renderer: device exposes no usable depth resolve (VK_KHR_depth_stencil_resolve with SAMPLE_ZERO) — disabling MSAA");
            impl.msaaSamples = 1;
        }
    }
    impl.fxSsao = preset.ssao;
    impl.fxBloom = preset.bloom;
    impl.fxSsr = preset.ssr;
    impl.fxTaa = preset.taa;
    impl.fxMotionBlur = preset.motionBlur;
    impl.lodBiasEff = preset.lodBias;
    impl.lodMaxDistance = preset.lodMaxDistance;
    impl.resolutionScaleEff = config.resolutionScale > 0.0f ? SanitizeResolutionScale(config.resolutionScale)
                                                           : SanitizeResolutionScale(preset.resolutionScale);
    RAVEN_LOG_INFO("Renderer: tier=%s internal-scale=%.2f msaa=%ux ssao=%d bloom=%d ssr=%d taa=%d motion-blur=%d",
                   preset.name, impl.resolutionScaleEff, impl.msaaSamples,
                   impl.fxSsao ? 1 : 0, impl.fxBloom ? 1 : 0, impl.fxSsr ? 1 : 0, impl.fxTaa ? 1 : 0, impl.fxMotionBlur ? 1 : 0);

    impl.CreateBaseResources();
    impl.CreateSamplers();
    impl.CreateDefaultTextures();
    impl.CreateDescriptorSetLayouts();  // layouts + set layouts + pipeline layouts
    impl.CreateGridMesh();
    impl.CreateDescriptorPoolAndSets(); // pool, frame UBOs, shadow map, frame sets, default material

    impl.stats.gpuName = impl.context.DeviceName();
    impl.stats.vramBytes = impl.context.DeviceLocalMemoryBytes();
    impl.stats.dynamicRendering = impl.context.SupportsDynamicRendering();
    impl.stats.timelineSemaphores = impl.context.SupportsTimelineSemaphores();
    impl.stats.cpuDevice = impl.context.IsCpuDevice();

    RAVEN_LOG_INFO("Renderer: initialized on '%s' (dynamic rendering: %s, timeline semaphores: %s, CPU device: %s)",
                   impl.stats.gpuName.c_str(),
                   impl.stats.dynamicRendering ? "yes" : "no",
                   impl.stats.timelineSemaphores ? "yes" : "no",
                   impl.stats.cpuDevice ? "yes" : "no");
    return true;
}

void Renderer::Shutdown() {
    if (!m_Impl) return;
    Impl& impl = *m_Impl;
    VkDevice device = impl.context.Device();
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);

        impl.swapchain.reset();

        // Pipelines
        impl.InvalidatePipelines();
        if (impl.pipelineCache != VK_NULL_HANDLE) vkDestroyPipelineCache(device, impl.pipelineCache, nullptr);

        // GPU-side asset caches
        for (auto& [uuid, mesh] : impl.meshes) {
            mesh.vertexBuffer.Destroy(device);
            mesh.indexBuffer.Destroy(device);
        }
        impl.meshes.clear();
        for (auto& [uuid, tex] : impl.textures) tex.image.Destroy(device);
        impl.textures.clear();
        impl.defaultWhiteTexture.image.Destroy(device);
        impl.defaultNormalTexture.image.Destroy(device);

        // Descriptors
        impl.InvalidateMaterialSets();  // no-op after meshes/textures torn down; frees material sets
        if (impl.defaultMaterialSet != VK_NULL_HANDLE) {
            vkFreeDescriptorSets(device, impl.descriptorPool, 1, &impl.defaultMaterialSet);
            impl.defaultMaterialSet = VK_NULL_HANDLE;
        }
        for (uint32_t i = 0; i < kTotalSlots; ++i) {
            if (impl.frameUboMapped[i]) {
                vkUnmapMemory(device, impl.frameUboBuffers[i].memory);
                impl.frameUboMapped[i] = nullptr;
            }
            impl.frameUboBuffers[i].Destroy(device);
        }
        if (impl.descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, impl.descriptorPool, nullptr);
        for (uint32_t i = 0; i < kTotalSlots; ++i) {
            if (impl.postUboMapped[i]) {
                vkUnmapMemory(device, impl.postUboBuffers[i].memory);
                impl.postUboMapped[i] = nullptr;
            }
            impl.postUboBuffers[i].Destroy(device);
            impl.DestroyPostChain(i);
        }
        if (impl.frameSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, impl.frameSetLayout, nullptr);
        if (impl.materialSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, impl.materialSetLayout, nullptr);
        if (impl.postSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, impl.postSetLayout, nullptr);
        if (impl.forwardPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, impl.forwardPipelineLayout, nullptr);
        if (impl.gridPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, impl.gridPipelineLayout, nullptr);
        if (impl.depthPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, impl.depthPipelineLayout, nullptr);
        if (impl.postPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, impl.postPipelineLayout, nullptr);

        // Samplers
        if (impl.shadowSampler != VK_NULL_HANDLE && impl.shadowSampler != impl.defaultSampler) {
            vkDestroySampler(device, impl.shadowSampler, nullptr);
        }
        if (impl.defaultSampler != VK_NULL_HANDLE) vkDestroySampler(device, impl.defaultSampler, nullptr);
        if (impl.postSamplerLinear != VK_NULL_HANDLE) vkDestroySampler(device, impl.postSamplerLinear, nullptr);
        if (impl.postSamplerNearest != VK_NULL_HANDLE) vkDestroySampler(device, impl.postSamplerNearest, nullptr);

        // Images
        impl.offscreenColor.Destroy(device);
        impl.offscreenDepth.Destroy(device);
        impl.windowDepth.Destroy(device);
        impl.shadowMap.Destroy(device);
        impl.gridVertexBuffer.Destroy(device);

        // Per-frame command/sync
        impl.DestroyBaseResources();
    }
    impl.context.Shutdown();
    m_Impl.reset();
}

bool Renderer::IsInitialized() const { return m_Impl != nullptr && m_Impl->context.Device() != VK_NULL_HANDLE; }
std::string Renderer::GpuName() const { return m_Impl ? m_Impl->context.DeviceName() : std::string(); }

bool Renderer::CreateSwapchain(VkSurfaceKHR surface, uint32_t width, uint32_t height, std::string* error) {
    if (!m_Impl || surface == VK_NULL_HANDLE) {
        if (error) *error = "Renderer not initialized or null surface";
        return false;
    }
    Impl& impl = *m_Impl;
    vkDeviceWaitIdle(impl.context.Device());
    impl.swapchain.reset();

    impl.swapchain = std::make_unique<vk::Swapchain>(impl.context, surface);
    if (!impl.swapchain->Create(width, height, error)) {
        impl.swapchain.reset();
        return false;
    }
    const VkExtent2D extent = impl.swapchain->Extent();
    impl.EnsureWindowDepth(extent.width, extent.height);
    impl.EnsurePipelines(impl.swapchain->ImageFormat());
    return true;
}

void Renderer::RequestSwapchainResize(uint32_t width, uint32_t height) {
    if (!m_Impl) return;
    m_Impl->requestedWidth = width;
    m_Impl->requestedHeight = height;
    m_Impl->resizeRequested = true;
}

bool Renderer::RenderFrame(Scene& scene, const Camera& camera, const FrameParams& params) {
    Impl& impl = *m_Impl;
    if (!impl.swapchain) return false;

    // Deferred resize: recreate depth + pipelines with the new format/extent.
    if (impl.resizeRequested) {
        vkDeviceWaitIdle(impl.context.Device());
        const VkExtent2D current = impl.swapchain->Extent();
        const uint32_t w = impl.requestedWidth ? impl.requestedWidth : current.width;
        const uint32_t h = impl.requestedHeight ? impl.requestedHeight : current.height;
        if (!impl.swapchain->Create(w, h, nullptr)) return false;
        const VkExtent2D extent = impl.swapchain->Extent();
        impl.EnsureWindowDepth(extent.width, extent.height);
        impl.EnsurePipelines(impl.swapchain->ImageFormat());
        impl.resizeRequested = false;
        return false;  // skip a frame; presentation resumes next tick
    }

    impl.frameStopwatch.Restart();
    const uint32_t slot = impl.frameIndex;

    const vk::Swapchain::AcquireResult acquired =
        impl.swapchain->AcquireNextImage(UINT64_MAX, impl.imageAvailable[slot]);
    if (acquired == vk::Swapchain::AcquireResult::OutOfDate) {
        const VkExtent2D extent = impl.swapchain->Extent();
        impl.requestedWidth = extent.width;
        impl.requestedHeight = extent.height;
        impl.resizeRequested = true;
        return false;
    }
    if (acquired == vk::Swapchain::AcquireResult::Failed) {
        RAVEN_LOG_ERROR("Renderer: swapchain acquire failed");
        return false;
    }
    const uint32_t imageIndex = impl.swapchain->CurrentImageIndex();

    VkCommandBuffer cb = impl.BeginSlot(slot, /*waitFence*/ true);

    Impl::FrameTargets targets;
    targets.colorImage = impl.swapchain->Images()[imageIndex];
    targets.colorView = impl.swapchain->ImageViews()[imageIndex];
    targets.depthImage = impl.windowDepth.image;
    targets.depthView = impl.windowDepth.view;
    targets.width = impl.swapchain->Extent().width;
    targets.height = impl.swapchain->Extent().height;
    targets.toPresent = true;
    targets.colorFormat = impl.swapchain->ImageFormat();

    FrameStats frameStats{};
    impl.RecordFrame(cb, targets, scene, camera, params, slot, frameStats);
    frameStats.cpuFrameMs = static_cast<float>(impl.frameStopwatch.ElapsedMilliseconds());
    frameStats.gpuName = impl.stats.gpuName;
    frameStats.vramBytes = impl.stats.vramBytes;
    frameStats.dynamicRendering = impl.stats.dynamicRendering;
    frameStats.timelineSemaphores = impl.stats.timelineSemaphores;
    frameStats.cpuDevice = impl.stats.cpuDevice;
    impl.stats.drawCalls = frameStats.drawCalls;
    impl.stats.triangles = frameStats.triangles;
    impl.stats.meshesDrawn = frameStats.meshesDrawn;
    impl.stats.culledPasses = frameStats.culledPasses;
    impl.stats.cpuFrameMs = frameStats.cpuFrameMs;

    impl.SubmitSlot(slot, impl.imageAvailable[slot], impl.renderFinished[slot]);

    if (acquired == vk::Swapchain::AcquireResult::Suboptimal) {
        const VkExtent2D extent = impl.swapchain->Extent();
        impl.requestedWidth = extent.width;
        impl.requestedHeight = extent.height;
        impl.resizeRequested = true;
    }

    switch (impl.swapchain->Present(impl.renderFinished[slot])) {
        case vk::Swapchain::PresentResult::OutOfDate: {
            const VkExtent2D extent = impl.swapchain->Extent();
            impl.requestedWidth = extent.width;
            impl.requestedHeight = extent.height;
            impl.resizeRequested = true;
            break;
        }
        case vk::Swapchain::PresentResult::Failed:
            RAVEN_LOG_ERROR("Renderer: swapchain present failed");
            break;
        default:
            break;
    }

    impl.frameIndex = (slot + 1) % kFramesInFlight;
    return true;
}

bool Renderer::CreateOffscreenTarget(uint32_t width, uint32_t height, std::string* error) {
    if (!m_Impl) {
        if (error) *error = "Renderer not initialized";
        return false;
    }
    if (width == 0 || height == 0) {
        if (error) *error = "offscreen target cannot have zero extent";
        return false;
    }
    m_Impl->offscreenWidth = width;
    m_Impl->offscreenHeight = height;
    m_Impl->EnsureOffscreenTarget(width, height);
    return m_Impl->offscreenColor.image != VK_NULL_HANDLE;
}

void Renderer::ResizeOffscreenTarget(uint32_t width, uint32_t height) {
    if (!m_Impl || (width == m_Impl->offscreenWidth && height == m_Impl->offscreenHeight)) return;
    m_Impl->offscreenWidth = width;
    m_Impl->offscreenHeight = height;
    m_Impl->EnsureOffscreenTarget(width, height);
}

bool Renderer::RenderOffscreen(Scene& scene, const Camera& camera, const FrameParams& params) {
    if (!m_Impl) return false;
    Impl& impl = *m_Impl;
    if (impl.offscreenColor.image == VK_NULL_HANDLE) {
        if (!CreateOffscreenTarget(640, 480, nullptr)) return false;
    }

    impl.frameStopwatch.Restart();
    const uint32_t slot = kOffscreenSlot;
    VkCommandBuffer cb = impl.BeginSlot(slot, /*waitFence*/ true);

    Impl::FrameTargets targets;
    targets.colorImage = impl.offscreenColor.image;
    targets.colorView = impl.offscreenColor.view;
    targets.depthImage = impl.offscreenDepth.image;
    targets.depthView = impl.offscreenDepth.view;
    targets.width = impl.offscreenWidth;
    targets.height = impl.offscreenHeight;
    targets.toPresent = false;
    targets.colorFormat = impl.offscreenColor.format;

    FrameStats frameStats{};
    impl.RecordFrame(cb, targets, scene, camera, params, slot, frameStats);
    frameStats.cpuFrameMs = static_cast<float>(impl.frameStopwatch.ElapsedMilliseconds());
    impl.stats.drawCalls = frameStats.drawCalls;
    impl.stats.triangles = frameStats.triangles;
    impl.stats.meshesDrawn = frameStats.meshesDrawn;
    impl.stats.culledPasses = frameStats.culledPasses;
    impl.stats.cpuFrameMs = frameStats.cpuFrameMs;

    impl.SubmitSlot(slot, VK_NULL_HANDLE, VK_NULL_HANDLE);
    return true;
}

VkImageView Renderer::OffscreenColorView() const { return m_Impl ? m_Impl->offscreenColor.view : VK_NULL_HANDLE; }
VkSampler Renderer::OffscreenSampler() const { return m_Impl ? m_Impl->offscreenSampler : VK_NULL_HANDLE; }
VkFormat Renderer::OffscreenFormat() const { return m_Impl ? m_Impl->offscreenColor.format : VK_FORMAT_UNDEFINED; }
uint32_t Renderer::OffscreenWidth() const { return m_Impl ? m_Impl->offscreenWidth : 0; }
uint32_t Renderer::OffscreenHeight() const { return m_Impl ? m_Impl->offscreenHeight : 0; }

bool Renderer::SaveOffscreenToPng(Scene& scene, const Camera& camera, const FrameParams& params,
                                  const std::filesystem::path& path, std::string* error) {
    if (!RenderOffscreen(scene, camera, params)) {
        if (error) *error = "offscreen render failed";
        return false;
    }
    Impl& impl = *m_Impl;
    VkDevice device = impl.context.Device();
    vkDeviceWaitIdle(device);

    const uint32_t width = impl.offscreenWidth, height = impl.offscreenHeight;
    const VkDeviceSize byteSize = static_cast<VkDeviceSize>(width) * height * 4;
    vk::BufferResource staging = vk::CreateHostVisibleBuffer(impl.context, byteSize,
                                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    VkCommandBuffer cb = impl.context.BeginSingleTimeCommands();
    vk::VulkanContext::TransitionImageLayout(cb, impl.offscreenColor.image,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                             VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);
    VkBufferImageCopy copy{};
    copy.bufferOffset = 0;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cb, impl.offscreenColor.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &copy);
    vk::VulkanContext::TransitionImageLayout(cb, impl.offscreenColor.image,
                                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                             VK_IMAGE_ASPECT_COLOR_BIT, 0, 1);
    impl.context.EndSingleTimeCommands(cb);

    void* mapped = nullptr;
    vkMapMemory(device, staging.memory, 0, byteSize, 0, &mapped);
    int ok = stbi_write_png(path.string().c_str(), static_cast<int>(width), static_cast<int>(height), 4, mapped,
                            static_cast<int>(width * 4));
    vkUnmapMemory(device, staging.memory);
    staging.Destroy(device);

    if (!ok) {
        if (error) *error = "stbi_write_png failed for " + path.string();
        return false;
    }
    return true;
}

FrameStats Renderer::GetFrameStats() const { return m_Impl ? m_Impl->stats : FrameStats{}; }

VkInstance Renderer::Instance() const { return m_Impl ? m_Impl->context.Instance() : VK_NULL_HANDLE; }
VkPhysicalDevice Renderer::PhysicalDevice() const { return m_Impl ? m_Impl->context.PhysicalDevice() : VK_NULL_HANDLE; }
VkDevice Renderer::Device() const { return m_Impl ? m_Impl->context.Device() : VK_NULL_HANDLE; }
VkQueue Renderer::GraphicsQueue() const { return m_Impl ? m_Impl->context.GraphicsQueue() : VK_NULL_HANDLE; }
uint32_t Renderer::GraphicsQueueFamily() const { return m_Impl ? m_Impl->context.QueueFamilies().graphics : 0; }

void Renderer::RegisterAssetUploadHandler() {
    AssetManager::Instance().SetGpuUploadHandler([this](const std::shared_ptr<Asset>& asset) {
        if (m_Impl) m_Impl->OnAssetUploaded(asset);
    });
}

} // namespace raven
