#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

#include "../Engine/Module/abi/module_api.h"
#include "module_bundle.hpp"

namespace {
bool write_text(const std::filesystem::path& path, const std::string& value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    return static_cast<bool>(output);
}

std::string manifest_json(const aeg::module::ModuleMetadata& metadata, std::string_view metadata_digest,
                          std::string_view binary_digest, std::string_view metadata_path = "module.metadata.json",
                          std::string_view binary_path = "game.dll", std::string_view build_id = "build-1",
                          std::string_view target = "windows-x64-vulkan", std::string_view configuration = "Debug",
                          std::string_view contract_digest = {}) {
    const std::string contract = contract_digest.empty() ? metadata.contract_digest : std::string(contract_digest);
    return "{\"manifest_schema_version\":1,\"metadata_path\":\"" + std::string(metadata_path) +
           "\",\"binary_path\":\"" + std::string(binary_path) + "\",\"build_id\":\"" + std::string(build_id) +
           "\",\"target\":\"" + std::string(target) + "\",\"configuration\":\"" + std::string(configuration) +
           "\",\"contract_digest\":\"" + contract + "\",\"metadata_sha256\":\"" + std::string(metadata_digest) +
           "\",\"binary_sha256\":\"" + std::string(binary_digest) + "\"}\n";
}
}  // namespace

int main() {
    using namespace aeg;
    const auto root = std::filesystem::temp_directory_path() / "aeg_module_metadata_smoke_bundle";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    if (error)
        return 1;

    module::ModuleMetadata metadata{};
    metadata.module.id = pipeline::ModuleId("com.aeg.test.metadata");
    metadata.module.name = "\xE6\xB5\x8B\xE8\xAF\x95\xE6\xA8\xA1\xE5\x9D\x97";
    metadata.module.version = {1, 2, 3};
    metadata.descriptor = metadata.module;
    metadata.build_id = "build-1";
    metadata.target = "windows-x64-vulkan";
    metadata.configuration = "Debug";
    metadata.abi_major = AEG_MODULE_ABI_MAJOR;
    metadata.abi_minor = AEG_MODULE_ABI_MINOR;

    pipeline::TypeCatalogEntry entry{};
    entry.key = {pipeline::AuthoringTypeKind::kPipeline, std::numeric_limits<uint64_t>::max() - 1u};
    entry.version = {2, 1, 0};
    entry.owner_module = metadata.module.id;
    entry.name = "Custom\xE6\xB5\x81\xE7\xA8\x8B";
    entry.display_name = "Custom Pipeline";
    entry.domains = 1u << static_cast<uint32_t>(pipeline::FeatureDomain::kRender);
    entry.scopes = 1u << static_cast<uint32_t>(pipeline::PipelineScope::kWorld);
    entry.ports.push_back({.id = 101,
                           .name = "Exposure",
                           .direction = pipeline::PortDirection::kInput,
                           .value_kind = pipeline::PortValueKind::kArtifact,
                           .schema_id = 1,
                           .binding_id = 101,
                           .input = pipeline::InputPortContract{
                               .allowed_sources = pipeline::input_source_mask(pipeline::InputSourceKind::kLiteral) |
                                                   pipeline::input_source_mask(pipeline::InputSourceKind::kDefault),
                               .presence = pipeline::InputPresenceRequirement::kOptional,
                               .variadic_group_id = 700,
                               .literal_schema = pipeline::ParameterDescriptor{.id = 101,
                                                                                .name = "Exposure",
                                                                                .display_name = "Exposure",
                                                                                .kind = pipeline::ParameterKind::kFloat64,
                                                                                .default_value = 1.0,
                                                                                .minimum = 0.0,
                                                                                .maximum = 4.0},
                               .default_value = pipeline::InputDefaultDescriptor{.id = 7,
                                                                                  .revision = 3,
                                                                                  .value = 1.0}}});
    entry.variadic_inputs.push_back({.id = 700,
                                     .name = "Layers",
                                     .member_port_id = 101,
                                     .min_slots = 0,
                                     .max_slots = 4,
                                     .ordered = true,
                                     .empty_is_valid = true});
    entry.declaration = pipeline::PipelineAuthoringDeclaration{};
    const auto add_entry = metadata.catalog.add(entry);
    if (!add_entry) {
        std::cerr << add_entry.message << '\n';
        return 2;
    }

    const auto contract = module::module_contract_sha256(metadata);
    if (!contract || contract.value.size() != 64u)
        return 3;
    metadata.contract_digest = contract.value;
    const auto encoded = module::encode_module_metadata(metadata);
    if (!encoded || encoded.value.find("18446744073709551614") == std::string::npos)
        return 4;
    const auto decoded = module::decode_module_metadata(encoded.value);
    const auto* decoded_entry = decoded ? decoded.value.catalog.find(entry.key) : nullptr;
    if (!decoded || decoded.value.module.name != metadata.module.name || decoded.value.catalog.entries().size() != 1u ||
        decoded_entry == nullptr || !decoded_entry->ports.front().input || decoded_entry->variadic_inputs.size() != 1u ||
        decoded_entry->ports.front().input->default_value->revision != 3u)
        return 5;
    const auto content_hash = module::module_content_sha256("abc");
    if (!content_hash || content_hash.value != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
        return 6;

    const auto metadata_path = root / "module.metadata.json";
    const auto binary_path = root / "game.dll";
    const auto manifest_path = root / "module.manifest.json";
    if (!write_text(metadata_path, encoded.value) || !write_text(binary_path, "binary-v1"))
        return 7;
    const auto metadata_digest = module::module_content_sha256(encoded.value);
    const auto binary_digest = module::module_content_sha256("binary-v1");
    if (!metadata_digest || !binary_digest ||
        !write_text(manifest_path, manifest_json(metadata, metadata_digest.value, binary_digest.value)))
        return 8;

    const auto valid_bundle = module::validate_module_bundle(manifest_path);
    if (!valid_bundle || valid_bundle.value.binary_path != binary_path ||
        valid_bundle.value.metadata_sha256 != metadata_digest.value)
        return 9;

    if (!write_text(binary_path, "binary-tampered") || module::validate_module_bundle(manifest_path))
        return 10;
    if (!write_text(binary_path, "binary-v1") || !write_text(metadata_path, encoded.value + " ") ||
        module::validate_module_bundle(manifest_path))
        return 11;
    if (!write_text(metadata_path, encoded.value) ||
        !write_text(manifest_path,
                    manifest_json(metadata, metadata_digest.value, binary_digest.value, "../module.metadata.json")) ||
        module::validate_module_bundle(manifest_path))
        return 12;
    if (!write_text(manifest_path, manifest_json(metadata, metadata_digest.value, binary_digest.value,
                                                 "module.metadata.json", "game.dll", "wrong-build")) ||
        module::validate_module_bundle(manifest_path))
        return 13;
    if (!write_text(manifest_path, manifest_json(metadata, metadata_digest.value, binary_digest.value,
                                                 "module.metadata.json", "game.dll", "build-1", "wrong-target")) ||
        module::validate_module_bundle(manifest_path))
        return 14;
    if (!write_text(manifest_path,
                    manifest_json(metadata, metadata_digest.value, binary_digest.value, "module.metadata.json",
                                  "game.dll", "build-1", "windows-x64-vulkan", "Debug", "deadbeef")) ||
        module::validate_module_bundle(manifest_path))
        return 15;

    std::filesystem::remove_all(root, error);
    std::cout << "module metadata smoke passed\n";
    return 0;
}
