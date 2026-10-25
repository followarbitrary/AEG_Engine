#include "project_cooker.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <span>
#include <system_error>
#include <unordered_set>
#include <utility>

#include "asset_cooker.hpp"
#include "asset_dependency_packer.hpp"
#include "asset_metadata.hpp"
#include "asset_package_reader.hpp"
#include "cooked_artifact_store.hpp"
#include "cooked_asset.hpp"
#include "game_build_manifest.hpp"
#include "module_bundle.hpp"
#include "feature_graph_asset.hpp"
#include "feature_graph_asset_importer.hpp"
#include "feature_registry.hpp"
#include "feature_world_transform.hpp"
#include "project_context.hpp"
#include "project_module_agreement.hpp"
#include "scene_asset.hpp"
#include "scene_payload.hpp"
#include "serialization.hpp"
#include "world_command_routes.hpp"

namespace aeg::tooling {
namespace {
ProjectDiagnostic diagnostic(std::string code, std::string message) {
    return {std::move(code), std::move(message), {}, {}, 0, 0};
}

std::atomic_uint64_t publication_sequence{0};

std::string next_publication_name() {
    const auto timestamp =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    return std::to_string(timestamp) + "_" + std::to_string(publication_sequence.fetch_add(1));
}

foundation::AssetId runtime_id(const AssetId& source) {
    foundation::AssetId result{};
    result.value.bytes = source.value.bytes;
    return result;
}

scene::SceneAssetId scene_id(const AssetId& source) {
    return scene::SceneAssetId::from_uuid(source.value);
}

foundation::ResultValue<scene::SceneAsset> decode_scene_archive(std::span<const std::byte> bytes) {
    serialization::BinaryReader reader({reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});
    auto scene = scene::SceneSerializer::deserialize(reader);
    if (!scene) return scene;
    if (reader.remaining() != 0)
        return {foundation::invalid_argument("Startup Scene archive has trailing bytes"), {}};
    return scene;
}

foundation::ResultValue<std::vector<asset::WorldCommandRoute>> load_world_command_routes(
    const std::filesystem::path& scene_path, AssetId product) {
    const auto sidecar = asset::world_command_routes_sidecar_path(scene_path);
    std::error_code error;
    const bool exists = std::filesystem::exists(sidecar, error);
    if (error)
        return {foundation::not_found("Cannot inspect startup Scene World route sidecar"), {}};
    if (!exists) return {foundation::Result::success(), {}};
    const auto size = std::filesystem::file_size(sidecar, error);
    if (error || size > asset::kMaxWorldCommandRoutesBytes)
        return {foundation::invalid_argument("Startup Scene World route sidecar exceeds its byte budget or is unreadable"), {}};
    std::ifstream input(sidecar, std::ios::binary);
    if (!input) return {foundation::not_found("Cannot read startup Scene World route sidecar"), {}};
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::char_traits<char>::eof())
        return {foundation::invalid_argument("Startup Scene World route sidecar changed while reading"), {}};
    auto routes = asset::decode_world_command_routes(bytes);
    if (!routes || routes.value.empty()) return routes;

    // Read the author document even on a warm cache. Its Scene identity is the
    // sidecar's scope; only the packaged route uses the .meta product identity.
    std::ifstream source(scene_path, std::ios::binary);
    if (!source) return {foundation::not_found("Cannot read World route author Scene"), {}};
    const std::string source_bytes((std::istreambuf_iterator<char>(source)), {});
    const auto raw = std::span<const std::byte>(reinterpret_cast<const std::byte*>(source_bytes.data()), source_bytes.size());
    const auto payload = asset::decode_scene_payload(raw);
    if (!payload && asset::has_scene_payload_header(raw)) return {payload.result, {}};
    const auto authored = decode_scene_archive(payload ? std::span<const std::byte>(payload.value.serialized_scene) : raw);
    if (!authored) return {authored.result, {}};
    const foundation::AssetId author_id{authored.value.id().uuid()};
    for (auto& route : routes.value) {
        if (route.scene_asset != author_id)
            return {foundation::invalid_argument("World route Scene identity does not match its author Scene"), {}};
        route.scene_asset = runtime_id(product);
    }
    return routes;
}

foundation::Result validate_packaged_world_command_routes(std::span<const asset::WorldCommandRoute> routes,
    const std::filesystem::path& package_path, std::span<const AssetId> roots,
    const feature::FeatureRegistry& features) {
    if (routes.empty()) return foundation::Result::success();
    const auto package = asset::AssetPackageReader::open(package_path);
    if (!package) return package.result;
    const auto scene = package.value.read_cooked(runtime_id(roots.front()));
    if (!scene) return scene.result;
    if (scene.value.kind != AssetKind::kScene)
        return foundation::invalid_argument("World route startup product is not a Scene");
    const auto payload = asset::decode_scene_payload(scene.value.payload);
    if (!payload) return payload.result;
    if (payload.value.schema_version != asset::kScenePayloadSchemaVersion)
        return foundation::unsupported("World route packaged Scene payload schema version is unsupported");
    const auto document = decode_scene_archive(payload.value.serialized_scene);
    if (!document) return document.result;
    if (document.value.id() != scene_id(roots.front()))
        return foundation::invalid_argument("World route packaged Scene identity does not match its product UUID");
    std::vector<feature::FeatureGraphDocument> graphs;
    graphs.reserve(roots.size() - 1);
    for (std::size_t index = 1; index < roots.size(); ++index) {
        const auto cooked = package.value.read_cooked(runtime_id(roots[index]));
        if (!cooked) return cooked.result;
        if (cooked.value.kind != AssetKind::kFeatureGraph)
            return foundation::invalid_argument("World route startup graph product is not a FeatureGraph");
        auto graph = asset::decode_feature_graph_asset(cooked.value.payload);
        if (!graph) return graph.result;
        const auto identity = AssetId::parse(graph.value.asset_uuid);
        if (!identity || identity.value != roots[index] || graph.value.schema_version != cooked.value.schema_version)
            return foundation::invalid_argument("World route packaged graph identity or schema does not match its envelope");
        if (const auto compiled = feature::FeatureGraphCompiler{}.compile(graph.value, features); !compiled)
            return compiled.result;
        graphs.push_back(std::move(graph.value));
    }
    const auto transform = ecs::ComponentTypeId(feature::kWorldTransformComponentName);
    const auto transform_schema = reflection::TypeId(feature::kWorldTransformComponentName);
    for (const auto& route : routes) {
        if (route.scene_asset != runtime_id(roots.front()))
            return foundation::invalid_argument("World route targets a different packaged startup Scene");
        if (const auto valid = asset::validate_world_command_route_source(route, graphs, features); !valid)
            return valid;
        const auto entity = scene::SceneEntityId::from_uuid(route.scene_entity);
        if (std::none_of(document.value.entities().begin(), document.value.entities().end(),
                         [&](const auto& record) { return record.id == entity; }))
            return foundation::invalid_argument("World route target entity is absent from the packaged Scene");
        const auto component = std::find_if(document.value.components().begin(), document.value.components().end(),
            [&](const auto& record) {
                return record.entity == entity && record.component_type == transform && record.schema_type == transform_schema;
            });
        if (component == document.value.components().end())
            return foundation::invalid_argument("World route packaged target entity has no Transform component");
        if (const auto valid = feature::validate_world_transform_component(component->payload, component->schema_version); !valid)
            return valid;
    }
    return foundation::Result::success();
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

struct StartupSourceFingerprint final {
    std::filesystem::path stamp;
    SourceId source;
    std::string bytes;
};

foundation::ResultValue<StartupSourceFingerprint> startup_source_fingerprint(
    const std::filesystem::path& source, const AssetId& expected, const std::filesystem::path& cache_root) {
    const auto meta_path = std::filesystem::path{source.string() + ".meta"};
    const auto metadata = AssetMetaCodec{}.load(meta_path);
    if (!metadata) return {metadata.result, {}};
    if (metadata.value.products.size() != 1 || metadata.value.products.front().id != expected)
        return {foundation::invalid_argument("Startup source metadata changed during Cook"), {}};
    const auto source_digest = digest_file(source);
    if (!source_digest) return {source_digest.result, {}};
    const auto meta_digest = digest_file(meta_path);
    if (!meta_digest) return {meta_digest.result, {}};
    // This is private Cooker provenance, separate from the immutable artifact
    // manifest and Runtime schemas. Legacy bundles have no stamp and reimport.
    return {foundation::Result::success(), {cache_root / ".project-cook-sources" /
        (metadata.value.source.to_string() + "_" + expected.to_string() + ".sha256"),
        metadata.value.source,
        source_digest.value + '\n' + meta_digest.value + '\n'}};
}

foundation::Result write_startup_source_fingerprint(const StartupSourceFingerprint& fingerprint) {
    std::error_code error;
    std::filesystem::create_directories(fingerprint.stamp.parent_path(), error);
    if (error) return foundation::Result::failure(foundation::ResultCode::kBackendError,
                                                 "Cannot create startup source fingerprint directory");
    auto temporary = fingerprint.stamp;
    temporary += "." + next_publication_name() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << fingerprint.bytes;
        output.close();
        if (!output) {
            std::filesystem::remove(temporary, error);
            return foundation::Result::failure(foundation::ResultCode::kBackendError,
                                                "Cannot write startup source fingerprint");
        }
    }
    const auto published = replace_file_atomically(temporary, fingerprint.stamp);
    if (!published) std::filesystem::remove(temporary, error);
    return published;
}

template <typename Import>
foundation::Result refresh_startup_source(const std::filesystem::path& source, const AssetId& expected,
                                           const std::filesystem::path& cache_root,
                                           const CookedArtifactIndex& index, Import&& import) {
    const auto before = startup_source_fingerprint(source, expected, cache_root);
    if (!before) return before.result;
    bool readable_stamp = false;
    std::string previous;
    {
        std::ifstream stamp(before.value.stamp, std::ios::binary);
        if (stamp) {
            previous.assign(std::istreambuf_iterator<char>(stamp), {});
            readable_stamp = !stamp.bad();
        }
    }
    if (readable_stamp && previous == before.value.bytes && index.read(expected))
        return foundation::Result::success();
    struct CacheSelection { bool known; std::filesystem::path manifest; };
    const auto selection = [&]() -> CacheSelection {
        const auto selected = CookedArtifactStore(cache_root).load_current(before.value.source);
        if (selected) return {true, selected.value.manifest};
        std::error_code error;
        const bool exists = std::filesystem::exists(
            cache_root / before.value.source.to_string() / "current.toml", error);
        return {!exists && !error, {}};
    };
    const auto selected_before = selection();
    std::error_code stamp_error;
    const bool absent_stamp = !std::filesystem::exists(before.value.stamp, stamp_error) && !stamp_error;
    // Invalidate before the importer can advance its current bundle. A crash,
    // failed readback or concurrent source edit must never leave stamp A
    // claiming that a newly published bundle B still came from source A.
    auto pending = before.value;
    pending.bytes = "import pending; source verification required\n";
    if (const auto invalidated = write_startup_source_fingerprint(pending); !invalidated) return invalidated;
    if (const auto imported = import(); !imported) {
        // A parser failure preserves the previous cache/stamp exactly. An
        // importer that failed after publication keeps the invalid marker.
        const auto selected_after = selection();
        if (selected_before.known && selected_after.known && selected_before.manifest == selected_after.manifest) {
            if (readable_stamp) {
                auto restored = before.value;
                restored.bytes = previous;
                if (const auto restore = write_startup_source_fingerprint(restored); !restore) return restore;
            } else if (absent_stamp) {
                std::error_code error;
                std::filesystem::remove(before.value.stamp, error);
                if (error) return foundation::Result::failure(foundation::ResultCode::kBackendError,
                                                              "Cannot restore startup source fingerprint absence");
            }
        }
        return imported;
    }
    const auto after = startup_source_fingerprint(source, expected, cache_root);
    if (!after) return after.result;
    if (before.value.stamp != after.value.stamp || before.value.bytes != after.value.bytes)
        return foundation::invalid_state("Startup source changed while Cook imported it; retry with a stable saved source");
    return write_startup_source_fingerprint(after.value);
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
                const auto entity = scene::SceneEntityId::from_uuid(foundation::Uuid{reference.scene_entity_uuid});
                if (entity == component.entity && reference.component_type == "MeshRenderer" &&
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
        const auto archive_bytes =
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
        serialization::BinaryReader reader(archive_bytes);
        const auto archive = scene::SceneSerializer::deserialize(reader);
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
                    reference.scene_entity_uuid = component.entity.uuid().bytes;
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
    // Both raw Editor archives and ScenePayload sources share the same product
    // relocation. Stable entity UUIDs and relocation references stay unchanged.
    auto archive = decode_scene_archive(payload.serialized_scene);
    if (!archive) return archive.result;
    if (archive.value.id() != scene_id(expected)) {
        if (const auto identity = archive.value.set_identity(
                scene_id(expected), std::string(archive.value.name()), archive.value.version()); !identity)
            return identity;
        serialization::BinaryWriter writer;
        if (const auto serialized = scene::SceneSerializer::serialize(archive.value, writer); !serialized)
            return serialized;
        payload.serialized_scene.assign(reinterpret_cast<const std::byte*>(writer.data().data()),
                                        reinterpret_cast<const std::byte*>(writer.data().data() + writer.size()));
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

foundation::Result ensure_feature_graph_cooked(const std::filesystem::path& source, const AssetId& expected,
                                               const std::filesystem::path& cache_root) {
    const auto metadata = AssetMetaCodec{}.load(source.string() + ".meta");
    if (!metadata)
        return metadata.result;
    FeatureGraphAssetImporter importer;
    const auto cooked = importer.cook(source, metadata.value);
    if (!cooked)
        return cooked.result;
    std::vector<CookedArtifact> artifacts;
    artifacts.reserve(cooked.value.size());
    for (const auto& asset : cooked.value) {
        if (asset.id != expected || asset.kind != AssetKind::kFeatureGraph)
            return foundation::invalid_argument("Startup FeatureGraph metadata does not match the project manifest");
        const auto envelope = CookedAssetCodec{}.encode(asset);
        if (!envelope)
            return envelope.result;
        artifacts.push_back({asset.id, asset.kind, asset.schema_version, envelope.value});
    }
    const auto published = CookedArtifactStore(cache_root).publish(metadata.value.source, artifacts);
    return published ? foundation::Result::success() : published.result;
}

foundation::Result append_feature_parameter_dependencies(CookedAsset& cooked,
                                                          const feature::FeatureRegistry& registry,
                                                          const feature::AssetKindResolver& resolve_kind) {
    const auto graph = asset::decode_feature_graph_asset(cooked.payload);
    if (!graph) return graph.result;
    const auto identity = AssetId::parse(graph.value.asset_uuid);
    if (!identity || identity.value != cooked.id || graph.value.schema_version != cooked.schema_version)
        return foundation::invalid_argument("FeatureGraph payload identity or schema does not match its cooked envelope");
    if (!graph.value.nodes.empty() && graph.value.contract_digest == 0)
        return foundation::invalid_argument("FeatureGraph contract digest is required for Cook");
    if (const auto compiled = feature::FeatureGraphCompiler{}.compile(graph.value, registry); !compiled)
        return compiled.result;

    // Only authored roots have parameter overrides. Expanded children receive
    // their own typed defaults, matching Runtime's separately owned instances.
    const auto append = [&](auto&& self, const feature::ExpandedFeature& expanded,
                            std::span<const uint8_t> blob) -> foundation::Result {
        const auto& schema = expanded.contract.parameters;
        const auto records = feature::decode_feature_parameters(blob, schema);
        if (!records) return records.result;
        for (const auto& record : records.value) {
            const auto parameter = std::find_if(schema.begin(), schema.end(), [&](const auto& item) {
                return item.id == record.id;
            });
            if (!parameter->metadata || parameter->metadata->type != feature::ParameterType::kAssetReference)
                continue;
            const auto value = feature::deserialize_parameter_value(record.payload, parameter->metadata->type);
            if (!value) return value.result;
            if (const auto valid = feature::validate_parameter_value(value.value, *parameter->metadata, resolve_kind);
                !valid) return valid;
            const auto& reference = std::get<foundation::AssetReference>(value.value);
            if (reference.uuid.empty()) continue;
            const auto dependency = AssetId::parse(reference.uuid);
            if (!dependency) return dependency.result;
            if (dependency.value != AssetId{}) cooked.runtime_dependencies.push_back(dependency.value);
        }
        for (const auto& child : expanded.children)
            if (const auto result = self(self, child, {}); !result)
                return foundation::Result::failure(result.code,
                    "Child Feature " + std::to_string(child.definition_id.value()) + ": " + result.message);
        return foundation::Result::success();
    };
    for (const auto& node : graph.value.nodes) {
        const auto expanded = registry.expand(node.definition_id, node.owner_path);
        if (!expanded) return expanded.result;
        if (const auto result = append(append, expanded.value, node.parameter_blob); !result)
            return foundation::Result::failure(result.code,
                "FeatureGraph " + graph.value.asset_uuid + " node " + std::to_string(node.node_id.value()) +
                ": " + result.message);
    }
    return foundation::Result::success();
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

    if (const auto agreement = detail::validate_project_module_agreement(project.value.config, module.value.metadata,
                                                                        request.configuration); !agreement) {
        result.diagnostics.push_back(diagnostic("cook.module_project", agreement.message));
        return {agreement, std::move(result)};
    }

    std::vector<AssetId> roots;
    const auto scene_path =
        paths.value.directory(application::project::ProjectDirectory::kRoot) / project.value.config.startup_scene;
    const auto scene = source_product(scene_path, AssetKind::kScene);
    if (!scene) {
        result.diagnostics.push_back(diagnostic("cook.startup_scene", scene.result.message));
        return {scene.result, std::move(result)};
    }
    roots.push_back(scene.value);
    for (const auto& feature_graph_path : project.value.config.startup_feature_graphs) {
        const auto path = paths.value.directory(application::project::ProjectDirectory::kRoot) / feature_graph_path;
        const auto feature_graph = source_product(path, AssetKind::kFeatureGraph);
        if (!feature_graph) {
            result.diagnostics.push_back(diagnostic("cook.startup_feature_graph", feature_graph.result.message));
            return {feature_graph.result, std::move(result)};
        }
        roots.push_back(feature_graph.value);
    }

    // Sidecars are author inputs, independent of immutable cooked cache hits.
    auto world_routes = load_world_command_routes(scene_path, scene.value);
    if (!world_routes) {
        result.diagnostics.push_back(diagnostic("cook.world_routes", world_routes.result.message));
        return {world_routes.result, std::move(result)};
    }

    std::error_code error;
    std::filesystem::create_directories(request.output_directory, error);
    if (error)
        return {
            foundation::Result::failure(foundation::ResultCode::kBackendError, "Cannot create cook output directory"),
            std::move(result)};
    const auto publication_name = next_publication_name();
    const auto publication_directory = request.output_directory / "versions" / publication_name;
    std::filesystem::create_directories(publication_directory, error);
    if (error)
        return {
            foundation::Result::failure(foundation::ResultCode::kBackendError,
                                        "Cannot create versioned cook output directory"),
            std::move(result)};
    const auto cache_root = paths.value.directory(application::project::ProjectDirectory::kCache);
    CookedArtifactIndex index(cache_root);
    if (const auto refresh = index.refresh(); !refresh) {
        result.diagnostics.push_back(diagnostic("cook.cache", refresh.message));
        return {refresh, std::move(result)};
    }
    // A saved startup asset can change without changing AssetId. Reuse an
    // immutable bundle only when its source and .meta fingerprint still match.
    // Unchanged sources retain their exact cache; final-package validation
    // below still checks cached payloads rather than trusting these stamps.
    if (const auto cook = refresh_startup_source(scene_path, scene.value, cache_root, index, [&] {
            return ensure_scene_cooked(scene_path, scene.value, cache_root,
                paths.value.directory(application::project::ProjectDirectory::kRoot));
        }); !cook) {
        result.diagnostics.push_back(diagnostic("cook.startup_scene", cook.message));
        return {cook, std::move(result)};
    }
    const auto package = publication_directory / "game.aegbin";
    for (const auto& feature_graph_path : project.value.config.startup_feature_graphs) {
        const auto source = paths.value.directory(application::project::ProjectDirectory::kRoot) / feature_graph_path;
        const auto product = source_product(source, AssetKind::kFeatureGraph);
        if (!product)
            return {product.result, std::move(result)};
        if (const auto cook = refresh_startup_source(source, product.value, cache_root, index, [&] {
                return ensure_feature_graph_cooked(source, product.value, cache_root);
            }); !cook) {
            result.diagnostics.push_back(diagnostic("cook.startup_feature_graph", cook.message));
            return {cook, std::move(result)};
        }
    }
    if (const auto refresh = index.refresh(); !refresh) {
        result.diagnostics.push_back(diagnostic("cook.cache", refresh.message));
        return {refresh, std::move(result)};
    }

    // Rebuild the immutable semantic registry from the validated module catalog.
    // No executable callback is needed for graph compilation or expansion.
    auto catalog_registry = feature::FeatureRegistry::from_catalog(module.value.metadata.feature_catalog);
    if (!catalog_registry) {
        result.diagnostics.push_back(diagnostic("cook.feature_catalog", catalog_registry.result.message));
        return {catalog_registry.result, std::move(result)};
    }
    const auto& features = catalog_registry.value;
    const feature::AssetKindResolver resolve_kind = [&index](AssetId id)
        -> foundation::ResultValue<AssetKind> {
        const auto bytes = index.read(id);
        if (!bytes) return {bytes.result, {}};
        const auto decoded = asset::decode_cooked_asset(bytes.value);
        if (!decoded) return {decoded.result, {}};
        if (decoded.value.id != id)
            return {foundation::invalid_argument("Cooked asset resolver returned the wrong UUID"), {}};
        return {foundation::Result::success(), decoded.value.kind};
    };
    const auto resolve_for_package = [&](AssetId id) -> foundation::ResultValue<std::vector<std::byte>> {
        auto bytes = index.read(id);
        if (!bytes) return bytes;
        auto cooked = asset::decode_cooked_asset(bytes.value);
        if (!cooked) return {cooked.result, {}};
        if (cooked.value.id != id)
            return {foundation::invalid_argument("Cooked asset resolver returned the wrong UUID"), {}};
        if (cooked.value.kind != AssetKind::kFeatureGraph) return bytes;
        if (const auto valid = append_feature_parameter_dependencies(cooked.value, features, resolve_kind); !valid)
            return {valid, {}};
        // Cache bundles remain immutable. The package receives a validated
        // envelope whose dependencies include all effective asset parameters.
        return asset::encode_cooked_asset(cooked.value);
    };
    if (const auto packed =
            AssetDependencyPacker{}.write(package, roots, resolve_for_package);
        !packed) {
        result.diagnostics.push_back(diagnostic("cook.dependencies", packed.message));
        return {packed, std::move(result)};
    }

    if (const auto validated = validate_packaged_world_command_routes(world_routes.value, package, roots, features);
        !validated) {
        result.diagnostics.push_back(diagnostic("cook.world_routes", validated.message));
        return {validated, std::move(result)};
    }

    // Deliver the exact validated module bundle beside the cooked package. Keep the
    // manifest's internal relative metadata/binary paths unchanged inside a private
    // module directory, then pin that manifest by its original bytes.
    const auto delivery_module_root = publication_directory / "module";
    std::filesystem::create_directories(delivery_module_root, error);
    if (error) {
        result.diagnostics.push_back(diagnostic("cook.module_delivery", "Cannot create module delivery directory"));
        return {foundation::Result::failure(foundation::ResultCode::kBackendError, "Cannot create module delivery directory"), std::move(result)};
    }
    const auto module_manifest_relative = module.value.manifest_path.lexically_relative(module.value.directory);
    const auto module_metadata_relative = module.value.metadata_path.lexically_relative(module.value.directory);
    const auto module_binary_relative = module.value.binary_path.lexically_relative(module.value.directory);
    if (module_manifest_relative.empty() || module_manifest_relative.is_absolute() ||
        module_metadata_relative.empty() || module_metadata_relative.is_absolute() ||
        module_binary_relative.empty() || module_binary_relative.is_absolute()) {
        result.diagnostics.push_back(diagnostic("cook.module_delivery", "Validated module bundle paths are not relative"));
        return {foundation::invalid_argument("Validated module bundle paths are not relative"), std::move(result)};
    }
    const auto copy_bundle_file = [&](const std::filesystem::path& relative, const std::filesystem::path& source) -> foundation::Result {
        const auto destination = delivery_module_root / relative;
        std::filesystem::create_directories(destination.parent_path(), error);
        if (error || !std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, error) || error)
            return foundation::Result::failure(foundation::ResultCode::kBackendError, "Cannot copy module bundle into delivery");
        return foundation::Result::success();
    };
    for (const auto& item : {std::pair{module_manifest_relative, module.value.manifest_path},
                             std::pair{module_metadata_relative, module.value.metadata_path},
                             std::pair{module_binary_relative, module.value.binary_path}}) {
        if (const auto copied = copy_bundle_file(item.first, item.second); !copied) {
            result.diagnostics.push_back(diagnostic("cook.module_delivery", copied.message));
            return {copied, std::move(result)};
        }
    }
    const auto delivered_manifest_path = delivery_module_root / module_manifest_relative;
    const auto delivered_bundle = module::validate_module_bundle(delivered_manifest_path);
    if (!delivered_bundle) {
        result.diagnostics.push_back(diagnostic("cook.module_delivery", delivered_bundle.result.message));
        return {delivered_bundle.result, std::move(result)};
    }
    const auto delivered_manifest_digest = digest_file(delivered_manifest_path);
    if (!delivered_manifest_digest) {
        result.diagnostics.push_back(diagnostic("cook.module_delivery", delivered_manifest_digest.result.message));
        return {delivered_manifest_digest.result, std::move(result)};
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
    manifest.world_command_routes = std::move(world_routes.value);
    for (std::size_t i = 1; i < roots.size(); ++i)
        manifest.startup_feature_graphs.push_back(runtime_id(roots[i]));
    manifest.target = project.value.config.target;
    manifest.configuration = request.configuration;
    manifest.package_name = package.filename().string();
    manifest.module_manifest_name = (std::filesystem::path("module") / module_manifest_relative).generic_string();
    manifest.module_manifest_sha256 = delivered_manifest_digest.value;
    const auto encoded_manifest = asset::encode_game_build_manifest(manifest);
    if (!encoded_manifest)
        return {encoded_manifest.result, std::move(result)};
    result.game_manifest = publication_directory / "game.manifest.bin";
    std::ofstream binary(result.game_manifest, std::ios::binary | std::ios::trunc);
    binary.write(reinterpret_cast<const char*>(encoded_manifest.value.data()),
                 static_cast<std::streamsize>(encoded_manifest.value.size()));
    if (!binary)
        return {foundation::Result::failure(foundation::ResultCode::kBackendError, "Cannot write game manifest"),
                std::move(result)};

    // A small text sidecar is retained for the bootstrap command-line reader;
    // the binary manifest above remains the portable contract.
    std::ofstream sidecar(publication_directory / "game.manifest", std::ios::trunc);
    sidecar << "package = \"" << package.filename().string() << "\"\n"
            << "build_id = \"" << manifest.build_id << "\"\n"
            << "target = \"" << manifest.target << "\"\n"
            << "configuration = \"" << manifest.configuration << "\"\n";
    if (!sidecar)
        return {
            foundation::Result::failure(foundation::ResultCode::kBackendError, "Cannot write game manifest sidecar"),
            std::move(result)};
    const auto selector_temporary = request.output_directory / ("current_" + publication_name + ".tmp");
    {
        std::ofstream selector(selector_temporary, std::ios::binary | std::ios::trunc);
        selector << "manifest = \"versions/" << publication_name << "/game.manifest.bin\"\n";
        if (!selector)
            return {foundation::Result::failure(foundation::ResultCode::kBackendError,
                                                 "Cannot write cook output selector"),
                    std::move(result)};
    }
    if (const auto published = replace_file_atomically(selector_temporary, request.output_directory / "current.toml");
        !published) {
        std::filesystem::remove(selector_temporary, error);
        return {published, std::move(result)};
    }
    result.package = package;
    return {foundation::Result::success(), std::move(result)};
}
}  // namespace aeg::tooling
