// Vulkan Memory Allocator — single implementation unit.
// Defined here so VMA's ~5k-line header is compiled exactly once (before any
#define VMA_IMPLEMENTATION
// user of the header) and nothing else in the engine needs to know.
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
