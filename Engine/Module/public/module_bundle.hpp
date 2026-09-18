#pragma once
#include <filesystem>
#include <string_view>

#include "game_module.hpp"
namespace aeg::module {
struct ModuleMetadata final {
    uint32_t schema_version{1};
    ModuleDescriptor descriptor{};
    // `module` is the B7 spelling. `descriptor` remains as a source-compatible
    // alias for the first metadata implementation; codecs keep both in sync.
    ModuleDescriptor module{};
    pipeline::TypeCatalog catalog{};
    std::string build_id{};
    std::string target{};
    std::string configuration{};
    std::string contract_digest{};
    uint32_t abi_major{1};
    uint32_t abi_minor{0};
    uint32_t sdk_contract_major{1};
    uint32_t sdk_contract_minor{0};
};
struct ModuleBundle final {
    std::filesystem::path directory{};
    std::filesystem::path dll_path{};
    std::filesystem::path metadata_path{};
    std::filesystem::path manifest_path{};
    std::filesystem::path binary_path{};
    ModuleMetadata metadata{};
    std::string binary_sha256{};
    std::string metadata_sha256{};
};
[[nodiscard]] foundation::ResultValue<ModuleMetadata> read_module_metadata(const std::filesystem::path& path);
[[nodiscard]] foundation::ResultValue<ModuleBundle> validate_module_bundle(const std::filesystem::path& manifest_path);
[[nodiscard]] foundation::ResultValue<std::string> encode_module_metadata(const ModuleMetadata& metadata);
[[nodiscard]] foundation::ResultValue<ModuleMetadata> decode_module_metadata(std::string_view json);
[[nodiscard]] foundation::ResultValue<std::string> module_contract_sha256(const ModuleMetadata& metadata);
[[nodiscard]] foundation::ResultValue<std::string> module_content_sha256(std::string_view bytes);
}  // namespace aeg::module
