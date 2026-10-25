#include "../public/module_bundle.hpp"

#include <fstream>
#include <nlohmann/json.hpp>
#include <system_error>

#include "module_metadata_json.hpp"

namespace aeg::module {
namespace {
using Json = nlohmann::json;
constexpr std::size_t kMaxManifestBytes = 8u * 1024u * 1024u;
constexpr std::size_t kMaxMetadataBytes = 16u * 1024u * 1024u;
constexpr std::size_t kMaxBinaryBytes = 512u * 1024u * 1024u;

foundation::Result invalid(std::string_view field) {
    return foundation::invalid_argument("module bundle field is invalid: " + std::string(field));
}

foundation::ResultValue<std::string> read_bytes(const std::filesystem::path& path, std::size_t max_bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {foundation::not_found("module bundle file is missing: " + path.string()), {}};
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < 0 || static_cast<std::uintmax_t>(end) > max_bytes)
        return {foundation::invalid_argument("module bundle file is too large"), {}};
    input.seekg(0, std::ios::beg);
    std::string contents(static_cast<std::size_t>(end), '\0');
    if (!contents.empty())
        input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!input && !input.eof())
        return {foundation::invalid_argument("module bundle file could not be read"), {}};
    return {foundation::Result::success(), std::move(contents)};
}

foundation::ResultValue<std::string> required_string(const Json& object, std::string_view name) {
    if (!object.contains(name) || !object.at(name).is_string() || object.at(name).get<std::string>().empty())
        return {invalid(name), {}};
    return {foundation::Result::success(), object.at(name).get<std::string>()};
}

foundation::ResultValue<std::filesystem::path> bundle_path(const std::filesystem::path& directory, const Json& manifest,
                                                           std::string_view field) {
    const auto value = required_string(manifest, field);
    if (!value)
        return {value.result, {}};
    const std::filesystem::path relative = std::filesystem::u8path(value.value);
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
        return {invalid(field), {}};
    const auto normalized = relative.lexically_normal();
    for (const auto& component : normalized) {
        if (component == "..")
            return {invalid(field), {}};
    }
    const auto candidate = (directory / normalized).lexically_normal();
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(directory, error);
    if (error)
        return {foundation::invalid_argument("module bundle directory could not be canonicalized"), {}};
    // Canonicalize both sides before comparing.  Mixing a canonical manifest
    // directory with an merely absolute candidate can produce a false
    // traversal result on Windows when the path contains non-ASCII segments.
    const auto candidate_absolute = std::filesystem::weakly_canonical(candidate, error);
    if (error)
        return {foundation::invalid_argument("module bundle path could not be canonicalized"), {}};
    const auto relative_to_root = candidate_absolute.lexically_relative(root);
    // Never use path::string() for validation on Windows: it converts through
    // the active ANSI code page and throws for perfectly valid UTF-8 paths.
    // Compare the path component itself so the check remains lossless.
    if (relative_to_root.empty() || relative_to_root == ".." ||
        *relative_to_root.begin() == std::filesystem::path{".."})
        return {invalid(field), {}};
    return {foundation::Result::success(), candidate};
}

bool same_path(const std::filesystem::path& left, const std::filesystem::path& right) {
    std::error_code error;
    return std::filesystem::equivalent(left, right, error) && !error;
}

foundation::ResultValue<ModuleBundle> validate_manifest(const std::filesystem::path& manifest_path,
                                                        const Json& manifest) {
    if (!manifest.contains("manifest_schema_version") || !manifest.at("manifest_schema_version").is_number_unsigned() ||
        manifest.at("manifest_schema_version").get<uint32_t>() != 1u)
        return {invalid("manifest_schema_version"), {}};
    const auto metadata_path = bundle_path(manifest_path.parent_path(), manifest, "metadata_path");
    const auto binary_path = bundle_path(manifest_path.parent_path(), manifest, "binary_path");
    const auto build_id = required_string(manifest, "build_id");
    const auto target = required_string(manifest, "target");
    const auto configuration = required_string(manifest, "configuration");
    const auto contract_digest = required_string(manifest, "contract_digest");
    const auto metadata_digest = required_string(manifest, "metadata_sha256");
    const auto binary_digest = required_string(manifest, "binary_sha256");
    if (!metadata_path)
        return {metadata_path.result, {}};
    if (!binary_path)
        return {binary_path.result, {}};
    if (!build_id)
        return {build_id.result, {}};
    if (!target)
        return {target.result, {}};
    if (!configuration)
        return {configuration.result, {}};
    if (!contract_digest)
        return {contract_digest.result, {}};
    if (!metadata_digest)
        return {metadata_digest.result, {}};
    if (!binary_digest)
        return {binary_digest.result, {}};

    if (same_path(metadata_path.value, manifest_path) || same_path(binary_path.value, manifest_path) ||
        same_path(metadata_path.value, binary_path.value))
        return {invalid("manifest paths"), {}};
    const auto metadata_bytes = read_bytes(metadata_path.value, kMaxMetadataBytes);
    const auto binary_bytes = read_bytes(binary_path.value, kMaxBinaryBytes);
    if (!metadata_bytes || !binary_bytes)
        return {metadata_bytes ? binary_bytes.result : metadata_bytes.result, {}};
    const auto metadata = detail::decode_metadata_json(metadata_bytes.value);
    if (!metadata)
        return {metadata.result, {}};
    if (metadata.value.build_id != build_id.value || metadata.value.target != target.value ||
        metadata.value.configuration != configuration.value || metadata.value.contract_digest != contract_digest.value)
        return {foundation::invalid_state("module metadata identity does not match manifest"), {}};
    const auto contract = module_contract_sha256(metadata.value);
    const auto metadata_hash = detail::metadata_sha256(metadata_bytes.value);
    const auto binary_hash = detail::metadata_sha256(binary_bytes.value);
    if (!contract || !metadata_hash || !binary_hash)
        return {foundation::invalid_state("module digest calculation failed"), {}};
    if (contract.value != contract_digest.value || metadata_hash.value != metadata_digest.value ||
        binary_hash.value != binary_digest.value)
        return {foundation::invalid_state("module bundle digest mismatch"), {}};

    ModuleBundle bundle;
    bundle.directory = manifest_path.parent_path();
    bundle.manifest_path = manifest_path;
    bundle.metadata_path = metadata_path.value;
    bundle.binary_path = binary_path.value;
    bundle.dll_path = binary_path.value;
    bundle.metadata = metadata.value;
    bundle.metadata_sha256 = metadata_hash.value;
    bundle.binary_sha256 = binary_hash.value;
    return {foundation::Result::success(), std::move(bundle)};
}
}  // namespace

foundation::ResultValue<ModuleMetadata> read_module_metadata(const std::filesystem::path& path) {
    const auto contents = read_bytes(path, kMaxMetadataBytes);
    if (!contents)
        return {contents.result, {}};
    return detail::decode_metadata_json(contents.value);
}

foundation::ResultValue<ModuleBundle> validate_module_bundle(const std::filesystem::path& manifest_path) {
    const auto contents = read_bytes(manifest_path, kMaxManifestBytes);
    if (!contents)
        return {contents.result, {}};
    try {
        const auto root = Json::parse(contents.value);
        if (!root.is_object())
            return {invalid("root"), {}};
        if (root.contains("metadata_path") || root.contains("binary_path") || root.contains("manifest_schema_version"))
            return validate_manifest(manifest_path, root);

        return {invalid("module bundle requires a manifest with metadata and binary paths"), {}};
    } catch (const std::exception& exception) {
        return {foundation::invalid_argument(std::string("module manifest JSON is invalid: ") + exception.what()), {}};
    }
}

foundation::ResultValue<std::string> encode_module_metadata(const ModuleMetadata& metadata) {
    return detail::encode_metadata_json(metadata);
}

foundation::ResultValue<ModuleMetadata> decode_module_metadata(std::string_view json) {
    return detail::decode_metadata_json(json);
}

foundation::ResultValue<std::string> module_contract_sha256(const ModuleMetadata& metadata) {
    ModuleMetadata contract = metadata;
    contract.build_id.clear();
    contract.target.clear();
    contract.configuration.clear();
    contract.contract_digest.clear();
    const auto encoded = detail::encode_metadata_json(contract);
    if (!encoded)
        return {encoded.result, {}};
    return detail::metadata_sha256(encoded.value);
}

foundation::ResultValue<std::string> module_content_sha256(std::string_view bytes) {
    return detail::metadata_sha256(bytes);
}
}  // namespace aeg::module
