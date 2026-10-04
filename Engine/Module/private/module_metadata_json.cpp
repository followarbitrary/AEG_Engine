#include "module_metadata_json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <locale>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace aeg::module::detail {
namespace {
using Json = nlohmann::json;

constexpr std::size_t kMaxMetadataBytes = 64u * 1024u * 1024u;

foundation::Result invalid(std::string_view field) {
    return foundation::invalid_argument("module metadata field is invalid: " + std::string(field));
}

template <typename Id>
Json id_json(Id id) {
    return std::to_string(id.value());
}

template <typename Id>
foundation::ResultValue<Id> id_value(const Json& value, std::string_view name) {
    if (!value.contains(name) || !value.at(name).is_string())
        return {invalid(name), {}};
    const auto text = value.at(name).get<std::string>();
    uint64_t number{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || number == 0)
        return {invalid(name), {}};
    return {foundation::Result::success(), Id{number}};
}

foundation::ResultValue<uint64_t> u64_value(const Json& value, std::string_view name) {
    if (!value.contains(name))
        return {invalid(name), {}};
    if (value.at(name).is_string()) {
        const auto text = value.at(name).get<std::string>();
        uint64_t number{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
        if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size())
            return {foundation::Result::success(), number};
    } else if (value.at(name).is_number_unsigned()) {
        return {foundation::Result::success(), value.at(name).get<uint64_t>()};
    }
    return {invalid(name), {}};
}

template <typename T>
foundation::ResultValue<T> u32_value(const Json& value, std::string_view name) {
    if (!value.contains(name) || !value.at(name).is_number_unsigned())
        return {invalid(name), {}};
    const auto number = value.at(name).get<uint64_t>();
    if (number > std::numeric_limits<T>::max())
        return {invalid(name), {}};
    return {foundation::Result::success(), static_cast<T>(number)};
}

foundation::ResultValue<std::string> string_value(const Json& value, std::string_view name) {
    if (!value.contains(name) || !value.at(name).is_string())
        return {invalid(name), {}};
    return {foundation::Result::success(), value.at(name).get<std::string>()};
}

foundation::ResultValue<bool> bool_value(const Json& value, std::string_view name) {
    if (!value.contains(name) || !value.at(name).is_boolean())
        return {invalid(name), {}};
    return {foundation::Result::success(), value.at(name).get<bool>()};
}

Json version_json(pipeline::Version value) {
    return Json{{"major", value.major}, {"minor", value.minor}, {"patch", value.patch}};
}
foundation::ResultValue<pipeline::Version> version_value(const Json& value, std::string_view name) {
    if (!value.contains(name) || !value.at(name).is_object())
        return {invalid(name), {}};
    const auto& item = value.at(name);
    pipeline::Version result{};
    const auto major = u32_value<uint32_t>(item, "major");
    const auto minor = u32_value<uint32_t>(item, "minor");
    const auto patch = u32_value<uint32_t>(item, "patch");
    if (!major || !minor || !patch)
        return {invalid(name), {}};
    result.major = major.value;
    result.minor = minor.value;
    result.patch = patch.value;
    return {foundation::Result::success(), result};
}

Json parameter_json(const pipeline::ParameterValue& value) {
    Json result;
    if (const auto* bool_value = std::get_if<bool>(&value))
        result = {{"kind", "bool"}, {"value", *bool_value}};
    else if (const auto* integer_value = std::get_if<int64_t>(&value))
        result = {{"kind", "i64"}, {"value", std::to_string(*integer_value)}};
    else if (const auto* unsigned_value = std::get_if<uint64_t>(&value))
        result = {{"kind", "u64"}, {"value", std::to_string(*unsigned_value)}};
    else if (const auto* float_value = std::get_if<double>(&value)) {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::setprecision(17) << (*float_value == 0.0 ? 0.0 : *float_value);
        result = {{"kind", "f64"}, {"value", stream.str()}};
    } else if (const auto* string_value = std::get_if<std::string>(&value))
        result = {{"kind", "string"}, {"value", *string_value}};
    else if (const auto* vector_value = std::get_if<std::vector<double>>(&value)) {
        Json array = Json::array();
        for (const double number : *vector_value) {
            std::ostringstream stream;
            stream.imbue(std::locale::classic());
            stream << std::setprecision(17) << (number == 0.0 ? 0.0 : number);
            array.push_back(stream.str());
        }
        result = {{"kind", "vector"}, {"value", std::move(array)}};
    } else
        result = {{"kind", "asset"}, {"value", std::get<pipeline::AssetReference>(value).uuid}};
    return result;
}

foundation::ResultValue<pipeline::ParameterValue> parameter_value(const Json& value) {
    const auto kind = string_value(value, "kind");
    if (!kind || !value.contains("value"))
        return {invalid("parameter"), {}};
    const auto& raw = value.at("value");
    if (kind.value == "bool" && raw.is_boolean())
        return {foundation::Result::success(), raw.get<bool>()};
    if (kind.value == "i64" && raw.is_string()) {
        int64_t number{};
        const auto text = raw.get<std::string>();
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
        if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size())
            return {foundation::Result::success(), number};
    }
    if (kind.value == "u64" && raw.is_string()) {
        uint64_t number{};
        const auto text = raw.get<std::string>();
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
        if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size())
            return {foundation::Result::success(), number};
    }
    if (kind.value == "string" && raw.is_string())
        return {foundation::Result::success(), raw.get<std::string>()};
    if (kind.value == "asset" && raw.is_string())
        return {foundation::Result::success(), pipeline::AssetReference{raw.get<std::string>()}};
    if (kind.value == "f64" && raw.is_string()) {
        try {
            const double number = std::stod(raw.get<std::string>());
            if (std::isfinite(number))
                return {foundation::Result::success(), number};
        } catch (...) {}
    }
    if (kind.value == "vector" && raw.is_array()) {
        std::vector<double> result;
        result.reserve(raw.size());
        for (const auto& item : raw) {
            if (!item.is_string())
                return {invalid("vector"), {}};
            try {
                const double number = std::stod(item.get<std::string>());
                if (!std::isfinite(number))
                    return {invalid("vector"), {}};
                result.push_back(number);
            } catch (...) {
                return {invalid("vector"), {}};
            }
        }
        return {foundation::Result::success(), std::move(result)};
    }
    return {invalid("parameter value"), {}};
}

Json parameter_descriptor_json(const pipeline::ParameterDescriptor& value) {
    Json result{{"id", std::to_string(value.id)},
                {"name", value.name},
                {"display_name", value.display_name},
                {"kind", static_cast<uint8_t>(value.kind)},
                {"default_value", parameter_json(value.default_value)},
                {"enum_values", value.enum_values},
                {"unit", value.unit},
                {"schema_version", value.schema_version},
                {"affects_topology", value.affects_topology}};
    if (value.minimum)
        result["minimum"] = parameter_json(*value.minimum);
    if (value.maximum)
        result["maximum"] = parameter_json(*value.maximum);
    return result;
}
foundation::ResultValue<pipeline::ParameterDescriptor> parameter_descriptor_value(const Json& value) {
    pipeline::ParameterDescriptor result{};
    const auto id = u64_value(value, "id");
    const auto name = string_value(value, "name");
    const auto display = string_value(value, "display_name");
    const auto kind = u32_value<uint32_t>(value, "kind");
    const auto schema = u32_value<uint32_t>(value, "schema_version");
    const auto topology = bool_value(value, "affects_topology");
    if (!id || !name || !display || !kind || !schema || !topology ||
        kind.value > static_cast<uint32_t>(pipeline::ParameterKind::kAsset) || !value.contains("default_value"))
        return {invalid("parameter descriptor"), {}};
    const auto parsed = parameter_value(value.at("default_value"));
    if (!parsed)
        return {parsed.result, {}};
    result.id = id.value;
    result.name = name.value;
    result.display_name = display.value;
    result.kind = static_cast<pipeline::ParameterKind>(kind.value);
    result.schema_version = schema.value;
    result.affects_topology = topology.value;
    result.default_value = parsed.value;
    if (value.contains("minimum")) {
        const auto item = parameter_value(value.at("minimum"));
        if (!item || !std::holds_alternative<double>(item.value))
            return {invalid("minimum"), {}};
        result.minimum = std::get<double>(item.value);
    }
    if (value.contains("maximum")) {
        const auto item = parameter_value(value.at("maximum"));
        if (!item || !std::holds_alternative<double>(item.value))
            return {invalid("maximum"), {}};
        result.maximum = std::get<double>(item.value);
    }
    if (value.contains("enum_values") && value.at("enum_values").is_array())
        for (const auto& item : value.at("enum_values")) {
            if (!item.is_string())
                return {invalid("enum_values"), {}};
            result.enum_values.push_back(item.get<std::string>());
        }
    if (value.contains("unit")) {
        if (!value.at("unit").is_string())
            return {invalid("unit"), {}};
        result.unit = value.at("unit").get<std::string>();
    }
    return {foundation::Result::success(), std::move(result)};
}

Json constraints_json(const pipeline::ResourceConstraints& value) {
    return Json{{"resource_class", static_cast<uint8_t>(value.resource_class)},
                {"formats", value.formats},
                {"size_rule", value.size_rule},
                {"sample_count", value.sample_count},
                {"capability_mask", std::to_string(value.capability_mask)},
                {"usage_mask", std::to_string(value.usage_mask)}};
}
foundation::ResultValue<pipeline::ResourceConstraints> constraints_value(const Json& value) {
    pipeline::ResourceConstraints result{};
    const auto cls = u32_value<uint32_t>(value, "resource_class");
    const auto sample = u32_value<uint32_t>(value, "sample_count");
    const auto capability = u64_value(value, "capability_mask");
    const auto usage = u64_value(value, "usage_mask");
    if (!cls || !sample || !capability || !usage ||
        cls.value > static_cast<uint32_t>(pipeline::ResourceClass::kTexture))
        return {invalid("resource_constraints"), {}};
    result.resource_class = static_cast<pipeline::ResourceClass>(cls.value);
    result.sample_count = sample.value;
    result.capability_mask = capability.value;
    result.usage_mask = usage.value;
    if (value.contains("size_rule"))
        result.size_rule = value.at("size_rule").get<std::string>();
    if (value.contains("formats"))
        result.formats = value.at("formats").get<std::vector<std::string>>();
    return {foundation::Result::success(), std::move(result)};
}
Json input_contract_json(const pipeline::InputPortContract& value) {
    Json result{{"allowed_sources", value.allowed_sources},
                {"presence", static_cast<uint8_t>(value.presence)},
                {"min_edges", value.min_edges},
                {"max_edges", value.max_edges},
                {"variadic_group_id", std::to_string(value.variadic_group_id)},
                {"context_keys", value.context_keys}};
    if (value.literal_schema)
        result["literal_schema"] = parameter_descriptor_json(*value.literal_schema);
    if (value.default_value)
        result["default_value"] = Json{{"id", std::to_string(value.default_value->id)},
                                         {"revision", std::to_string(value.default_value->revision)},
                                         {"value", parameter_json(value.default_value->value)}};
    return result;
}
foundation::ResultValue<pipeline::InputPortContract> input_contract_value(const Json& value) {
    pipeline::InputPortContract result{};
    const auto allowed = u32_value<uint32_t>(value, "allowed_sources");
    const auto presence = u32_value<uint32_t>(value, "presence");
    const auto min = u32_value<uint32_t>(value, "min_edges");
    const auto max = u32_value<uint32_t>(value, "max_edges");
    const auto group = u64_value(value, "variadic_group_id");
    if (!allowed || !presence || !min || !max || !group ||
        allowed.value & ~pipeline::kAllInputSources ||
        presence.value > static_cast<uint32_t>(pipeline::InputPresenceRequirement::kRequired))
        return {invalid("input contract"), {}};
    result.allowed_sources = allowed.value;
    result.presence = static_cast<pipeline::InputPresenceRequirement>(presence.value);
    result.min_edges = min.value;
    result.max_edges = max.value;
    result.variadic_group_id = group.value;
    if (value.contains("context_keys")) {
        if (!value.at("context_keys").is_array())
            return {invalid("context_keys"), {}};
        result.context_keys = value.at("context_keys").get<std::vector<std::string>>();
    }
    if (value.contains("literal_schema")) {
        const auto schema = parameter_descriptor_value(value.at("literal_schema"));
        if (!schema)
            return {schema.result, {}};
        result.literal_schema = schema.value;
    }
    if (value.contains("default_value")) {
        const auto& item = value.at("default_value");
        const auto id = u64_value(item, "id");
        const auto revision = u64_value(item, "revision");
        if (!id || !revision || !item.contains("value"))
            return {invalid("input default"), {}};
        const auto parsed = parameter_value(item.at("value"));
        if (!parsed)
            return {parsed.result, {}};
        result.default_value = pipeline::InputDefaultDescriptor{id.value, revision.value, parsed.value};
    }
    return {foundation::Result::success(), std::move(result)};
}
Json port_json(const pipeline::PortDescriptor& value) {
    Json result{{"id", std::to_string(value.id)},
                {"name", value.name},
                {"direction", static_cast<uint8_t>(value.direction)},
                {"value_kind", static_cast<uint8_t>(value.value_kind)},
                {"schema_id", std::to_string(value.schema_id)},
                {"schema_version", value.schema_version},
                {"required", value.required},
                {"min_connections", value.min_connections},
                {"max_connections", value.max_connections},
                {"binding_id", std::to_string(value.binding_id)},
                {"resource_constraints", constraints_json(value.resource_constraints)}};
    if (value.input)
        result["input"] = input_contract_json(*value.input);
    return result;
}
foundation::ResultValue<pipeline::PortDescriptor> port_value(const Json& value) {
    pipeline::PortDescriptor result{};
    const auto id = u64_value(value, "id");
    const auto name = string_value(value, "name");
    const auto direction = u32_value<uint32_t>(value, "direction");
    const auto kind = u32_value<uint32_t>(value, "value_kind");
    const auto schema = u64_value(value, "schema_id");
    const auto version = u32_value<uint32_t>(value, "schema_version");
    const auto required = bool_value(value, "required");
    const auto min = u32_value<uint32_t>(value, "min_connections");
    const auto max = u32_value<uint32_t>(value, "max_connections");
    const auto binding = u64_value(value, "binding_id");
    if (!id || !name || !direction || !kind || !schema || !version || !required || !min || !max || !binding ||
        direction.value > 1 || kind.value > static_cast<uint32_t>(pipeline::PortValueKind::kTexture) ||
        !value.contains("resource_constraints"))
        return {invalid("port"), {}};
    const auto constraints = constraints_value(value.at("resource_constraints"));
    if (!constraints)
        return {constraints.result, {}};
    result.id = id.value;
    result.name = name.value;
    result.direction = static_cast<pipeline::PortDirection>(direction.value);
    result.value_kind = static_cast<pipeline::PortValueKind>(kind.value);
    result.schema_id = schema.value;
    result.schema_version = version.value;
    result.required = required.value;
    result.min_connections = min.value;
    result.max_connections = max.value;
    result.binding_id = binding.value;
    result.resource_constraints = constraints.value;
    if (value.contains("input")) {
        const auto input = input_contract_value(value.at("input"));
        if (!input)
            return {input.result, {}};
        result.input = input.value;
    }
    return {foundation::Result::success(), std::move(result)};
}

Json variadic_input_json(const pipeline::VariadicInputDescriptor& value) {
    return Json{{"id", std::to_string(value.id)},
                {"name", value.name},
                {"member_port_id", std::to_string(value.member_port_id)},
                {"min_slots", value.min_slots},
                {"max_slots", value.max_slots},
                {"ordered", value.ordered},
                {"empty_is_valid", value.empty_is_valid}};
}
foundation::ResultValue<pipeline::VariadicInputDescriptor> variadic_input_value(const Json& value) {
    const auto id = u64_value(value, "id");
    const auto name = string_value(value, "name");
    const auto member = u64_value(value, "member_port_id");
    const auto min = u32_value<uint32_t>(value, "min_slots");
    const auto max = u32_value<uint32_t>(value, "max_slots");
    const auto ordered = bool_value(value, "ordered");
    const auto empty = bool_value(value, "empty_is_valid");
    if (!id || !name || !member || !min || !max || !ordered || !empty)
        return {invalid("variadic input"), {}};
    return {foundation::Result::success(), pipeline::VariadicInputDescriptor{id.value, name.value, member.value,
                                                                              min.value, max.value, ordered.value,
                                                                              empty.value}};
}

Json stage_json(const pipeline::AuthoringStage& value) {
    return Json{{"id", id_json(value.id)},
                {"name", value.name},
                {"phase", id_json(value.phase)},
                {"execution_domain", static_cast<uint8_t>(value.execution_domain)},
                {"time_policy", static_cast<uint8_t>(value.time_policy)},
                {"editable", value.editable}};
}
foundation::ResultValue<pipeline::AuthoringStage> stage_value(const Json& value) {
    pipeline::AuthoringStage result{};
    const auto id = id_value<pipeline::StageId>(value, "id");
    const auto name = string_value(value, "name");
    const auto phase = id_value<pipeline::PhaseId>(value, "phase");
    const auto execution = u32_value<uint32_t>(value, "execution_domain");
    const auto time = u32_value<uint32_t>(value, "time_policy");
    const auto editable = bool_value(value, "editable");
    if (!id || !name || !phase || !execution || !time || !editable ||
        execution.value > static_cast<uint32_t>(pipeline::ExecutionDomain::kGpu) ||
        time.value > static_cast<uint32_t>(pipeline::TimePolicy::kShutdown))
        return {invalid("stage"), {}};
    result.id = id.value;
    result.name = name.value;
    result.phase = phase.value;
    result.execution_domain = static_cast<pipeline::ExecutionDomain>(execution.value);
    result.time_policy = static_cast<pipeline::TimePolicy>(time.value);
    result.editable = editable.value;
    return {foundation::Result::success(), std::move(result)};
}
Json artifact_json(const pipeline::AuthoringArtifact& value) {
    return Json{{"id", id_json(value.id)},
                {"name", value.name},
                {"type_id", id_json(value.type_id)},
                {"version", value.version},
                {"owner_scope", static_cast<uint8_t>(value.owner_scope)},
                {"visible_to", std::to_string(value.visible_to)},
                {"external_input", value.external_input},
                {"storage", static_cast<uint8_t>(value.storage)},
                {"lifetime", static_cast<uint8_t>(value.lifetime)},
                {"ownership", static_cast<uint8_t>(value.ownership)}};
}
foundation::ResultValue<pipeline::AuthoringArtifact> artifact_value(const Json& value) {
    pipeline::AuthoringArtifact result{};
    const auto id = id_value<pipeline::ArtifactId>(value, "id");
    const auto name = string_value(value, "name");
    const auto type = id_value<pipeline::ArtifactTypeId>(value, "type_id");
    const auto version = u32_value<uint32_t>(value, "version");
    const auto scope = u32_value<uint32_t>(value, "owner_scope");
    const auto visible = u64_value(value, "visible_to");
    const auto external = bool_value(value, "external_input");
    const auto storage = u32_value<uint32_t>(value, "storage");
    const auto lifetime = u32_value<uint32_t>(value, "lifetime");
    const auto ownership = u32_value<uint32_t>(value, "ownership");
    if (!id || !name || !type || !version || !scope || !visible || !external || !storage || !lifetime || !ownership ||
        scope.value > static_cast<uint32_t>(pipeline::PipelineScope::kEditor) ||
        storage.value > static_cast<uint32_t>(pipeline::ArtifactStorage::kCompletionToken) ||
        lifetime.value > static_cast<uint32_t>(pipeline::ArtifactLifetime::kEngine) ||
        ownership.value > static_cast<uint32_t>(pipeline::OwnershipPolicy::kExternalBorrowed))
        return {invalid("artifact"), {}};
    result.id = id.value;
    result.name = name.value;
    result.type_id = type.value;
    result.version = version.value;
    result.owner_scope = static_cast<pipeline::PipelineScope>(scope.value);
    result.visible_to = visible.value;
    result.external_input = external.value;
    result.storage = static_cast<pipeline::ArtifactStorage>(storage.value);
    result.lifetime = static_cast<pipeline::ArtifactLifetime>(lifetime.value);
    result.ownership = static_cast<pipeline::OwnershipPolicy>(ownership.value);
    return {foundation::Result::success(), std::move(result)};
}

Json parameter_block_json(const pipeline::ParameterBlock& value) {
    Json result = Json::array();
    for (const auto& [id, item] : value)
        result.push_back(Json{{"id", std::to_string(id)}, {"value", parameter_json(item)}});
    return result;
}
foundation::ResultValue<pipeline::ParameterBlock> parameter_block_value(const Json& value) {
    if (!value.is_array())
        return {invalid("parameters"), {}};
    pipeline::ParameterBlock result;
    for (const auto& item : value) {
        const auto id = u64_value(item, "id");
        if (!id || !item.contains("value"))
            return {invalid("parameters"), {}};
        const auto parsed = parameter_value(item.at("value"));
        if (!parsed || !result.emplace(id.value, parsed.value).second)
            return {invalid("parameters"), {}};
    }
    return {foundation::Result::success(), std::move(result)};
}
Json endpoint_json(const pipeline::DocumentEndpoint& value) {
    return Json{{"node_id", value.node_id}, {"port_id", std::to_string(value.port_id)}};
}
foundation::ResultValue<pipeline::DocumentEndpoint> endpoint_value(const Json& value) {
    pipeline::DocumentEndpoint result{};
    const auto node = string_value(value, "node_id");
    const auto port = u64_value(value, "port_id");
    if (!node || !port)
        return {invalid("endpoint"), {}};
    result.node_id = node.value;
    result.port_id = port.value;
    return {foundation::Result::success(), std::move(result)};
}
Json edge_json(const pipeline::DocumentEdge& value) {
    return Json{{"edge_id", value.edge_id},
                {"kind", static_cast<uint8_t>(value.kind)},
                {"source", endpoint_json(value.source)},
                {"destination", endpoint_json(value.destination)},
                {"binding_id", std::to_string(value.binding_id)}};
}
foundation::ResultValue<pipeline::DocumentEdge> edge_value(const Json& value) {
    pipeline::DocumentEdge result{};
    const auto id = string_value(value, "edge_id");
    const auto kind = u32_value<uint32_t>(value, "kind");
    const auto source = endpoint_value(value.at("source"));
    const auto destination = endpoint_value(value.at("destination"));
    const auto binding = u64_value(value, "binding_id");
    if (!id || !kind || !source || !destination || !binding ||
        kind.value > static_cast<uint32_t>(pipeline::DocumentEdgeKind::kRenderState))
        return {invalid("edge"), {}};
    result.edge_id = id.value;
    result.kind = static_cast<pipeline::DocumentEdgeKind>(kind.value);
    result.source = source.value;
    result.destination = destination.value;
    result.binding_id = binding.value;
    return {foundation::Result::success(), std::move(result)};
}

Json declaration_json(const pipeline::AuthoringDeclaration& value) {
    Json result;
    if (const auto* pipeline_value = std::get_if<pipeline::PipelineAuthoringDeclaration>(&value)) {
        result["kind"] = "pipeline";
        result["stages"] = Json::array();
        for (const auto& item : pipeline_value->stages)
            result["stages"].push_back(stage_json(item));
        result["feature_points"] = Json::array();
        for (const auto& item : pipeline_value->feature_points)
            result["feature_points"].push_back(Json{{"id", id_json(item.id)},
                                                    {"name", item.name},
                                                    {"anchor_stage", id_json(item.anchor_stage)},
                                                    {"position", static_cast<uint8_t>(item.position)},
                                                    {"domains", std::to_string(item.domains)},
                                                    {"scopes", std::to_string(item.scopes)},
                                                    {"flags", std::to_string(static_cast<uint32_t>(item.flags))}});
        result["artifacts"] = Json::array();
        for (const auto& item : pipeline_value->artifacts)
            result["artifacts"].push_back(artifact_json(item));
        result["fixed_orderings"] = Json::array();
        for (const auto& item : pipeline_value->fixed_orderings)
            result["fixed_orderings"].push_back(Json{{"before", id_json(item.before)}, {"after", id_json(item.after)}});
        result["default_features"] = Json::array();
        for (const auto& item : pipeline_value->default_features)
            result["default_features"].push_back(Json{{"node_id", item.node_id},
                                                      {"type", id_json(item.type)},
                                                      {"version", version_json(item.version)},
                                                      {"point", id_json(item.point)},
                                                      {"parameters", parameter_block_json(item.parameters)}});
    } else if (const auto* feature_value = std::get_if<pipeline::FeatureAuthoringDeclaration>(&value)) {
        result["kind"] = "feature";
        result["stages"] = Json::array();
        for (const auto& item : feature_value->stages)
            result["stages"].push_back(stage_json(item));
        result["artifacts"] = Json::array();
        for (const auto& item : feature_value->artifacts)
            result["artifacts"].push_back(artifact_json(item));
        result["parts"] = Json::array();
        for (const auto& item : feature_value->parts)
            result["parts"].push_back(Json{
                {"part_index", item.part_index}, {"domain", static_cast<uint8_t>(item.domain)}, {"name", item.name}});
        result["default_passes"] = Json::array();
        for (const auto& item : feature_value->default_passes)
            result["default_passes"].push_back(Json{{"node_id", item.node_id},
                                                    {"type", id_json(item.type)},
                                                    {"version", version_json(item.version)},
                                                    {"part_id", item.part_id},
                                                    {"parameters", parameter_block_json(item.parameters)},
                                                    {"enabled", item.enabled}});
        result["default_resources"] = Json::array();
        for (const auto& item : feature_value->default_resources)
            result["default_resources"].push_back(Json{{"node_id", item.node_id},
                                                       {"resource_id", id_json(item.resource_id)},
                                                       {"constraints", constraints_json(item.constraints)}});
        result["default_edges"] = Json::array();
        for (const auto& item : feature_value->default_edges)
            result["default_edges"].push_back(edge_json(item));
        result["allowed_pass_types"] = Json::array();
        for (const auto& item : feature_value->allowed_pass_types)
            result["allowed_pass_types"].push_back(id_json(item));
        result["allow_add_passes"] = feature_value->allow_add_passes;
    } else {
        const auto& pass_value = std::get<pipeline::PassAuthoringDeclaration>(value);
        result = Json{{"kind", "pass"},
                      {"type", id_json(pass_value.type)},
                      {"allowed_queue_mask", std::to_string(pass_value.allowed_queue_mask)}};
    }
    return result;
}

foundation::ResultValue<pipeline::AuthoringDeclaration> declaration_value(const Json& value) {
    const auto kind = string_value(value, "kind");
    if (!kind)
        return {kind.result, {}};
    if (kind.value == "pipeline") {
        pipeline::PipelineAuthoringDeclaration result{};
        for (const auto& item : value.at("stages")) {
            auto parsed = stage_value(item);
            if (!parsed)
                return {parsed.result, {}};
            result.stages.push_back(parsed.value);
        }
        for (const auto& item : value.at("feature_points")) {
            pipeline::AuthoringFeaturePoint point{};
            const auto id = id_value<pipeline::FeaturePointId>(item, "id");
            const auto name = string_value(item, "name");
            const auto anchor = id_value<pipeline::StageId>(item, "anchor_stage");
            const auto position = u32_value<uint32_t>(item, "position");
            const auto domains = u64_value(item, "domains");
            const auto scopes = u64_value(item, "scopes");
            const auto flags = u64_value(item, "flags");
            if (!id || !name || !anchor || !position || !domains || !scopes || !flags)
                return {invalid("feature point"), {}};
            point.id = id.value;
            point.name = name.value;
            point.anchor_stage = anchor.value;
            point.position = static_cast<pipeline::FeaturePointPosition>(position.value);
            point.domains = domains.value;
            point.scopes = scopes.value;
            point.flags = static_cast<pipeline::FeaturePointFlags>(flags.value);
            result.feature_points.push_back(std::move(point));
        }
        for (const auto& item : value.at("artifacts")) {
            auto parsed = artifact_value(item);
            if (!parsed)
                return {parsed.result, {}};
            result.artifacts.push_back(parsed.value);
        }
        for (const auto& item : value.at("fixed_orderings")) {
            const auto before = id_value<pipeline::StageId>(item, "before");
            const auto after = id_value<pipeline::StageId>(item, "after");
            if (!before || !after)
                return {invalid("fixed_orderings"), {}};
            result.fixed_orderings.push_back({before.value, after.value});
        }
        for (const auto& item : value.at("default_features")) {
            auto node = string_value(item, "node_id");
            auto type = id_value<pipeline::FeatureId>(item, "type");
            auto version = version_value(item, "version");
            auto point = id_value<pipeline::FeaturePointId>(item, "point");
            auto parameters = parameter_block_value(item.at("parameters"));
            if (!node || !type || !version || !point || !parameters)
                return {invalid("default_features"), {}};
            result.default_features.push_back(
                {node.value, type.value, version.value, point.value, std::move(parameters.value)});
        }
        return {foundation::Result::success(), pipeline::AuthoringDeclaration{std::move(result)}};
    }
    if (kind.value == "feature") {
        pipeline::FeatureAuthoringDeclaration result{};
        for (const auto& item : value.at("stages")) {
            auto parsed = stage_value(item);
            if (!parsed)
                return {parsed.result, {}};
            result.stages.push_back(parsed.value);
        }
        for (const auto& item : value.at("artifacts")) {
            auto parsed = artifact_value(item);
            if (!parsed)
                return {parsed.result, {}};
            result.artifacts.push_back(parsed.value);
        }
        for (const auto& item : value.at("parts")) {
            auto index = u32_value<uint32_t>(item, "part_index");
            auto domain = u32_value<uint32_t>(item, "domain");
            auto name = string_value(item, "name");
            if (!index || !domain || !name)
                return {invalid("parts"), {}};
            result.parts.push_back({index.value, static_cast<pipeline::FeatureDomain>(domain.value), name.value});
        }
        for (const auto& item : value.at("default_passes")) {
            auto node = string_value(item, "node_id");
            auto type = id_value<pipeline::RenderPassTypeId>(item, "type");
            auto version = version_value(item, "version");
            auto part = string_value(item, "part_id");
            auto parameters = parameter_block_value(item.at("parameters"));
            auto enabled = bool_value(item, "enabled");
            if (!node || !type || !version || !part || !parameters || !enabled)
                return {invalid("default_passes"), {}};
            result.default_passes.push_back(
                {node.value, type.value, version.value, part.value, std::move(parameters.value), enabled.value});
        }
        for (const auto& item : value.at("default_resources")) {
            auto node = string_value(item, "node_id");
            auto id = id_value<pipeline::RenderResourceId>(item, "resource_id");
            auto constraints = constraints_value(item.at("constraints"));
            if (!node || !id || !constraints)
                return {invalid("default_resources"), {}};
            result.default_resources.push_back({node.value, id.value, std::move(constraints.value)});
        }
        for (const auto& item : value.at("default_edges")) {
            auto edge = edge_value(item);
            if (!edge)
                return {edge.result, {}};
            result.default_edges.push_back(edge.value);
        }
        for (const auto& item : value.at("allowed_pass_types")) {
            if (!item.is_string())
                return {invalid("allowed_pass_types"), {}};
            uint64_t id{};
            const auto text = item.get<std::string>();
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
            if (parsed.ec != std::errc{})
                return {invalid("allowed_pass_types"), {}};
            result.allowed_pass_types.emplace_back(id);
        }
        result.allow_add_passes = value.value("allow_add_passes", false);
        return {foundation::Result::success(), pipeline::AuthoringDeclaration{std::move(result)}};
    }
    if (kind.value == "pass") {
        const auto type = id_value<pipeline::RenderPassTypeId>(value, "type");
        const auto mask = u64_value(value, "allowed_queue_mask");
        if (!type || !mask)
            return {invalid("pass"), {}};
        return {foundation::Result::success(),
                pipeline::AuthoringDeclaration{pipeline::PassAuthoringDeclaration{type.value, mask.value}}};
    }
    return {invalid("declaration kind"), {}};
}

Json catalog_entry_json(const pipeline::TypeCatalogEntry& value) {
    Json result{{"kind", static_cast<uint8_t>(value.key.kind)},
                {"id", std::to_string(value.key.id)},
                {"version", version_json(value.version)},
                {"owner_module", id_json(value.owner_module)},
                {"name", value.name},
                {"display_name", value.display_name},
                {"domains", std::to_string(value.domains)},
                {"scopes", std::to_string(value.scopes)},
                {"parameters", Json::array()},
                {"ports", Json::array()},
                {"variadic_inputs", Json::array()},
                {"declaration", declaration_json(value.declaration)}};
    std::vector<const pipeline::ParameterDescriptor*> parameters;
    for (const auto& item : value.parameters)
        parameters.push_back(&item);
    std::sort(parameters.begin(), parameters.end(), [](auto left, auto right) { return left->id < right->id; });
    for (const auto* item : parameters)
        result["parameters"].push_back(parameter_descriptor_json(*item));
    std::vector<const pipeline::PortDescriptor*> ports;
    for (const auto& item : value.ports)
        ports.push_back(&item);
    std::sort(ports.begin(), ports.end(), [](auto left, auto right) { return left->id < right->id; });
    for (const auto* item : ports)
        result["ports"].push_back(port_json(*item));
    std::vector<const pipeline::VariadicInputDescriptor*> variadic_inputs;
    for (const auto& item : value.variadic_inputs)
        variadic_inputs.push_back(&item);
    std::sort(variadic_inputs.begin(), variadic_inputs.end(),
              [](auto left, auto right) { return left->id < right->id; });
    for (const auto* item : variadic_inputs)
        result["variadic_inputs"].push_back(variadic_input_json(*item));
    return result;
}
foundation::ResultValue<pipeline::TypeCatalogEntry> catalog_entry_value(const Json& value) {
    pipeline::TypeCatalogEntry result{};
    const auto kind = u32_value<uint32_t>(value, "kind");
    const auto id = u64_value(value, "id");
    const auto version = version_value(value, "version");
    const auto owner = id_value<pipeline::ModuleId>(value, "owner_module");
    const auto name = string_value(value, "name");
    const auto display = string_value(value, "display_name");
    const auto domains = u64_value(value, "domains");
    const auto scopes = u64_value(value, "scopes");
    if (!kind || !id || !version || !owner || !name || !display || !domains || !scopes ||
        kind.value > static_cast<uint32_t>(pipeline::AuthoringTypeKind::kRenderPass))
        return {invalid("catalog entry"), {}};
    result.key = {static_cast<pipeline::AuthoringTypeKind>(kind.value), id.value};
    result.version = version.value;
    result.owner_module = owner.value;
    result.name = name.value;
    result.display_name = display.value;
    result.domains = domains.value;
    result.scopes = scopes.value;
    for (const auto& item : value.at("parameters")) {
        auto parsed = parameter_descriptor_value(item);
        if (!parsed)
            return {parsed.result, {}};
        result.parameters.push_back(parsed.value);
    }
    for (const auto& item : value.at("ports")) {
        auto parsed = port_value(item);
        if (!parsed)
            return {parsed.result, {}};
        result.ports.push_back(parsed.value);
    }
    if (value.contains("variadic_inputs")) {
        if (!value.at("variadic_inputs").is_array())
            return {invalid("variadic_inputs"), {}};
        for (const auto& item : value.at("variadic_inputs")) {
            const auto parsed = variadic_input_value(item);
            if (!parsed)
                return {parsed.result, {}};
            result.variadic_inputs.push_back(parsed.value);
        }
    }
    auto declaration = declaration_value(value.at("declaration"));
    if (!declaration)
        return {declaration.result, {}};
    result.declaration = std::move(declaration.value);
    return {foundation::Result::success(), std::move(result)};
}

Json descriptor_json(const ModuleDescriptor& value) {
    Json result{{"id", id_json(value.id)},
                {"name", value.name},
                {"version", version_json(value.version)},
                {"dependencies", Json::array()}};
    std::vector<ModuleDependency> dependencies = value.dependencies;
    std::sort(dependencies.begin(), dependencies.end(),
              [](const auto& left, const auto& right) { return left.module.value() < right.module.value(); });
    for (const auto& item : dependencies)
        result["dependencies"].push_back(
            Json{{"module", id_json(item.module)}, {"minimum_version", version_json(item.minimum_version)}});
    return result;
}
foundation::ResultValue<ModuleDescriptor> descriptor_value(const Json& value) {
    ModuleDescriptor result{};
    const auto id = id_value<pipeline::ModuleId>(value, "id");
    const auto name = string_value(value, "name");
    const auto version = version_value(value, "version");
    if (!id || !name || !version)
        return {invalid("module"), {}};
    result.id = id.value;
    result.name = name.value;
    result.version = version.value;
    for (const auto& item : value.at("dependencies")) {
        const auto module = id_value<pipeline::ModuleId>(item, "module");
        const auto minimum = version_value(item, "minimum_version");
        if (!module || !minimum)
            return {invalid("dependency"), {}};
        result.dependencies.push_back({module.value, minimum.value});
    }
    return {foundation::Result::success(), std::move(result)};
}

Json metadata_json(const ModuleMetadata& input) {
    const auto descriptor = input.module.id.is_valid() ? input.module : input.descriptor;
    Json result{{"abi_major", input.abi_major},
                {"abi_minor", input.abi_minor},
                {"build_id", input.build_id},
                {"catalog", Json::array()},
                {"configuration", input.configuration},
                {"contract_digest", input.contract_digest},
                {"module", descriptor_json(descriptor)},
                {"schema_version", input.schema_version},
                {"sdk_contract_major", input.sdk_contract_major},
                {"sdk_contract_minor", input.sdk_contract_minor},
                {"target", input.target}};
    for (const auto& entry : input.catalog.entries())
        result["catalog"].push_back(catalog_entry_json(entry));
    return result;
}

std::string sha256(std::string_view bytes) {
    constexpr uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
    // The implementation is kept deterministic and private; nlohmann handles
    // JSON syntax while this small digest avoids an engine crypto dependency.
    std::vector<uint8_t> input(bytes.begin(), bytes.end());
    const uint64_t bits = static_cast<uint64_t>(input.size()) * 8u;
    input.push_back(0x80u);
    while (input.size() % 64u != 56u)
        input.push_back(0);
    for (int shift = 7; shift >= 0; --shift)
        input.push_back(static_cast<uint8_t>(bits >> (shift * 8)));
    uint32_t s[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    auto r = [](uint32_t x, uint32_t n) { return (x >> n) | (x << (32u - n)); };
    for (size_t off = 0; off < input.size(); off += 64) {
        uint32_t w[64]{};
        for (uint32_t i = 0; i < 16; i++)
            w[i] = (uint32_t(input[off + i * 4]) << 24) | (uint32_t(input[off + i * 4 + 1]) << 16) |
                   (uint32_t(input[off + i * 4 + 2]) << 8) | input[off + i * 4 + 3];
        for (uint32_t i = 16; i < 64; i++) {
            uint32_t a = r(w[i - 15], 7) ^ r(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t b = r(w[i - 2], 17) ^ r(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + a + w[i - 7] + b;
        }
        uint32_t a = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];
        for (uint32_t i = 0; i < 64; i++) {
            uint32_t q = r(e, 6) ^ r(e, 11) ^ r(e, 25), ch = (e & f) ^ ((~e) & g), t1 = h + q + ch + k[i] + w[i],
                     p = r(a, 2) ^ r(a, 13) ^ r(a, 22), maj = (a & b) ^ (a & c) ^ (b & c), t2 = p + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        s[0] += a;
        s[1] += b;
        s[2] += c;
        s[3] += d;
        s[4] += e;
        s[5] += f;
        s[6] += g;
        s[7] += h;
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (auto item : s)
        out << std::setw(8) << item;
    return out.str();
}

}  // namespace

foundation::ResultValue<std::string> encode_metadata_json(const ModuleMetadata& metadata) {
    if (metadata.schema_version == 0 || metadata.abi_major == 0 || metadata.sdk_contract_major == 0)
        return {foundation::invalid_argument("module metadata version is invalid"), {}};
    const auto result = metadata_json(metadata).dump();
    if (result.size() > kMaxMetadataBytes)
        return {foundation::invalid_argument("module metadata is too large"), {}};
    return {foundation::Result::success(), result + "\n"};
}
foundation::ResultValue<ModuleMetadata> decode_metadata_json(std::string_view json) {
    if (json.empty() || json.size() > kMaxMetadataBytes)
        return {foundation::invalid_argument("module metadata size is invalid"), {}};
    try {
        const auto root = Json::parse(json);
        if (!root.is_object())
            return {invalid("root"), {}};
        ModuleMetadata result{};
        const auto build = string_value(root, "build_id");
        const auto contract = string_value(root, "contract_digest");
        if (!build || !contract)
            return {foundation::invalid_argument("module metadata is missing required fields"), {}};
        result.build_id = build.value;
        result.contract_digest = contract.value;
        if (!root.contains("schema_version"))
            return {foundation::Result::success(), std::move(result)};
        const auto schema = u32_value<uint32_t>(root, "schema_version");
        const auto abi_major = u32_value<uint32_t>(root, "abi_major");
        const auto abi_minor = u32_value<uint32_t>(root, "abi_minor");
        const auto sdk_major = u32_value<uint32_t>(root, "sdk_contract_major");
        const auto sdk_minor = u32_value<uint32_t>(root, "sdk_contract_minor");
        const auto target = string_value(root, "target");
        const auto configuration = string_value(root, "configuration");
        if (!schema || !abi_major || !abi_minor || !sdk_major || !sdk_minor || !target || !configuration ||
            !root.contains("module") || !root.contains("catalog"))
            return {invalid("header"), {}};
        const auto descriptor = descriptor_value(root.at("module"));
        if (!descriptor || !root.at("catalog").is_array())
            return {invalid("catalog"), {}};
        result.schema_version = schema.value;
        result.abi_major = abi_major.value;
        result.abi_minor = abi_minor.value;
        result.sdk_contract_major = sdk_major.value;
        result.sdk_contract_minor = sdk_minor.value;
        result.target = target.value;
        result.configuration = configuration.value;
        result.descriptor = descriptor.value;
        result.module = descriptor.value;
        for (const auto& item : root.at("catalog")) {
            const auto entry = catalog_entry_value(item);
            if (!entry)
                return {entry.result, {}};
            if (const auto add = result.catalog.add(entry.value); !add)
                return {add, {}};
        }
        return {foundation::Result::success(), std::move(result)};
    } catch (const std::exception& exception) {
        return {foundation::invalid_argument(std::string("module metadata JSON is invalid: ") + exception.what()), {}};
    }
}
foundation::ResultValue<std::string> metadata_sha256(std::string_view bytes) {
    return {foundation::Result::success(), sha256(bytes)};
}
}  // namespace aeg::module::detail
