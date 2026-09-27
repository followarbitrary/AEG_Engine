#include "pipeline_asset.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_set>

namespace aeg::asset {
namespace {
using namespace pipeline;
constexpr uint32_t kVersion = 2;
constexpr uint32_t kMaxCount = 1'000'000;
constexpr uint32_t kMaxString = 16 * 1024 * 1024;

struct Writer final {
    std::vector<std::byte> bytes{};
    template <typename T>
    void value(T input) {
        const auto* ptr = reinterpret_cast<const std::byte*>(&input);
        bytes.insert(bytes.end(), ptr, ptr + sizeof(T));
    }
    void string(std::string_view input) {
        value(static_cast<uint32_t>(input.size()));
        bytes.insert(bytes.end(), reinterpret_cast<const std::byte*>(input.data()),
                     reinterpret_cast<const std::byte*>(input.data() + input.size()));
    }
    void id(uint64_t input) { value(input); }
};

struct Reader final {
    std::span<const std::byte> bytes{};
    std::size_t offset{};
    template <typename T>
    bool value(T& output) {
        if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
            return false;
        std::memcpy(&output, bytes.data() + offset, sizeof(T));
        offset += sizeof(T);
        return true;
    }
    bool string(std::string& output) {
        uint32_t size{};
        if (!value(size) || size > kMaxString || size > bytes.size() - offset)
            return false;
        output.assign(reinterpret_cast<const char*>(bytes.data() + offset), size);
        offset += size;
        return true;
    }
    bool id(uint64_t& output) { return value(output) && output != 0; }
};

void write_parameters(Writer& writer, const ParameterBlock& parameters) {
    writer.value(static_cast<uint32_t>(parameters.size()));
    for (const auto& [id, parameter] : parameters) {
        writer.id(id);
        uint8_t tag = static_cast<uint8_t>(parameter.index());
        writer.value(tag);
        if (const auto* bool_value = std::get_if<bool>(&parameter))
            writer.value(static_cast<uint8_t>(*bool_value));
        else if (const auto* int_value = std::get_if<int64_t>(&parameter))
            writer.value(*int_value);
        else if (const auto* uint_value = std::get_if<uint64_t>(&parameter))
            writer.value(*uint_value);
        else if (const auto* float_value = std::get_if<double>(&parameter))
            writer.value(*float_value);
        else if (const auto* string_value = std::get_if<std::string>(&parameter))
            writer.string(*string_value);
        else if (const auto* vector_value = std::get_if<std::vector<double>>(&parameter)) {
            writer.value(static_cast<uint32_t>(vector_value->size()));
            for (double item : *vector_value)
                writer.value(item);
        } else if (const auto* asset_value = std::get_if<AssetReference>(&parameter))
            writer.string(asset_value->uuid);
    }
}

bool read_parameters(Reader& reader, ParameterBlock& parameters) {
    uint32_t count{};
    if (!reader.value(count) || count > kMaxCount)
        return false;
    for (uint32_t index = 0; index < count; ++index) {
        uint64_t id{};
        uint8_t tag{};
        if (!reader.id(id) || !reader.value(tag) || tag > 6)
            return false;
        ParameterValue decoded_value{};
        switch (tag) {
            case 0: {
                uint8_t item{};
                if (!reader.value(item) || item > 1)
                    return false;
                decoded_value = item != 0;
                break;
            }
            case 1: {
                int64_t item{};
                if (!reader.value(item))
                    return false;
                decoded_value = item;
                break;
            }
            case 2: {
                uint64_t item{};
                if (!reader.value(item))
                    return false;
                decoded_value = item;
                break;
            }
            case 3: {
                double item{};
                if (!reader.value(item) || !std::isfinite(item))
                    return false;
                decoded_value = item;
                break;
            }
            case 4: {
                std::string item;
                if (!reader.string(item))
                    return false;
                decoded_value = std::move(item);
                break;
            }
            case 5: {
                uint32_t size{};
                if (!reader.value(size) || size > kMaxCount ||
                    size > (reader.bytes.size() - reader.offset) / sizeof(double))
                    return false;
                std::vector<double> item(size);
                for (double& number : item)
                    if (!reader.value(number) || !std::isfinite(number))
                        return false;
                decoded_value = std::move(item);
                break;
            }
            case 6: {
                std::string item;
                if (!reader.string(item) || item.empty())
                    return false;
                decoded_value = AssetReference{std::move(item)};
                break;
            }
            default:
                return false;
        }
        if (!parameters.emplace(id, std::move(decoded_value)).second)
            return false;
    }
    return true;
}

void write_common(Writer& writer, std::string_view node_id, Version version, const ParameterBlock& parameters) {
    writer.string(node_id);
    writer.value(version.major);
    writer.value(version.minor);
    writer.value(version.patch);
    write_parameters(writer, parameters);
}
bool read_common(Reader& reader, std::string& node_id, Version& version, ParameterBlock& parameters) {
    return reader.string(node_id) && reader.value(version.major) && reader.value(version.minor) &&
           reader.value(version.patch) && read_parameters(reader, parameters) && !node_id.empty();
}

bool unique_node(const PipelineDocument& document) {
    std::unordered_set<std::string> ids;
    for (const auto& node : document.pipeline_nodes)
        if (!ids.insert(node.node_id).second)
            return false;
    for (const auto& node : document.feature_nodes)
        if (!ids.insert(node.node_id).second)
            return false;
    for (const auto& node : document.pass_nodes)
        if (!ids.insert(node.node_id).second)
            return false;
    for (const auto& node : document.resources)
        if (!ids.insert(node.node_id).second)
            return false;
    std::unordered_set<std::string> edges;
    for (const auto& edge : document.edges)
        if (edge.edge_id.empty() || !edges.insert(edge.edge_id).second)
            return false;
    return true;
}

}  // namespace

foundation::ResultValue<std::vector<std::byte>> encode_pipeline_asset(const PipelineDocument& document) {
    if (document.schema_version == 0 || document.asset_uuid.empty() || document.name.empty() || !unique_node(document))
        return {foundation::invalid_argument("Pipeline asset document identity is invalid"), {}};
    if (document.pipeline_nodes.size() > kMaxCount || document.feature_nodes.size() > kMaxCount ||
        document.pass_nodes.size() > kMaxCount || document.resources.size() > kMaxCount ||
        document.edges.size() > kMaxCount || document.present_connections.size() > kMaxCount)
        return {foundation::invalid_argument("Pipeline asset document is too large"), {}};
    for (const auto& source : document.present_connections)
        if (source.empty())
            return {foundation::invalid_argument("Pipeline asset Present connection is invalid"), {}};
    Writer writer;
    writer.bytes.insert(writer.bytes.end(), {static_cast<std::byte>('A'), static_cast<std::byte>('E'),
                                             static_cast<std::byte>('G'), static_cast<std::byte>('P')});
    writer.value(kVersion);
    writer.value(document.schema_version);
    writer.string(document.asset_uuid);
    writer.string(document.name);
    writer.value(static_cast<uint32_t>(document.module_requirements.size()));
    for (auto id : document.module_requirements)
        writer.id(id.value());
    writer.value(static_cast<uint32_t>(document.pipeline_nodes.size()));
    for (const auto& node : document.pipeline_nodes) {
        write_common(writer, node.node_id, node.version, node.parameters);
        writer.id(node.type.value());
        writer.value(static_cast<uint8_t>(node.scope));
        writer.string(node.owner_binding);
    }
    writer.value(static_cast<uint32_t>(document.feature_nodes.size()));
    for (const auto& node : document.feature_nodes) {
        write_common(writer, node.node_id, node.version, node.parameters);
        writer.id(node.type.value());
        writer.string(node.pipeline_node);
        writer.id(node.point.value());
        writer.value(static_cast<uint8_t>(node.enabled));
    }
    writer.value(static_cast<uint32_t>(document.pass_nodes.size()));
    for (const auto& node : document.pass_nodes) {
        write_common(writer, node.node_id, node.version, node.parameters);
        writer.id(node.type.value());
        writer.string(node.feature_node);
        writer.string(node.part_id);
        writer.value(static_cast<uint8_t>(node.enabled));
    }
    writer.value(static_cast<uint32_t>(document.resources.size()));
    for (const auto& node : document.resources) {
        write_common(writer, node.node_id, {1, 0, 0}, node.parameters);
        writer.id(node.resource_id.value());
        writer.id(node.type.value());
        writer.value(node.version);
        writer.value(static_cast<uint8_t>(node.owner_scope));
        writer.value(node.visible_to);
        writer.value(static_cast<uint8_t>(node.external_input));
        writer.value(static_cast<uint8_t>(node.storage));
        writer.value(static_cast<uint8_t>(node.lifetime));
        writer.value(static_cast<uint8_t>(node.ownership));
        writer.value(static_cast<uint8_t>(node.resource_constraints.resource_class));
        writer.value(static_cast<uint32_t>(node.resource_constraints.formats.size()));
        for (const auto& format : node.resource_constraints.formats)
            writer.string(format);
        writer.string(node.resource_constraints.size_rule);
        writer.value(node.resource_constraints.sample_count);
        writer.value(node.resource_constraints.capability_mask);
        writer.value(node.resource_constraints.usage_mask);
    }
    writer.value(static_cast<uint32_t>(document.edges.size()));
    for (const auto& edge : document.edges) {
        writer.string(edge.edge_id);
        writer.value(static_cast<uint8_t>(edge.kind));
        writer.string(edge.source.node_id);
        writer.id(edge.source.port_id);
        writer.string(edge.destination.node_id);
        writer.id(edge.destination.port_id);
        writer.value(edge.binding_id);
    }
    writer.value(static_cast<uint32_t>(document.present_connections.size()));
    for (const auto& source : document.present_connections)
        writer.string(source);
    return {foundation::Result::success(), std::move(writer.bytes)};
}

foundation::ResultValue<PipelineDocument> decode_pipeline_asset(std::span<const std::byte> bytes) {
    Reader reader{bytes};
    std::array<char, 4> magic{};
    for (char& item : magic) {
        std::byte byte{};
        if (!reader.value(byte))
            return {foundation::invalid_argument("Pipeline asset header is truncated"), {}};
        item = static_cast<char>(byte);
    }
    uint32_t version{}, schema{};
    if (magic != std::array<char, 4>{'A', 'E', 'G', 'P'} || !reader.value(version) ||
        (version != 1 && version != kVersion) || !reader.value(schema) || schema == 0)
        return {foundation::invalid_argument("Pipeline asset header is invalid"), {}};
    PipelineDocument document;
    document.schema_version = schema;
    if (!reader.string(document.asset_uuid) || !reader.string(document.name))
        return {foundation::invalid_argument("Pipeline asset identity is invalid"), {}};
    auto count = [&](uint32_t& output) { return reader.value(output) && output <= kMaxCount; };
    uint32_t size{};
    if (!count(size))
        return {foundation::invalid_argument("Pipeline asset module count is invalid"), {}};
    document.module_requirements.reserve(size);
    for (uint32_t i = 0; i < size; ++i) {
        uint64_t id{};
        if (!reader.id(id))
            return {foundation::invalid_argument("Pipeline asset module ID is invalid"), {}};
        document.module_requirements.emplace_back(id);
    }
    if (!count(size))
        return {foundation::invalid_argument("Pipeline asset Pipeline count is invalid"), {}};
    for (uint32_t i = 0; i < size; ++i) {
        PipelineNodeDocument node;
        if (!read_common(reader, node.node_id, node.version, node.parameters))
            return {foundation::invalid_argument("Pipeline asset Pipeline node is invalid"), {}};
        uint64_t id{};
        uint8_t scope{};
        if (!reader.id(id) || !reader.value(scope) || scope > 3 || !reader.string(node.owner_binding))
            return {foundation::invalid_argument("Pipeline asset Pipeline node is invalid"), {}};
        node.type = PipelineId(id);
        node.scope = static_cast<PipelineScope>(scope);
        document.pipeline_nodes.push_back(std::move(node));
    }
    if (!count(size))
        return {foundation::invalid_argument("Pipeline asset Feature count is invalid"), {}};
    for (uint32_t i = 0; i < size; ++i) {
        FeatureNodeDocument node;
        if (!read_common(reader, node.node_id, node.version, node.parameters))
            return {foundation::invalid_argument("Pipeline asset Feature node is invalid"), {}};
        uint64_t type{}, point{};
        uint8_t enabled{};
        if (!reader.id(type) || !reader.string(node.pipeline_node) || !reader.id(point) || !reader.value(enabled) ||
            enabled > 1)
            return {foundation::invalid_argument("Pipeline asset Feature node is invalid"), {}};
        node.type = FeatureId(type);
        node.point = FeaturePointId(point);
        node.enabled = enabled != 0;
        document.feature_nodes.push_back(std::move(node));
    }
    if (!count(size))
        return {foundation::invalid_argument("Pipeline asset Pass count is invalid"), {}};
    for (uint32_t i = 0; i < size; ++i) {
        PassNodeDocument node;
        if (!read_common(reader, node.node_id, node.version, node.parameters))
            return {foundation::invalid_argument("Pipeline asset Pass node is invalid"), {}};
        uint64_t type{};
        uint8_t enabled{};
        if (!reader.id(type) || !reader.string(node.feature_node) || !reader.string(node.part_id) ||
            !reader.value(enabled) || enabled > 1)
            return {foundation::invalid_argument("Pipeline asset Pass node is invalid"), {}};
        node.type = RenderPassTypeId(type);
        node.enabled = enabled != 0;
        document.pass_nodes.push_back(std::move(node));
    }
    if (!count(size))
        return {foundation::invalid_argument("Pipeline asset resource count is invalid"), {}};
    for (uint32_t i = 0; i < size; ++i) {
        ResourceNodeDocument node;
        Version ignored{};
        if (!read_common(reader, node.node_id, ignored, node.parameters))
            return {foundation::invalid_argument("Pipeline asset resource node is invalid"), {}};
        uint64_t resource{}, type{};
        uint8_t scope{}, external{}, storage{}, lifetime{}, ownership{}, klass{};
        if (!reader.id(resource) || !reader.id(type) || !reader.value(node.version) || !reader.value(scope) ||
            scope > 3 || !reader.value(node.visible_to) || !reader.value(external) || external > 1 ||
            !reader.value(storage) || storage > 6 || !reader.value(lifetime) || lifetime > 5 ||
            !reader.value(ownership) || ownership > 4 || !reader.value(klass) || klass > 2)
            return {foundation::invalid_argument("Pipeline asset resource node is invalid"), {}};
        node.resource_id = RenderResourceId(resource);
        node.type = ArtifactTypeId(type);
        node.owner_scope = static_cast<PipelineScope>(scope);
        node.external_input = external != 0;
        node.storage = static_cast<ArtifactStorage>(storage);
        node.lifetime = static_cast<ArtifactLifetime>(lifetime);
        node.ownership = static_cast<OwnershipPolicy>(ownership);
        node.resource_constraints.resource_class = static_cast<ResourceClass>(klass);
        uint32_t formats{};
        if (!count(formats))
            return {foundation::invalid_argument("Pipeline asset resource formats are invalid"), {}};
        for (uint32_t f = 0; f < formats; ++f) {
            std::string format;
            if (!reader.string(format))
                return {foundation::invalid_argument("Pipeline asset resource format is invalid"), {}};
            node.resource_constraints.formats.push_back(std::move(format));
        }
        if (!reader.string(node.resource_constraints.size_rule) ||
            !reader.value(node.resource_constraints.sample_count) ||
            !reader.value(node.resource_constraints.capability_mask) ||
            !reader.value(node.resource_constraints.usage_mask))
            return {foundation::invalid_argument("Pipeline asset resource constraints are invalid"), {}};
        document.resources.push_back(std::move(node));
    }
    if (!count(size))
        return {foundation::invalid_argument("Pipeline asset edge count is invalid"), {}};
    for (uint32_t i = 0; i < size; ++i) {
        DocumentEdge edge;
        uint8_t kind{};
        if (!reader.string(edge.edge_id) || !reader.value(kind) || kind > 3 || !reader.string(edge.source.node_id) ||
            !reader.value(edge.source.port_id) || !reader.string(edge.destination.node_id) ||
            !reader.value(edge.destination.port_id) || !reader.value(edge.binding_id))
            return {foundation::invalid_argument("Pipeline asset edge is invalid"), {}};
        edge.kind = static_cast<DocumentEdgeKind>(kind);
        if (edge.kind != DocumentEdgeKind::kOrdering &&
            (edge.source.port_id == 0 || edge.destination.port_id == 0 || edge.binding_id == 0))
            return {foundation::invalid_argument("Pipeline asset data edge binding is invalid"), {}};
        document.edges.push_back(std::move(edge));
    }
    if (version >= 2) {
        if (!count(size))
            return {foundation::invalid_argument("Pipeline asset Present connection count is invalid"), {}};
        document.present_connections.reserve(size);
        for (uint32_t i = 0; i < size; ++i) {
            std::string source;
            if (!reader.string(source) || source.empty())
                return {foundation::invalid_argument("Pipeline asset Present connection is invalid"), {}};
            document.present_connections.push_back(std::move(source));
        }
    }
    if (reader.offset != reader.bytes.size() || !unique_node(document))
        return {foundation::invalid_argument("Pipeline asset contains duplicate or trailing data"), {}};
    return {foundation::Result::success(), std::move(document)};
}

}  // namespace aeg::asset
