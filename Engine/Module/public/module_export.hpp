#pragma once
#include <cstring>

#include "../abi/module_api.h"
#include "game_module.hpp"
#if defined(_WIN32)
#define AEG_GAME_MODULE_EXPORT extern "C" __declspec(dllexport)
#else
#define AEG_GAME_MODULE_EXPORT extern "C"
#endif
#define AEG_DEFINE_GAME_MODULE(function_name) AEG_GAME_MODULE_EXPORT aeg::module::ModuleDefinition function_name()

// Every dynamic GameModule must expose the fixed-width ABI handshake.  The
// declaration deliberately carries no C++ objects across the DLL boundary;
// the host later consumes the verified metadata sidecar to discover types.
#define AEG_EXPORT_GAME_MODULE_API()                                                                              \
    extern "C" AEG_MODULE_EXPORT AegAbiResult AEG_ABI_CALL aeg_get_module_api(                                    \
        uint32_t requested_major, uint32_t requested_minor, uint32_t output_size, AegAbiModuleApi* output) {      \
        AegAbiResult result{sizeof(AegAbiResult), AEG_ABI_OK, {nullptr, 0}};                                      \
        if (output == nullptr || output_size < sizeof(uint32_t) * 4u) {                                           \
            result.code = AEG_ABI_INVALID_ARGUMENT;                                                               \
            return result;                                                                                        \
        }                                                                                                         \
        if (requested_major != AEG_MODULE_ABI_MAJOR || requested_minor > AEG_MODULE_ABI_MINOR) {                  \
            result.code = AEG_ABI_UNSUPPORTED;                                                                    \
            return result;                                                                                        \
        }                                                                                                         \
        /* The handshake itself exposes no optional callback tables.  Keep the                                    \
           capability mask empty until a module supplies the corresponding                                        \
           versioned table; metadata sidecars remain the editor discovery path. */                                \
        const AegAbiModuleApi api{sizeof(AegAbiModuleApi), AEG_MODULE_ABI_MAJOR, AEG_MODULE_ABI_MINOR, 0u, 0u};   \
        const uint32_t bytes =                                                                                    \
            output_size < sizeof(AegAbiModuleApi) ? output_size : static_cast<uint32_t>(sizeof(AegAbiModuleApi)); \
        std::memcpy(output, &api, bytes);                                                                         \
        return result;                                                                                            \
    }
