#pragma once
#include <filesystem>
#include <string_view>

#include "feature_catalog.hpp"
#include "module_descriptor.hpp"
namespace aeg::module {
inline constexpr uint32_t kModuleMetadataSchemaVersion = 3;
inline constexpr uint32_t kModuleSdkContractMajor = 3;
inline constexpr uint32_t kModuleSdkContractMinor = 0;

struct ModuleMetadata final {
    uint32_t schema_version{kModuleMetadataSchemaVersion};
    ModuleDescriptor module{};
    // Exported Feature contracts used by tooling/catalog discovery.  Contract
    // metadata includes port, side-effect, scope, clock and composition schema;
    // executable callbacks remain module-owned and are never serialized.
    feature::FeatureCatalog feature_catalog{};
    std::string build_id{};
    std::string target{};
    std::string configuration{};
    std::string contract_digest{};
    uint32_t abi_major{2};
    uint32_t abi_minor{0};
    uint32_t sdk_contract_major{kModuleSdkContractMajor};
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
