#include "pipeline_document_io.hpp"

#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <toml.hpp>

#include "pipeline_authoring.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace aeg::application::project {
namespace {
using namespace pipeline;

uint64_t read_u64(const toml::value& value);

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

toml::array encode_parameters(const ParameterBlock& parameters) {
    toml::array encoded;
    for (const auto& [id, value] : parameters) {
        toml::table item;
        item["id"] = encode_u64(id);
        if (const auto* bool_value = std::get_if<bool>(&value)) {
            item["bool"] = *bool_value;
        } else if (const auto* int_value = std::get_if<int64_t>(&value)) {
            item["int"] = *int_value;
        } else if (const auto* uint_value = std::get_if<uint64_t>(&value)) {
            item["uint"] = encode_u64(*uint_value);
        } else if (const auto* float_value = std::get_if<double>(&value)) {
            item["float"] = *float_value;
        } else if (const auto* string_value = std::get_if<std::string>(&value)) {
            item["string"] = *string_value;
        } else if (const auto* vector_value = std::get_if<std::vector<double>>(&value)) {
            item["vector"] = *vector_value;
        } else if (const auto* asset_value = std::get_if<AssetReference>(&value)) {
            item["asset"] = asset_value->uuid;
        }
        encoded.emplace_back(std::move(item));
    }
    return encoded;
}

uint64_t read_u64(const toml::value& value);
double read_number(const toml::value& value);

ParameterBlock decode_parameters(const toml::value& node) {
    ParameterBlock result;
    if (!node.contains("parameters") || !node.at("parameters").is_array())
        return result;
    for (const auto& item : node.at("parameters").as_array()) {
        if (!item.is_table() || !item.contains("id"))
            continue;
        const uint64_t id = read_u64(item.at("id"));
        if (item.contains("bool"))
            result[id] = item.at("bool").as_boolean();
        else if (item.contains("int"))
            result[id] = item.at("int").as_integer();
        else if (item.contains("uint"))
            result[id] = read_u64(item.at("uint"));
        else if (item.contains("float"))
            result[id] = read_number(item.at("float"));
        else if (item.contains("string"))
            result[id] = item.at("string").as_string();
        else if (item.contains("vector")) {
            std::vector<double> vector;
            for (const auto& value : item.at("vector").as_array())
                vector.push_back(read_number(value));
            result[id] = std::move(vector);
        } else if (item.contains("asset"))
            result[id] = AssetReference{item.at("asset").as_string()};
    }
    return result;
}

toml::array encode_version(const Version& version) {
    return toml::array{static_cast<int64_t>(version.major), static_cast<int64_t>(version.minor),
                       static_cast<int64_t>(version.patch)};
}

Version decode_version(const toml::value& value) {
    const auto& values = value.as_array();
    return {static_cast<uint32_t>(values.at(0).as_integer()), static_cast<uint32_t>(values.at(1).as_integer()),
            static_cast<uint32_t>(values.at(2).as_integer())};
}

template <typename Id>
toml::value encode_id(Id id) {
    return encode_u64(id.value());
}

void encode_common(toml::table& item, std::string_view node_id, Version version, const ParameterBlock& parameters) {
    item["node_id"] = std::string(node_id);
    item["version"] = encode_version(version);
    item["parameters"] = encode_parameters(parameters);
}

uint64_t read_u64(const toml::value& value) {
    if (value.is_string())
        return std::stoull(value.as_string());
    const auto integer = value.as_integer();
    if (integer < 0)
        throw std::invalid_argument("unsigned Pipeline document value is negative");
    return static_cast<uint64_t>(integer);
}

bool known_root_key(std::string_view key) {
    static constexpr std::string_view kEys[] = {
        "schema_version", "graph_version", "draft_revision", "layout_revision", "asset_uuid", "name",
        "module_requirements", "pipeline_nodes", "feature_nodes",
        "pass_nodes",     "resources",  "edges", "input_bindings", "variadic_slots", "present_connections",
        "layouts",        "extensions"};
    for (const auto known : kEys)
        if (key == known)
            return true;
    return false;
}

double read_number(const toml::value& value) {
    return value.is_floating() ? value.as_floating() : static_cast<double>(value.as_integer());
}

void encode_resource_fields(toml::table& item, const ResourceNodeDocument& node) {
    item["resource_id"] = encode_id(node.resource_id);
    item["artifact_type"] = encode_id(node.type);
    item["resource_version"] = static_cast<int64_t>(node.version);
    item["owner_scope"] = static_cast<int64_t>(node.owner_scope);
    item["visible_to"] = static_cast<int64_t>(node.visible_to);
    item["external_input"] = node.external_input;
    item["storage"] = static_cast<int64_t>(node.storage);
    item["lifetime"] = static_cast<int64_t>(node.lifetime);
    item["ownership"] = static_cast<int64_t>(node.ownership);
    item["resource_class"] = static_cast<int64_t>(node.resource_constraints.resource_class);
    item["formats"] = node.resource_constraints.formats;
    item["size_rule"] = node.resource_constraints.size_rule;
    item["sample_count"] = static_cast<int64_t>(node.resource_constraints.sample_count);
    item["capability_mask"] = encode_u64(node.resource_constraints.capability_mask);
    item["usage_mask"] = encode_u64(node.resource_constraints.usage_mask);
}

}  // namespace

foundation::ResultValue<PipelineDocument> load_pipeline_document(const std::filesystem::path& file) {
    try {
        const toml::value root = toml::parse(file);
        PipelineDocument document;
        document.schema_version = static_cast<uint32_t>(toml::find<int64_t>(root, "schema_version"));
        document.graph_version = root.contains("graph_version") ? read_u64(root.at("graph_version")) : 1;
        document.draft_revision = root.contains("draft_revision") ? read_u64(root.at("draft_revision")) : 1;
        document.layout_revision = root.contains("layout_revision") ? read_u64(root.at("layout_revision")) : 1;
        document.asset_uuid = toml::find<std::string>(root, "asset_uuid");
        document.name = toml::find<std::string>(root, "name");
        if (root.contains("module_requirements"))
            for (const auto& value : root.at("module_requirements").as_array())
                document.module_requirements.emplace_back(read_id<ModuleId>(value));
        if (root.contains("pipeline_nodes"))
            for (const auto& value : root.at("pipeline_nodes").as_array()) {
                const auto& item = value.as_table();
                PipelineNodeDocument node;
                node.node_id = item.at("node_id").as_string();
                node.type = read_id<PipelineId>(item.at("type"));
                node.version = decode_version(item.at("version"));
                node.scope = static_cast<PipelineScope>(item.at("scope").as_integer());
                node.owner_binding =
                    item.contains("owner_binding") ? item.at("owner_binding").as_string() : std::string{};
                node.parameters = decode_parameters(value);
                document.pipeline_nodes.push_back(std::move(node));
            }
        if (root.contains("feature_nodes"))
            for (const auto& value : root.at("feature_nodes").as_array()) {
                const auto& item = value.as_table();
                FeatureNodeDocument node;
                node.node_id = item.at("node_id").as_string();
                node.type = read_id<FeatureId>(item.at("type"));
                node.version = decode_version(item.at("version"));
                node.pipeline_node = item.at("pipeline_node").as_string();
                node.point = read_id<FeaturePointId>(item.at("point"));
                node.enabled = item.contains("enabled") ? item.at("enabled").as_boolean() : true;
                node.parameters = decode_parameters(value);
                document.feature_nodes.push_back(std::move(node));
            }
        if (root.contains("pass_nodes"))
            for (const auto& value : root.at("pass_nodes").as_array()) {
                const auto& item = value.as_table();
                PassNodeDocument node;
                node.node_id = item.at("node_id").as_string();
                node.type = read_id<RenderPassTypeId>(item.at("type"));
                node.version = decode_version(item.at("version"));
                node.feature_node = item.at("feature_node").as_string();
                node.part_id = item.at("part_id").as_string();
                node.enabled = item.contains("enabled") ? item.at("enabled").as_boolean() : true;
                node.parameters = decode_parameters(value);
                document.pass_nodes.push_back(std::move(node));
            }
        if (root.contains("resources"))
            for (const auto& value : root.at("resources").as_array()) {
                const auto& item = value.as_table();
                ResourceNodeDocument node;
                node.node_id = item.at("node_id").as_string();
                node.resource_id = read_id<RenderResourceId>(item.at("resource_id"));
                node.type = read_id<ArtifactTypeId>(item.at("artifact_type"));
                node.version = static_cast<uint32_t>(item.at("resource_version").as_integer());
                node.owner_scope = static_cast<PipelineScope>(item.at("owner_scope").as_integer());
                node.visible_to = static_cast<PipelineScopeMask>(item.at("visible_to").as_integer());
                node.external_input = item.at("external_input").as_boolean();
                node.storage = static_cast<ArtifactStorage>(item.at("storage").as_integer());
                node.lifetime = static_cast<ArtifactLifetime>(item.at("lifetime").as_integer());
                node.ownership = static_cast<OwnershipPolicy>(item.at("ownership").as_integer());
                node.resource_constraints.resource_class =
                    static_cast<ResourceClass>(item.at("resource_class").as_integer());
                if (item.contains("formats") && item.at("formats").is_array())
                    for (const auto& format : item.at("formats").as_array())
                        node.resource_constraints.formats.push_back(format.as_string());
                node.resource_constraints.size_rule = item.at("size_rule").as_string();
                node.resource_constraints.sample_count = static_cast<uint32_t>(item.at("sample_count").as_integer());
                node.resource_constraints.capability_mask = read_u64(item.at("capability_mask"));
                node.resource_constraints.usage_mask = read_u64(item.at("usage_mask"));
                node.parameters = decode_parameters(value);
                document.resources.push_back(std::move(node));
            }
        if (root.contains("edges"))
            for (const auto& value : root.at("edges").as_array()) {
                const auto& item = value.as_table();
                DocumentEdge edge;
                edge.edge_id = item.at("edge_id").as_string();
                edge.kind = static_cast<DocumentEdgeKind>(item.at("kind").as_integer());
                edge.source = {item.at("source_node").as_string(), read_u64(item.at("source_port")),
                               item.contains("source_slot") ? read_u64(item.at("source_slot")) : 0};
                edge.destination = {item.at("destination_node").as_string(), read_u64(item.at("destination_port")),
                                    item.contains("destination_slot") ? read_u64(item.at("destination_slot")) : 0};
                edge.binding_id = read_u64(item.at("binding_id"));
                document.edges.push_back(std::move(edge));
            }
        if (root.contains("input_bindings"))
            for (const auto& value : root.at("input_bindings").as_array()) {
                const auto& item = value.as_table();
                InputBindingDocument binding;
                binding.node_id = item.at("node_id").as_string();
                binding.port_id = read_u64(item.at("port_id"));
                binding.slot_id = item.contains("slot_id") ? read_u64(item.at("slot_id")) : 0;
                binding.source = static_cast<InputSourceKind>(item.at("source").as_integer());
                binding.status = item.contains("status")
                                     ? static_cast<InputBindingStatus>(item.at("status").as_integer())
                                     : InputBindingStatus::kUnset;
                if (item.contains("edge_ids") && item.at("edge_ids").is_array())
                    for (const auto& edge_id : item.at("edge_ids").as_array())
                        binding.edge_ids.push_back(edge_id.as_string());
                if (item.contains("asset_uuid"))
                    binding.asset.uuid = item.at("asset_uuid").as_string();
                if (item.contains("product_id"))
                    binding.product_id = item.at("product_id").as_string();
                if (item.contains("target_variant"))
                    binding.target_variant = item.at("target_variant").as_string();
                if (item.contains("content_revision"))
                    binding.content_revision = read_u64(item.at("content_revision"));
                if (item.contains("default_id"))
                    binding.default_id = read_u64(item.at("default_id"));
                if (item.contains("default_revision"))
                    binding.default_revision = read_u64(item.at("default_revision"));
                if (item.contains("context_key"))
                    binding.context_key = item.at("context_key").as_string();
                if (item.contains("literal")) {
                    toml::value wrapper(toml::table{});
                    wrapper["parameters"] = item.at("literal");
                    const auto literal = decode_parameters(wrapper);
                    if (!literal.empty())
                        binding.literal = literal.begin()->second;
                }
                document.input_bindings.push_back(std::move(binding));
            }
        if (root.contains("variadic_slots"))
            for (const auto& value : root.at("variadic_slots").as_array()) {
                const auto& item = value.as_table();
                VariadicInputSlotDocument slot;
                slot.node_id = item.at("node_id").as_string();
                slot.group_id = read_u64(item.at("group_id"));
                slot.slot_id = read_u64(item.at("slot_id"));
                slot.order = static_cast<uint32_t>(item.at("order").as_integer());
                slot.parameters = decode_parameters(value);
                document.variadic_slots.push_back(std::move(slot));
            }
        if (root.contains("present_connections"))
            for (const auto& value : root.at("present_connections").as_array())
                document.present_connections.push_back(value.as_string());
        if (root.contains("layouts"))
            for (const auto& value : root.at("layouts").as_array()) {
                const auto& item = value.as_table();
                document.layouts.push_back({item.at("node_id").as_string(), item.at("x").as_floating(),
                                            item.at("y").as_floating(),
                                            item.contains("expanded") ? item.at("expanded").as_boolean() : true});
            }
        if (root.contains("extensions"))
            for (const auto& value : root.at("extensions").as_array()) {
                const auto& item = value.as_table();
                document.extensions.push_back({item.at("owner_node").as_string(),
                                               static_cast<OpaqueAuthoringEncoding>(item.at("encoding").as_integer()),
                                               item.at("payload").as_string()});
            }
        // Retain unknown root values as opaque authoring records so newer editor
        // fields survive an older editor's load/save cycle.
        for (const auto& [key, value] : root.as_table()) {
            if (!known_root_key(key))
                document.extensions.push_back({"@root:" + key, OpaqueAuthoringEncoding::kToml, toml::format(value)});
        }
        if (const auto diagnostics = validate_pipeline_document(document, TypeCatalog{}); !diagnostics.empty()) {
            // Type-dependent validation is performed by the caller; reject only malformed basic identity here.
            for (const auto& diagnostic : diagnostics)
                if (diagnostic.code == "document.schema" || diagnostic.code == "document.asset_uuid" ||
                    diagnostic.code == "document.name")
                    return {foundation::invalid_argument(diagnostic.message), {}};
        }
        return {foundation::Result::success(), std::move(document)};
    } catch (const std::exception& exception) {
        return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument, exception.what()), {}};
    }
}

foundation::Result save_pipeline_document(const std::filesystem::path& file, const PipelineDocument& document) {
    if (file.empty() || file.filename().empty())
        return foundation::invalid_argument("Pipeline document path is invalid");
    try {
        toml::value root(toml::table{});
        root["schema_version"] = static_cast<int64_t>(document.schema_version);
        root["graph_version"] = encode_u64(document.graph_version);
        root["draft_revision"] = encode_u64(document.draft_revision);
        root["layout_revision"] = encode_u64(document.layout_revision);
        root["asset_uuid"] = document.asset_uuid;
        root["name"] = document.name;
        toml::array modules;
        for (const auto id : document.module_requirements)
            modules.emplace_back(encode_id(id));
        root["module_requirements"] = std::move(modules);
        toml::array pipelines;
        for (const auto& node : document.pipeline_nodes) {
            toml::table item;
            item["type"] = encode_id(node.type);
            item["scope"] = static_cast<int64_t>(node.scope);
            item["owner_binding"] = node.owner_binding;
            encode_common(item, node.node_id, node.version, node.parameters);
            pipelines.emplace_back(std::move(item));
        }
        root["pipeline_nodes"] = std::move(pipelines);
        toml::array features;
        for (const auto& node : document.feature_nodes) {
            toml::table item;
            item["type"] = encode_id(node.type);
            item["pipeline_node"] = node.pipeline_node;
            item["point"] = encode_id(node.point);
            item["enabled"] = node.enabled;
            encode_common(item, node.node_id, node.version, node.parameters);
            features.emplace_back(std::move(item));
        }
        root["feature_nodes"] = std::move(features);
        toml::array passes;
        for (const auto& node : document.pass_nodes) {
            toml::table item;
            item["type"] = encode_id(node.type);
            item["feature_node"] = node.feature_node;
            item["part_id"] = node.part_id;
            item["enabled"] = node.enabled;
            encode_common(item, node.node_id, node.version, node.parameters);
            passes.emplace_back(std::move(item));
        }
        root["pass_nodes"] = std::move(passes);
        toml::array resources;
        for (const auto& node : document.resources) {
            toml::table item;
            encode_common(item, node.node_id, {1, 0, 0}, node.parameters);
            encode_resource_fields(item, node);
            resources.emplace_back(std::move(item));
        }
        root["resources"] = std::move(resources);
        toml::array edges;
        for (const auto& edge : document.edges) {
            toml::table item;
            item["edge_id"] = edge.edge_id;
            item["kind"] = static_cast<int64_t>(edge.kind);
            item["source_node"] = edge.source.node_id;
            item["source_port"] = encode_u64(edge.source.port_id);
            item["source_slot"] = encode_u64(edge.source.slot_id);
            item["destination_node"] = edge.destination.node_id;
            item["destination_port"] = encode_u64(edge.destination.port_id);
            item["destination_slot"] = encode_u64(edge.destination.slot_id);
            item["binding_id"] = encode_u64(edge.binding_id);
            edges.emplace_back(std::move(item));
        }
        root["edges"] = std::move(edges);
        toml::array input_bindings;
        for (const auto& binding : document.input_bindings) {
            toml::table item;
            item["node_id"] = binding.node_id;
            item["port_id"] = encode_u64(binding.port_id);
            item["slot_id"] = encode_u64(binding.slot_id);
            item["source"] = static_cast<int64_t>(binding.source);
            item["status"] = static_cast<int64_t>(binding.status);
            toml::array edge_ids;
            for (const auto& edge_id : binding.edge_ids)
                edge_ids.emplace_back(edge_id);
            item["edge_ids"] = std::move(edge_ids);
            item["asset_uuid"] = binding.asset.uuid;
            item["product_id"] = binding.product_id;
            item["target_variant"] = binding.target_variant;
            item["content_revision"] = encode_u64(binding.content_revision);
            item["default_id"] = encode_u64(binding.default_id);
            item["default_revision"] = encode_u64(binding.default_revision);
            item["literal"] = encode_parameters({{binding.port_id, binding.literal}});
            item["context_key"] = binding.context_key;
            input_bindings.emplace_back(std::move(item));
        }
        root["input_bindings"] = std::move(input_bindings);
        toml::array variadic_slots;
        for (const auto& slot : document.variadic_slots) {
            toml::table item;
            item["node_id"] = slot.node_id;
            item["group_id"] = encode_u64(slot.group_id);
            item["slot_id"] = encode_u64(slot.slot_id);
            item["order"] = static_cast<int64_t>(slot.order);
            item["parameters"] = encode_parameters(slot.parameters);
            variadic_slots.emplace_back(std::move(item));
        }
        root["variadic_slots"] = std::move(variadic_slots);
        toml::array present_connections;
        for (const auto& source_node : document.present_connections)
            present_connections.emplace_back(source_node);
        root["present_connections"] = std::move(present_connections);
        toml::array layouts;
        for (const auto& layout : document.layouts) {
            toml::table item;
            item["node_id"] = layout.node_id;
            item["x"] = layout.x;
            item["y"] = layout.y;
            item["expanded"] = layout.expanded;
            layouts.emplace_back(std::move(item));
        }
        root["layouts"] = std::move(layouts);
        toml::array extensions;
        for (const auto& extension : document.extensions) {
            if (extension.encoding == OpaqueAuthoringEncoding::kToml && extension.owner_node.rfind("@root:", 0) == 0) {
                try {
                    const auto parsed = toml::parse("value = " + extension.payload);
                    root[extension.owner_node.substr(6)] = parsed.at("value");
                    continue;
                } catch (...) {
                    // Fall through to the explicit opaque extension record.
                }
            }
            toml::table item;
            item["owner_node"] = extension.owner_node;
            item["encoding"] = static_cast<int64_t>(extension.encoding);
            item["payload"] = extension.payload;
            extensions.emplace_back(std::move(item));
        }
        root["extensions"] = std::move(extensions);
        const auto temporary = file.string() + ".tmp";
        std::ofstream output(temporary, std::ios::trunc);
        if (!output)
            return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                               "Cannot open Pipeline document for writing");
        output << toml::format(root);
        output.close();
        if (!output)
            return foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot write Pipeline document");
        std::error_code error;
        const auto verified = load_pipeline_document(temporary);
        if (!verified || document_semantic_digest(verified.value) != document_semantic_digest(document)) {
            std::filesystem::remove(temporary, error);
            return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                               "Pipeline document integrity verification failed");
        }
        const auto backup = file.string() + ".bak";
        if (std::filesystem::exists(file, error)) {
            std::filesystem::copy_file(file, backup, std::filesystem::copy_options::overwrite_existing, error);
            if (error) {
                std::filesystem::remove(temporary, error);
                return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                                   "Cannot create Pipeline document backup");
            }
        }
#if defined(_WIN32)
        if (!MoveFileExW(std::filesystem::path(temporary).c_str(), file.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::filesystem::remove(temporary, error);
            return foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot replace Pipeline document");
        }
#else
        std::filesystem::rename(temporary, file, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            return foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot replace Pipeline document");
        }
#endif
        return foundation::Result::success();
    } catch (const std::exception& exception) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what());
    }
}

}  // namespace aeg::application::project
