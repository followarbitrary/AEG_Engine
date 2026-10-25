#include "feature_graph_io.hpp"
#include "feature_graph_source_storage.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <toml.hpp>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace aeg::application::authoring {
using namespace aeg::feature;
namespace {

foundation::Result invalid(std::string message) {
    return foundation::Result::failure(foundation::ResultCode::kInvalidArgument, std::move(message));
}

std::string path_text(const std::filesystem::path& file) {
    const auto text = file.generic_u8string();
    return {text.begin(), text.end()};
}

void require_known_fields(const toml::table& table, std::initializer_list<std::string_view> allowed,
                          std::string_view owner) {
    for (const auto& [key, value] : table) {
        (void)value;
        bool known = false;
        for (const auto name : allowed) if (key == name) { known = true; break; }
        if (!known) throw std::invalid_argument("unknown " + std::string(owner) + " field: " + key +
            "; source was preserved and must be migrated explicitly");
    }
}

uint64_t read_u64(const toml::value& value) {
    if (value.is_string()) {
        const auto& text = value.as_string();
        if (text.empty()) {
            throw std::invalid_argument("unsigned FeatureGraph value is empty");
        }
        for (const char character : text) {
            if (character < '0' || character > '9') {
                throw std::invalid_argument("unsigned FeatureGraph value is not a decimal integer");
            }
        }
        std::size_t consumed = 0;
        const auto parsed = std::stoull(text, &consumed, 10);
        if (consumed != text.size()) {
            throw std::invalid_argument("unsigned FeatureGraph value has trailing characters");
        }
        return static_cast<uint64_t>(parsed);
    }
    if (!value.is_integer()) {
        throw std::invalid_argument("unsigned FeatureGraph value must be an integer or decimal string");
    }
    const auto number = value.as_integer();
    if (number < 0) {
        throw std::invalid_argument("unsigned FeatureGraph value is negative");
    }
    return static_cast<uint64_t>(number);
}

uint32_t read_u32(const toml::value& value, std::string_view field_name) {
    const auto number = read_u64(value);
    if (number > std::numeric_limits<uint32_t>::max()) {
        throw std::invalid_argument("FeatureGraph " + std::string(field_name) + " is out of range");
    }
    return static_cast<uint32_t>(number);
}

EdgeKind read_edge_kind(const toml::value& value) {
    if (!value.is_integer()) {
        throw std::invalid_argument("FeatureGraph edge kind must be an integer");
    }
    const auto number = value.as_integer();
    if (number < 0 || number > static_cast<int64_t>(EdgeKind::kReceipt)) {
        throw std::invalid_argument("FeatureGraph edge kind is invalid");
    }
    return static_cast<EdgeKind>(number);
}

template <typename Id>
Id read_id(const toml::value& value) {
    return Id(read_u64(value));
}

toml::value encode_u64(uint64_t value) {
    if (value > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())) {
        return std::to_string(value);
    }
    return static_cast<int64_t>(value);
}

toml::array encode_version(const Version& version) {
    return toml::array{static_cast<int64_t>(version.major), static_cast<int64_t>(version.minor),
                       static_cast<int64_t>(version.patch)};
}

Version read_version(const toml::value& value) {
    const auto& values = value.as_array();
    if (values.size() != 3) {
        throw std::invalid_argument("FeatureGraph version must have three components");
    }
    const auto read_component = [](const toml::value& component) {
        const auto number = component.as_integer();
        if (number < 0 || number > std::numeric_limits<uint16_t>::max()) {
            throw std::invalid_argument("FeatureGraph version component is out of range");
        }
        return static_cast<uint16_t>(number);
    };
    return {read_component(values[0]), read_component(values[1]), read_component(values[2])};
}

std::vector<std::string> read_strings(const toml::value& value) {
    std::vector<std::string> result;
    for (const auto& item : value.as_array()) {
        result.push_back(item.as_string());
    }
    return result;
}

toml::array encode_strings(const std::vector<std::string>& values) {
    toml::array result;
    for (const auto& value : values) {
        result.emplace_back(value);
    }
    return result;
}

std::vector<uint8_t> read_blob(const toml::value& value) {
    std::vector<uint8_t> result;
    for (const auto& item : value.as_array()) {
        const auto number = item.as_integer();
        if (number < 0 || number > std::numeric_limits<uint8_t>::max()) {
            throw std::invalid_argument("FeatureGraph parameter byte is out of range");
        }
        result.push_back(static_cast<uint8_t>(number));
    }
    return result;
}

bool known_root_key(std::string_view key) {
    static constexpr std::string_view keys[] = {
        "schema_version", "graph_version", "draft_revision", "layout_revision", "asset_uuid",
        "module_requirements", "nodes", "edges", "process_subgraphs", "input_bindings",
        "variadic_slots", "extensions", "contract_digest", "semantic_digest"};
    for (const auto known : keys) {
        if (key == known) {
            return true;
        }
    }
    return false;
}

}  // namespace

static foundation::ResultValue<FeatureGraphDocument> decode_graph_source(
    const std::filesystem::path& file, std::string_view bytes) {
    try {
        if (const auto valid = detail::validate_source_integrity(bytes); !valid) return {valid, {}};
        std::istringstream input{std::string(bytes)};
        const auto root = toml::parse(input, path_text(file));
        for (const auto& [key, value] : root.as_table()) {
            if (!known_root_key(key)) {
                return {invalid("FeatureGraph document contains an unknown root key: " + key), {}};
            }
        }
        FeatureGraphDocument graph{};
        graph.schema_version = read_u32(root.at("schema_version"), "schema_version");
        graph.graph_version = read_u64(root.at("graph_version"));
        graph.draft_revision = root.contains("draft_revision") ? read_u64(root.at("draft_revision")) : 1;
        graph.layout_revision = root.contains("layout_revision") ? read_u64(root.at("layout_revision")) : 1;
        graph.asset_uuid = root.at("asset_uuid").as_string();
        if (root.contains("module_requirements")) {
            graph.module_requirements = read_strings(root.at("module_requirements"));
        }
        if (root.contains("nodes")) {
            for (const auto& value : root.at("nodes").as_array()) {
                const auto& item = value.as_table();
                require_known_fields(item, {"node_id", "definition_id", "version", "owner_path", "parameters"}, "node");
                FeatureNode node{};
                node.node_id = read_id<FeatureNodeId>(item.at("node_id"));
                node.definition_id = read_id<FeatureDefinitionId>(item.at("definition_id"));
                node.definition_version = read_version(item.at("version"));
                if (item.contains("owner_path")) {
                    for (const auto& path : item.at("owner_path").as_array()) {
                        node.owner_path.nodes.push_back(read_id<FeatureNodeId>(path));
                    }
                }
                if (item.contains("parameters")) {
                    node.parameter_blob = read_blob(item.at("parameters"));
                }
                graph.nodes.push_back(std::move(node));
            }
        }
        if (root.contains("edges")) {
            for (const auto& value : root.at("edges").as_array()) {
                const auto& item = value.as_table();
                require_known_fields(item, {"source_node", "source_port", "target_node", "target_port", "kind",
                    "source_clock", "target_clock", "version", "capacity", "deadline_ticks"}, "edge");
                FeatureEdge edge{};
                edge.source_node = read_id<FeatureNodeId>(item.at("source_node"));
                edge.source_port = read_id<PortId>(item.at("source_port"));
                edge.target_node = read_id<FeatureNodeId>(item.at("target_node"));
                edge.target_port = read_id<PortId>(item.at("target_port"));
                edge.kind = read_edge_kind(item.at("kind"));
                edge.source_clock = read_id<ClockId>(item.at("source_clock"));
                edge.target_clock = read_id<ClockId>(item.at("target_clock"));
                edge.version = read_u32(item.at("version"), "edge version");
                edge.capacity = read_u32(item.at("capacity"), "edge capacity");
                edge.deadline_ticks = read_u32(item.at("deadline_ticks"), "edge deadline_ticks");
                graph.edges.push_back(edge);
            }
        }
        if (root.contains("process_subgraphs")) {
            for (const auto& value : root.at("process_subgraphs").as_array()) {
                const auto& item = value.as_table();
                require_known_fields(item, {"owner_node", "states", "transitions", "state_schema_version",
                    "initial_state", "terminal_states"}, "Process");
                ProcessSubgraph process{};
                process.owner_node = read_id<FeatureNodeId>(item.at("owner_node"));
                process.states = read_strings(item.at("states"));
                process.transitions = read_strings(item.at("transitions"));
                process.state_schema_version = read_u32(item.at("state_schema_version"),
                                                        "process state_schema_version");
                if (item.contains("initial_state")) process.initial_state = item.at("initial_state").as_string();
                if (item.contains("terminal_states")) process.terminal_states = read_strings(item.at("terminal_states"));
                graph.process_subgraphs.push_back(std::move(process));
            }
        }
        if (root.contains("input_bindings")) graph.input_bindings = read_strings(root.at("input_bindings"));
        if (root.contains("variadic_slots")) graph.variadic_slots = read_strings(root.at("variadic_slots"));
        if (root.contains("extensions")) graph.extensions = read_strings(root.at("extensions"));
        if (root.contains("contract_digest")) graph.contract_digest = read_u64(root.at("contract_digest"));
        if (graph.asset_uuid.empty() || graph.schema_version == 0 || graph.graph_version == 0) {
            return {invalid("FeatureGraph identity and schema version are required"), {}};
        }
        if (graph.schema_version != 1)
            return {invalid("Unsupported FeatureGraph source schema; preserve the source and migrate explicitly"), {}};
        if (root.contains("semantic_digest") && read_u64(root.at("semantic_digest")) != graph_semantic_digest(graph))
            return {invalid("FeatureGraph semantic digest does not match its author data"), {}};
        return {foundation::Result::success(), std::move(graph)};
    } catch (const std::exception& exception) {
        return {invalid(std::string("FeatureGraph document is invalid: ") + exception.what()), {}};
    }
}

foundation::ResultValue<FeatureGraphSourceSnapshot> load_feature_graph_source(
    const std::filesystem::path& file) {
    auto revision = inspect_feature_graph_file_revision(file);
    if (!revision) return {revision.result, {}};
    if (!revision.value.exists) return {foundation::not_found("FeatureGraph source is missing: " + path_text(file)), {}};
    auto decoded = decode_graph_source(file, revision.value.contents);
    if (!decoded) return {decoded.result, {}};
    return {foundation::Result::success(), {std::move(decoded.value), std::move(revision.value)}};
}

foundation::ResultValue<FeatureGraphDocument> load_feature_graph_document(const std::filesystem::path& file) {
    auto source = load_feature_graph_source(file);
    if (!source) return {source.result, {}};
    return {foundation::Result::success(), std::move(source.value.document)};
}

static foundation::ResultValue<FeatureGraphFileRevision> save_graph_source(const std::filesystem::path& file,
    const FeatureGraphDocument& graph, const std::optional<FeatureGraphFileRevision>& expected) {
    if (graph.asset_uuid.empty() || graph.schema_version == 0 || graph.graph_version == 0) {
        return {invalid("FeatureGraph identity and schema version are required"), {}};
    }
    if (graph.schema_version != 1)
        return {invalid("Unsupported FeatureGraph source schema; preserve the source and migrate explicitly"), {}};
    toml::table root;
    root["schema_version"] = static_cast<int64_t>(graph.schema_version);
    root["graph_version"] = encode_u64(graph.graph_version);
    root["draft_revision"] = encode_u64(graph.draft_revision);
    root["layout_revision"] = encode_u64(graph.layout_revision);
    root["asset_uuid"] = graph.asset_uuid;
    root["module_requirements"] = encode_strings(graph.module_requirements);
    toml::array nodes;
    for (const auto& node : graph.nodes) {
        toml::table item;
        item["node_id"] = encode_u64(node.node_id.value());
        item["definition_id"] = encode_u64(node.definition_id.value());
        item["version"] = encode_version(node.definition_version);
        toml::array owner_path;
        for (const auto path : node.owner_path.nodes) owner_path.emplace_back(encode_u64(path.value()));
        item["owner_path"] = std::move(owner_path);
        toml::array parameters;
        for (const auto parameter : node.parameter_blob) parameters.emplace_back(static_cast<int64_t>(parameter));
        item["parameters"] = std::move(parameters);
        nodes.emplace_back(std::move(item));
    }
    root["nodes"] = std::move(nodes);
    toml::array edges;
    for (const auto& edge : graph.edges) {
        toml::table item;
        item["source_node"] = encode_u64(edge.source_node.value());
        item["source_port"] = encode_u64(edge.source_port.value());
        item["target_node"] = encode_u64(edge.target_node.value());
        item["target_port"] = encode_u64(edge.target_port.value());
        item["kind"] = static_cast<int64_t>(edge.kind);
        item["source_clock"] = encode_u64(edge.source_clock.value());
        item["target_clock"] = encode_u64(edge.target_clock.value());
        item["version"] = static_cast<int64_t>(edge.version);
        item["capacity"] = static_cast<int64_t>(edge.capacity);
        item["deadline_ticks"] = static_cast<int64_t>(edge.deadline_ticks);
        edges.emplace_back(std::move(item));
    }
    root["edges"] = std::move(edges);
    toml::array processes;
    for (const auto& process : graph.process_subgraphs) {
        toml::table item;
        item["owner_node"] = encode_u64(process.owner_node.value());
        item["states"] = encode_strings(process.states);
        item["transitions"] = encode_strings(process.transitions);
        item["state_schema_version"] = static_cast<int64_t>(process.state_schema_version);
        item["initial_state"] = process.initial_state;
        item["terminal_states"] = encode_strings(process.terminal_states);
        processes.emplace_back(std::move(item));
    }
    root["process_subgraphs"] = std::move(processes);
    root["input_bindings"] = encode_strings(graph.input_bindings);
    root["variadic_slots"] = encode_strings(graph.variadic_slots);
    root["extensions"] = encode_strings(graph.extensions);
    root["contract_digest"] = encode_u64(graph.contract_digest);
    root["semantic_digest"] = encode_u64(graph_semantic_digest(graph));
    FeatureGraphFileRevision revision{true,
        detail::protect_source_integrity(toml::format(toml::value(std::move(root))))};
    const auto published = detail::publish_feature_graph_source(file, revision.contents, expected,
        [](const auto& path) { return load_feature_graph_document(path).result; });
    if (!published) return {published, {}};
    static_assert(std::is_nothrow_move_constructible_v<FeatureGraphFileRevision>);
    return {foundation::Result::success(), std::move(revision)};
}

foundation::ResultValue<FeatureGraphFileRevision> save_feature_graph_source(const std::filesystem::path& file,
    const FeatureGraphDocument& graph, const FeatureGraphFileRevision* expected) {
    return save_graph_source(file, graph, expected ? std::optional{*expected} : std::nullopt);
}

foundation::Result save_feature_graph_document(const std::filesystem::path& file,
                                               const FeatureGraphDocument& graph) {
    return save_graph_source(file, graph, std::nullopt).result;
}

foundation::Result save_feature_graph_document(const std::filesystem::path& file,
    const FeatureGraphDocument& graph, const FeatureGraphFileRevision& expected) {
    return save_graph_source(file, graph, expected).result;
}

foundation::Result recover_feature_graph_document(const std::filesystem::path& file,
    const FeatureGraphFileRevision& expected, const FeatureGraphFileRevision* expected_backup) {
    auto backup = file;
    backup += ".bak";
    const auto source = load_feature_graph_source(backup);
    if (!source) return source.result;
    if (expected_backup && source.value.revision != *expected_backup)
        return foundation::Result::failure(foundation::ResultCode::kOutOfDate,
            "FeatureGraph backup changed after inspection; inspect it again before restoring");
    return detail::publish_feature_graph_source(file, source.value.revision.contents, expected,
        [](const auto& path) { return load_feature_graph_document(path).result; }, false);
}

}  // namespace aeg::application::authoring
