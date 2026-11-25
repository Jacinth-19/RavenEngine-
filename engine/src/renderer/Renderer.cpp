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
    bool m_QueryShadowExecuted = false;

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

        // Timestamp query pool (4 slots: shadow begin/end, forward begin/end).
        if (context.TimestampPeriod() > 0.0f) {
            VkQueryPoolCreateInfo queryInfo{};
            queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queryInfo.queryCount = 4;
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
    }

    void CreateDescriptorPoolAndSets() {
        VkDescriptorPoolSize poolSizes[2] = {};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[0].descriptorCount = 8;
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[1].descriptorCount = 1024;
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 512;
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
        forwardPipeline = VK_NULL_HANDLE;
        gridPipeline = VK_NULL_HANDLE;
        depthPipeline = VK_NULL_HANDLE;
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
    void ReadQueryResults();
    void EnsureOffscreenTarget(uint32_t width, uint32_t height);
    void EnsureWindowDepth(uint32_t width, uint32_t height);
    void ApplyBarrier(VkCommandBuffer cb, const BarrierOp& op, VkImage image);
    VkImage ImageForResource(const std::string& name, VkImage colorImage, VkImage depthImage) const;
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
    if (pipelineColorFormat == colorFormat && forwardPipeline != VK_NULL_HANDLE) return;
    InvalidatePipelines();

    // --- forward pipeline ---
    {
        std::vector<uint8_t> vertSpv, fragSpv;
        if (!LoadSpvFile("forward.vert.spv", vertSpv) || !LoadSpvFile("forward.frag.spv", fragSpv)) {
            RAVEN_LOG_ERROR("Renderer: forward shader SPIR-V missing — pipeline not created");
            return;
        }
        VkShaderModule vertModule = CreateShaderModule(context, vertSpv);
        VkShaderModule fragModule = CreateShaderModule(context, fragSpv);

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertModule;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragModule;
        stages[1].pName = "main";

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

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = 2;
        dynamicState.pDynamicStates = dynamicStates;

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
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
        renderingInfo.pColorAttachmentFormats = &colorFormat;
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

    // --- grid pipeline (line list, opaque) ---
    {
        std::vector<uint8_t> vertSpv, fragSpv;
        if (LoadSpvFile("grid.vert.spv", vertSpv) && LoadSpvFile("grid.frag.spv", fragSpv)) {
            VkShaderModule vertModule = CreateShaderModule(context, vertSpv);
            VkShaderModule fragModule = CreateShaderModule(context, fragSpv);

            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = vertModule;
            stages[0].pName = "main";
            stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = fragModule;
            stages[1].pName = "main";

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

            VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dynamicState{};
            dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dynamicState.dynamicStateCount = 2;
            dynamicState.pDynamicStates = dynamicStates;

            VkPipelineViewportStateCreateInfo viewportState{};
            viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            viewportState.viewportCount = 1;
            viewportState.scissorCount = 1;

            VkPipelineRasterizationStateCreateInfo rasterizer{};
            rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
            rasterizer.cullMode = VK_CULL_MODE_NONE;
            rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rasterizer.lineWidth = 1.0f;

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
            renderingInfo.pColorAttachmentFormats = &colorFormat;
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
    {
        std::vector<uint8_t> vertSpv;
        if (LoadSpvFile("depth.vert.spv", vertSpv)) {
            VkShaderModule vertModule = CreateShaderModule(context, vertSpv);

            VkPipelineShaderStageCreateInfo stage{};
            stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
            stage.module = vertModule;
            stage.pName = "main";

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

            VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
            inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

            VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dynamicState{};
            dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dynamicState.dynamicStateCount = 2;
            dynamicState.pDynamicStates = dynamicStates;

            VkPipelineViewportStateCreateInfo viewportState{};
            viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            viewportState.viewportCount = 1;
            viewportState.scissorCount = 1;

            VkPipelineRasterizationStateCreateInfo rasterizer{};
            rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
            rasterizer.cullMode = VK_CULL_MODE_NONE;
            rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rasterizer.lineWidth = 1.0f;
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

    pipelineColorFormat = colorFormat;
    RAVEN_LOG_INFO("Renderer: pipelines (re)created for format %d", static_cast<int>(colorFormat));
}

void Renderer::Impl::ReadQueryResults() {
    if (queryPool == VK_NULL_HANDLE || !queriesPending) return;
    uint64_t results[4] = {};
    VkResult res = vkGetQueryPoolResults(context.Device(), queryPool, 0, 4, sizeof(results), results, sizeof(uint64_t),
                                         VK_QUERY_RESULT_64_BIT);
    if (res != VK_SUCCESS) return;  // not ready yet — keep previous stats
    float periodNs = 1e-6f * context.TimestampPeriod();  // ticks -> us
    stats.passTimes.clear();
    float total = 0.0f;
    float shadowMs = (float)((double)(results[1] - results[0]) * context.TimestampPeriod() / 1e6);
    float forwardMs = (float)((double)(results[3] - results[2]) * context.TimestampPeriod() / 1e6);
    (void)periodNs;
    if (m_QueryShadowExecuted) stats.passTimes.push_back({"shadow", shadowMs});
    stats.passTimes.push_back({"forward", forwardMs});
    total = (m_QueryShadowExecuted ? shadowMs : 0.0f) + forwardMs;
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

vk::ImageResource CreateColorImage2D(vk::VulkanContext& context, uint32_t width, uint32_t height, VkFormat format) {
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
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
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

vk::ImageResource CreateDepthImage2D(vk::VulkanContext& context, uint32_t width, uint32_t height) {
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
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
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

VkImage Renderer::Impl::ImageForResource(const std::string& name, VkImage colorImage, VkImage depthImage) const {
    if (name == "mainColor") return colorImage;
    if (name == "mainDepth") return depthImage;
    if (name == "shadow") return shadowMap.image;
    return VK_NULL_HANDLE;
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
    const float aspect = targets.height > 0 ? static_cast<float>(targets.width) / static_cast<float>(targets.height) : 1.0f;
    const glm::mat4 view = camera.ViewMatrix();
    const glm::mat4 proj = camera.ProjectionMatrix(aspect);
    const float timeSeconds = static_cast<float>(
        std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count());

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

    if (queryPool != VK_NULL_HANDLE) vkCmdResetQueryPool(cb, queryPool, 0, 4);

    // --- render graph: pass declarations + barrier solve ---
    const bool shadowPass = hasSun && sun.castShadow;
    RenderGraph graph;
    graph.AddResource({"shadow", GraphResourceType::DepthImage, shadowMapSize, shadowMapSize});
    graph.AddResource({"mainColor", GraphResourceType::ColorImage, targets.width, targets.height});
    graph.AddResource({"mainDepth", GraphResourceType::DepthImage, targets.width, targets.height});

    GraphPassDesc shadowDesc;
    shadowDesc.name = "shadow";
    shadowDesc.enabled = shadowPass;
    shadowDesc.writes = {{ "shadow", GraphResourceState::DepthAttachmentWrite }};
    graph.AddPass(shadowDesc);

    GraphPassDesc forwardDesc;
    forwardDesc.name = "forward";
    if (shadowPass) forwardDesc.reads = {{ "shadow", GraphResourceState::SampledRead }};
    forwardDesc.writes = {
        { "mainColor", GraphResourceState::ColorAttachmentWrite },
        { "mainDepth", GraphResourceState::DepthAttachmentWrite },
    };
    graph.AddPass(forwardDesc);

    if (params.drawGrid) {
        GraphPassDesc gridDesc;
        gridDesc.name = "grid";
        gridDesc.sideEffect = true;  // draws UI-ish content with no consumers; never cull
        gridDesc.reads = {{ "mainColor", GraphResourceState::ColorAttachmentWrite }};   // LOAD existing color
        gridDesc.writes = {{ "mainColor", GraphResourceState::ColorAttachmentWrite }};
        graph.AddPass(gridDesc);
    }

    GraphPassDesc finalDesc;
    finalDesc.name = targets.toPresent ? "present" : "to-host";
    finalDesc.sideEffect = true;
    finalDesc.reads = {{ "mainColor", targets.toPresent ? GraphResourceState::Present : GraphResourceState::SampledRead }};
    finalDesc.writes = {{ "mainColor", targets.toPresent ? GraphResourceState::Present : GraphResourceState::SampledRead }};
    graph.AddPass(finalDesc);
    graph.SetExternalOutput("mainColor");

    const CompiledGraph compiled = graph.Compile();
    if (!compiled.ok) {
        RAVEN_LOG_ERROR("Renderer: render graph failed: %s", compiled.error.c_str());
    }

    auto applyBarriersFor = [&](const std::string& passName) {
        for (const BarrierOp& op : compiled.barriers) {
            if (op.beforePass != passName) continue;
            VkImage image = ImageForResource(op.resource, targets.colorImage, targets.depthImage);
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
    scene.World().ForEach<MeshRendererComponent>([&](ecs::Entity entity, const MeshRendererComponent& mr) {
        if (!mr.visible || mr.mesh.IsNil()) return;
        MeshGpu& gpu = GetOrCreateMeshGpu(mr.mesh);
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
        draws.push_back({&gpu, scene.GetWorldMatrix(entity), materialSet, albedo, mrParams, mr.castShadow});
    });

    uint32_t drawCalls = 0, triangles = 0, meshesDrawn = 0;

    const VkClearValue clearColor{{{0.022f, 0.028f, 0.048f, 1.0f}}};
    const VkClearValue depthClear{{1.0f, 0}};
    const VkClearValue shadowDepthClear{{1.0f, 0}};

    for (const GraphPassDesc* pass : compiled.executionOrder) {
        applyBarriersFor(pass->name);

        if (pass->name == "shadow") {
            if (queryPool != VK_NULL_HANDLE) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 0);

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
            if (queryPool != VK_NULL_HANDLE) vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 2);

            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, forwardPipeline);
            VkViewport viewport{0.0f, 0.0f, static_cast<float>(targets.width), static_cast<float>(targets.height), 0.0f, 1.0f};
            VkRect2D scissor{{0, 0}, {targets.width, targets.height}};
            vkCmdSetViewport(cb, 0, 1, &viewport);
            vkCmdSetScissor(cb, 0, 1, &scissor);

            VkRenderingAttachmentInfo colorAttachment{};
            colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAttachment.imageView = targets.colorView;
            colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colorAttachment.clearValue = clearColor;

            VkRenderingAttachmentInfo depthAttachment{};
            depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depthAttachment.imageView = targets.depthView;
            depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;  // grid pass needs the depth
            depthAttachment.clearValue = depthClear;

            VkRenderingInfo renderingInfo{};
            renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            renderingInfo.renderArea = scissor;
            renderingInfo.layerCount = 1;
            renderingInfo.colorAttachmentCount = 1;
            renderingInfo.pColorAttachments = &colorAttachment;
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
            VkViewport viewport{0.0f, 0.0f, static_cast<float>(targets.width), static_cast<float>(targets.height), 0.0f, 1.0f};
            VkRect2D scissor{{0, 0}, {targets.width, targets.height}};
            vkCmdSetViewport(cb, 0, 1, &viewport);
            vkCmdSetScissor(cb, 0, 1, &scissor);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gridPipeline);

            VkRenderingAttachmentInfo colorAttachment{};
            colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colorAttachment.imageView = targets.colorView;
            colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingAttachmentInfo depthAttachment{};
            depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depthAttachment.imageView = targets.depthView;
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

            GridPushConstants push{proj * view};
            vkCmdPushConstants(cb, gridPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
            VkBuffer vertexBuffers[] = {gridVertexBuffer.buffer};
            VkDeviceSize offsets[] = {0};
            vkCmdBindVertexBuffers(cb, 0, 1, vertexBuffers, offsets);
            vkCmdDraw(cb, gridVertexCount, 1, 0, 0);
            vkCmdEndRendering(cb);
        }
        // "present" / "to-host" passes need no commands; the graph emitted the barrier.
    }

    outStats.drawCalls = drawCalls;
    outStats.triangles = triangles;
    outStats.meshesDrawn = meshesDrawn;
    outStats.culledPasses = static_cast<uint32_t>(compiled.culledPasses.size());
    m_QueryShadowExecuted = shadowPass;
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

    // Quality tier drives shadow map size (and future MSAA/texture budgets) before base resources are built.
    impl.shadowMapSize = QualityPresetFor(config.qualityTier).shadowMapSize;

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
        if (impl.frameSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, impl.frameSetLayout, nullptr);
        if (impl.materialSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, impl.materialSetLayout, nullptr);
        if (impl.forwardPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, impl.forwardPipelineLayout, nullptr);
        if (impl.gridPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, impl.gridPipelineLayout, nullptr);
        if (impl.depthPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, impl.depthPipelineLayout, nullptr);

        // Samplers
        if (impl.shadowSampler != VK_NULL_HANDLE && impl.shadowSampler != impl.defaultSampler) {
            vkDestroySampler(device, impl.shadowSampler, nullptr);
        }
        if (impl.defaultSampler != VK_NULL_HANDLE) vkDestroySampler(device, impl.defaultSampler, nullptr);

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
