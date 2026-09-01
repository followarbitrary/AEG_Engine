#pragma once

#ifndef AEG_RHI_ENABLE_VALIDATION
#define AEG_RHI_ENABLE_VALIDATION 0
#endif

#ifndef AEG_RHI_ENABLE_DEBUG_UTILS
#define AEG_RHI_ENABLE_DEBUG_UTILS 0
#endif

#ifndef AEG_RHI_ENABLE_DEBUG_NAMES
#define AEG_RHI_ENABLE_DEBUG_NAMES 0
#endif

namespace aeg::rhi::config {
inline constexpr bool kValidationEnabled = AEG_RHI_ENABLE_VALIDATION != 0;
inline constexpr bool kDebugUtilsEnabled = AEG_RHI_ENABLE_DEBUG_UTILS != 0;
inline constexpr bool kDebugNamesEnabled = AEG_RHI_ENABLE_DEBUG_NAMES != 0;
}  // namespace aeg::rhi::config
