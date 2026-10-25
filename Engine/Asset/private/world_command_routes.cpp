#include "world_command_routes.hpp"

#include <algorithm>
#include <new>
#include <set>
#include <tuple>
#include <utility>

#include "feature_graph.hpp"
#include "feature_registry.hpp"
#include "feature_world_transform.hpp"
#include "serialization.hpp"

namespace aeg::asset {
namespace {
constexpr uint64_t kMagic = 0x31444e4942574741ull;  // AGWBIND1
constexpr uint32_t kSchemaVersion = 2;
constexpr std::size_t kHeaderBytes = 16;
constexpr std::size_t kFixedRouteBytes = 68;

foundation::Result validate_identity(const WorldCommandRoute& route) {
    if (route.graph_asset_uuid.empty() || route.graph_asset_uuid.size() > kMaxWorldCommandRouteGraphBytes ||
        !route.node_id.is_valid() || route.owner_path.nodes.size() > kMaxWorldCommandRouteOwnerDepth ||
        route.scene_asset == foundation::AssetId{} || route.scene_entity == foundation::Uuid{} ||
        !route.effect.is_valid() || route.version == 0 ||
        std::any_of(route.owner_path.nodes.begin(), route.owner_path.nodes.end(),
                    [](feature::FeatureNodeId owner) { return !owner.is_valid(); }))
        return foundation::invalid_argument("World command route identity or field size is invalid");
    return foundation::Result::success();
}

// Pointer keys keep the validation set bounded without copying graph strings or owner paths.
struct SourceLess final {
    bool operator()(const WorldCommandRoute* left, const WorldCommandRoute* right) const {
        return std::tie(left->node_id, left->effect, left->owner_path.nodes, left->graph_asset_uuid) <
               std::tie(right->node_id, right->effect, right->owner_path.nodes, right->graph_asset_uuid);
    }
};

foundation::Result validate_routes(std::span<const WorldCommandRoute> routes) {
    if (routes.size() > kMaxWorldCommandRoutes)
        return foundation::invalid_argument("World command route count exceeds its budget");
    std::size_t total = kHeaderBytes;
    std::set<const WorldCommandRoute*, SourceLess> sources;
    for (const auto& route : routes) {
        if (const auto result = validate_identity(route); !result) return result;
        const auto size = kFixedRouteBytes + route.graph_asset_uuid.size() + route.owner_path.nodes.size() * 8u;
        if (size > kMaxWorldCommandRoutesBytes - total)
            return foundation::invalid_argument("World command routes exceed their total byte budget");
        total += size;
        if (!sources.insert(&route).second)
            return foundation::invalid_argument("World command routes contain a duplicate authored source");
    }
    return foundation::Result::success();
}
}  // namespace

foundation::ResultValue<std::vector<std::byte>> encode_world_command_routes(
    std::span<const WorldCommandRoute> routes) {
    try {
        if (const auto result = validate_routes(routes); !result) return {result, {}};
        serialization::BinaryWriter writer;
        if (const auto result = writer.write_u64(kMagic); !result) return {result, {}};
        if (const auto result = writer.write_u32(kSchemaVersion); !result) return {result, {}};
        if (const auto result = writer.write_u32(static_cast<uint32_t>(routes.size())); !result) return {result, {}};
        for (const auto& route : routes) {
            if (const auto result = writer.write_string(route.graph_asset_uuid, kMaxWorldCommandRouteGraphBytes); !result)
                return {result, {}};
            if (const auto result = writer.write_u64(route.node_id.value()); !result) return {result, {}};
            if (const auto result = writer.write_u32(static_cast<uint32_t>(route.owner_path.nodes.size())); !result)
                return {result, {}};
            for (const auto owner : route.owner_path.nodes)
                if (const auto result = writer.write_u64(owner.value()); !result) return {result, {}};
            if (const auto result = writer.write_bytes(route.scene_asset.value.bytes); !result) return {result, {}};
            if (const auto result = writer.write_bytes(route.scene_entity.bytes); !result) return {result, {}};
            if (const auto result = writer.write_u64(route.effect.value()); !result) return {result, {}};
            if (const auto result = writer.write_u32(route.version); !result) return {result, {}};
            if (const auto result = writer.write_u64(route.observation_port.value()); !result) return {result, {}};
        }
        const auto bytes = std::as_bytes(writer.data());
        return {foundation::Result::success(), {bytes.begin(), bytes.end()}};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("World command route encoding allocation failed"), {}};
    }
}

foundation::ResultValue<std::vector<WorldCommandRoute>> decode_world_command_routes(std::span<const std::byte> bytes) {
    if (bytes.size() > kMaxWorldCommandRoutesBytes)
        return {foundation::invalid_argument("World command routes exceed their total byte budget"), {}};
    try {
        serialization::BinaryReader reader({reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});
        const auto magic = reader.read_u64();
        const auto version = reader.read_u32();
        const auto count = reader.read_u32();
        if (!magic || !version || !count || magic.value != kMagic || version.value != kSchemaVersion ||
            count.value > kMaxWorldCommandRoutes || count.value > reader.remaining() / kFixedRouteBytes)
            return {foundation::invalid_argument("World command route header is invalid or unsupported"), {}};
        std::vector<WorldCommandRoute> routes;
        routes.reserve(count.value);
        for (uint32_t index = 0; index < count.value; ++index) {
            auto graph = reader.read_string(kMaxWorldCommandRouteGraphBytes);
            const auto node = reader.read_u64();
            const auto owner_count = reader.read_u32();
            if (!graph || !node || !owner_count || owner_count.value > kMaxWorldCommandRouteOwnerDepth ||
                owner_count.value > reader.remaining() / 8u)
                return {foundation::invalid_argument("World command route source is invalid or truncated"), {}};
            WorldCommandRoute route;
            route.graph_asset_uuid = std::move(graph.value);
            route.node_id = feature::FeatureNodeId{node.value};
            route.owner_path.nodes.reserve(owner_count.value);
            for (uint32_t owner = 0; owner < owner_count.value; ++owner) {
                const auto id = reader.read_u64();
                if (!id) return {id.result, {}};
                route.owner_path.nodes.emplace_back(id.value);
            }
            const auto scene = reader.read_bytes(16);
            const auto entity = reader.read_bytes(16);
            const auto effect = reader.read_u64();
            const auto route_version = reader.read_u32();
            const auto observation = reader.read_u64();
            if (!scene || !entity || !effect || !route_version || !observation)
                return {foundation::invalid_argument("World command route target is truncated"), {}};
            std::copy(scene.value.begin(), scene.value.end(), route.scene_asset.value.bytes.begin());
            std::copy(entity.value.begin(), entity.value.end(), route.scene_entity.bytes.begin());
            route.effect = feature::SideEffectId{effect.value};
            route.version = route_version.value;
            route.observation_port = feature::PortId{observation.value};
            routes.push_back(std::move(route));
        }
        if (reader.remaining() != 0)
            return {foundation::invalid_argument("World command routes have trailing bytes"), {}};
        if (const auto result = validate_routes(routes); !result) return {result, {}};
        return {foundation::Result::success(), std::move(routes)};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("World command route decoding allocation failed"), {}};
    }
}

std::filesystem::path world_command_routes_sidecar_path(const std::filesystem::path& scene_path) {
    auto result = scene_path;
    result += ".aegworldbindings";
    return result;
}

foundation::Result validate_world_command_route_contract(
    const WorldCommandRoute& route, const feature::FeatureContract& contract) {
    if (const auto result = validate_identity(route); !result) return result;
    if (route.effect != feature::kWorldTransformCommandEffect || route.version != feature::kWorldTransformCommandVersion)
        return foundation::invalid_argument("World command route does not use the canonical Transform protocol");
    const auto effect = std::find_if(contract.side_effects.begin(), contract.side_effects.end(),
        [&](const feature::SideEffectContract& candidate) {
            return candidate.id == route.effect && candidate.version == route.version &&
                   candidate.kind == feature::SideEffectKind::kCommand &&
                   candidate.commit == feature::CommitPolicy::kOutbox;
        });
    if (effect == contract.side_effects.end())
        return foundation::invalid_argument("World command route is not declared by the selected Feature contract");
    if (route.observation_port.is_valid()) {
        const auto port = std::find_if(contract.ports.begin(), contract.ports.end(),
            [&](const feature::PortContract& candidate) {
                return candidate.id == route.observation_port && candidate.direction == feature::PortDirection::kInput &&
                       candidate.schema == feature::kWorldTransformInputSchema &&
                       candidate.version == feature::kWorldTransformInputVersion;
            });
        if (port == contract.ports.end())
            return foundation::invalid_argument("World command observation port does not match the canonical Transform input");
    }
    return foundation::Result::success();
}

foundation::Result validate_world_command_route_source(
    const WorldCommandRoute& route, std::span<const feature::FeatureGraphDocument> graphs,
    const feature::FeatureRegistry& registry) {
    if (const auto result = validate_identity(route); !result) return result;
    try {
        const feature::FeatureGraphDocument* graph = nullptr;
        for (const auto& candidate : graphs) {
            if (candidate.asset_uuid != route.graph_asset_uuid) continue;
            if (graph != nullptr)
                return foundation::invalid_argument("World command route graph is ambiguous in the candidate batch");
            graph = &candidate;
        }
        if (graph == nullptr)
            return foundation::invalid_argument("World command route graph is missing from the candidate batch");
        const feature::FeatureNode* node = nullptr;
        for (const auto& candidate : graph->nodes) {
            if (candidate.node_id != route.node_id) continue;
            if (node != nullptr)
                return foundation::invalid_argument("World command route authored node is ambiguous");
            node = &candidate;
        }
        if (node == nullptr)
            return foundation::invalid_argument("World command route authored node is missing");
        const auto expanded = registry.expand(node->definition_id, node->owner_path);
        if (!expanded) return expanded.result;
        const feature::ExpandedFeature* selected = nullptr;
        const auto visit = [&](const auto& self, const feature::ExpandedFeature& current) -> foundation::Result {
            auto authored_path = node->owner_path;
            if (current.owner_path.nodes.size() < node->owner_path.nodes.size() ||
                !std::equal(node->owner_path.nodes.begin(), node->owner_path.nodes.end(), current.owner_path.nodes.begin()))
                return foundation::invalid_argument("Feature expansion lost authored World route ancestry");
            if (&current != &expanded.value) {
                authored_path.nodes.push_back(node->node_id);
                authored_path.nodes.insert(authored_path.nodes.end(),
                    current.owner_path.nodes.begin() + static_cast<std::ptrdiff_t>(node->owner_path.nodes.size()),
                    current.owner_path.nodes.end());
            }
            if (authored_path.nodes == route.owner_path.nodes) {
                if (selected != nullptr)
                    return foundation::invalid_argument("World command route owner attribution is ambiguous");
                selected = &current;
            }
            for (const auto& child : current.children)
                if (const auto result = self(self, child); !result) return result;
            return foundation::Result::success();
        };
        if (const auto result = visit(visit, expanded.value); !result) return result;
        if (selected == nullptr)
            return foundation::invalid_argument("World command route has no exact authored owner");
        return validate_world_command_route_contract(route, selected->contract);
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("World command route source validation allocation failed");
    }
}
}  // namespace aeg::asset
