/**
 * @file rhi_config.hpp
 * @brief Compile-time switches for optional RHI diagnostics and validation.
 */
#pragma once

#ifndef AEG_RHI_ENABLE_VALIDATION
#define AEG_RHI_ENABLE_VALIDATION 0
#endif

#ifndef AEG_RHI_ENABLE_DEBUG
#define AEG_RHI_ENABLE_DEBUG 0
#endif

#ifndef AEG_RHI_ENABLE_DEBUG_NAMES
#define AEG_RHI_ENABLE_DEBUG_NAMES 0
#endif

namespace aeg::rhi::config {
    /** @brief Whether API argument and state validation is compiled in. */
    inline constexpr bool kValidationEnabled = AEG_RHI_ENABLE_VALIDATION != 0;
    /** @brief Whether backend debug instrumentation is compiled in. */
    inline constexpr bool kDebugEnabled = AEG_RHI_ENABLE_DEBUG != 0;
    /** @brief Whether debug object names are forwarded to the backend. */
    inline constexpr bool kDebugNamesEnabled = AEG_RHI_ENABLE_DEBUG_NAMES != 0;
} // namespace aeg::rhi::config
