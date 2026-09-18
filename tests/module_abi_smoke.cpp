#include <array>
#include <cstring>
#include <iostream>
#include <limits>

#include "../Engine/Module/private/module_abi.hpp"
extern "C" AegAbiResult AEG_ABI_CALL aeg_get_module_api(uint32_t, uint32_t, uint32_t, AegAbiModuleApi*);
int main() {
    AegAbiModuleApi api{sizeof(AegAbiModuleApi), AEG_MODULE_ABI_MAJOR, AEG_MODULE_ABI_MINOR, 0, 0};
    if (!aeg::module::validate_module_api(api, AEG_CAP_METADATA))
        return 1;
    AegAbiModuleApi provided{};
    if (aeg_get_module_api(AEG_MODULE_ABI_MAJOR, AEG_MODULE_ABI_MINOR, sizeof(provided), &provided).code != AEG_ABI_OK)
        return 3;
    if (provided.required_capabilities != 0 || provided.metadata != nullptr || provided.registration != nullptr ||
        provided.declaration != nullptr)
        return 11;
    std::array<unsigned char, sizeof(AegAbiModuleApi) + 8> bounded{};
    std::memset(bounded.data() + sizeof(AegAbiModuleApi), 0xA5, 8);
    if (aeg_get_module_api(AEG_MODULE_ABI_MAJOR, AEG_MODULE_ABI_MINOR,
                           static_cast<uint32_t>(sizeof(AegAbiModuleApi) - sizeof(uint64_t)),
                           reinterpret_cast<AegAbiModuleApi*>(bounded.data()))
            .code != AEG_ABI_OK)
        return 4;
    for (size_t i = sizeof(AegAbiModuleApi); i < bounded.size(); ++i)
        if (bounded[i] != 0xA5)
            return 5;
    api.abi_major = 2;
    if (aeg::module::validate_module_api(api, AEG_CAP_METADATA))
        return 2;
    api.abi_major = AEG_MODULE_ABI_MAJOR;
    AegAbiMetadataApi malformed_metadata{sizeof(uint32_t) * 2u - 1u, 0u, nullptr};
    api.metadata = &malformed_metadata;
    if (aeg::module::validate_module_api(api, AEG_CAP_METADATA))
        return 10;
    api.metadata = nullptr;
    api.required_capabilities = AEG_CAP_METADATA;
    if (aeg::module::validate_module_api(api, AEG_CAP_METADATA))
        return 12;
    api.required_capabilities = 0;
    AegAbiValue text{};
    text.kind = AEG_VALUE_STRING;
    text.data.string = {"outline", 7};
    if (!aeg::module::validate_abi_value(text))
        return 6;
    AegAbiValue invalid_text{};
    invalid_text.kind = AEG_VALUE_STRING;
    invalid_text.data.string = {nullptr, 1};
    if (aeg::module::validate_abi_value(invalid_text).code != aeg::foundation::ResultCode::kInvalidArgument)
        return 7;
    AegAbiValue invalid_kind{};
    invalid_kind.kind = 99u;
    if (aeg::module::validate_abi_value(invalid_kind).code != aeg::foundation::ResultCode::kInvalidArgument)
        return 8;
    AegAbiValue huge_array{};
    huge_array.kind = AEG_VALUE_F64_ARRAY;
    huge_array.data.f64_array = {nullptr, 65537u};
    if (aeg::module::validate_abi_value(huge_array).code != aeg::foundation::ResultCode::kInvalidArgument)
        return 9;
    std::cout << "module ABI smoke passed\n";
    return 0;
}
