// RavenEngine — internal GPU resource helpers (buffers, images, staging uploads).
// Uploads are blocking (transient command buffer + queue wait) and safe by contract.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace raven::vk {

struct VulkanContext;

struct BufferResource {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    BufferResource() = default;
    BufferResource(const BufferResource&) = delete;
    BufferResource& operator=(const BufferResource&) = delete;
    BufferResource(BufferResource&& other) noexcept { *this = std::move(other); }
    BufferResource& operator=(BufferResource&& other) noexcept {
        buffer = other.buffer; memory = other.memory; size = other.size;
        other.buffer = VK_NULL_HANDLE; other.memory = VK_NULL_HANDLE; other.size = 0;
        return *this;
    }
    void Destroy(VkDevice device);
};

struct ImageResource {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;
    VkFormat format = VK_FORMAT_UNDEFINED;
    ImageResource() = default;
    ImageResource(const ImageResource&) = delete;
    ImageResource& operator=(const ImageResource&) = delete;
    ImageResource(ImageResource&& other) noexcept { *this = std::move(other); }
    ImageResource& operator=(ImageResource&& other) noexcept {
        image = other.image; memory = other.memory; view = other.view;
        width = other.width; height = other.height; mipLevels = other.mipLevels; format = other.format;
        other.image = VK_NULL_HANDLE; other.memory = VK_NULL_HANDLE; other.view = VK_NULL_HANDLE;
        other.width = other.height = 0; other.mipLevels = 1; other.format = VK_FORMAT_UNDEFINED;
        return *this;
    }
    void Destroy(VkDevice device);
};

uint32_t FindMemoryType(const VkPhysicalDeviceMemoryProperties& props, uint32_t typeBits, VkMemoryPropertyFlags required);

// Creates a host-visible buffer (UBO/vertex staging/etc.). Memory is NOT mapped.
BufferResource CreateHostVisibleBuffer(VulkanContext& context, VkDeviceSize size,
                                       VkBufferUsageFlags usage, VkMemoryPropertyFlags propertyFlags);

// Blocking upload: creates a device-local buffer filled with `data`.
BufferResource UploadBuffer(VulkanContext& context, const void* data, VkDeviceSize size,
                            VkBufferUsageFlags usage);

// Blocking upload: creates a 2D image with the given mip chain and returns it with a view,
// ending in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL.
ImageResource UploadImage(VulkanContext& context,
                          const std::vector<std::pair<uint32_t, uint32_t>>& mipSizes,
                          const std::vector<std::vector<uint8_t>>& mipPixels,
                          VkFormat format);

VkImageView CreateImageView(VulkanContext& context, VkImage image, VkFormat format, VkImageAspectFlags aspect,
                            uint32_t baseMipLevel, uint32_t levelCount);

} // namespace raven::vk
