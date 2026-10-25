#pragma once

#include "feature_module_export.hpp"

// Factories are local C++ functions. Only the fixed C entry point is exported.
#define AEG_DEFINE_GAME_MODULE(function_name) \
    ::aeg::module::FeatureModuleDefinition function_name()

#define AEG_EXPORT_GAME_MODULE_API(factory)                                                                    \
    extern "C" AEG_MODULE_EXPORT AegAbiResult AEG_ABI_CALL aeg_get_module_api(                                  \
        uint32_t requested_major, uint32_t requested_minor, uint32_t output_size, AegAbiModuleApi* output) {    \
        try {                                                                                                 \
            const auto request = aeg_check_module_api_request(                                                 \
                requested_major, requested_minor, output_size, output);                                        \
            if (request.code != AEG_ABI_OK) return request;                                                     \
            static const ::aeg::module::detail::FeatureModuleExport bridge((factory)());                        \
            return bridge.negotiate(requested_major, requested_minor, output_size, output);                    \
        } catch (...) {                                                                                       \
            return {sizeof(AegAbiResult), AEG_ABI_INVALID_STATE, {nullptr, 0}};                                 \
        }                                                                                                     \
    }
