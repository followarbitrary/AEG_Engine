/**
 * @file vma_impl.cpp
 * @brief Provides the single VMA implementation translation unit for the Vulkan backend.
 */
#include <vulkan/vulkan.h>

// VMA's implementation must be compiled exactly once in the final target.
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
