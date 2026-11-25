// RavenEngine — Vulkan instance/device management (internal renderer header).
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace raven::vk {

struct VulkanContextConfig {
    const char* applicationName = "RavenEngine";
    uint32_t applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    bool enableValidation = false;         // VK_LAYER_KHRONOS_validation when present
    bool allowCpuDeviceFallback = true;    // lavapipe/SwiftShader-style CPU Vulkan
    bool requireSwapchain = false;         // true when rendering to a window surface
    std::vector<const char*> requiredInstanceExtensions;  // e.g. GLFW's glfwGetRequiredInstanceExtensions
    bool enablePortabilityEnumeration = false;            // macOS/iOS MoltenVK
};

struct QueueFamilySelection {
    uint32_t graphics = UINT32_MAX;
    uint32_t present = UINT32_MAX;
};

class VulkanContext {
public:
    VulkanContext() = default;
    ~VulkanContext();
    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    bool Initialize(const VulkanContextConfig& config, std::string* error = nullptr);
    void Shutdown();

    VkInstance Instance() const { return m_Instance; }
    VkPhysicalDevice PhysicalDevice() const { return m_PhysicalDevice; }
    VkDevice Device() const { return m_Device; }
    VkQueue GraphicsQueue() const { return m_GraphicsQueue; }
    VkQueue PresentQueue() const { return m_PresentQueue; }
    const QueueFamilySelection& QueueFamilies() const { return m_QueueFamilies; }

    bool SupportsDynamicRendering() const { return m_DynamicRendering; }
    bool SupportsSynchronization2() const { return m_Synchronization2; }
    bool SupportsTimelineSemaphores() const { return m_TimelineSemaphores; }
    bool IsCpuDevice() const { return m_CpuDevice; }

    std::string DeviceName() const { return m_DeviceName; }
    uint64_t DeviceLocalMemoryBytes() const { return m_DeviceLocalBytes; }
    float TimestampPeriod() const { return m_TimestampPeriod; }

    VkFormat FindDepthFormat() const;
    VkSampleCountFlagBits MaxSampleCount() const { return m_MaxSampleCount; }

    VkCommandPool CreateCommandPool(uint32_t queueFamily, VkCommandPoolCreateFlags flags = 0) const;
    VkCommandBuffer AllocateSecondaryCommandBuffer(VkCommandPool pool) const;

    VkCommandBuffer BeginSingleTimeCommands();
    bool EndSingleTimeCommands(VkCommandBuffer commandBuffer);

    // Creates an image + device-local memory + view (does not transition layout).
    VkImage CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
                        VkImageAspectFlags aspect, VkImageTiling tiling, VkImageCreateFlags flags,
                        VkDeviceMemory* outMemory) const;

    static void TransitionImageLayout(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout,
                                      VkImageLayout newLayout, VkImageAspectFlags aspect,
                                      uint32_t baseMipLevel, uint32_t levelCount,
                                      uint32_t baseArrayLayer = 0, uint32_t layerCount = 1);

private:
    bool CreateInstance(const VulkanContextConfig& config, std::string* error);
    bool PickPhysicalDevice(const VulkanContextConfig& config, std::string* error);
    bool CreateDevice(std::string* error);

    VulkanContextConfig m_Config{};
    VkInstance m_Instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
    VkDevice m_Device = VK_NULL_HANDLE;
    VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
    VkQueue m_PresentQueue = VK_NULL_HANDLE;
    QueueFamilySelection m_QueueFamilies{};
    VkCommandPool m_TransientPool = VK_NULL_HANDLE;   // shared transient pool for one-time commands
    VkPhysicalDeviceProperties m_Properties{};
    VkPhysicalDeviceMemoryProperties m_MemoryProperties{};
    VkDebugUtilsMessengerEXT m_DebugMessenger = VK_NULL_HANDLE;

    bool m_DynamicRendering = false;
    bool m_Synchronization2 = false;
    bool m_TimelineSemaphores = false;
    bool m_CpuDevice = false;
    float m_TimestampPeriod = 0.0f;
    VkSampleCountFlagBits m_MaxSampleCount = VK_SAMPLE_COUNT_1_BIT;
    std::string m_DeviceName;
    uint64_t m_DeviceLocalBytes = 0;
};

const char* VkResultToString(VkResult result);
void LogVk( VkResult result, const char* what);

} // namespace raven::vk
