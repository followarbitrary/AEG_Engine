/**
 * @file vulkan_config.hpp
 * @brief Vulkan-only compile-time aliases for common RHI diagnostics.
 */
#pragma once

#include "../../public/rhi_config.hpp"

#ifndef AEG_VULKAN_ENABLE_VALIDATION
#define AEG_VULKAN_ENABLE_VALIDATION AEG_RHI_ENABLE_VALIDATION
#endif

#ifndef AEG_VULKAN_ENABLE_DEBUG_UTILS
#define AEG_VULKAN_ENABLE_DEBUG_UTILS AEG_RHI_ENABLE_DEBUG
#endif

#ifndef AEG_VULKAN_ENABLE_DEBUG_NAMES
#define AEG_VULKAN_ENABLE_DEBUG_NAMES AEG_RHI_ENABLE_DEBUG_NAMES
#endif
