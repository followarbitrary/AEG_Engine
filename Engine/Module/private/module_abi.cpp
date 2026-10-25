#include "module_abi.hpp"

#include <cstddef>
#include <cstring>

#include "../abi/feature_module_api.h"
#include "../abi/module_reflection_api.h"
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
    if (api.required_capabilities & ~(supported & static_cast<uint64_t>(AEG_CAP_ALL)))
        return foundation::unsupported("module capability is unsupported");
    if (api.struct_size >= offsetof(AegAbiModuleApi, metadata) + sizeof(api.metadata)) {
        if (const auto result = validate_optional_table(api.metadata, "metadata"); !result)
            return result;
    }
    if (api.struct_size >= offsetof(AegAbiModuleApi, features) + sizeof(api.features) && api.features) {
        const auto& table = *api.features;
        if (table.struct_size < sizeof(AegFeatureExecutionApi) || table.version != AEG_FEATURE_EXECUTION_API_VERSION ||
            !table.user_data || table.reserved != 0 || table.feature_count > AEG_FEATURE_ABI_MAX_DEFINITIONS ||
            !table.write_contracts || !table.describe_feature || !table.execute || !table.validate_state)
            return foundation::invalid_argument("module Feature execution table is invalid");
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
    if ((api.required_capabilities & AEG_CAP_FEATURES) && !has_table(offsetof(AegAbiModuleApi, features), api.features))
        return foundation::invalid_argument("module Feature capability has no execution table");
    if ((api.required_capabilities & AEG_CAP_VALUE_REFLECTION) &&
        !has_table(offsetof(AegAbiModuleApi, reflection), api.reflection))
        return foundation::invalid_argument("module reflection capability has no reflection table");
    return foundation::Result::success();
}
}  // namespace aeg::module

extern "C" AEG_MODULE_EXPORT AegAbiResult AEG_ABI_CALL aeg_get_module_api(uint32_t requested_major,
                                                                          uint32_t requested_minor,
                                                                          uint32_t output_size,
                                                                          AegAbiModuleApi* output) {
    const auto result = aeg_check_module_api_request(requested_major, requested_minor, output_size, output);
    if (result.code != AEG_ABI_OK) return result;
    const uint32_t bytes =
        output_size < sizeof(AegAbiModuleApi) ? output_size : static_cast<uint32_t>(sizeof(AegAbiModuleApi));
    // This fallback export only negotiates the fixed ABI prefix. Optional
    // capability tables must be supplied by a concrete module before they are
    // advertised in required_capabilities.
    AegAbiModuleApi api{};
    api.struct_size = sizeof(AegAbiModuleApi);
    api.abi_major = AEG_MODULE_ABI_MAJOR;
    api.abi_minor = AEG_MODULE_ABI_MINOR;
    std::memcpy(output, &api, bytes);
    return result;
}
