#include <utility>
#include "GpuResources.h"
#include "VulkanContext.h"

#include <cstring>

#include "raven/core/Logger.h"

namespace raven::vk {

void BufferResource::Destroy(VkDevice device) {
    if (buffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, buffer, nullptr); buffer = VK_NULL_HANDLE; }
    if (memory != VK_NULL_HANDLE) { vkFreeMemory(device, memory, nullptr); memory = VK_NULL_HANDLE; }
    size = 0;
}

void ImageResource::Destroy(VkDevice device) {
    if (view != VK_NULL_HANDLE) { vkDestroyImageView(device, view, nullptr); view = VK_NULL_HANDLE; }
    if (image != VK_NULL_HANDLE) { vkDestroyImage(device, image, nullptr); image = VK_NULL_HANDLE; }
    if (memory != VK_NULL_HANDLE) { vkFreeMemory(device, memory, nullptr); memory = VK_NULL_HANDLE; }
    width = height = 0;
    mipLevels = 1;
    format = VK_FORMAT_UNDEFINED;
}

uint32_t FindMemoryType(const VkPhysicalDeviceMemoryProperties& props, uint32_t typeBits, VkMemoryPropertyFlags required) {
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & required) == required) return i;
    }
    return UINT32_MAX;
}

BufferResource CreateHostVisibleBuffer(VulkanContext& context, VkDeviceSize size,
                                       VkBufferUsageFlags usage, VkMemoryPropertyFlags propertyFlags) {
    BufferResource resource;
    resource.size = size;
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(context.Device(), &bufferInfo, nullptr, &resource.buffer) != VK_SUCCESS) {
        RAVEN_LOG_ERROR("Vulkan: vkCreateBuffer failed (%llu bytes)", static_cast<unsigned long long>(size));
        return {};
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(context.Device(), resource.buffer, &req);
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(context.PhysicalDevice(), &memProps);
    uint32_t typeIndex = FindMemoryType(memProps, req.memoryTypeBits, propertyFlags);
    if (typeIndex == UINT32_MAX) {
        RAVEN_LOG_ERROR("Vulkan: no suitable memory type for buffer");
        resource.Destroy(context.Device());
        return {};
    }
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = req.size;
    allocInfo.memoryTypeIndex = typeIndex;
    if (vkAllocateMemory(context.Device(), &allocInfo, nullptr, &resource.memory) != VK_SUCCESS) {
        resource.Destroy(context.Device());
        return {};
    }
    vkBindBufferMemory(context.Device(), resource.buffer, resource.memory, 0);
    return resource;
}

BufferResource UploadBuffer(VulkanContext& context, const void* data, VkDeviceSize size, VkBufferUsageFlags usage) {
    BufferResource staging = CreateHostVisibleBuffer(context, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (staging.buffer == VK_NULL_HANDLE) return {};
    void* mapped = nullptr;
    vkMapMemory(context.Device(), staging.memory, 0, size, 0, &mapped);
    std::memcpy(mapped, data, static_cast<size_t>(size));
    vkUnmapMemory(context.Device(), staging.memory);

    BufferResource deviceLocal = CreateHostVisibleBuffer(context, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (deviceLocal.buffer == VK_NULL_HANDLE) {
        staging.Destroy(context.Device());
        return {};
    }

    VkCommandBuffer cmd = context.BeginSingleTimeCommands();
    VkBufferCopy copyRegion{};
    copyRegion.size = size;
    vkCmdCopyBuffer(cmd, staging.buffer, deviceLocal.buffer, 1, &copyRegion);
    context.EndSingleTimeCommands(cmd);   // waits for completion

    staging.Destroy(context.Device());
    return deviceLocal;
}

ImageResource UploadImage(VulkanContext& context,
                          const std::vector<std::pair<uint32_t, uint32_t>>& mipSizes,
                          const std::vector<std::vector<uint8_t>>& mipPixels,
                          VkFormat format) {
    ImageResource resource;
    if (mipSizes.empty()) return resource;
    resource.width = mipSizes[0].first;
    resource.height = mipSizes[0].second;
    resource.mipLevels = static_cast<uint32_t>(mipSizes.size());
    resource.format = format;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {resource.width, resource.height, 1};
    imageInfo.mipLevels = resource.mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    if (vkCreateImage(context.Device(), &imageInfo, nullptr, &resource.image) != VK_SUCCESS) return {};

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(context.Device(), resource.image, &req);
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(context.PhysicalDevice(), &memProps);
    uint32_t typeIndex = FindMemoryType(memProps, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = req.size;
    allocInfo.memoryTypeIndex = typeIndex;
    if (vkAllocateMemory(context.Device(), &allocInfo, nullptr, &resource.memory) != VK_SUCCESS) {
        resource.Destroy(context.Device());
        return {};
    }
    vkBindImageMemory(context.Device(), resource.image, resource.memory, 0);

    VkCommandBuffer cmd = context.BeginSingleTimeCommands();
    VulkanContext::TransitionImageLayout(cmd, resource.image, VK_IMAGE_LAYOUT_UNDEFINED,
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
                                         0, resource.mipLevels);
    std::vector<BufferResource> stagingBuffers;
    stagingBuffers.reserve(resource.mipLevels);
    for (uint32_t level = 0; level < resource.mipLevels; ++level) {
        const auto& pixels = mipPixels[level];
        BufferResource staging = CreateHostVisibleBuffer(context, pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (staging.buffer == VK_NULL_HANDLE) {
            for (auto& s : stagingBuffers) s.Destroy(context.Device());
            context.EndSingleTimeCommands(cmd);
            resource.Destroy(context.Device());
            return {};
        }
        void* mapped = nullptr;
        vkMapMemory(context.Device(), staging.memory, 0, pixels.size(), 0, &mapped);
        std::memcpy(mapped, pixels.data(), pixels.size());
        vkUnmapMemory(context.Device(), staging.memory);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = level;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {mipSizes[level].first, mipSizes[level].second, 1};
        vkCmdCopyBufferToImage(cmd, staging.buffer, resource.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        stagingBuffers.push_back(std::move(staging));
    }
    VulkanContext::TransitionImageLayout(cmd, resource.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT,
                                         0, resource.mipLevels);
    context.EndSingleTimeCommands(cmd);   // submits once and waits for completion

    for (auto& s : stagingBuffers) s.Destroy(context.Device());

    resource.view = CreateImageView(context, resource.image, format, VK_IMAGE_ASPECT_COLOR_BIT, 0, resource.mipLevels);
    return resource;
}

VkImageView CreateImageView(VulkanContext& context, VkImage image, VkFormat format, VkImageAspectFlags aspect,
                            uint32_t baseMipLevel, uint32_t levelCount) {
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspect;
    viewInfo.subresourceRange.baseMipLevel = baseMipLevel;
    viewInfo.subresourceRange.levelCount = levelCount;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    VkResult result = vkCreateImageView(context.Device(), &viewInfo, nullptr, &view);
    if (result != VK_SUCCESS) RAVEN_LOG_ERROR("Vulkan: vkCreateImageView failed: %s", VkResultToString(result));
    return view;
}

} // namespace raven::vk
