#include "feature_clock_test_fixture.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include "feature_graph.hpp"
#include "feature_registry.hpp"
#include "feature_world_transform.hpp"
#include "world_command_routes.hpp"

namespace {
using namespace aeg;

bool require(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

asset::WorldCommandRoute route() {
    asset::WorldCommandRoute result{};
    result.graph_asset_uuid = "ABCDEFAB-CDEF-4ABC-8DEF-ABCDEFABCDEF";
    result.node_id = feature::FeatureNodeId{0x0102030405060708ull};
    result.owner_path.nodes = {feature::FeatureNodeId{11}, feature::FeatureNodeId{22}};
    result.scene_asset = foundation::AssetId::parse("12345678-90ab-4cde-8f01-23456789abcd").value;
    result.scene_entity = foundation::Uuid::parse("fedcba98-7654-4321-8abc-def012345678").value;
    result.effect = feature::kWorldTransformCommandEffect;
    result.version = feature::kWorldTransformCommandVersion;
    result.observation_port = feature::PortId{"Observation"};
    return result;
}

bool same_route(const asset::WorldCommandRoute& left, const asset::WorldCommandRoute& right) {
    return left.graph_asset_uuid == right.graph_asset_uuid && left.node_id == right.node_id &&
           left.owner_path.nodes == right.owner_path.nodes && left.scene_asset == right.scene_asset &&
           left.scene_entity == right.scene_entity && left.effect == right.effect &&
           left.version == right.version && left.observation_port == right.observation_port;
}

void append_number(std::vector<std::byte>& bytes, uint64_t value, unsigned width) {
    for (unsigned index = 0; index < width; ++index)
        bytes.push_back(static_cast<std::byte>((value >> (8 * index)) & 0xff));
}

// Independent wire fixtures let malformed decode cases bypass the validating encoder.
std::vector<std::byte> raw_routes(std::span<const asset::WorldCommandRoute> routes) {
    std::vector<std::byte> bytes;
    for (const char value : std::string{"AGWBIND1"}) bytes.push_back(static_cast<std::byte>(value));
    append_number(bytes, 2, 4);
    append_number(bytes, routes.size(), 4);
    for (const auto& item : routes) {
        append_number(bytes, item.graph_asset_uuid.size(), 4);
        for (const char value : item.graph_asset_uuid) bytes.push_back(static_cast<std::byte>(value));
        append_number(bytes, item.node_id.value(), 8);
        append_number(bytes, item.owner_path.nodes.size(), 4);
        for (const auto owner : item.owner_path.nodes) append_number(bytes, owner.value(), 8);
        for (const auto value : item.scene_asset.value.bytes) bytes.push_back(static_cast<std::byte>(value));
        for (const auto value : item.scene_entity.bytes) bytes.push_back(static_cast<std::byte>(value));
        append_number(bytes, item.effect.value(), 8);
        append_number(bytes, item.version, 4);
        append_number(bytes, item.observation_port.value(), 8);
    }
    return bytes;
}

bool roundtrip_and_strict_wire() {
    auto first = route();
    auto second = first;
    second.graph_asset_uuid = "Editor graph: not a UUID";
    second.owner_path.nodes.clear();
    second.observation_port = {};
    second.effect = feature::SideEffectId{"Other.Command"};
    second.version = 17;
    const std::array source{first, second};
    const auto encoded = asset::encode_world_command_routes(source);
    if (!require(bool(encoded), "routes encode failed") ||
        !require(encoded.value == raw_routes(source), "route wire lost LE values or UUID byte order")) return false;
    const auto decoded = asset::decode_world_command_routes(encoded.value);
    if (!require(decoded && decoded.value.size() == 2, "routes decode failed") ||
        !require(same_route(decoded.value[0], first) && same_route(decoded.value[1], second),
                 "route roundtrip lost full identity, protocol, or graph spelling")) return false;
    const auto empty = asset::encode_world_command_routes({});
    if (!require(empty && !empty.value.empty(), "empty routes need an explicit versioned record")) return false;
    const auto decoded_empty = asset::decode_world_command_routes(empty.value);
    if (!require(decoded_empty && decoded_empty.value.empty(), "empty route record failed")) return false;
    auto old_version = encoded.value;
    old_version[8] = std::byte{1};
    if (!require(!asset::decode_world_command_routes(old_version), "sidecar schema1 was accepted")) return false;
    auto future_version = encoded.value;
    future_version[8] = std::byte{3};
    if (!require(!asset::decode_world_command_routes(future_version), "future sidecar schema was accepted")) return false;
    auto bad_magic = encoded.value;
    bad_magic[0] = std::byte{0};
    if (!require(!asset::decode_world_command_routes(bad_magic), "wrong sidecar magic was accepted")) return false;
    auto trailing = encoded.value;
    trailing.push_back(std::byte{0});
    if (!require(!asset::decode_world_command_routes(trailing), "trailing route data was accepted")) return false;
    for (std::size_t length = 0; length < encoded.value.size(); ++length)
        if (!require(!asset::decode_world_command_routes(std::span{encoded.value}.first(length)),
                     "truncated route data was accepted")) return false;
    return require(asset::world_command_routes_sidecar_path(std::filesystem::path{"folder/scene.aegscene"}) ==
                       std::filesystem::path{"folder/scene.aegscene.aegworldbindings"},
                   "route sidecar was not adjacent to the complete Scene filename");
}

bool rejects_invalid_identity_and_duplicate_sources() {
    const auto original = route();
    std::vector<asset::WorldCommandRoute> invalids;
    auto invalid = original; invalid.graph_asset_uuid.clear(); invalids.push_back(invalid);
    invalid = original; invalid.node_id = {}; invalids.push_back(invalid);
    invalid = original; invalid.owner_path.nodes[1] = {}; invalids.push_back(invalid);
    invalid = original; invalid.scene_asset = {}; invalids.push_back(invalid);
    invalid = original; invalid.scene_entity = {}; invalids.push_back(invalid);
    invalid = original; invalid.effect = {}; invalids.push_back(invalid);
    invalid = original; invalid.version = 0; invalids.push_back(invalid);
    for (const auto& item : invalids) {
        const std::array source{item};
        if (!require(!asset::encode_world_command_routes(source), "invalid identity encoded") ||
            !require(!asset::decode_world_command_routes(raw_routes(source)), "invalid identity decoded")) return false;
    }
    auto other_target = original;
    other_target.scene_asset.value.bytes[0] ^= 0x01;
    other_target.scene_entity.bytes[0] ^= 0x01;
    other_target.version = 2;
    other_target.observation_port = {};
    const std::array duplicate{original, other_target};
    if (!require(!asset::encode_world_command_routes(duplicate), "duplicate source with a different target encoded") ||
        !require(!asset::decode_world_command_routes(raw_routes(duplicate)), "duplicate source decoded")) return false;
    std::array distinct{original, original, original, original, original};
    distinct[1].graph_asset_uuid += "-other";
    distinct[2].node_id = feature::FeatureNodeId{99};
    distinct[3].owner_path.nodes.push_back(feature::FeatureNodeId{99});
    distinct[4].effect = feature::SideEffectId{"Other.Command"};
    const auto encoded = asset::encode_world_command_routes(distinct);
    const auto decoded = encoded ? asset::decode_world_command_routes(encoded.value)
                                : foundation::ResultValue<std::vector<asset::WorldCommandRoute>>{encoded.result, {}};
    return require(decoded && decoded.value.size() == distinct.size(), "source dimensions were incorrectly collapsed");
}

bool enforces_all_budgets() {
    auto item = route();
    item.graph_asset_uuid.assign(4096, 'G');
    item.owner_path.nodes.assign(256, feature::FeatureNodeId{1});
    const std::array largest_fields{item};
    const auto fields = asset::encode_world_command_routes(largest_fields);
    if (!require(fields && asset::decode_world_command_routes(fields.value), "legal field budget boundary rejected")) return false;
    item.owner_path.nodes.emplace_back(feature::FeatureNodeId{1});
    const std::array deep{item};
    if (!require(!asset::encode_world_command_routes(deep) && !asset::decode_world_command_routes(raw_routes(deep)),
                 "owner depth above 256 accepted")) return false;
    item.owner_path.nodes.clear();
    item.graph_asset_uuid.push_back('G');
    const std::array long_graph{item};
    if (!require(!asset::encode_world_command_routes(long_graph) &&
                 !asset::decode_world_command_routes(raw_routes(long_graph)), "graph above 4096 bytes accepted")) return false;
    item.graph_asset_uuid = "G";
    std::vector<asset::WorldCommandRoute> records(4096, item);
    for (std::size_t index = 0; index < records.size(); ++index)
        records[index].node_id = feature::FeatureNodeId{index + 1};
    const auto max_count = asset::encode_world_command_routes(records);
    if (!require(max_count && asset::decode_world_command_routes(max_count.value), "4096 route boundary rejected")) return false;
    records.push_back(item);
    records.back().node_id = feature::FeatureNodeId{4097};
    if (!require(!asset::encode_world_command_routes(records) &&
                 !asset::decode_world_command_routes(raw_routes(records)), "route count above 4096 accepted")) return false;
    records.pop_back();
    // 16 header bytes + 4096 * (68 fixed bytes + graph bytes) = exactly 16 MiB.
    for (auto& record : records) record.graph_asset_uuid.assign(4028, 'G');
    records.back().graph_asset_uuid.resize(4012);
    const auto at_limit = asset::encode_world_command_routes(records);
    if (!require(at_limit && at_limit.value.size() == 16u * 1024u * 1024u,
                 "exactly 16 MiB route record rejected") ||
        !require(bool(asset::decode_world_command_routes(at_limit.value)), "16 MiB route record did not decode")) return false;
    records.back().graph_asset_uuid.push_back('G');
    if (!require(!asset::encode_world_command_routes(records), "route total above 16 MiB encoded")) return false;
    auto oversized = at_limit.value;
    oversized.push_back(std::byte{0});
    return require(!asset::decode_world_command_routes(oversized), "route total above 16 MiB decoded");
}

feature::FeatureDefinition definition(feature::FeatureDefinitionId id, bool command = true) {
    feature::FeatureDefinition result{};
    result.id = id;
    result.version = {1, 0, 0};
    auto& contract = result.contract;
    contract.definition_id = id;
    contract.version = result.version;
    contract.domains = feature::to_mask(feature::FeatureDomain::kLogic);
    contract.scope.id = feature::ScopeId{"World"};
    contract.clock = aeg::feature::make_periodic_clock_contract(feature::ClockId{"LogicTick"}, 60, 2, feature::LateDataPolicy::kReject,
        aeg::feature::DeadlinePolicy::kObserve, contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    contract.permissions.version = 1;
    contract.ports = {{feature::PortId{"Observation"}, feature::PortDirection::kInput,
                       feature::kWorldTransformInputSchema, feature::kWorldTransformInputVersion}};
    if (command) contract.side_effects = {{feature::kWorldTransformCommandEffect, 1,
        feature::SideEffectKind::kCommand, feature::CommitPolicy::kOutbox}};
    return result;
}

bool validates_canonical_protocol() {
    const auto item = route();
    const auto good = definition(feature::FeatureDefinitionId{"Good"}).contract;
    if (!require(bool(asset::validate_world_command_route_contract(item, good)), "canonical contract rejected")) return false;
    auto without_observation = item;
    without_observation.observation_port = {};
    auto no_ports = good;
    no_ports.ports.clear();
    if (!require(bool(asset::validate_world_command_route_contract(without_observation, no_ports)),
                 "optional observation required a port")) return false;
    auto bad_route = item;
    bad_route.effect = feature::SideEffectId{"Other.Command"};
    auto matching_other = good;
    matching_other.side_effects[0].id = bad_route.effect;
    if (!require(!asset::validate_world_command_route_contract(bad_route, matching_other),
                 "a noncanonical effect passed pure contract validation")) return false;
    bad_route = item;
    bad_route.version = 2;
    auto matching_version = good;
    matching_version.side_effects[0].version = 2;
    if (!require(!asset::validate_world_command_route_contract(bad_route, matching_version),
                 "a noncanonical protocol version passed")) return false;
    for (unsigned variant = 0; variant < 7; ++variant) {
        auto invalid = good;
        switch (variant) {
            case 0: invalid.side_effects.clear(); break;
            case 1: invalid.side_effects[0].kind = feature::SideEffectKind::kIntent; break;
            case 2: invalid.side_effects[0].commit = feature::CommitPolicy::kOwnerBoundary; break;
            case 3: invalid.ports[0].direction = feature::PortDirection::kOutput; break;
            case 4: invalid.ports[0].schema = feature::ValueSchemaId{"Other.Schema"}; break;
            case 5: invalid.ports[0].version = 2; break;
            case 6: invalid.ports[0].id = feature::PortId{"Other.Port"}; break;
        }
        if (!require(!asset::validate_world_command_route_contract(item, invalid), "incompatible route contract accepted")) return false;
    }
    return true;
}

bool validates_exact_graph_node_and_owner() {
    feature::FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    const feature::FeatureDefinitionId good_id{"Good"}, wrong_id{"Wrong"}, middle_id{"Middle"}, composite_id{"Composite"};
    auto middle = definition(middle_id, false);
    middle.contract.structure = feature::FeatureStructure::kComposite; middle.contract.clock.work_budget_ns = 0;
    middle.contract.children = {wrong_id, good_id};
    auto composite = definition(composite_id, false);
    composite.contract.structure = feature::FeatureStructure::kComposite; composite.contract.clock.work_budget_ns = 0;
    composite.contract.children = {middle_id};
    if (!require(bool(aeg::feature::test::register_clock_test_definition(registry, definition(good_id))) &&
                 bool(aeg::feature::test::register_clock_test_definition(registry, definition(wrong_id, false))) &&
                 bool(aeg::feature::test::register_clock_test_definition(registry, middle)) && bool(aeg::feature::test::register_clock_test_definition(registry, composite)),
                 "source registry fixture invalid")) return false;
    auto item = route();
    feature::FeatureGraphDocument graph{};
    graph.asset_uuid = item.graph_asset_uuid;
    graph.nodes = {{item.node_id, good_id, {1, 0, 0}, item.owner_path, {}}};
    auto other_graph = graph;
    other_graph.asset_uuid = "other graph with same node";
    other_graph.nodes[0].definition_id = wrong_id;
    std::array graphs{other_graph, graph};
    if (!require(bool(asset::validate_world_command_route_source(item, graphs, registry)),
                 "exact atomic root failed with same node in another graph")) return false;
    auto invalid = item;
    invalid.graph_asset_uuid = other_graph.asset_uuid;
    if (!require(!asset::validate_world_command_route_source(invalid, graphs, registry), "source ignored graph identity")) return false;
    invalid = item;
    invalid.graph_asset_uuid = "abcdefab-cdef-4abc-8def-abcdefabcdef";
    if (!require(!asset::validate_world_command_route_source(invalid, graphs, registry), "source normalized graph spelling")) return false;
    invalid = item;
    invalid.node_id = feature::FeatureNodeId{99};
    if (!require(!asset::validate_world_command_route_source(invalid, graphs, registry), "missing source node accepted")) return false;
    invalid = item;
    invalid.owner_path.nodes.push_back(item.node_id);
    if (!require(!asset::validate_world_command_route_source(invalid, graphs, registry), "atomic root incorrectly appended authored node")) return false;
    const std::array ambiguous{graph, graph};
    if (!require(!asset::validate_world_command_route_source(item, ambiguous, registry), "ambiguous graph accepted")) return false;
    auto duplicate_node_graph = graph;
    duplicate_node_graph.nodes.push_back(graph.nodes.front());
    const std::array duplicate_nodes{duplicate_node_graph};
    if (!require(!asset::validate_world_command_route_source(item, duplicate_nodes, registry),
                 "ambiguous authored node accepted")) return false;
    graph.nodes[0].definition_id = composite_id;
    const std::array composite_graphs{other_graph, graph};
    item.owner_path.nodes.push_back(item.node_id);
    item.owner_path.nodes.push_back(feature::FeatureNodeId{middle_id.value()});
    item.owner_path.nodes.push_back(feature::FeatureNodeId{good_id.value()});
    if (!require(bool(asset::validate_world_command_route_source(item, composite_graphs, registry)),
                 "full nested Composite suffix failed")) return false;
    invalid = item;
    invalid.owner_path.nodes.back() = feature::FeatureNodeId{wrong_id.value()};
    if (!require(!asset::validate_world_command_route_source(invalid, composite_graphs, registry),
                 "Composite leaf contract was not selected exactly")) return false;
    invalid = item;
    invalid.owner_path.nodes.erase(invalid.owner_path.nodes.begin() + 2);
    if (!require(!asset::validate_world_command_route_source(invalid, composite_graphs, registry),
                 "Composite accepted suffix without authored node")) return false;
    invalid = item;
    invalid.owner_path.nodes.erase(invalid.owner_path.nodes.begin() + 3);
    if (!require(!asset::validate_world_command_route_source(invalid, composite_graphs, registry),
                 "Composite accepted incomplete owner suffix")) return false;
    graph.nodes[0].owner_path.nodes.clear();
    graph.nodes[0].definition_id = good_id;
    item.owner_path.nodes.clear();
    const std::array empty_owner{graph};
    return require(bool(asset::validate_world_command_route_source(item, empty_owner, registry)), "empty authored root owner rejected");
}
}  // namespace

int main() {
    if (!roundtrip_and_strict_wire() || !rejects_invalid_identity_and_duplicate_sources() ||
        !enforces_all_budgets() || !validates_canonical_protocol() || !validates_exact_graph_node_and_owner()) return 1;
    std::cout << "World command routes smoke passed\n";
    return 0;
}
