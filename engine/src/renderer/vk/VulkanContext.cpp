#include "VulkanContext.h"

#include <cstring>
#include <iostream>
#include <algorithm>
#include <set>

#include "raven/core/Logger.h"

namespace raven::vk {

const char* VkResultToString(VkResult result) {
    switch (result) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        default: return "VK_RESULT_UNKNOWN";
    }
}

void LogVk(VkResult result, const char* what) {
    if (result != VK_SUCCESS) RAVEN_LOG_ERROR("Vulkan: %s failed: %s", what, VkResultToString(result));
}

static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
                                                    const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                    void* /*user*/) {
    LogLevel level = severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? LogLevel::Error
                   : severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT ? LogLevel::Warn
                   : LogLevel::Debug;
    Logger::Instance().Log(level, std::string("[validation] ") + data->pMessage);
    return VK_FALSE;
}

bool VulkanContext::Initialize(const VulkanContextConfig& config, std::string* error) {
    m_Config = config;
    if (!CreateInstance(config, error)) return false;
    if (!PickPhysicalDevice(config, error)) return false;
    if (!CreateDevice(error)) return false;
    return true;
}

void VulkanContext::Shutdown() {
    if (m_Device != VK_NULL_HANDLE && m_TransientPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_Device, m_TransientPool, nullptr);
        m_TransientPool = VK_NULL_HANDLE;
    }
    if (m_Device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_Device);
        vkDestroyDevice(m_Device, nullptr);
        m_Device = VK_NULL_HANDLE;
    }
    if (m_DebugMessenger != VK_NULL_HANDLE) {
        auto fn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_Instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (fn) fn(m_Instance, m_DebugMessenger, nullptr);
        m_DebugMessenger = VK_NULL_HANDLE;
    }
    if (m_Instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_Instance, nullptr);
        m_Instance = VK_NULL_HANDLE;
    }
    m_PhysicalDevice = VK_NULL_HANDLE;
    m_GraphicsQueue = VK_NULL_HANDLE;
    m_PresentQueue = VK_NULL_HANDLE;
}

VulkanContext::~VulkanContext() {
    Shutdown();
}

bool VulkanContext::CreateInstance(const VulkanContextConfig& config, std::string* error) {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = config.applicationName;
    appInfo.applicationVersion = config.applicationVersion;
    appInfo.pEngineName = "RavenEngine";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> extensions(config.requiredInstanceExtensions);
    std::vector<const char*> layers;

    if (config.enablePortabilityEnumeration) {
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    }

    // Debug utils + validation layer: only when requested and available.
    if (config.enableValidation) {
        uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> availableLayers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());
        bool validationAvailable = false;
        for (const auto& layer : availableLayers) {
            if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0) validationAvailable = true;
        }
        if (validationAvailable) {
            layers.push_back("VK_LAYER_KHRONOS_validation");
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        } else {
            RAVEN_LOG_WARN("Vulkan: validation requested but VK_LAYER_KHRONOS_validation is not installed");
        }
    }

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
    createInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
    createInfo.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();
    if (config.enablePortabilityEnumeration) {
        createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    VkResult result = vkCreateInstance(&createInfo, nullptr, &m_Instance);
    if (result != VK_SUCCESS) {
        if (error) *error = std::string("vkCreateInstance failed: ") + VkResultToString(result);
        return false;
    }

    // Set up the debug messenger if we enabled the layer.
    if (config.enableValidation && !layers.empty()) {
        VkDebugUtilsMessengerCreateInfoEXT debugInfo{};
        debugInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debugInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debugInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debugInfo.pfnUserCallback = DebugCallback;
        auto createDebug = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_Instance, "vkCreateDebugUtilsMessengerEXT"));
        if (createDebug) createDebug(m_Instance, &debugInfo, nullptr, &m_DebugMessenger);
    }
    return true;
}

bool VulkanContext::PickPhysicalDevice(const VulkanContextConfig& config, std::string* error) {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(m_Instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        if (error) *error = "no Vulkan-capable GPU found";
        return false;
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(m_Instance, &deviceCount, devices.data());

    uint32_t bestScore = 0;
    VkPhysicalDevice best = VK_NULL_HANDLE;
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(device, &props);

        // Queue families: must have a graphics-capable queue.
        uint32_t queueCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueCount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queueCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueCount, queues.data());

        QueueFamilySelection selection{};
        for (uint32_t i = 0; i < queueCount; ++i) {
            if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && selection.graphics == UINT32_MAX) selection.graphics = i;
            if (config.requireSwapchain) {
                VkBool32 presentSupport = VK_FALSE;
                // Surface is not yet attached at this stage; treat graphics==present for scoring.
                (void)presentSupport;
            }
        }
        if (selection.graphics == UINT32_MAX) continue;  // no graphics queue at all
        selection.present = selection.graphics;

        // Required device extensions.
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extCount);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, extensions.data());
        bool hasSwapchain = false;
        bool hasPortabilitySubset = false;
        for (const auto& ext : extensions) {
            if (std::strcmp(ext.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) hasSwapchain = true;
            if (std::strcmp(ext.extensionName, "VK_KHR_portability_subset") == 0) hasPortabilitySubset = true;
        }
        if (config.requireSwapchain && !hasSwapchain) continue;

        // Vulkan 1.3 features we need: dynamic rendering + synchronization2.
        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        VkPhysicalDeviceVulkan12Features features12{};
        features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        features12.pNext = &features13;
        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = &features12;
        vkGetPhysicalDeviceFeatures2(device, &features2);
        if (!features13.dynamicRendering) {
            RAVEN_LOG_DEBUG("Vulkan: skipping device '%s' (no dynamicRendering)", props.deviceName);
            continue;
        }

        uint32_t score = 0;
        switch (props.deviceType) {
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: score = 1000; break;
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score = 600; break;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: score = 300; break;
            case VK_PHYSICAL_DEVICE_TYPE_CPU:
                score = config.allowCpuDeviceFallback ? 100 : 0;
                break;
            default: score = 10;
        }
        if (hasPortabilitySubset) score += 5;  // MoltenVK works fine
        if (score == 0) continue;

        if (score > bestScore) {
            bestScore = score;
            best = device;
            m_QueueFamilies = selection;
            m_Properties = props;
            m_CpuDevice = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
            m_DynamicRendering = features13.dynamicRendering == VK_TRUE;
            m_Synchronization2 = features13.synchronization2 == VK_TRUE;
            m_TimelineSemaphores = features12.timelineSemaphore == VK_TRUE;
            m_TimestampPeriod = props.limits.timestampPeriod;
            m_DeviceName = props.deviceName;
            (void)features2;
        }
    }

    if (best == VK_NULL_HANDLE) {
        if (error) *error = "no suitable Vulkan physical device (need dynamicRendering support)";
        return false;
    }
    m_PhysicalDevice = best;
    vkGetPhysicalDeviceMemoryProperties(m_PhysicalDevice, &m_MemoryProperties);

    // Sum device-local heaps for display in the profiler.
    m_DeviceLocalBytes = 0;
    for (uint32_t i = 0; i < m_MemoryProperties.memoryHeapCount; ++i) {
        if (m_MemoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            m_DeviceLocalBytes += m_MemoryProperties.memoryHeaps[i].size;
    }

    // Max MSAA: start from 4x and intersect with device support.
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(m_PhysicalDevice, &supported);
    m_MaxSampleCount = VK_SAMPLE_COUNT_1_BIT;
    if (supported.sampleRateShading) {
        VkSampleCountFlags counts = m_Properties.limits.framebufferColorSampleCounts;
        if (counts & VK_SAMPLE_COUNT_4_BIT) m_MaxSampleCount = VK_SAMPLE_COUNT_4_BIT;
        else if (counts & VK_SAMPLE_COUNT_2_BIT) m_MaxSampleCount = VK_SAMPLE_COUNT_2_BIT;
    }

    RAVEN_LOG_INFO("Vulkan: selected '%s' (%s)%s", m_DeviceName.c_str(),
                   m_CpuDevice ? "CPU/lavapipe" : (m_Properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "discrete" : "integrated"),
                   m_TimelineSemaphores ? " [timeline-semaphores]" : "");
    RAVEN_LOG_INFO("Vulkan: memory %.1f MiB device-local, dynamicRendering=%d sync2=%d",
                   static_cast<double>(m_DeviceLocalBytes) / (1024.0 * 1024.0),
                   static_cast<int>(m_DynamicRendering), static_cast<int>(m_Synchronization2));
    return true;
}

bool VulkanContext::CreateDevice(std::string* error) {
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = m_QueueFamilies.graphics;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    std::vector<const char*> deviceExtensions;
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> available(extCount);
    vkEnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &extCount, available.data());
    auto hasExt = [&](const char* name) {
        for (const auto& e : available) if (std::strcmp(e.extensionName, name) == 0) return true;
        return false;
    };
    if (hasExt(VK_KHR_SWAPCHAIN_EXTENSION_NAME)) deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    // MoltenVK on macOS/iOS requires this subset extension.
    if (hasExt("VK_KHR_portability_subset")) deviceExtensions.push_back("VK_KHR_portability_subset");

    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;

    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.timelineSemaphore = m_TimelineSemaphores ? VK_TRUE : VK_FALSE;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &features13;
    features13.pNext = &features12;
    features12.timelineSemaphore = m_TimelineSemaphores ? VK_TRUE : VK_FALSE;

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext = &features2;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    createInfo.ppEnabledExtensionNames = deviceExtensions.empty() ? nullptr : deviceExtensions.data();

    VkResult result = vkCreateDevice(m_PhysicalDevice, &createInfo, nullptr, &m_Device);
    if (result != VK_SUCCESS) {
        if (error) *error = std::string("vkCreateDevice failed: ") + VkResultToString(result);
        return false;
    }
    vkGetDeviceQueue(m_Device, m_QueueFamilies.graphics, 0, &m_GraphicsQueue);
    vkGetDeviceQueue(m_Device, m_QueueFamilies.present, 0, &m_PresentQueue);
    return true;
}

VkFormat VulkanContext::FindDepthFormat() const {
    const VkFormat candidates[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT};
    for (VkFormat format : candidates) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(m_PhysicalDevice, format, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) return format;
    }
    return VK_FORMAT_D32_SFLOAT;  // mandatory to exist
}

VkCommandPool VulkanContext::CreateCommandPool(uint32_t queueFamily, VkCommandPoolCreateFlags flags) const {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = queueFamily;
    poolInfo.flags = flags;
    VkCommandPool pool = VK_NULL_HANDLE;
    LogVk(vkCreateCommandPool(m_Device, &poolInfo, nullptr, &pool), "vkCreateCommandPool");
    return pool;
}

VkCommandBuffer VulkanContext::AllocateSecondaryCommandBuffer(VkCommandPool pool) const {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;  // secondary pools can pass secondary flag via flags
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer buffer = VK_NULL_HANDLE;
    LogVk(vkAllocateCommandBuffers(m_Device, &allocInfo, &buffer), "vkAllocateCommandBuffers");
    return buffer;
}

VkCommandBuffer VulkanContext::BeginSingleTimeCommands() {
    if (m_TransientPool == VK_NULL_HANDLE) {
        m_TransientPool =
            CreateCommandPool(m_QueueFamilies.graphics, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
    }
    vkResetCommandPool(m_Device, m_TransientPool, 0);
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = m_TransientPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_Device, &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);
    return cmd;
}

bool VulkanContext::EndSingleTimeCommands(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    VkResult result = vkQueueSubmit(m_GraphicsQueue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(m_GraphicsQueue);
    // Command buffer memory returns to the transient pool; pool is reset on next use.
    return result == VK_SUCCESS;
}

VkImage VulkanContext::CreateImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
                                   VkImageAspectFlags /*aspect*/, VkImageTiling tiling, VkImageCreateFlags flags,
                                   VkDeviceMemory* outMemory) const {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = tiling;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.flags = flags;

    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(m_Device, &imageInfo, nullptr, &image) != VK_SUCCESS) return VK_NULL_HANDLE;

    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(m_Device, image, &memReq);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < m_MemoryProperties.memoryTypeCount; ++i) {
        if ((memReq.memoryTypeBits & (1u << i)) && (m_MemoryProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            allocInfo.memoryTypeIndex = i;
            break;
        }
    }
    if (allocInfo.memoryTypeIndex == UINT32_MAX) {
        vkDestroyImage(m_Device, image, nullptr);
        return VK_NULL_HANDLE;
    }
    if (vkAllocateMemory(m_Device, &allocInfo, nullptr, outMemory) != VK_SUCCESS) {
        vkDestroyImage(m_Device, image, nullptr);
        return VK_NULL_HANDLE;
    }
    vkBindImageMemory(m_Device, image, *outMemory, 0);
    return image;
}

void VulkanContext::TransitionImageLayout(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout,
                                          VkImageLayout newLayout, VkImageAspectFlags aspect,
                                          uint32_t baseMipLevel, uint32_t levelCount,
                                          uint32_t baseArrayLayer, uint32_t layerCount) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.baseMipLevel = baseMipLevel;
    barrier.subresourceRange.levelCount = levelCount;
    barrier.subresourceRange.baseArrayLayer = baseArrayLayer;
    barrier.subresourceRange.layerCount = layerCount;

    // Sensible fixed stage/access pairs for the transitions we perform.
    switch (oldLayout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            barrier.srcAccessMask = VK_ACCESS_2_NONE;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            break;
        default:
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
            break;
    }
    switch (newLayout) {
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            break;
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
            break;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            break;
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
            barrier.dstAccessMask = VK_ACCESS_2_NONE;
            break;
        default:
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
            break;
    }

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

} // namespace raven::vk
