#include "Swapchain.h"
#include "VulkanContext.h"

#include <algorithm>
#include <limits>
#include <set>

#include "raven/core/Logger.h"

namespace raven::vk {

Swapchain::Swapchain(const VulkanContext& context, VkSurfaceKHR surface)
    : m_Context(context), m_Surface(surface) {}

Swapchain::~Swapchain() {
    Destroy();
}

void Swapchain::Destroy() {
    VkDevice device = m_Context.Device();
    for (VkImageView view : m_ImageViews) vkDestroyImageView(device, view, nullptr);
    m_ImageViews.clear();
    m_Images.clear();
    if (m_Swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, m_Swapchain, nullptr);
        m_Swapchain = VK_NULL_HANDLE;
    }
}

bool Swapchain::Create(uint32_t width, uint32_t height, std::string* error) {
    Destroy();

    // Query capabilities.
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_Context.PhysicalDevice(), m_Surface, &caps);

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_Context.PhysicalDevice(), m_Surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_Context.PhysicalDevice(), m_Surface, &formatCount, formats.data());

    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_Context.PhysicalDevice(), m_Surface, &presentModeCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_Context.PhysicalDevice(), m_Surface, &presentModeCount, presentModes.data());

    // Pick format: prefer B8G8R8A8 SRGB.
    VkSurfaceFormatKHR chosenFormat = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : formats[0];
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosenFormat = f;
            break;
        }
    }
    m_ImageFormat = chosenFormat.format;
    m_ColorSpace = chosenFormat.colorSpace;

    // Pick present mode: mailbox > immediate > fifo (fifo is guaranteed).
    VkPresentModeKHR chosenMode = VK_PRESENT_MODE_FIFO_KHR;
    for (const auto& mode : presentModes) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) { chosenMode = mode; break; }
        if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR) chosenMode = mode;
    }

    // Extent: use the surface's current extent when fixed, else clamp.
    if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        m_Extent = caps.currentExtent;
    } else {
        m_Extent = {std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width),
                    std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height)};
    }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = m_Surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = m_ImageFormat;
    createInfo.imageColorSpace = m_ColorSpace;
    createInfo.imageExtent = m_Extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.preTransform = caps.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = chosenMode;
    createInfo.clipped = VK_TRUE;

    VkResult result = vkCreateSwapchainKHR(m_Context.Device(), &createInfo, nullptr, &m_Swapchain);
    if (result != VK_SUCCESS) {
        if (error) *error = std::string("vkCreateSwapchainKHR failed: ") + VkResultToString(result);
        return false;
    }

    uint32_t actualImageCount = 0;
    vkGetSwapchainImagesKHR(m_Context.Device(), m_Swapchain, &actualImageCount, nullptr);
    m_Images.resize(actualImageCount);
    vkGetSwapchainImagesKHR(m_Context.Device(), m_Swapchain, &actualImageCount, m_Images.data());

    m_ImageViews.resize(m_Images.size());
    for (size_t i = 0; i < m_Images.size(); ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = m_Images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = m_ImageFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(m_Context.Device(), &viewInfo, nullptr, &m_ImageViews[i]) != VK_SUCCESS) {
            if (error) *error = "failed creating swapchain image view";
            Destroy();
            return false;
        }
    }
    RAVEN_LOG_INFO("Vulkan: swapchain %ux%u, %u images, format %d", m_Extent.width, m_Extent.height,
                   static_cast<uint32_t>(m_Images.size()), static_cast<int>(m_ImageFormat));
    return true;
}

Swapchain::AcquireResult Swapchain::AcquireNextImage(uint64_t timeoutNs, VkSemaphore imageAvailableSemaphore) {
    uint32_t imageIndex = 0;
    VkResult result = vkAcquireNextImageKHR(m_Context.Device(), m_Swapchain, timeoutNs,
                                            imageAvailableSemaphore, VK_NULL_HANDLE, &imageIndex);
    if (result == VK_SUCCESS) {
        m_CurrentImage = imageIndex;
        return AcquireResult::Success;
    }
    if (result == VK_SUBOPTIMAL_KHR) {
        m_CurrentImage = imageIndex;
        return AcquireResult::Suboptimal;
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR) return AcquireResult::OutOfDate;
    return AcquireResult::Failed;
}

Swapchain::PresentResult Swapchain::Present(VkSemaphore renderFinishedSemaphore) {
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &renderFinishedSemaphore;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &m_Swapchain;
    presentInfo.pImageIndices = &m_CurrentImage;

    VkResult result = vkQueuePresentKHR(m_Context.PresentQueue(), &presentInfo);
    if (result == VK_SUCCESS) return PresentResult::Success;
    if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR) return PresentResult::OutOfDate;
    return PresentResult::Failed;
}

} // namespace raven::vk
