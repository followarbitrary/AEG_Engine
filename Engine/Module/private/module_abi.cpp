#include "module_abi.hpp"

#include <cstddef>
#include <cstring>

#include "../abi/module_pipeline_api.h"
#include "../abi/module_reflection_api.h"
#include "../abi/module_render_api.h"
namespace aeg::module {
namespace {
template <typename Table>
foundation::Result validate_optional_table(const Table* table, const char* name) noexcept {
    if (table == nullptr)
        return foundation::Result::success();
    if (table->struct_size < sizeof(uint32_t) * 2u || table->version == 0) {
        return foundation::invalid_argument(std::string("module ABI ") + name + " table prefix is invalid");
    }
    return foundation::Result::success();
}
}  // namespace

foundation::Result validate_abi_value(const AegAbiValue& value) noexcept {
    if (value.reserved != 0 || value.kind > AEG_VALUE_ASSET_UUID) {
        return foundation::invalid_argument("module ABI value kind or reserved bits are invalid");
    }
    if (value.kind == AEG_VALUE_STRING || value.kind == AEG_VALUE_ASSET_UUID) {
        if (value.data.string.data == nullptr && value.data.string.size != 0) {
            return foundation::invalid_argument("module ABI string has a null data pointer");
        }
        if (value.data.string.size > 1024u * 1024u) {
            return foundation::invalid_argument("module ABI string exceeds the 1 MiB limit");
        }
    }
    if (value.kind == AEG_VALUE_F64_ARRAY) {
        if (value.data.f64_array.data == nullptr && value.data.f64_array.size != 0) {
            return foundation::invalid_argument("module ABI array has a null data pointer");
        }
        if (value.data.f64_array.size > 65536u) {
            return foundation::invalid_argument("module ABI array exceeds the element limit");
        }
    }
    return foundation::Result::success();
}

foundation::Result validate_module_api(const AegAbiModuleApi& api, uint64_t supported) noexcept {
    constexpr uint32_t kRequiredPrefix =
        static_cast<uint32_t>(offsetof(AegAbiModuleApi, required_capabilities) + sizeof(api.required_capabilities));
    if (api.struct_size < kRequiredPrefix || api.abi_major != AEG_MODULE_ABI_MAJOR ||
        api.abi_minor > AEG_MODULE_ABI_MINOR)
        return foundation::invalid_argument("module ABI version or table size is invalid");
    if (api.reserved != 0)
        return foundation::invalid_argument("module ABI reserved field is non-zero");
    if (api.required_capabilities & ~supported)
        return foundation::unsupported("module capability is unsupported");
    if (api.struct_size >= offsetof(AegAbiModuleApi, metadata) + sizeof(api.metadata)) {
        if (const auto result = validate_optional_table(api.metadata, "metadata"); !result)
            return result;
    }
    if (api.struct_size >= offsetof(AegAbiModuleApi, registration) + sizeof(api.registration)) {
        if (const auto result = validate_optional_table(api.registration, "registration"); !result)
            return result;
    }
    if (api.struct_size >= offsetof(AegAbiModuleApi, declaration) + sizeof(api.declaration)) {
        if (const auto result = validate_optional_table(api.declaration, "declaration"); !result)
            return result;
    }
    if (api.struct_size >= offsetof(AegAbiModuleApi, task) + sizeof(api.task)) {
        if (const auto result = validate_optional_table(api.task, "task"); !result)
            return result;
    }
    if (api.struct_size >= offsetof(AegAbiModuleApi, render_graph) + sizeof(api.render_graph)) {
        if (const auto result = validate_optional_table(api.render_graph, "render graph"); !result)
            return result;
    }
    if (api.struct_size >= offsetof(AegAbiModuleApi, pass_setup) + sizeof(api.pass_setup)) {
        if (const auto result = validate_optional_table(api.pass_setup, "pass setup"); !result)
            return result;
    }
    if (api.struct_size >= offsetof(AegAbiModuleApi, pass_command) + sizeof(api.pass_command)) {
        if (const auto result = validate_optional_table(api.pass_command, "pass command"); !result)
            return result;
    }
    if (api.struct_size >= offsetof(AegAbiModuleApi, reflection) + sizeof(api.reflection)) {
        if (const auto result = validate_optional_table(api.reflection, "reflection"); !result)
            return result;
    }
    const auto has_table = [&api](std::size_t offset, const void* table) noexcept {
        return api.struct_size >= offset + sizeof(table) && table != nullptr;
    };
    if ((api.required_capabilities & AEG_CAP_METADATA) && !has_table(offsetof(AegAbiModuleApi, metadata), api.metadata))
        return foundation::invalid_argument("module metadata capability has no metadata table");
    if ((api.required_capabilities & AEG_CAP_PIPELINE_DECLARATION) &&
        !has_table(offsetof(AegAbiModuleApi, declaration), api.declaration))
        return foundation::invalid_argument("module pipeline capability has no declaration table");
    if ((api.required_capabilities & AEG_CAP_LOGIC) && !has_table(offsetof(AegAbiModuleApi, task), api.task))
        return foundation::invalid_argument("module logic capability has no task table");
    if ((api.required_capabilities & AEG_CAP_RENDER) &&
        !has_table(offsetof(AegAbiModuleApi, render_graph), api.render_graph))
        return foundation::invalid_argument("module render capability has no render graph table");
    if ((api.required_capabilities & AEG_CAP_VALUE_REFLECTION) &&
        !has_table(offsetof(AegAbiModuleApi, reflection), api.reflection))
        return foundation::invalid_argument("module reflection capability has no reflection table");
    if ((api.required_capabilities & AEG_CAP_RENDER_PROGRAMS) &&
        !has_table(offsetof(AegAbiModuleApi, pass_command), api.pass_command))
        return foundation::invalid_argument("module render program capability has no pass command table");
    return foundation::Result::success();
}
}  // namespace aeg::module

extern "C" AEG_MODULE_EXPORT AegAbiResult AEG_ABI_CALL aeg_get_module_api(uint32_t requested_major,
                                                                          uint32_t requested_minor,
                                                                          uint32_t output_size,
                                                                          AegAbiModuleApi* output) {
    AegAbiResult result{sizeof(AegAbiResult), AEG_ABI_OK, {nullptr, 0}};
    if (!output || output_size < sizeof(uint32_t) * 4) {
        result.code = AEG_ABI_INVALID_ARGUMENT;
        return result;
    }
    if (requested_major != AEG_MODULE_ABI_MAJOR || requested_minor > AEG_MODULE_ABI_MINOR) {
        result.code = AEG_ABI_UNSUPPORTED;
        return result;
    }
    const uint32_t bytes =
        output_size < sizeof(AegAbiModuleApi) ? output_size : static_cast<uint32_t>(sizeof(AegAbiModuleApi));
    // This fallback export only negotiates the fixed ABI prefix. Optional
    // capability tables must be supplied by a concrete module before they are
    // advertised in required_capabilities.
    AegAbiModuleApi api{sizeof(AegAbiModuleApi), AEG_MODULE_ABI_MAJOR, AEG_MODULE_ABI_MINOR, 0, 0};
    std::memcpy(output, &api, bytes);
    return result;
}
