#pragma once

#include <filesystem>
#include <fstream>
#include <string_view>

#include "module_bundle.hpp"
#include "feature_module.hpp"
#include "feature_render_test_fixture.hpp"

namespace module_bundle_test {
using namespace aeg;
using namespace aeg::module;
using render_feature_test::require;

inline ModuleBundle write_bundle(const std::filesystem::path& binary, const std::filesystem::path& directory,
                          ModuleMetadata metadata, std::string_view build_id = "b",
                          std::string_view target = "Fixture") {
    std::filesystem::create_directories(directory);
    metadata.target = target;
    metadata.configuration = "Debug";
    metadata.build_id = build_id;
    const auto contract = module_contract_sha256(metadata);
    require(static_cast<bool>(contract), "fixture contract digest failed");
    metadata.contract_digest = contract.value;
    const auto source = encode_module_metadata(metadata);
    require(static_cast<bool>(source), "fixture metadata encoding failed");
    { std::ofstream out(directory / "module.metadata.json", std::ios::binary); out << source.value; }
    const auto copied = directory / binary.filename();
    if (binary != copied) std::filesystem::copy_file(binary, copied, std::filesystem::copy_options::overwrite_existing);
    std::ifstream stream(copied, std::ios::binary);
    const std::string contents{std::istreambuf_iterator<char>{stream}, {}};
    const auto binary_hash = module_content_sha256(contents);
    const auto metadata_hash = module_content_sha256(source.value);
    require(static_cast<bool>(binary_hash) && static_cast<bool>(metadata_hash), "fixture content digest failed");
    const auto manifest = directory / "module.manifest.json";
    { std::ofstream out(manifest, std::ios::binary);
      out << "{\"manifest_schema_version\":1,\"metadata_path\":\"module.metadata.json\",\"binary_path\":\""
          << copied.filename().generic_string() << "\",\"build_id\":\"" << metadata.build_id
          << "\",\"target\":\"" << metadata.target << "\",\"configuration\":\"Debug\","
          << "\"contract_digest\":\"" << contract.value << "\",\"metadata_sha256\":\"" << metadata_hash.value
          << "\",\"binary_sha256\":\"" << binary_hash.value << "\"}"; }
    const auto bundle = validate_module_bundle(manifest);
    if (!bundle) throw std::runtime_error("fixture formal bundle validation failed: " + bundle.result.message);
    return bundle.value;
}
inline ModuleMetadata metadata_for(const FeatureModuleDefinition& source) {
    ModuleMetadata metadata{};
    metadata.module = source.descriptor;
    for (auto entry = source.feature_catalog.entries().rbegin(); entry != source.feature_catalog.entries().rend(); ++entry)
        require(static_cast<bool>(metadata.feature_catalog.add_entry(*entry)), "reordered fixture catalog failed");
    return metadata;
}
}  // namespace module_bundle_test
