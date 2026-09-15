/**
 * @file cooked_artifact_store.cpp
 * @brief Immutable cooked-bundle storage implementation.
 */
#include "cooked_artifact_store.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace aeg::tooling {
namespace {
std::atomic_uint64_t publication_sequence{0};

[[nodiscard]] const char* extension_for(AssetKind kind) {
    switch (kind) {
        case AssetKind::kMesh:
            return ".aegmesh";
        case AssetKind::kTexture:
            return ".aegtex";
        case AssetKind::kMaterial:
            return ".aegmatbin";
        case AssetKind::kScene:
            return ".aegscene";
        case AssetKind::kFont:
            return ".aegfont";
    }
    return ".aegasset";
}

[[nodiscard]] std::string kind_name(AssetKind kind) {
    switch (kind) {
        case AssetKind::kMesh:
            return "mesh";
        case AssetKind::kTexture:
            return "texture";
        case AssetKind::kMaterial:
            return "material";
        case AssetKind::kScene:
            return "scene";
        case AssetKind::kFont:
            return "font";
    }
    return {};
}

[[nodiscard]] foundation::ResultValue<AssetKind> parse_kind(std::string_view value) {
    constexpr std::array<std::pair<std::string_view, AssetKind>, 5> kKinds{{
        {"mesh", AssetKind::kMesh},
        {"texture", AssetKind::kTexture},
        {"material", AssetKind::kMaterial},
        {"scene", AssetKind::kScene},
        {"font", AssetKind::kFont},
    }};
    for (const auto& [name, kind] : kKinds) {
        if (value == name) {
            return {{}, kind};
        }
    }
    return {foundation::invalid_argument("cooked manifest contains an unknown asset kind"), {}};
}

[[nodiscard]] foundation::Result write_bytes(const std::filesystem::path& file, const std::vector<std::byte>& bytes) {
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return foundation::Result::failure(foundation::ResultCode::kBackendError,
                                           "failed to open cooked artifact for writing");
    }
    if (!bytes.empty()) {
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    if (!stream) {
        return foundation::Result::failure(foundation::ResultCode::kBackendError, "failed to write cooked artifact");
    }
    return foundation::Result::success();
}

[[nodiscard]] foundation::Result replace_file(const std::filesystem::path& source,
                                              const std::filesystem::path& destination) {
#if defined(_WIN32)
    if (::MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        return foundation::Result::failure(foundation::ResultCode::kBackendError,
                                           "failed to atomically replace cooked manifest");
    }
    return foundation::Result::success();
#else
    std::error_code error;
    std::filesystem::rename(source, destination, error);
    if (error) {
        return foundation::Result::failure(foundation::ResultCode::kBackendError,
                                           "failed to atomically replace cooked manifest");
    }
    return foundation::Result::success();
#endif
}

[[nodiscard]] bool is_safe_relative_path(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute()) {
        return false;
    }
    return std::none_of(path.begin(), path.end(), [](const std::filesystem::path& part) { return part == ".."; });
}

[[nodiscard]] std::string next_bundle_name() {
    const auto timestamp =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    return std::to_string(timestamp) + "_" + std::to_string(publication_sequence.fetch_add(1));
}
}  // namespace

CookedArtifactStore::CookedArtifactStore(std::filesystem::path cache_root) : cache_root_(std::move(cache_root)) {}

foundation::ResultValue<CookedArtifactBundle> CookedArtifactStore::publish(
    const SourceId& source, const std::vector<CookedArtifact>& artifacts) const {
    if (artifacts.empty()) {
        return {foundation::invalid_argument("a cooked bundle must contain at least one artifact"), {}};
    }

    auto ordered = artifacts;
    std::sort(ordered.begin(), ordered.end(),
              [](const CookedArtifact& left, const CookedArtifact& right) { return left.id < right.id; });
    if (std::adjacent_find(ordered.begin(), ordered.end(), [](const CookedArtifact& left, const CookedArtifact& right) {
            return left.id == right.id;
        }) != ordered.end()) {
        return {foundation::invalid_argument("a cooked bundle contains duplicate asset identities"), {}};
    }

    const auto source_directory = cache_root_ / source.to_string();
    const auto bundle_name = next_bundle_name();
    const auto bundle_directory = source_directory / bundle_name;
    const auto manifest = bundle_directory / "manifest.toml";
    std::error_code error;
    std::filesystem::create_directories(bundle_directory, error);
    if (error) {
        return {foundation::Result::failure(foundation::ResultCode::kBackendError,
                                            "failed to create cooked bundle directory"),
                {}};
    }

    std::ostringstream manifest_text;
    manifest_text << "version = 1\nsource_uuid = \"" << source.to_string() << "\"\n";
    for (const auto& artifact : ordered) {
        const auto filename = artifact.id.to_string() + extension_for(artifact.kind);
        const auto write_result = write_bytes(bundle_directory / filename, artifact.bytes);
        if (!write_result) {
            std::filesystem::remove_all(bundle_directory, error);
            return {write_result, {}};
        }
        manifest_text << "\n[[artifacts]]\nid = \"" << artifact.id.to_string() << "\"\nkind = \""
                      << kind_name(artifact.kind) << "\"\nschema_version = " << artifact.schema_version << "\nfile = \""
                      << filename << "\"\n";
    }
    {
        std::ofstream stream(manifest, std::ios::binary | std::ios::trunc);
        stream << manifest_text.str();
        if (!stream) {
            std::filesystem::remove_all(bundle_directory, error);
            return {foundation::Result::failure(foundation::ResultCode::kBackendError,
                                                "failed to write cooked bundle manifest"),
                    {}};
        }
    }

    const auto pointer_temporary = source_directory / ("current_" + bundle_name + ".tmp");
    {
        std::ofstream stream(pointer_temporary, std::ios::binary | std::ios::trunc);
        stream << "manifest = \"" << bundle_name << "/manifest.toml\"\n";
        if (!stream) {
            std::filesystem::remove_all(bundle_directory, error);
            return {foundation::Result::failure(foundation::ResultCode::kBackendError,
                                                "failed to write cooked bundle selector"),
                    {}};
        }
    }
    const auto replace_result = replace_file(pointer_temporary, source_directory / "current.toml");
    if (!replace_result) {
        std::filesystem::remove(pointer_temporary, error);
        return {replace_result, {}};
    }
    return load_current(source);
}

foundation::ResultValue<CookedArtifactBundle> CookedArtifactStore::load_current(const SourceId& source) const {
    const auto source_directory = cache_root_ / source.to_string();
    std::ifstream pointer(source_directory / "current.toml", std::ios::binary);
    std::string line;
    if (!std::getline(pointer, line) || !line.starts_with("manifest = \"") || !line.ends_with("\"")) {
        return {foundation::not_found("no valid cooked bundle is selected for this source"), {}};
    }
    const auto relative_manifest = line.substr(12, line.size() - 13);
    const std::filesystem::path manifest_relative_path(relative_manifest);
    if (!is_safe_relative_path(manifest_relative_path)) {
        return {foundation::invalid_argument("cooked bundle selector escapes its source cache directory"), {}};
    }
    const auto manifest = source_directory / manifest_relative_path;
    std::ifstream stream(manifest, std::ios::binary);
    if (!stream) {
        return {foundation::not_found("selected cooked bundle manifest is unavailable"), {}};
    }

    CookedArtifactBundle bundle{};
    bundle.manifest = manifest;
    std::string source_line;
    std::getline(stream, source_line);
    std::getline(stream, source_line);
    const auto expected_source_line = "source_uuid = \"" + source.to_string() + "\"";
    if (source_line != expected_source_line) {
        return {foundation::invalid_argument("cooked bundle manifest belongs to a different source"), {}};
    }
    std::string id_line;
    while (std::getline(stream, line)) {
        if (line != "[[artifacts]]" || !std::getline(stream, id_line)) {
            continue;
        }
        std::string kind_line;
        std::string schema_line;
        std::string file_line;
        if (!std::getline(stream, kind_line) || !std::getline(stream, schema_line) ||
            !std::getline(stream, file_line)) {
            return {foundation::invalid_argument("cooked bundle manifest is truncated"), {}};
        }
        if (!id_line.starts_with("id = \"") || !id_line.ends_with("\"") ||
            !kind_line.starts_with("kind = \"") || !kind_line.ends_with("\"")) {
            return {foundation::invalid_argument("cooked bundle manifest contains invalid artifact metadata"), {}};
        }
        const auto id_text = id_line.substr(6, id_line.size() - 7);
        const auto kind_text = kind_line.substr(8, kind_line.size() - 9);
        const auto parsed_id = AssetId::parse(id_text);
        const auto parsed_kind = parse_kind(kind_text);
        if (!parsed_id || !parsed_kind || !schema_line.starts_with("schema_version = ") ||
            !file_line.starts_with("file = \"") || !file_line.ends_with("\"")) {
            return {foundation::invalid_argument("cooked bundle manifest contains invalid artifact metadata"), {}};
        }
        uint32_t schema_version{};
        try {
            schema_version = static_cast<uint32_t>(std::stoul(schema_line.substr(17)));
        } catch (const std::exception&) {
            return {foundation::invalid_argument("cooked bundle manifest contains an invalid schema version"), {}};
        }
        const auto filename = file_line.substr(8, file_line.size() - 9);
        const std::filesystem::path artifact_relative_path(filename);
        if (!is_safe_relative_path(artifact_relative_path)) {
            return {foundation::invalid_argument("cooked bundle manifest escapes its bundle directory"), {}};
        }
        std::ifstream bytes_file(manifest.parent_path() / artifact_relative_path, std::ios::binary | std::ios::ate);
        if (!bytes_file) {
            return {foundation::not_found("cooked artifact referenced by manifest is unavailable"), {}};
        }
        const auto size = bytes_file.tellg();
        if (size < 0) {
            return {foundation::Result::failure(foundation::ResultCode::kBackendError,
                                                "failed to read cooked artifact size"),
                    {}};
        }
        CookedArtifact artifact{parsed_id.value, parsed_kind.value, schema_version, {}};
        artifact.bytes.resize(static_cast<size_t>(size));
        bytes_file.seekg(0);
        if (!artifact.bytes.empty()) {
            bytes_file.read(reinterpret_cast<char*>(artifact.bytes.data()), size);
        }
        if (!bytes_file) {
            return {
                foundation::Result::failure(foundation::ResultCode::kBackendError, "failed to read cooked artifact"),
                {}};
        }
        bundle.artifacts.push_back(std::move(artifact));
    }
    if (bundle.artifacts.empty()) {
        return {foundation::invalid_argument("cooked bundle manifest contains no artifacts"), {}};
    }
    return {{}, std::move(bundle)};
}

CookedArtifactIndex::CookedArtifactIndex(std::filesystem::path cache_root) : cache_root_(std::move(cache_root)) {}

foundation::Result CookedArtifactIndex::refresh() {
    std::error_code error;
    if (!std::filesystem::exists(cache_root_, error)) {
        return error ? foundation::Result::failure(foundation::ResultCode::kBackendError,
                                                   "failed to inspect cooked artifact cache")
                     : foundation::Result::success();
    }
    if (error || !std::filesystem::is_directory(cache_root_, error)) {
        return foundation::Result::failure(foundation::ResultCode::kBackendError,
                                           "cooked artifact cache root is not a directory");
    }

    std::map<AssetId, CookedArtifact> next;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(cache_root_, error)) {
        if (error) {
            return foundation::Result::failure(foundation::ResultCode::kBackendError,
                                               "failed while enumerating cooked artifact cache");
        }
        if (!entry.is_directory(error)) {
            if (error) {
                return foundation::Result::failure(foundation::ResultCode::kBackendError,
                                                   "failed while inspecting cooked artifact cache entry");
            }
            continue;
        }
        const auto source = SourceId::parse(entry.path().filename().string());
        if (!source) {
            continue;
        }
        const auto bundle = CookedArtifactStore(cache_root_).load_current(source.value);
        if (!bundle) {
            return bundle.result;
        }
        for (const CookedArtifact& artifact : bundle.value.artifacts) {
            if (!next.emplace(artifact.id, artifact).second) {
                return foundation::invalid_argument("multiple current bundles publish the same asset identity");
            }
        }
    }
    artifacts_ = std::move(next);
    return foundation::Result::success();
}

foundation::ResultValue<std::vector<std::byte>> CookedArtifactIndex::read(AssetId id) const {
    const auto found = artifacts_.find(id);
    if (found == artifacts_.end()) {
        return {foundation::not_found("asset identity is absent from the cooked artifact index"), {}};
    }
    return {{}, found->second.bytes};
}
}  // namespace aeg::tooling
