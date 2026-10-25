#include "game_build_manifest.hpp"

#include <algorithm>
#include <array>
#include <new>
#include <utility>

#include "serialization.hpp"

namespace aeg::asset {
namespace {
constexpr uint32_t kMagic = 0x314d4247u;  // GBM1
constexpr uint32_t kSchemaVersion = 3;
constexpr uint32_t kMaxString = 16u * 1024u * 1024u;
constexpr uint32_t kMaxFeatureGraphs = 4096;

template <typename T>
auto string_fields(T& manifest) {
    return std::array{&manifest.project_uuid, &manifest.module_id, &manifest.build_id,
                      &manifest.module_contract_digest, &manifest.package_digest, &manifest.target,
                      &manifest.configuration, &manifest.package_name, &manifest.module_manifest_name,
                      &manifest.module_manifest_sha256};
}

foundation::Result validate_manifest(const GameBuildManifest& manifest, std::size_t route_bytes) {
    if (manifest.schema_version != kSchemaVersion || manifest.startup_scene == foundation::AssetId{} ||
        manifest.startup_feature_graphs.size() > kMaxFeatureGraphs ||
        std::any_of(manifest.startup_feature_graphs.begin(), manifest.startup_feature_graphs.end(),
                    [](foundation::AssetId id) { return id == foundation::AssetId{}; }))
        return foundation::invalid_argument("Game build manifest identity, version, or graph count is invalid");
    // Header, ten string lengths, startup Scene UUID, graph count, and route block length.
    std::size_t total = 72u + manifest.startup_feature_graphs.size() * 16u;
    if (route_bytes > kMaxGameBuildManifestBytes - total)
        return foundation::invalid_argument("Game build manifest exceeds its total byte budget");
    total += route_bytes;
    for (const auto* value : string_fields(manifest)) {
        if (value->empty() || value->size() > kMaxString || value->size() > kMaxGameBuildManifestBytes - total)
            return foundation::invalid_argument("Game build manifest identity or total byte size is invalid");
        total += value->size();
    }
    return foundation::Result::success();
}

foundation::Result read_uuid(serialization::BinaryReader& reader, foundation::AssetId& output) {
    const auto bytes = reader.read_bytes(output.value.bytes.size());
    if (!bytes) return bytes.result;
    std::copy(bytes.value.begin(), bytes.value.end(), output.value.bytes.begin());
    return foundation::Result::success();
}
}  // namespace

foundation::ResultValue<std::vector<std::byte>> encode_game_build_manifest(const GameBuildManifest& manifest) {
    try {
        if (const auto result = validate_manifest(manifest, 0); !result) return {result, {}};
        const auto routes = encode_world_command_routes(manifest.world_command_routes);
        if (!routes) return {routes.result, {}};
        if (const auto result = validate_manifest(manifest, routes.value.size()); !result) return {result, {}};
        serialization::BinaryWriter writer;
        if (const auto result = writer.write_u32(kMagic); !result) return {result, {}};
        if (const auto result = writer.write_u32(kSchemaVersion); !result) return {result, {}};
        const auto fields = string_fields(manifest);
        for (std::size_t index = 0; index < 5; ++index)
            if (const auto result = writer.write_string(*fields[index], kMaxString); !result) return {result, {}};
        if (const auto result = writer.write_bytes(manifest.startup_scene.value.bytes); !result) return {result, {}};
        if (const auto result = writer.write_u32(static_cast<uint32_t>(manifest.startup_feature_graphs.size())); !result)
            return {result, {}};
        for (const auto id : manifest.startup_feature_graphs)
            if (const auto result = writer.write_bytes(id.value.bytes); !result) return {result, {}};
        for (std::size_t index = 5; index < fields.size(); ++index)
            if (const auto result = writer.write_string(*fields[index], kMaxString); !result) return {result, {}};
        if (const auto result = writer.write_u32(static_cast<uint32_t>(routes.value.size())); !result) return {result, {}};
        if (const auto result = writer.write_bytes(
                {reinterpret_cast<const uint8_t*>(routes.value.data()), routes.value.size()}); !result)
            return {result, {}};
        const auto bytes = std::as_bytes(writer.data());
        return {foundation::Result::success(), {bytes.begin(), bytes.end()}};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("Game build manifest encoding allocation failed"), {}};
    }
}

foundation::ResultValue<GameBuildManifest> decode_game_build_manifest(std::span<const std::byte> bytes) {
    if (bytes.size() > kMaxGameBuildManifestBytes)
        return {foundation::invalid_argument("Game build manifest exceeds its total byte budget"), {}};
    try {
        serialization::BinaryReader reader({reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});
        GameBuildManifest result;
        const auto magic = reader.read_u32();
        const auto version = reader.read_u32();
        if (!magic || !version || magic.value != kMagic || version.value != kSchemaVersion)
            return {foundation::invalid_argument("Game build manifest header is invalid or unsupported"), {}};
        result.schema_version = version.value;
        const auto fields = string_fields(result);
        for (std::size_t index = 0; index < 5; ++index) {
            auto value = reader.read_string(kMaxString);
            if (!value) return {value.result, {}};
            *fields[index] = std::move(value.value);
        }
        if (const auto status = read_uuid(reader, result.startup_scene); !status) return {status, {}};
        const auto count = reader.read_u32();
        if (!count || count.value > kMaxFeatureGraphs || count.value > reader.remaining() / 16u)
            return {foundation::invalid_argument("Game build manifest FeatureGraph count is invalid"), {}};
        result.startup_feature_graphs.reserve(count.value);
        for (uint32_t index = 0; index < count.value; ++index) {
            foundation::AssetId id;
            if (const auto status = read_uuid(reader, id); !status) return {status, {}};
            result.startup_feature_graphs.push_back(id);
        }
        for (std::size_t index = 5; index < fields.size(); ++index) {
            auto value = reader.read_string(kMaxString);
            if (!value) return {value.result, {}};
            *fields[index] = std::move(value.value);
        }
        const auto route_size = reader.read_u32();
        if (!route_size || route_size.value > kMaxWorldCommandRoutesBytes || route_size.value != reader.remaining())
            return {foundation::invalid_argument("Game build manifest route block length is invalid"), {}};
        const auto route_bytes = reader.read_bytes(route_size.value);
        if (!route_bytes) return {route_bytes.result, {}};
        auto routes = decode_world_command_routes(std::as_bytes(route_bytes.value));
        if (!routes) return {routes.result, {}};
        result.world_command_routes = std::move(routes.value);
        if (const auto status = validate_manifest(result, route_size.value); !status) return {status, {}};
        return {foundation::Result::success(), std::move(result)};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("Game build manifest decoding allocation failed"), {}};
    }
}
}  // namespace aeg::asset
