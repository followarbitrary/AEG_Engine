/** @file world_command_routes.hpp
 *  @brief Runtime-neutral authored World command routes and their bounded wire format.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "asset_identity.hpp"
#include "feature_contract.hpp"

namespace aeg::feature {
struct FeatureGraphDocument;
class FeatureRegistry;
}

namespace aeg::asset {
inline constexpr std::size_t kMaxWorldCommandRoutesBytes = 16u * 1024u * 1024u;
inline constexpr uint32_t kMaxWorldCommandRoutes = 4096;
inline constexpr uint32_t kMaxWorldCommandRouteOwnerDepth = 256;
inline constexpr uint32_t kMaxWorldCommandRouteGraphBytes = 4096;

struct WorldCommandRoute final {
    // GraphHost identity preserves the authored spelling, including non-UUID Editor names.
    std::string graph_asset_uuid{};
    feature::FeatureNodeId node_id{};
    feature::OwnerPath owner_path{};
    foundation::AssetId scene_asset{};
    foundation::Uuid scene_entity{};
    feature::SideEffectId effect{};
    uint32_t version{0};
    feature::PortId observation_port{};
};

/** AGWBIND1/schema2; Scene and entity are canonical 16-byte Foundation UUID values. */
[[nodiscard]] foundation::ResultValue<std::vector<std::byte>> encode_world_command_routes(
    std::span<const WorldCommandRoute> routes);
[[nodiscard]] foundation::ResultValue<std::vector<WorldCommandRoute>> decode_world_command_routes(
    std::span<const std::byte> bytes);
[[nodiscard]] std::filesystem::path world_command_routes_sidecar_path(const std::filesystem::path& scene_path);

/** Requires World.Transform/1, Command/Outbox, and any selected canonical observation input. */
[[nodiscard]] foundation::Result validate_world_command_route_contract(
    const WorldCommandRoute& route, const feature::FeatureContract& contract);
/** Selects the exact authored graph, node, and complete Composite owner path without live World state. */
[[nodiscard]] foundation::Result validate_world_command_route_source(
    const WorldCommandRoute& route, std::span<const feature::FeatureGraphDocument> graphs,
    const feature::FeatureRegistry& registry);
}  // namespace aeg::asset
