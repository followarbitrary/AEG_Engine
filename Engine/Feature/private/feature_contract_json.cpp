#include "feature_contract_json.hpp"
#include "feature_json_codec.hpp"
#include "../../Serialization/private/json_value_codec.hpp"

namespace aeg::feature {
namespace {
using namespace serialization::detail;
constexpr std::size_t kMaxContractBytes = 64u * 1024u * 1024u;

Json numeric_range_json(const feature::NumericRange& range) {
    return Json{{"min", range.min}, {"max", range.max}, {"step", range.step}, {"normalized", range.normalized}};
}
foundation::ResultValue<feature::NumericRange> numeric_range_value(const Json& value) {
    const auto normalized = bool_value(value, "normalized");
    if (!normalized || !value.contains("min") || !value.at("min").is_number() ||
        !value.contains("max") || !value.at("max").is_number() ||
        !value.contains("step") || !value.at("step").is_number()) return {invalid("numeric range"), {}};
    return {foundation::Result::success(), {value.at("min").get<double>(), value.at("max").get<double>(),
        value.at("step").get<double>(), normalized.value}};
}
Json feature_parameter_metadata_json(const feature::ParameterMetadata& value) {
    Json result{{"type", static_cast<uint8_t>(value.type)}, {"display_name", value.display_name},
        {"description", value.description}, {"expert_only", value.expert_only},
        {"ui_group", value.ui_group}, {"ui_order", value.ui_order},
        {"constraint", Json{{"kind", value.constraint.index()}}}};
    if (value.default_value) result["default_value"] = parameter_json(*value.default_value);
    auto& constraint = result["constraint"];
    if (const auto* range = std::get_if<feature::NumericRange>(&value.constraint)) constraint["value"] = numeric_range_json(*range);
    else if (const auto* choices = std::get_if<feature::EnumChoices>(&value.constraint)) {
        constraint["labels"] = choices->labels; constraint["descriptions"] = choices->descriptions;
    } else if (const auto* pattern = std::get_if<feature::StringPattern>(&value.constraint)) {
        constraint["regex"] = pattern->regex; constraint["min_length"] = pattern->min_length;
        constraint["max_length"] = pattern->max_length; constraint["is_file_path"] = pattern->is_file_path;
        constraint["is_color"] = pattern->is_color;
    } else if (const auto* asset = std::get_if<feature::AssetConstraint>(&value.constraint)) {
        constraint["optional"] = asset->optional;
        if (asset->kind) constraint["asset_kind"] = static_cast<uint8_t>(*asset->kind);
    }
    else if (const auto* vector = std::get_if<feature::VectorConstraint>(&value.constraint)) {
        constraint["normalized"] = vector->normalized;
        if (vector->component_range) constraint["component_range"] = numeric_range_json(*vector->component_range);
    }
    return result;
}
foundation::ResultValue<feature::ParameterMetadata> feature_parameter_metadata_value(const Json& value) {
    feature::ParameterMetadata result{};
    const auto type = u32_value<uint32_t>(value, "type");
    const auto name = string_value(value, "display_name");
    const auto description = string_value(value, "description");
    const auto expert = bool_value(value, "expert_only");
    const auto group = string_value(value, "ui_group");
    const auto order = u32_value<uint32_t>(value, "ui_order");
    if (!type || type.value > static_cast<uint32_t>(feature::ParameterType::kColor) || !name ||
        !description || !expert || !group || !order || !value.contains("constraint"))
        return {invalid("typed parameter metadata"), {}};
    result.type = static_cast<feature::ParameterType>(type.value);
    result.display_name = name.value; result.description = description.value;
    result.expert_only = expert.value; result.ui_group = group.value; result.ui_order = order.value;
    if (value.contains("default_value")) {
        const auto parsed = parameter_value(value.at("default_value"));
        if (!parsed) return {parsed.result, {}};
        result.default_value = parsed.value;
    }
    const auto& constraint = value.at("constraint");
    const auto kind = u32_value<uint32_t>(constraint, "kind");
    if (!kind || kind.value > 5) return {invalid("typed parameter constraint"), {}};
    switch (kind.value) {
    case 0: break;
    case 1: {
        if (!constraint.contains("value")) return {invalid("numeric range"), {}};
        const auto range = numeric_range_value(constraint.at("value"));
        if (!range) return {range.result, {}};
        result.constraint = range.value; break;
    }
    case 2: {
        if (!constraint.contains("labels") || !constraint.at("labels").is_array() ||
            !constraint.contains("descriptions") || !constraint.at("descriptions").is_array()) return {invalid("enum choices"), {}};
        feature::EnumChoices choices{};
        for (const auto& label : constraint.at("labels")) { if (!label.is_string()) return {invalid("enum value"), {}}; choices.labels.push_back(label.get<std::string>()); }
        for (const auto& label : constraint.at("descriptions")) { if (!label.is_string()) return {invalid("enum description"), {}}; choices.descriptions.push_back(label.get<std::string>()); }
        result.constraint = std::move(choices); break;
    }
    case 3: {
        const auto regex = string_value(constraint, "regex");
        const auto min = u32_value<uint32_t>(constraint, "min_length");
        const auto max = u32_value<uint32_t>(constraint, "max_length");
        const auto path = bool_value(constraint, "is_file_path");
        const auto color = bool_value(constraint, "is_color");
        if (!regex || !min || !max || !path || !color) return {invalid("string constraint"), {}};
        result.constraint = feature::StringPattern{regex.value, min.value, max.value, path.value, color.value}; break;
    }
    case 4: {
        const auto optional = bool_value(constraint, "optional");
        if (!optional) return {optional.result, {}};
        feature::AssetConstraint asset{optional.value};
        if (constraint.contains("asset_kind")) {
            const auto asset_kind = u32_value<uint32_t>(constraint, "asset_kind");
            if (!asset_kind || asset_kind.value > static_cast<uint32_t>(foundation::AssetKind::kShader))
                return {invalid("asset constraint kind"), {}};
            asset.kind = static_cast<foundation::AssetKind>(asset_kind.value);
        }
        result.constraint = asset; break;
    }
    case 5: {
        const auto normalized = bool_value(constraint, "normalized");
        if (!normalized) return {normalized.result, {}};
        feature::VectorConstraint vector{};
        vector.normalized = normalized.value;
        if (constraint.contains("component_range")) {
            const auto range = numeric_range_value(constraint.at("component_range"));
            if (!range) return {range.result, {}};
            vector.component_range = range.value;
        }
        result.constraint = vector; break;
    }
    }
    if (const auto valid = feature::validate_parameter_metadata(result); !valid) return {valid, {}};
    return {foundation::Result::success(), std::move(result)};
}

Json feature_contract_json(const feature::FeatureContract& contract) {
    Json contract_json{{"definition_id", std::to_string(contract.definition_id.value())},
                        {"version", version_json({contract.version.major, contract.version.minor,
                                                   contract.version.patch})},
                        {"structure", static_cast<uint8_t>(contract.structure)},
                        {"domains", std::to_string(contract.domains)},
                        {"execution", static_cast<uint8_t>(contract.execution)},
                        {"state", static_cast<uint8_t>(contract.state)},
                        {"scope", std::to_string(contract.scope.id.value())},
                        {"clock", Json{{"id", std::to_string(contract.clock.id.value())},
                                         {"frequency_hz", contract.clock.frequency_hz},
                                         {"max_backlog", contract.clock.max_backlog},
                                         {"late_data_policy", static_cast<uint8_t>(contract.clock.late_data_policy)},
                                         {"capability_version", version_json({contract.clock.capability_version.major,
                                             contract.clock.capability_version.minor, contract.clock.capability_version.patch})},
                                         {"work_budget_ns", std::to_string(contract.clock.work_budget_ns)},
                                         {"timing", nullptr}}},
                        {"permissions", Json{{"capability_mask", std::to_string(contract.permissions.capability_mask)},
                                               {"version", contract.permissions.version}}},
                        {"failure", static_cast<uint8_t>(contract.failure)},
                        {"retry", Json{{"max_attempts", contract.retry.max_attempts},
                                         {"backoff_ticks", contract.retry.backoff_ticks}}},
                        {"children", Json::array()},
                        {"ports", Json::array()},
                        {"parameters", Json::array()},
                        {"side_effects", Json::array()},
                        {"child_port_mappings", Json::array()},
                        {"has_continuation", contract.has_continuation},
                        {"allows_child_process", contract.allows_child_process}};
    if (contract.clock.timing) {
        const auto& timing = *contract.clock.timing;
        contract_json["clock"]["timing"] = Json{{"phase_ns", std::to_string(timing.phase_ns)},
            {"relative_deadline_ns", std::to_string(timing.relative_deadline_ns)},
            {"deadline_policy", static_cast<uint8_t>(timing.deadline_policy)}};
    }
    if (feature::owns_state_payload(contract.state)) {
        const auto schema = contract.state_payload.schema_version;
        contract_json["state_payload"] = Json{{"schema_version", version_json({schema.major, schema.minor, schema.patch})},
                                               {"max_size", contract.state_payload.max_size}};
    }
    for (const auto child : contract.children)
        contract_json["children"].push_back(std::to_string(child.value()));
    for (const auto& port : contract.ports)
        contract_json["ports"].push_back(Json{{"id", std::to_string(port.id.value())},
                                               {"direction", static_cast<uint8_t>(port.direction)},
                                               {"schema", std::to_string(port.schema.value())},
                                               {"version", port.version}});
    for (const auto& parameter : contract.parameters) {
        Json item{{"id", std::to_string(parameter.id.value())}, {"version", parameter.version},
                  {"max_size", parameter.max_size}, {"required", parameter.required}};
        if (parameter.metadata) item["metadata"] = feature_parameter_metadata_json(*parameter.metadata);
        contract_json["parameters"].push_back(std::move(item));
    }
    for (const auto& effect : contract.side_effects)
        contract_json["side_effects"].push_back(
            Json{{"id", std::to_string(effect.id.value())}, {"version", effect.version},
                 {"kind", static_cast<uint8_t>(effect.kind)},
                 {"commit", static_cast<uint8_t>(effect.commit)}});
    for (const auto& mapping : contract.child_port_mappings)
        contract_json["child_port_mappings"].push_back(
            Json{{"parent_port", std::to_string(mapping.parent_port.value())},
                 {"child_definition", std::to_string(mapping.child_definition.value())},
                 {"child_port", std::to_string(mapping.child_port.value())},
                 {"direction", static_cast<uint8_t>(mapping.direction)}});
    return contract_json;
}

Json feature_catalog_entry_json(const feature::FeatureCatalogEntry& value) {
    Json result{{"id", std::to_string(value.definition_id.value())},
                {"owner_module", id_json(value.owner_module)},
                {"version", version_json({value.version.major, value.version.minor, value.version.patch})},
                {"domains", std::to_string(value.domains)},
                {"execution", static_cast<uint8_t>(value.execution)},
                {"state", static_cast<uint8_t>(value.state)},
                {"clock", std::to_string(value.clock.value())},
                {"contract_digest", std::to_string(value.contract_digest)},
                {"ports", Json::array()}};
    for (const auto& port : value.ports) {
        result["ports"].push_back(Json{{"direction", static_cast<uint8_t>(port.direction)},
                                        {"id", std::to_string(port.id.value())},
                                        {"schema", std::to_string(port.schema.value())},
                                        {"version", port.version}});
    }

    if (value.contract.definition_id.is_valid())
        result["contract"] = feature_contract_json(value.contract);
    return result;
}

foundation::ResultValue<feature::FeatureDomainMask> feature_domains_value(const Json& value) {
    if (value.is_string()) {
        uint64_t mask{};
        const auto text = value.get<std::string>();
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), mask);
        if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && mask <= feature::kAllFeatureDomains)
            return {foundation::Result::success(), static_cast<feature::FeatureDomainMask>(mask)};
        return {invalid("feature domains"), {}};
    }
    if (!value.is_array())
        return {invalid("feature domains"), {}};
    feature::FeatureDomainMask mask = 0;
    for (const auto& item : value) {
        if (!item.is_string())
            return {invalid("feature domain"), {}};
        const auto name = item.get<std::string>();
        if (name == "logic") mask |= feature::to_mask(feature::FeatureDomain::kLogic);
        else if (name == "physics" || name == "physic") mask |= feature::to_mask(feature::FeatureDomain::kPhysics);
        else if (name == "render") mask |= feature::to_mask(feature::FeatureDomain::kRender);
        else if (name == "audio") mask |= feature::to_mask(feature::FeatureDomain::kAudio);
        else if (name == "stream") mask |= feature::to_mask(feature::FeatureDomain::kStream);
        else return {invalid("feature domain"), {}};
    }
    return {foundation::Result::success(), mask};
}

foundation::ResultValue<feature::FeatureDefinitionId> feature_definition_id_value(const Json& value) {
    if (!value.is_string())
        return {invalid("feature id"), {}};
    const auto text = value.get<std::string>();
    uint64_t number{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
    if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && number != 0)
        return {foundation::Result::success(), feature::FeatureDefinitionId(number)};
    if (text.empty())
        return {invalid("feature id"), {}};
    return {foundation::Result::success(), feature::FeatureDefinitionId(text)};
}

template <typename Id>
foundation::ResultValue<Id> feature_id_value(const Json& value, std::string_view name) {
    if (!value.contains(name) || !value.at(name).is_string())
        return {invalid(name), {}};
    const auto text = value.at(name).get<std::string>();
    uint64_t number{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
    if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && number != 0)
        return {foundation::Result::success(), Id{number}};
    // Authoring metadata may use stable symbolic names for nested contract
    // identities (ports, schemas, scopes, clocks, and effects).  Preserve
    // the same FNV-1a identity used by the module's C++ contract while still
    // accepting the canonical decimal form emitted by the encoder.
    if (text.empty())
        return {invalid(name), {}};
    return {foundation::Result::success(), Id{text}};
}

foundation::ResultValue<feature::FeatureContract> feature_contract_value(const Json& value) {
    if (!value.is_object())
        return {invalid("feature contract"), {}};
    feature::FeatureContract result{};
    const auto definition_id = feature_id_value<feature::FeatureDefinitionId>(value, "definition_id");
    const auto version = version_value(value, "version");
    const auto domains = feature_domains_value(value.value("domains", Json{}));
    if (!definition_id || !version || !domains || version.value.major == 0 || domains.value == 0 ||
        version.value.major > std::numeric_limits<uint16_t>::max() ||
        version.value.minor > std::numeric_limits<uint16_t>::max() ||
        version.value.patch > std::numeric_limits<uint16_t>::max())
        return {invalid("feature contract identity"), {}};
    result.definition_id = definition_id.value;
    result.version = {static_cast<uint16_t>(version.value.major),
                      static_cast<uint16_t>(version.value.minor),
                      static_cast<uint16_t>(version.value.patch)};
    result.domains = domains.value;
    const auto structure = u32_value<uint32_t>(value, "structure");
    const auto execution = u32_value<uint32_t>(value, "execution");
    const auto state = u32_value<uint32_t>(value, "state");
    const auto failure = u32_value<uint32_t>(value, "failure");
    if (!structure || !execution || !state || !failure ||
        structure.value > static_cast<uint32_t>(feature::FeatureStructure::kComposite) ||
        execution.value > static_cast<uint32_t>(feature::ExecutionKind::kContinuous) ||
        state.value > static_cast<uint32_t>(feature::StatePolicy::kExternalSnapshot) ||
        failure.value > static_cast<uint32_t>(feature::FailurePolicy::kCompensate))
        return {invalid("feature contract enum"), {}};
    result.structure = static_cast<feature::FeatureStructure>(structure.value);
    result.execution = static_cast<feature::ExecutionKind>(execution.value);
    result.state = static_cast<feature::StatePolicy>(state.value);
    result.failure = static_cast<feature::FailurePolicy>(failure.value);
    if (feature::owns_state_payload(result.state) || value.contains("state_payload")) {
        if (!value.contains("state_payload") || !value.at("state_payload").is_object())
            return {invalid("feature state payload contract"), {}};
        const auto& payload = value.at("state_payload");
        const auto schema = version_value(payload, "schema_version");
        const auto max_size = u32_value<uint32_t>(payload, "max_size");
        if (!schema || !max_size || schema.value.major == 0 || max_size.value == 0 ||
            schema.value.major > std::numeric_limits<uint16_t>::max() ||
            schema.value.minor > std::numeric_limits<uint16_t>::max() ||
            schema.value.patch > std::numeric_limits<uint16_t>::max())
            return {invalid("feature state payload contract"), {}};
        result.state_payload = {{static_cast<uint16_t>(schema.value.major), static_cast<uint16_t>(schema.value.minor),
                                 static_cast<uint16_t>(schema.value.patch)}, max_size.value};
    }

    const auto scope = feature_id_value<feature::ScopeId>(value, "scope");
    if (!scope)
        return {invalid("feature contract scope"), {}};
    result.scope.id = scope.value;

    if (!value.contains("clock") || !value.at("clock").is_object())
        return {invalid("feature contract clock"), {}};
    const auto& clock = value.at("clock");
    const auto clock_id = feature_id_value<feature::ClockId>(clock, "id");
    const auto frequency = u32_value<uint32_t>(clock, "frequency_hz");
    const auto backlog = u32_value<uint32_t>(clock, "max_backlog");
    const auto late_policy = u32_value<uint32_t>(clock, "late_data_policy");
    if (!clock_id || !frequency || !backlog || !late_policy ||
        late_policy.value > static_cast<uint32_t>(feature::LateDataPolicy::kDrop))
        return {invalid("feature contract clock"), {}};
    result.clock = {clock_id.value, frequency.value, backlog.value,
                    static_cast<feature::LateDataPolicy>(late_policy.value)};
    const auto capability_version = version_value(clock, "capability_version");
    const auto work_budget = u64_value(clock, "work_budget_ns");
    if (!capability_version || !work_budget || capability_version.value.major == 0 ||
        capability_version.value.major > std::numeric_limits<uint16_t>::max() ||
        capability_version.value.minor > std::numeric_limits<uint16_t>::max() ||
        capability_version.value.patch > std::numeric_limits<uint16_t>::max() ||
        !clock.contains("timing") || !clock.at("timing").is_object())
        return {invalid("feature contract explicit clock"), {}};
    const auto& timing = clock.at("timing");
    const auto phase = u64_value(timing, "phase_ns");
    const auto deadline = u64_value(timing, "relative_deadline_ns");
    const auto deadline_policy = u32_value<uint32_t>(timing, "deadline_policy");
    if (!phase || !deadline || !deadline_policy ||
        (deadline_policy.value != static_cast<uint32_t>(feature::DeadlinePolicy::kObserve) &&
         deadline_policy.value != static_cast<uint32_t>(feature::DeadlinePolicy::kRejectCommit)))
        return {invalid("feature contract clock timing"), {}};
    result.clock.capability_version = {static_cast<uint16_t>(capability_version.value.major),
        static_cast<uint16_t>(capability_version.value.minor), static_cast<uint16_t>(capability_version.value.patch)};
    result.clock.timing = feature::ClockTiming{phase.value, deadline.value,
        static_cast<feature::DeadlinePolicy>(deadline_policy.value)};
    result.clock.work_budget_ns = work_budget.value;
    if (const auto valid = feature::validate_clock_contract(result.clock, result.structure); !valid)
        return {valid, {}};

    if (!value.contains("permissions") || !value.at("permissions").is_object())
        return {invalid("feature contract permissions"), {}};
    const auto capability_mask = u64_value(value.at("permissions"), "capability_mask");
    const auto permission_version = u32_value<uint32_t>(value.at("permissions"), "version");
    if (!capability_mask || !permission_version)
        return {invalid("feature contract permissions"), {}};
    result.permissions = {capability_mask.value, permission_version.value};

    if (!value.contains("retry") || !value.at("retry").is_object())
        return {invalid("feature contract retry"), {}};
    const auto retry_max = u32_value<uint32_t>(value.at("retry"), "max_attempts");
    const auto retry_backoff = u32_value<uint32_t>(value.at("retry"), "backoff_ticks");
    if (!retry_max || !retry_backoff)
        return {invalid("feature contract retry"), {}};
    result.retry = {retry_max.value, retry_backoff.value};
    const auto continuation = bool_value(value, "has_continuation");
    const auto child_process = bool_value(value, "allows_child_process");
    if (!continuation || !child_process)
        return {invalid("feature contract continuation"), {}};
    result.has_continuation = continuation.value;
    result.allows_child_process = child_process.value;

    if (!value.contains("children") || !value.at("children").is_array() ||
        !value.contains("ports") || !value.at("ports").is_array() ||
        !value.contains("side_effects") || !value.at("side_effects").is_array() ||
        !value.contains("child_port_mappings") || !value.at("child_port_mappings").is_array())
        return {invalid("feature contract arrays"), {}};
    for (const auto& item : value.at("children")) {
        const auto child = feature_id_value<feature::FeatureDefinitionId>(Json{{"id", item}}, "id");
        if (!child)
            return {invalid("feature contract child"), {}};
        result.children.push_back(child.value);
    }
    for (const auto& item : value.at("ports")) {
        const auto id = feature_id_value<feature::PortId>(item, "id");
        const auto schema = feature_id_value<feature::ValueSchemaId>(item, "schema");
        const auto version_number = u32_value<uint32_t>(item, "version");
        const auto direction = u32_value<uint32_t>(item, "direction");
        if (!id || !schema || !version_number || !direction ||
            direction.value > static_cast<uint32_t>(feature::PortDirection::kOutput))
            return {invalid("feature contract port"), {}};
        result.ports.push_back({id.value, static_cast<feature::PortDirection>(direction.value),
                                schema.value, version_number.value});
    }
    if (value.contains("parameters")) {
        if (!value.at("parameters").is_array())
            return {invalid("feature contract parameters"), {}};
        for (const auto& item : value.at("parameters")) {
            const auto id = feature_id_value<feature::ParameterId>(item, "id");
            const auto version_number = u32_value<uint32_t>(item, "version");
            const auto max_size = u32_value<uint32_t>(item, "max_size");
            const auto required = bool_value(item, "required");
            if (!id || !version_number || !max_size || !required)
                return {invalid("feature contract parameter"), {}};
            feature::ParameterContract parameter{id.value, version_number.value, max_size.value, required.value};
            if (item.contains("metadata")) {
                const auto parsed = feature_parameter_metadata_value(item.at("metadata"));
                if (!parsed) return {parsed.result, {}};
                parameter.metadata = parsed.value;
            }
            result.parameters.push_back(std::move(parameter));
        }
    }
    for (const auto& item : value.at("side_effects")) {
        const auto id = feature_id_value<feature::SideEffectId>(item, "id");
        const auto version_number = u32_value<uint32_t>(item, "version");
        const auto kind = u32_value<uint32_t>(item, "kind");
        const auto commit = u32_value<uint32_t>(item, "commit");
        if (!id || !version_number || !kind || !commit ||
            kind.value > static_cast<uint32_t>(feature::SideEffectKind::kExternalIo) ||
            commit.value > static_cast<uint32_t>(feature::CommitPolicy::kExternalOwner))
            return {invalid("feature contract side effect"), {}};
        result.side_effects.push_back({id.value, version_number.value,
                                       static_cast<feature::SideEffectKind>(kind.value),
                                       static_cast<feature::CommitPolicy>(commit.value)});
    }
    for (const auto& item : value.at("child_port_mappings")) {
        const auto parent = feature_id_value<feature::PortId>(item, "parent_port");
        const auto child_definition = feature_id_value<feature::FeatureDefinitionId>(item, "child_definition");
        const auto child_port = feature_id_value<feature::PortId>(item, "child_port");
        const auto direction = u32_value<uint32_t>(item, "direction");
        if (!parent || !child_definition || !child_port || !direction ||
            direction.value > static_cast<uint32_t>(feature::PortDirection::kOutput))
            return {invalid("feature contract child port mapping"), {}};
        result.child_port_mappings.push_back({parent.value, child_definition.value, child_port.value,
                                              static_cast<feature::PortDirection>(direction.value)});
    }
    return {foundation::Result::success(), std::move(result)};
}

foundation::ResultValue<feature::FeatureCatalogEntry> feature_catalog_entry_value(const Json& value) {
    const auto id = feature_definition_id_value(value.value("id", Json{}));
    const auto owner = id_value<feature::ModuleIdentity>(value, "owner_module");
    const auto version = version_value(value, "version");
    const auto domains = value.contains("domains") ? feature_domains_value(value.at("domains"))
                                                    : foundation::ResultValue<feature::FeatureDomainMask>{invalid("feature domains"), {}};
    if (!id || !owner || !version || !domains || version.value.major == 0 || domains.value == 0 ||
        version.value.major > std::numeric_limits<uint16_t>::max() ||
        version.value.minor > std::numeric_limits<uint16_t>::max() ||
        version.value.patch > std::numeric_limits<uint16_t>::max())
        return {invalid("feature catalog entry"), {}};
    feature::FeatureCatalogEntry result{};
    result.definition_id = id.value;
    result.owner_module = owner.value;
    result.version = {static_cast<uint16_t>(version.value.major), static_cast<uint16_t>(version.value.minor),
                      static_cast<uint16_t>(version.value.patch)};
    result.domains = domains.value;
    if (value.contains("execution")) {
        if (value.at("execution").is_string()) {
            const auto execution = value.at("execution").get<std::string>();
            if (execution == "call") result.execution = feature::ExecutionKind::kCall;
            else if (execution == "process") result.execution = feature::ExecutionKind::kProcess;
            else if (execution == "continuous") result.execution = feature::ExecutionKind::kContinuous;
            else return {invalid("feature execution"), {}};
        } else {
            const auto execution = u32_value<uint32_t>(value, "execution");
            if (!execution || execution.value > static_cast<uint32_t>(feature::ExecutionKind::kContinuous))
                return {invalid("feature execution"), {}};
            result.execution = static_cast<feature::ExecutionKind>(execution.value);
        }
    }
    if (value.contains("state")) {
        if (value.at("state").is_string()) {
            const auto state = value.at("state").get<std::string>();
            if (state == "stateless") result.state = feature::StatePolicy::kStateless;
            else if (state == "instance_state") result.state = feature::StatePolicy::kInstanceState;
            else if (state == "process_state") result.state = feature::StatePolicy::kProcessState;
            else if (state == "external_snapshot") result.state = feature::StatePolicy::kExternalSnapshot;
            else return {invalid("feature state"), {}};
        } else {
            const auto state = u32_value<uint32_t>(value, "state");
            if (!state || state.value > static_cast<uint32_t>(feature::StatePolicy::kExternalSnapshot))
                return {invalid("feature state"), {}};
            result.state = static_cast<feature::StatePolicy>(state.value);
        }
    }
    if (!value.contains("clock"))
        return {invalid("feature clock"), {}};
    const auto& clock = value.at("clock");
    if (clock.is_object()) {
        const auto clock_id = feature_definition_id_value(clock.value("id", Json{}));
        if (!clock_id)
            return {invalid("feature clock id"), {}};
        result.clock = feature::ClockId(clock_id.value.value());
    } else {
        const auto clock_id = feature_definition_id_value(clock);
        if (!clock_id)
            return {invalid("feature clock id"), {}};
        result.clock = feature::ClockId(clock_id.value.value());
    }
    if (!result.clock.is_valid())
        return {invalid("feature clock"), {}};
    if (value.contains("contract_digest")) {
        const auto digest = u64_value(value, "contract_digest");
        if (!digest)
            return {invalid("feature contract digest"), {}};
        result.contract_digest = digest.value;
    }
    if (value.contains("ports")) {
        if (!value.at("ports").is_array())
            return {invalid("feature ports"), {}};
        for (const auto& item : value.at("ports")) {
            const auto id = feature_definition_id_value(item.value("id", Json{}));
            const auto schema = feature_definition_id_value(item.value("schema", Json{}));
            const auto version_number = u32_value<uint32_t>(item, "version");
            if (!id || !schema || !version_number || version_number.value == 0 ||
                !item.contains("direction"))
                return {invalid("feature port"), {}};
            feature::PortDirection direction{};
            const auto& raw_direction = item.at("direction");
            if (raw_direction.is_string()) {
                const auto direction_name = raw_direction.get<std::string>();
                if (direction_name == "input") direction = feature::PortDirection::kInput;
                else if (direction_name == "output") direction = feature::PortDirection::kOutput;
                else return {invalid("feature port direction"), {}};
            } else {
                const auto direction_number = u32_value<uint32_t>(item, "direction");
                if (!direction_number || direction_number.value > static_cast<uint32_t>(feature::PortDirection::kOutput))
                    return {invalid("feature port direction"), {}};
                direction = static_cast<feature::PortDirection>(direction_number.value);
            }
            result.ports.push_back({feature::PortId(id.value.value()), direction,
                                    feature::ValueSchemaId(schema.value.value()), version_number.value});
        }
    }
    if (value.contains("contract")) {
        const auto contract = feature_contract_value(value.at("contract"));
        if (!contract)
            return {contract.result, {}};
        result.contract = contract.value;
        if (result.contract.definition_id != result.definition_id ||
            result.contract.version != result.version ||
            result.contract.domains != result.domains ||
            result.contract.execution != result.execution ||
            result.contract.state != result.state ||
            result.contract.clock.id != result.clock ||
            result.contract.ports != result.ports)
            return {invalid("feature contract summary"), {}};
        if (const auto valid = feature::validate_feature_contract(result.contract); !valid)
            return {valid, {}};
    }
    return {foundation::Result::success(), result};
}

}  // namespace
namespace detail {
nlohmann::json encode_catalog_entry_json(const FeatureCatalogEntry& entry) {
    return feature_catalog_entry_json(entry);
}
foundation::ResultValue<FeatureCatalogEntry> decode_catalog_entry_json(const nlohmann::json& value) {
    return feature_catalog_entry_value(value);
}
}  // namespace detail

foundation::ResultValue<std::string> encode_feature_contract_json(const FeatureContract& contract) {
    try {
        auto result = feature_contract_json(contract).dump();
        if (result.size() > kMaxContractBytes)
            return {foundation::invalid_argument("Feature contract JSON is too large"), {}};
        return {foundation::Result::success(), std::move(result)};
    } catch (const std::exception& error) {
        return {foundation::invalid_argument(std::string("Feature contract JSON is invalid: ") + error.what()), {}};
    }
}
foundation::ResultValue<FeatureContract> decode_feature_contract_json(std::string_view json) {
    if (json.size() > kMaxContractBytes)
        return {foundation::invalid_argument("Feature contract JSON is too large"), {}};
    try {
        return feature_contract_value(Json::parse(json));
    } catch (const std::exception& error) {
        return {foundation::invalid_argument(std::string("Feature contract JSON is invalid: ") + error.what()), {}};
    }
}
}  // namespace aeg::feature
