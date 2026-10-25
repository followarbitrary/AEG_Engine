#include "feature_graph_asset.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "serialization.hpp"

namespace aeg::asset {
using namespace aeg::feature;
namespace {
using serialization::BinaryReader;
using serialization::BinaryWriter;

constexpr std::array<uint8_t, 4> kMagic{'A', 'E', 'G', 'F'};
constexpr uint32_t kMaxCollectionEntries = 100000;
constexpr uint32_t kMaxStringEntries = 4096;
constexpr uint32_t kMaxStringBytes = 16u * 1024u * 1024u;

foundation::Result invalid(std::string message,
                           foundation::ResultCode code = foundation::ResultCode::kInvalidArgument) {
    return foundation::Result::failure(code, std::move(message));
}

foundation::Result write_u8(BinaryWriter& writer, uint8_t value) {
    return writer.write_bytes(std::span<const uint8_t>(&value, 1));
}

foundation::Result write_strings(BinaryWriter& writer, const std::vector<std::string>& values) {
    if (values.size() > kMaxStringEntries) {
        return invalid("FeatureGraph string collection exceeds configured count limit");
    }
    if (const auto result = writer.write_u32(static_cast<uint32_t>(values.size())); !result) return result;
    for (const auto& value : values) {
        if (const auto result = writer.write_string(value, kMaxStringBytes); !result) return result;
    }
    return foundation::Result::success();
}

foundation::Result read_strings(BinaryReader& reader, std::vector<std::string>& values,
                                uint32_t maximum) {
    const auto count = reader.read_u32();
    if (!count) return count.result;
    if (count.value > maximum) {
        return invalid("FeatureGraph string collection exceeds configured count limit");
    }
    values.reserve(count.value);
    for (uint32_t index = 0; index < count.value; ++index) {
        const auto value = reader.read_string(kMaxStringBytes);
        if (!value) return value.result;
        values.push_back(value.value);
    }
    return foundation::Result::success();
}

foundation::ResultValue<std::vector<std::byte>> copy_bytes(const BinaryWriter& writer) {
    std::vector<std::byte> bytes;
    bytes.reserve(writer.size());
    for (const uint8_t value : writer.data()) bytes.push_back(static_cast<std::byte>(value));
    return {foundation::Result::success(), std::move(bytes)};
}
}  // namespace

foundation::ResultValue<std::vector<std::byte>> encode_feature_graph_asset(
    const FeatureGraphDocument& graph) {
    if (graph.asset_uuid.empty() || graph.schema_version == 0 || graph.graph_version == 0) {
        return {invalid("FeatureGraph asset requires identity and schema"), {}};
    }
    BinaryWriter writer;
    if (const auto result = writer.write_bytes(kMagic); !result) return {result, {}};
    if (const auto result = writer.write_u32(graph.schema_version); !result) return {result, {}};
    if (const auto result = writer.write_u64(graph.graph_version); !result) return {result, {}};
    if (const auto result = writer.write_u64(graph.draft_revision); !result) return {result, {}};
    if (const auto result = writer.write_u64(graph.layout_revision); !result) return {result, {}};
    if (const auto result = writer.write_string(graph.asset_uuid, kMaxStringBytes); !result) return {result, {}};
    if (const auto result = write_strings(writer, graph.module_requirements); !result) return {result, {}};
    if (graph.nodes.size() > kMaxCollectionEntries) {
        return {invalid("FeatureGraph node count exceeds configured limit"), {}};
    }
    if (const auto result = writer.write_u32(static_cast<uint32_t>(graph.nodes.size())); !result) return {result, {}};
    for (const auto& node : graph.nodes) {
        if (const auto result = writer.write_u64(node.node_id.value()); !result) return {result, {}};
        if (const auto result = writer.write_u64(node.definition_id.value()); !result) return {result, {}};
        if (const auto result = writer.write_u16(node.definition_version.major); !result) return {result, {}};
        if (const auto result = writer.write_u16(node.definition_version.minor); !result) return {result, {}};
        if (const auto result = writer.write_u16(node.definition_version.patch); !result) return {result, {}};
        if (node.owner_path.nodes.size() > kMaxCollectionEntries) {
            return {invalid("FeatureGraph owner path exceeds configured limit"), {}};
        }
        if (const auto result = writer.write_u32(static_cast<uint32_t>(node.owner_path.nodes.size())); !result) return {result, {}};
        for (const auto path_node : node.owner_path.nodes) {
            if (const auto result = writer.write_u64(path_node.value()); !result) return {result, {}};
        }
        if (node.parameter_blob.size() > std::numeric_limits<uint32_t>::max()) {
            return {invalid("FeatureGraph parameter blob exceeds wire format capacity"), {}};
        }
        if (const auto result = writer.write_u32(static_cast<uint32_t>(node.parameter_blob.size())); !result) return {result, {}};
        if (const auto result = writer.write_bytes(node.parameter_blob); !result) return {result, {}};
    }
    if (graph.edges.size() > kMaxCollectionEntries) {
        return {invalid("FeatureGraph edge count exceeds configured limit"), {}};
    }
    if (const auto result = writer.write_u32(static_cast<uint32_t>(graph.edges.size())); !result) return {result, {}};
    for (const auto& edge : graph.edges) {
        if (const auto result = writer.write_u64(edge.source_node.value()); !result) return {result, {}};
        if (const auto result = writer.write_u64(edge.source_port.value()); !result) return {result, {}};
        if (const auto result = writer.write_u64(edge.target_node.value()); !result) return {result, {}};
        if (const auto result = writer.write_u64(edge.target_port.value()); !result) return {result, {}};
        if (const auto result = write_u8(writer, static_cast<uint8_t>(edge.kind)); !result) return {result, {}};
        if (const auto result = writer.write_u64(edge.source_clock.value()); !result) return {result, {}};
        if (const auto result = writer.write_u64(edge.target_clock.value()); !result) return {result, {}};
        if (const auto result = writer.write_u32(edge.version); !result) return {result, {}};
        if (const auto result = writer.write_u32(edge.capacity); !result) return {result, {}};
        if (const auto result = writer.write_u32(edge.deadline_ticks); !result) return {result, {}};
    }
    if (graph.process_subgraphs.size() > kMaxCollectionEntries) {
        return {invalid("FeatureGraph process subgraph count exceeds configured limit"), {}};
    }
    if (const auto result = writer.write_u32(static_cast<uint32_t>(graph.process_subgraphs.size())); !result) return {result, {}};
    for (const auto& process : graph.process_subgraphs) {
        if (const auto result = writer.write_u64(process.owner_node.value()); !result) return {result, {}};
        if (const auto result = write_strings(writer, process.states); !result) return {result, {}};
        if (const auto result = write_strings(writer, process.transitions); !result) return {result, {}};
        if (const auto result = writer.write_u32(process.state_schema_version); !result) return {result, {}};
        if (const auto result = writer.write_string(process.initial_state, kMaxStringBytes); !result) return {result, {}};
        if (const auto result = write_strings(writer, process.terminal_states); !result) return {result, {}};
    }
    if (const auto result = write_strings(writer, graph.input_bindings); !result) return {result, {}};
    if (const auto result = write_strings(writer, graph.variadic_slots); !result) return {result, {}};
    if (const auto result = write_strings(writer, graph.extensions); !result) return {result, {}};
    if (const auto result = writer.write_u64(graph.contract_digest); !result) return {result, {}};
    return copy_bytes(writer);
}

foundation::ResultValue<FeatureGraphDocument> decode_feature_graph_asset(
    std::span<const std::byte> bytes) {
    const auto input = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    BinaryReader reader(input);
    const auto magic = reader.read_bytes(kMagic.size());
    if (!magic || !std::equal(magic.value.begin(), magic.value.end(), kMagic.begin())) {
        return {invalid("unsupported FeatureGraph asset magic", foundation::ResultCode::kUnsupported), {}};
    }
    FeatureGraphDocument graph{};
    const auto schema = reader.read_u32();
    const auto graph_version = reader.read_u64();
    const auto draft_revision = reader.read_u64();
    const auto layout_revision = reader.read_u64();
    if (!schema || !graph_version || !draft_revision || !layout_revision) return {invalid("truncated FeatureGraph asset"), {}};
    graph.schema_version = schema.value;
    graph.graph_version = graph_version.value;
    graph.draft_revision = draft_revision.value;
    graph.layout_revision = layout_revision.value;
    const auto asset_uuid = reader.read_string(kMaxStringBytes);
    if (!asset_uuid || !read_strings(reader, graph.module_requirements, kMaxStringEntries)) return {invalid("truncated FeatureGraph asset"), {}};
    graph.asset_uuid = asset_uuid.value;

    const auto node_count = reader.read_u32();
    if (!node_count || node_count.value > kMaxCollectionEntries) return {invalid("invalid FeatureGraph node count"), {}};
    graph.nodes.reserve(node_count.value);
    for (uint32_t index = 0; index < node_count.value; ++index) {
        FeatureNode node{};
        const auto node_id = reader.read_u64();
        const auto definition_id = reader.read_u64();
        const auto major = reader.read_u16();
        const auto minor = reader.read_u16();
        const auto patch = reader.read_u16();
        if (!node_id || !definition_id || !major || !minor || !patch) return {invalid("truncated FeatureGraph node"), {}};
        node.node_id = FeatureNodeId(node_id.value);
        node.definition_id = FeatureDefinitionId(definition_id.value);
        node.definition_version = Version{major.value, minor.value, patch.value};
        const auto path_count = reader.read_u32();
        if (!path_count || path_count.value > kMaxCollectionEntries) return {invalid("invalid FeatureGraph owner path"), {}};
        node.owner_path.nodes.reserve(path_count.value);
        for (uint32_t path = 0; path < path_count.value; ++path) {
            const auto path_id = reader.read_u64();
            if (!path_id) return {invalid("truncated FeatureGraph owner path"), {}};
            node.owner_path.nodes.emplace_back(path_id.value);
        }
        const auto parameter_size = reader.read_u32();
        if (!parameter_size) return {invalid("truncated FeatureGraph node parameters"), {}};
        const auto parameter_blob = reader.read_bytes(parameter_size.value);
        if (!parameter_blob) return {invalid("truncated FeatureGraph node parameters"), {}};
        node.parameter_blob.assign(parameter_blob.value.begin(), parameter_blob.value.end());
        graph.nodes.push_back(std::move(node));
    }

    const auto edge_count = reader.read_u32();
    if (!edge_count || edge_count.value > kMaxCollectionEntries) return {invalid("invalid FeatureGraph edge count"), {}};
    graph.edges.reserve(edge_count.value);
    for (uint32_t index = 0; index < edge_count.value; ++index) {
        FeatureEdge edge{};
        const auto source_node = reader.read_u64();
        const auto source_port = reader.read_u64();
        const auto target_node = reader.read_u64();
        const auto target_port = reader.read_u64();
        const auto kind = reader.read_bytes(1);
        const auto source_clock = reader.read_u64();
        const auto target_clock = reader.read_u64();
        const auto version = reader.read_u32();
        const auto capacity = reader.read_u32();
        const auto deadline = reader.read_u32();
        if (!source_node || !source_port || !target_node || !target_port || !kind ||
            kind.value[0] > static_cast<uint8_t>(EdgeKind::kReceipt) || !source_clock || !target_clock ||
            !version || !capacity || !deadline) return {invalid("truncated or invalid FeatureGraph edge"), {}};
        edge.source_node = FeatureNodeId(source_node.value);
        edge.source_port = PortId(source_port.value);
        edge.target_node = FeatureNodeId(target_node.value);
        edge.target_port = PortId(target_port.value);
        edge.kind = static_cast<EdgeKind>(kind.value[0]);
        edge.source_clock = ClockId(source_clock.value);
        edge.target_clock = ClockId(target_clock.value);
        edge.version = version.value;
        edge.capacity = capacity.value;
        edge.deadline_ticks = deadline.value;
        graph.edges.push_back(edge);
    }

    const auto process_count = reader.read_u32();
    if (!process_count || process_count.value > kMaxCollectionEntries) return {invalid("invalid FeatureGraph process subgraph count"), {}};
    graph.process_subgraphs.reserve(process_count.value);
    for (uint32_t index = 0; index < process_count.value; ++index) {
        ProcessSubgraph process{};
        const auto owner_node = reader.read_u64();
        if (!owner_node || !read_strings(reader, process.states, kMaxStringEntries) ||
            !read_strings(reader, process.transitions, kMaxStringEntries)) return {invalid("truncated FeatureGraph process subgraph"), {}};
        const auto state_schema_version = reader.read_u32();
        const auto initial_state = reader.read_string(kMaxStringBytes);
        if (!state_schema_version || !initial_state ||
            !read_strings(reader, process.terminal_states, kMaxStringEntries))
            return {invalid("truncated FeatureGraph process subgraph"), {}};
        process.owner_node = FeatureNodeId(owner_node.value);
        process.state_schema_version = state_schema_version.value;
        process.initial_state = initial_state.value;
        graph.process_subgraphs.push_back(std::move(process));
    }
    if (!read_strings(reader, graph.input_bindings, kMaxStringEntries) ||
        !read_strings(reader, graph.variadic_slots, kMaxStringEntries) ||
        !read_strings(reader, graph.extensions, kMaxStringEntries)) return {invalid("truncated FeatureGraph metadata"), {}};
    const auto contract_digest = reader.read_u64();
    if (!contract_digest) return {invalid("truncated FeatureGraph metadata"), {}};
    graph.contract_digest = contract_digest.value;
    if (graph.schema_version == 0 || graph.graph_version == 0) return {invalid("invalid FeatureGraph schema version"), {}};
    if (reader.remaining() != 0) return {invalid("FeatureGraph asset has trailing bytes"), {}};
    return {foundation::Result::success(), std::move(graph)};
}
}  // namespace aeg::asset
