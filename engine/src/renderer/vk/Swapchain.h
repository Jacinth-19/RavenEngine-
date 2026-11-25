// RavenEngine — swapchain (internal renderer header).
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace raven::vk {

struct VulkanContext;

class Swapchain {
public:
    Swapchain(const VulkanContext& context, VkSurfaceKHR surface);
    ~Swapchain();
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    // (Re)creates the swapchain for the given size. Returns false on hard failure.
    bool Create(uint32_t width, uint32_t height, std::string* error = nullptr);
    void Destroy();

    VkSwapchainKHR Handle() const { return m_Swapchain; }
    VkFormat ImageFormat() const { return m_ImageFormat; }
    VkColorSpaceKHR ColorSpace() const { return m_ColorSpace; }
    VkExtent2D Extent() const { return m_Extent; }
    uint32_t ImageCount() const { return static_cast<uint32_t>(m_Images.size()); }
    const std::vector<VkImage>& Images() const { return m_Images; }
    const std::vector<VkImageView>& ImageViews() const { return m_ImageViews; }
    uint32_t CurrentImageIndex() const { return m_CurrentImage; }

    // Acquire the next image; call before recording the frame. Returns false to skip the frame
    // (e.g. window minimized) — caller must not present.
    enum class AcquireResult { Success, Suboptimal, OutOfDate, Failed };
    AcquireResult AcquireNextImage(uint64_t timeoutNs, VkSemaphore imageAvailableSemaphore);

    // Presents the last acquired image. Returns false when the swapchain is stale (resize it).
    enum class PresentResult { Success, OutOfDate, Failed };
    PresentResult Present(VkSemaphore renderFinishedSemaphore);

private:
    const VulkanContext& m_Context;
    VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
    VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;
    VkFormat m_ImageFormat = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR m_ColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D m_Extent{0, 0};
    std::vector<VkImage> m_Images;
    std::vector<VkImageView> m_ImageViews;
    uint32_t m_CurrentImage = 0;
};

} // namespace raven::vk
