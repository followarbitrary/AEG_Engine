#include "project_cooker.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <span>
#include <system_error>
#include <unordered_set>
#include <utility>

#include "asset_cooker.hpp"
#include "asset_dependency_packer.hpp"
#include "asset_metadata.hpp"
#include "cooked_artifact_store.hpp"
#include "cooked_asset.hpp"
#include "game_build_manifest.hpp"
#include "module_bundle.hpp"
#include "pipeline_asset_importer.hpp"
#include "project_context.hpp"
#include "scene_asset.hpp"
#include "scene_payload.hpp"
#include "serialization.hpp"

namespace aeg::tooling {
namespace {
pipeline::DocumentDiagnostic diagnostic(std::string code, std::string message) {
    return {std::move(code), std::move(message), {}, {}, 0, 0};
}

asset::AssetId runtime_id(const AssetId& source) {
    asset::AssetId result{};
    result.value.bytes = source.value.bytes;
    return result;
}

scene::SceneAssetId scene_id(const AssetId& source) {
    uint64_t high = 0;
    uint64_t low = 0;
    for (std::size_t index = 0; index < 8; ++index)
        high = (high << 8u) | source.value.bytes[index];
    for (std::size_t index = 8; index < 16; ++index)
        low = (low << 8u) | source.value.bytes[index];
    return scene::SceneAssetId{high, low};
}

foundation::ResultValue<AssetId> source_product(const std::filesystem::path& file, AssetKind expected) {
    const auto metadata = AssetMetaCodec{}.load(file.string() + ".meta");
    if (!metadata)
        return {metadata.result, {}};
    if (metadata.value.products.size() != 1 || metadata.value.products.front().kind != expected ||
        metadata.value.products.front().id == AssetId{}) {
        return {foundation::invalid_argument("Startup asset metadata must declare one product of the expected kind"),
                {}};
    }
    return {foundation::Result::success(), metadata.value.products.front().id};
}

foundation::ResultValue<std::string> digest_file(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input)
        return {foundation::not_found("Cannot read file for digest: " + file.string()), {}};
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    return module::module_content_sha256(bytes);
}

foundation::Result validate_scene_reference_archive(const asset::ScenePayload& payload) {
    const auto archive = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(payload.serialized_scene.data()),
                                                  payload.serialized_scene.size());
    serialization::BinaryReader reader(archive);
    const auto decoded = scene::SceneSerializer::deserialize(reader);
    if (!decoded)
        return decoded.result;
    std::vector<bool> consumed(payload.references.size(), false);
    const auto mesh_schema = reflection::TypeId("Game.MeshRenderer");
    const auto mesh_field = reflection::FieldId("mesh_asset_id").value();
    const auto material_field = reflection::FieldId("material_asset_id").value();
    for (const auto& component : decoded.value.components()) {
        if (component.schema_type != mesh_schema)
            continue;
        uint64_t entity_high = component.entity.high(), entity_low = component.entity.low();
        std::vector<std::pair<uint64_t, uint64_t>> fields;
        if (component.payload.size() < 16)
            return foundation::invalid_argument("Scene component reflection payload is truncated");
        uint32_t count = 0;
        std::memcpy(&count, component.payload.data() + 12, sizeof(count));
        std::size_t offset = 16;
        for (uint32_t index = 0; index < count; ++index) {
            if (offset > component.payload.size() || component.payload.size() - offset < 12)
                return foundation::invalid_argument("Scene component field is truncated");
            uint64_t id = 0;
            uint32_t size = 0;
            std::memcpy(&id, component.payload.data() + offset, sizeof(id));
            std::memcpy(&size, component.payload.data() + offset + 8, sizeof(size));
            offset += 12;
            if (size > component.payload.size() - offset)
                return foundation::invalid_argument("Scene component field is truncated");
            if ((id == mesh_field || id == material_field) && size != sizeof(uint64_t))
                return foundation::invalid_argument("Scene asset field width is invalid");
            uint64_t value = 0;
            if ((id == mesh_field || id == material_field) && size == sizeof(uint64_t))
                std::memcpy(&value, component.payload.data() + offset, sizeof(value));
            fields.emplace_back(id, value);
            offset += size;
        }
        for (const auto& [id, value] : fields) {
            if ((id != mesh_field && id != material_field) || value == 0)
                continue;
            std::size_t matches = 0;
            for (std::size_t index = 0; index < payload.references.size(); ++index) {
                const auto& reference = payload.references[index];
                uint64_t high = 0, low = 0;
                for (int byte = 0; byte != 8; ++byte)
                    high = (high << 8u) | reference.scene_entity_uuid[static_cast<std::size_t>(byte)];
                for (int byte = 0; byte != 8; ++byte)
                    low = (low << 8u) | reference.scene_entity_uuid[static_cast<std::size_t>(8 + byte)];
                if (high == entity_high && low == entity_low && reference.component_type == "MeshRenderer" &&
                    reference.field_id == id) {
                    if ((id == mesh_field && reference.kind != AssetKind::kMesh) ||
                        (id == material_field && reference.kind != AssetKind::kMaterial))
                        return foundation::invalid_argument("Scene reference kind does not match component field");
                    if (++matches != 1)
                        return foundation::invalid_argument("Scene component reference is duplicated");
                    consumed[index] = true;
                }
            }
            if (matches != 1)
                return foundation::invalid_argument("Scene component asset field has no relocation reference");
        }
    }
    for (bool matched : consumed)
        if (!matched)
            return foundation::invalid_argument("Scene relocation reference does not match archive");
    return foundation::Result::success();
}

foundation::Result ensure_scene_cooked(const std::filesystem::path& source, const AssetId& expected,
                                       const std::filesystem::path& cache_root,
                                       const std::filesystem::path& project_root) {
    const auto metadata = AssetMetaCodec{}.load(source.string() + ".meta");
    if (!metadata)
        return metadata.result;
    if (metadata.value.products.size() != 1 || metadata.value.products.front().id != expected ||
        metadata.value.products.front().kind != AssetKind::kScene)
        return foundation::invalid_argument("Startup scene metadata does not match the project manifest");
    std::ifstream input(source, std::ios::binary);
    if (!input)
        return foundation::not_found("Startup scene source is missing");
    const std::string bytes((std::istreambuf_iterator<char>(input)), {});
    asset::ScenePayload payload{};
    std::vector<uint64_t> stable_ids;
    const auto decoded = asset::decode_scene_payload(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
    if (decoded) {
        payload = decoded.value;
    } else if (asset::has_scene_payload_header(
                   std::span<const std::byte>(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()))) {
        return decoded.result;
    } else {
        payload.serialized_scene.assign(reinterpret_cast<const std::byte*>(bytes.data()),
                                        reinterpret_cast<const std::byte*>(bytes.data() + bytes.size()));
        // Project scaffolds and older editor exports may contain a raw
        // SceneSerializer archive whose stable identity predates the .meta
        // product UUID. Relocate that identity during Cook so the runtime
        // package has one authoritative Scene UUID.
        const auto archive_bytes =
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
        serialization::BinaryReader reader(archive_bytes);
        const auto archive = scene::SceneSerializer::deserialize(reader);
        if (archive && archive.value.id() != scene_id(expected)) {
            auto relocated = std::move(archive.value);
            if (const auto identity =
                    relocated.set_identity(scene_id(expected), std::string(relocated.name()), relocated.version());
                !identity)
                return identity;
            serialization::BinaryWriter writer;
            if (const auto serialized = scene::SceneSerializer::serialize(relocated, writer); !serialized)
                return serialized;
            payload.serialized_scene.assign(reinterpret_cast<const std::byte*>(writer.data().data()),
                                            reinterpret_cast<const std::byte*>(writer.data().data() + writer.size()));
        }
        if (archive) {
            // Build relocation records directly from the reflected wire payload.
            // The tooling target intentionally does not depend on Runtime; the
            // schema names and field ids are stable FNV identities shared by the
            // runtime reflection descriptors.
            auto hash_name = [](std::string_view text) {
                uint64_t hash = 14695981039346656037ull;
                for (const unsigned char c : text) {
                    hash ^= c;
                    hash *= 1099511628211ull;
                }
                return hash == 0 ? 1ull : hash;
            };
            const uint64_t mesh_type = hash_name("Game.MeshRenderer");
            const uint64_t text_type = hash_name("Game.TextRenderer");
            const uint64_t mesh_field = hash_name("mesh_asset_id");
            const uint64_t material_field = hash_name("material_asset_id");
            const uint64_t font_field = hash_name("font_asset_id");
            for (const auto& component : archive.value.components()) {
                const uint64_t type = component.schema_type.value();
                if (type != mesh_type && type != text_type)
                    continue;
                serialization::BinaryReader fields(
                    std::span<const uint8_t>{component.payload.data(), component.payload.size()});
                const auto encoded_type = fields.read_u64();
                const auto encoded_version = fields.read_u32();
                const auto field_count = fields.read_u32();
                if (!encoded_type || !encoded_version || !field_count || encoded_type.value != type) {
                    return foundation::invalid_argument("Scene component reflection payload is malformed");
                }
                for (uint32_t i = 0; i < field_count.value; ++i) {
                    const auto field_id = fields.read_u64();
                    const auto field_size = fields.read_u32();
                    if (!field_id || !field_size)
                        return foundation::invalid_argument("Scene component field is truncated");
                    const auto bytes = fields.read_bytes(field_size.value);
                    if (!bytes)
                        return bytes.result;
                    const bool is_mesh = type == mesh_type && field_id.value == mesh_field;
                    const bool is_material = type == mesh_type && field_id.value == material_field;
                    const bool is_font = type == text_type && field_id.value == font_field;
                    if (!is_mesh && !is_material && !is_font)
                        continue;
                    if (bytes.value.size() != sizeof(uint64_t))
                        return foundation::invalid_argument("Scene asset field width is invalid");
                    uint64_t stable = 0;
                    std::memcpy(&stable, bytes.value.data(), sizeof(stable));
                    if (stable == 0)
                        continue;
                    asset::SceneAssetReference reference{};
                    for (int n = 0; n != 8; ++n)
                        reference.scene_entity_uuid[static_cast<size_t>(n)] =
                            static_cast<uint8_t>(component.entity.high() >> (56 - n * 8));
                    for (int n = 0; n != 8; ++n)
                        reference.scene_entity_uuid[static_cast<size_t>(8 + n)] =
                            static_cast<uint8_t>(component.entity.low() >> (56 - n * 8));
                    reference.component_type = type == mesh_type ? "MeshRenderer" : "TextRenderer";
                    reference.component_version = component.schema_version;
                    reference.field_id = field_id.value;
                    reference.kind =
                        is_mesh ? AssetKind::kMesh : (is_material ? AssetKind::kMaterial : AssetKind::kFont);
                    reference.asset = AssetId{};
                    payload.references.push_back(reference);
                    // Keep the stable editor id in a side-car list for the
                    // resolver below. The order matches payload.references.
                    stable_ids.push_back(stable);
                }
            }
        }
    }
    if (!stable_ids.empty()) {
        const auto manifest = project_root / ".aeg" / "asset_database.tsv";
        std::ifstream db(manifest);
        if (!db)
            return foundation::not_found("Scene asset references require .aeg/asset_database.tsv");
        StableAssetIdIndex index;
        std::map<uint64_t, std::vector<std::pair<AssetKind, AssetId>>> products;
        std::unordered_set<uint64_t> needed(stable_ids.begin(), stable_ids.end());
        std::string line;
        if (!std::getline(db, line) || line != "AEG_ASSET_DATABASE\t1")
            return foundation::invalid_argument("Asset database manifest header is invalid");
        const auto root = std::filesystem::weakly_canonical(project_root);
        AssetCooker cooker(cache_root);
        if (const auto registered = cooker.register_default_importers(); !registered)
            return registered;
        std::unordered_set<std::string> cooked_sources;
        while (std::getline(db, line)) {
            if (line.empty())
                continue;
            std::vector<std::string> fields;
            std::size_t start = 0;
            while (start <= line.size()) {
                const auto end = line.find('\t', start);
                fields.push_back(line.substr(start, end == std::string::npos ? line.size() - start : end - start));
                if (end == std::string::npos)
                    break;
                start = end + 1;
            }
            if (fields.size() != 8 && fields.size() != 9)
                return foundation::invalid_argument("Asset database manifest record is malformed");
            auto parse_u64 = [](std::string_view text, uint64_t& value) {
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
                return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
            };
            uint64_t stable = 0, kind_value = 0, status_value = 0, file_size = 0, modified = 0, hash = 0;
            if (!parse_u64(fields[0], stable) || !parse_u64(fields[1], kind_value) ||
                !parse_u64(fields[2], status_value) || !parse_u64(fields[4], file_size) ||
                !parse_u64(fields[5], modified) || !parse_u64(fields[6], hash) || stable == 0 || status_value != 0)
                return foundation::invalid_argument("Asset database manifest number or status is invalid");
            if (!needed.contains(stable))
                continue;
            const std::filesystem::path relative = std::filesystem::path(fields[3]);
            if (relative.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory())
                return foundation::invalid_argument("Asset database path is not project-relative");
            const auto metadata_source = std::filesystem::weakly_canonical(root / relative);
            auto root_text = root.generic_string();
            if (root_text.back() != '/')
                root_text.push_back('/');
            const auto source_text = metadata_source.generic_string();
            if (source_text != root.generic_string() && source_text.rfind(root_text, 0) != 0)
                return foundation::invalid_argument("Asset database path escapes project root");
            std::filesystem::path metadata_path = metadata_source;
            metadata_path += ".meta";
            const auto meta = AssetMetaCodec{}.load(metadata_path);
            if (!meta)
                return meta.result;
            const auto expected_editor_kind = [](AssetKind kind) -> uint64_t {
                switch (kind) {
                    case AssetKind::kTexture:
                        return 1;
                    case AssetKind::kMesh:
                        return 2;
                    case AssetKind::kMaterial:
                        return 3;
                    case AssetKind::kScene:
                        return 4;
                    case AssetKind::kFont:
                        return 5;
                    default:
                        return UINT64_MAX;
                }
            };
            for (const auto& product : meta.value.products) {
                products[stable].emplace_back(product.kind, product.id);
                if (expected_editor_kind(product.kind) != kind_value)
                    return foundation::invalid_argument("Asset database kind does not match metadata");
            }
            if (products[stable].empty())
                return foundation::invalid_argument("Asset database metadata has no products");
            const auto canonical_source = metadata_source.generic_string();
            if (cooked_sources.insert(canonical_source).second) {
                const auto cooked = cooker.cook(metadata_source);
                if (!cooked)
                    return foundation::Result::failure(
                        cooked.result.code, "Referenced asset could not be cooked: " + cooked.result.message);
            }
        }
        for (std::size_t i = 0; i < stable_ids.size(); ++i) {
            const auto found = products.find(stable_ids[i]);
            if (found == products.end())
                return foundation::not_found("Stable asset id has no metadata mapping");
            const auto matching = std::count_if(found->second.begin(), found->second.end(), [&](const auto& product) {
                return product.first == payload.references[i].kind;
            });
            if (matching != 1)
                return foundation::invalid_argument("Stable asset id has ambiguous or wrong-kind metadata");
            if (const auto added = index.add(
                    stable_ids[i],
                    std::find_if(found->second.begin(), found->second.end(),
                                 [&](const auto& product) { return product.first == payload.references[i].kind; })
                        ->second);
                !added)
                return added;
        }
        const auto resolve = [&](uint64_t stable) { return index.resolve(stable); };
        if (stable_ids.size() != payload.references.size())
            return foundation::invalid_argument("Scene asset relocation records are inconsistent");
        if (const auto relocated = asset::relocate_scene_assets(payload, stable_ids, resolve); !relocated)
            return relocated;
    }
    if (const auto validated = validate_scene_reference_archive(payload); !validated)
        return validated;
    const auto encoded = asset::encode_scene_payload(payload);
    if (!encoded)
        return encoded.result;
    std::vector<AssetId> dependencies;
    dependencies.reserve(payload.references.size());
    for (const auto& reference : payload.references)
        dependencies.push_back(reference.asset);
    CookedAsset cooked{expected, AssetKind::kScene, payload.schema_version, std::move(dependencies), encoded.value};
    const auto envelope = CookedAssetCodec{}.encode(cooked);
    if (!envelope)
        return envelope.result;
    const auto published =
        CookedArtifactStore(cache_root)
            .publish(metadata.value.source, {{expected, AssetKind::kScene, payload.schema_version, envelope.value}});
    return published ? foundation::Result::success() : published.result;
}

foundation::Result ensure_pipeline_cooked(const std::filesystem::path& source, const AssetId& expected,
                                          const pipeline::TypeCatalog& catalog,
                                          const std::filesystem::path& cache_root) {
    const auto metadata = AssetMetaCodec{}.load(source.string() + ".meta");
    if (!metadata)
        return metadata.result;
    auto catalog_copy = std::make_shared<pipeline::TypeCatalog>(catalog);
    PipelineAssetImporter importer(std::move(catalog_copy));
    const auto cooked = importer.cook(source, metadata.value);
    if (!cooked)
        return cooked.result;
    std::vector<CookedArtifact> artifacts;
    artifacts.reserve(cooked.value.size());
    for (const auto& asset : cooked.value) {
        if (asset.id != expected || asset.kind != AssetKind::kPipeline)
            return foundation::invalid_argument("Startup pipeline metadata does not match the project manifest");
        const auto envelope = CookedAssetCodec{}.encode(asset);
        if (!envelope)
            return envelope.result;
        artifacts.push_back({asset.id, asset.kind, asset.schema_version, envelope.value});
    }
    const auto published = CookedArtifactStore(cache_root).publish(metadata.value.source, artifacts);
    return published ? foundation::Result::success() : published.result;
}
}  // namespace

foundation::ResultValue<ProjectCookResult> cook_project(const ProjectCookRequest& request) {
    ProjectCookResult result{};
    if (request.project_file.empty() || request.module_manifest.empty() || request.output_directory.empty() ||
        request.configuration.empty()) {
        result.diagnostics.push_back(
            diagnostic("cook.request", "Project cook requires project, module, output and configuration"));
        return {foundation::invalid_argument(result.diagnostics.back().message), std::move(result)};
    }
    const auto module = module::validate_module_bundle(request.module_manifest);
    if (!module) {
        result.diagnostics.push_back(diagnostic("cook.module", module.result.message));
        return {module.result, std::move(result)};
    }
    const auto project = application::project::ProjectFileCodec{}.load(request.project_file);
    if (!project) {
        result.diagnostics.push_back(diagnostic("cook.project", project.result.message));
        return {project.result, std::move(result)};
    }
    const auto paths = application::project::ProjectPaths::resolve(request.project_file, project.value.config.paths);
    if (!paths)
        return {paths.result, std::move(result)};

    std::vector<AssetId> roots;
    const auto scene_path =
        paths.value.directory(application::project::ProjectDirectory::kRoot) / project.value.config.startup_scene;
    const auto scene = source_product(scene_path, AssetKind::kScene);
    if (!scene) {
        result.diagnostics.push_back(diagnostic("cook.startup_scene", scene.result.message));
        return {scene.result, std::move(result)};
    }
    roots.push_back(scene.value);
    for (const auto& pipeline_path : project.value.config.startup_pipelines) {
        const auto path = paths.value.directory(application::project::ProjectDirectory::kRoot) / pipeline_path;
        const auto pipeline = source_product(path, AssetKind::kPipeline);
        if (!pipeline) {
            result.diagnostics.push_back(diagnostic("cook.startup_pipeline", pipeline.result.message));
            return {pipeline.result, std::move(result)};
        }
        roots.push_back(pipeline.value);
    }

    std::error_code error;
    std::filesystem::create_directories(request.output_directory, error);
    if (error)
        return {
            foundation::Result::failure(foundation::ResultCode::kBackendError, "Cannot create cook output directory"),
            std::move(result)};
    const auto cache_root = paths.value.directory(application::project::ProjectDirectory::kCache);
    CookedArtifactIndex index(cache_root);
    if (const auto refresh = index.refresh(); !refresh) {
        result.diagnostics.push_back(diagnostic("cook.cache", refresh.message));
        return {refresh, std::move(result)};
    }
    // A clean external project has no Editor-derived cache yet.  Cook the
    // startup closure here using the same metadata/catalog validation as the
    // Editor asset pipeline, then refresh the immutable index before packing.
    if (!index.read(scene.value)) {
        if (const auto cook = ensure_scene_cooked(scene_path, scene.value, cache_root,
                                                  paths.value.directory(application::project::ProjectDirectory::kRoot));
            !cook) {
            result.diagnostics.push_back(diagnostic("cook.startup_scene", cook.message));
            return {cook, std::move(result)};
        }
    }
    const auto package = request.output_directory / "game.aegbin";
    for (const auto& pipeline_path : project.value.config.startup_pipelines) {
        const auto source = paths.value.directory(application::project::ProjectDirectory::kRoot) / pipeline_path;
        const auto product = source_product(source, AssetKind::kPipeline);
        if (!product)
            return {product.result, std::move(result)};
        if (!index.read(product.value)) {
            if (const auto cook =
                    ensure_pipeline_cooked(source, product.value, module.value.metadata.catalog, cache_root);
                !cook) {
                result.diagnostics.push_back(diagnostic("cook.startup_pipeline", cook.message));
                return {cook, std::move(result)};
            }
        }
    }
    if (const auto refresh = index.refresh(); !refresh) {
        result.diagnostics.push_back(diagnostic("cook.cache", refresh.message));
        return {refresh, std::move(result)};
    }
    if (const auto packed =
            AssetDependencyPacker{}.write(package, roots, [&index](AssetId id) { return index.read(id); });
        !packed) {
        result.diagnostics.push_back(diagnostic("cook.dependencies", packed.message));
        return {packed, std::move(result)};
    }

    asset::GameBuildManifest manifest{};
    manifest.project_uuid = project.value.config.project_uuid;
    manifest.module_id = project.value.config.module.id;
    manifest.build_id = module.value.metadata.build_id;
    manifest.module_contract_digest = module.value.metadata.contract_digest;
    const auto package_digest = digest_file(package);
    if (!package_digest) {
        result.diagnostics.push_back(diagnostic("cook.package_digest", package_digest.result.message));
        return {package_digest.result, std::move(result)};
    }
    manifest.package_digest = package_digest.value;
    manifest.startup_scene = runtime_id(roots.front());
    for (std::size_t i = 1; i < roots.size(); ++i)
        manifest.startup_pipelines.push_back(runtime_id(roots[i]));
    manifest.target = project.value.config.target;
    manifest.configuration = request.configuration;
    manifest.package_name = package.filename().string();
    const auto encoded_manifest = asset::encode_game_build_manifest(manifest);
    if (!encoded_manifest)
        return {encoded_manifest.result, std::move(result)};
    result.game_manifest = request.output_directory / "game.manifest.bin";
    std::ofstream binary(result.game_manifest, std::ios::binary | std::ios::trunc);
    binary.write(reinterpret_cast<const char*>(encoded_manifest.value.data()),
                 static_cast<std::streamsize>(encoded_manifest.value.size()));
    if (!binary)
        return {foundation::Result::failure(foundation::ResultCode::kBackendError, "Cannot write game manifest"),
                std::move(result)};

    // A small text sidecar is retained for the bootstrap command-line reader;
    // the binary manifest above remains the portable contract.
    std::ofstream sidecar(request.output_directory / "game.manifest", std::ios::trunc);
    sidecar << "package = \"" << package.filename().string() << "\"\n"
            << "build_id = \"" << manifest.build_id << "\"\n"
            << "target = \"" << manifest.target << "\"\n"
            << "configuration = \"" << manifest.configuration << "\"\n";
    if (!sidecar)
        return {
            foundation::Result::failure(foundation::ResultCode::kBackendError, "Cannot write game manifest sidecar"),
            std::move(result)};
    result.package = package;
    return {foundation::Result::success(), std::move(result)};
}
}  // namespace aeg::tooling
