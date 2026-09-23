/** @file pipeline_authoring.cpp @brief Authoring catalog implementation. */
#include "pipeline_authoring.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>
#include <unordered_set>
namespace aeg::pipeline {
namespace {
bool finite(double value) noexcept {
    return std::isfinite(value);
}
bool valid_kind(ParameterKind value) noexcept {
    return static_cast<uint8_t>(value) <= static_cast<uint8_t>(ParameterKind::kAsset);
}
bool valid_type_kind(AuthoringTypeKind value) noexcept {
    return static_cast<uint8_t>(value) <= static_cast<uint8_t>(AuthoringTypeKind::kRenderPass);
}
bool valid_port_kind(PortValueKind value) noexcept {
    return static_cast<uint8_t>(value) <= static_cast<uint8_t>(PortValueKind::kTexture);
}
// Mirrors Tooling Uuid::parse without introducing a Pipeline -> Tooling dependency.
bool valid_asset_reference(const AssetReference& reference) noexcept {
    const auto& uuid = reference.uuid;
    if (uuid.size() != 36)
        return false;
    for (size_t index = 0; index < uuid.size(); ++index) {
        const char value = uuid[index];
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value != '-')
                return false;
        } else if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
                     (value >= 'A' && value <= 'F'))) {
            return false;
        }
    }
    return true;
}
bool finite_value(const ParameterValue& value) {
    if (const auto* scalar = std::get_if<double>(&value))
        return finite(*scalar);
    if (const auto* vector = std::get_if<std::vector<double>>(&value))
        return std::all_of(vector->begin(), vector->end(), finite);
    return true;
}
bool matches(const ParameterValue& value, ParameterKind kind) {
    switch (kind) {
        case ParameterKind::kBool:
            return std::holds_alternative<bool>(value);
        case ParameterKind::kInt64:
            return std::holds_alternative<int64_t>(value);
        case ParameterKind::kUInt64:
            return std::holds_alternative<uint64_t>(value);
        case ParameterKind::kFloat64:
            return std::holds_alternative<double>(value) && finite_value(value);
        case ParameterKind::kString:
        case ParameterKind::kEnum:
            return std::holds_alternative<std::string>(value);
        case ParameterKind::kVector:
            return std::holds_alternative<std::vector<double>>(value) && finite_value(value);
        case ParameterKind::kAsset:
            return std::holds_alternative<AssetReference>(value) &&
                   valid_asset_reference(std::get<AssetReference>(value));
    }
    return false;
}
foundation::Result validate_parameters(std::span<const ParameterDescriptor> parameters) {
    std::unordered_set<uint64_t> ids;
    std::unordered_set<std::string> names;
    for (const auto& item : parameters) {
        if (!item.id || item.name.empty() || item.display_name.empty() || !item.schema_version ||
            !valid_kind(item.kind) || !ids.insert(item.id).second || !names.insert(item.name).second ||
            !matches(item.default_value, item.kind))
            return foundation::invalid_argument("invalid parameter descriptor");
        if (!finite_value(item.default_value) || (item.minimum && !finite(*item.minimum)) ||
            (item.maximum && !finite(*item.maximum)) || (item.minimum && item.maximum && *item.minimum > *item.maximum))
            return foundation::invalid_argument("non-finite or inverted parameter bounds");
        if ((item.minimum || item.maximum) && item.kind != ParameterKind::kFloat64)
            return foundation::invalid_argument("bounds require float parameter");
        if (item.kind == ParameterKind::kFloat64 &&
            ((item.minimum && std::get<double>(item.default_value) < *item.minimum) ||
             (item.maximum && std::get<double>(item.default_value) > *item.maximum)))
            return foundation::invalid_argument("default value is outside bounds");
        std::unordered_set<std::string> labels;
        for (const auto& label : item.enum_values)
            if (label.empty() || !labels.insert(label).second)
                return foundation::invalid_argument("empty or duplicate enum label");
        if (item.kind != ParameterKind::kEnum && !item.enum_values.empty())
            return foundation::invalid_argument("enum labels require enum parameter");
        if (item.kind == ParameterKind::kEnum &&
            (item.enum_values.empty() ||
             std::find(item.enum_values.begin(), item.enum_values.end(), std::get<std::string>(item.default_value)) ==
                 item.enum_values.end()))
            return foundation::invalid_argument("enum parameter has invalid values");
    }
    return foundation::Result::success();
}
bool valid_resource_constraints(const ResourceConstraints& value) {
    if (static_cast<uint8_t>(value.resource_class) > static_cast<uint8_t>(ResourceClass::kTexture))
        return false;
    if (value.resource_class == ResourceClass::kNone)
        return value.formats.empty() && value.size_rule.empty() && value.sample_count == 0 &&
               value.capability_mask == 0 && value.usage_mask == 0;
    const uint64_t capabilities = value.resource_class == ResourceClass::kBuffer
                                      ? (static_cast<uint64_t>(rhi::BufferCapability::kAccelerationInput) << 1) - 1
                                      : (static_cast<uint64_t>(rhi::TextureCapability::kDepthStencil) << 1) - 1;
    constexpr auto kUsages =
        (uint64_t{1} << (static_cast<uint8_t>(rhi::ResourceUsage::kAccelerationStructureBuildWrite) + 1)) - 1;
    if ((value.capability_mask & ~capabilities) || (value.usage_mask & ~kUsages))
        return false;
    if (value.sample_count && ((value.sample_count & (value.sample_count - 1)) || value.sample_count > 64))
        return false;
    if (value.resource_class == ResourceClass::kBuffer && value.sample_count > 1)
        return false;
    std::unordered_set<std::string> formats;
    for (const auto& format : value.formats)
        if (format.empty() || !formats.insert(format).second)
            return false;
    return true;
}

foundation::Result validate_ports(std::span<const PortDescriptor> ports) {
    std::unordered_set<uint64_t> ids;
    std::unordered_set<std::string> names;
    for (const auto& port : ports) {
        if (!port.id || port.name.empty() || !port.schema_id || !port.schema_version || !port.binding_id ||
            (port.required && port.min_connections == 0) || !valid_port_kind(port.value_kind) ||
            static_cast<uint8_t>(port.direction) > static_cast<uint8_t>(PortDirection::kOutput) ||
            port.min_connections > port.max_connections || !ids.insert(port.id).second ||
            !names.insert(port.name).second || !valid_resource_constraints(port.resource_constraints))
            return foundation::invalid_argument("invalid port descriptor");
        const auto resource_class = port.resource_constraints.resource_class;
        const bool consistent = [&] {
            switch (port.value_kind) {
                case PortValueKind::kArtifact:
                case PortValueKind::kRenderState:
                    return resource_class == ResourceClass::kNone;
                case PortValueKind::kRenderResource:
                    return resource_class != ResourceClass::kNone;
                case PortValueKind::kBuffer:
                    return resource_class == ResourceClass::kBuffer;
                case PortValueKind::kTexture:
                    return resource_class == ResourceClass::kTexture;
            }
            return false;
        }();
        if (!consistent)
            return foundation::invalid_argument("port value kind and resource class disagree");
    }
    return foundation::Result::success();
}

foundation::Result validate_stages(std::span<const AuthoringStage> stages) {
    std::unordered_set<uint64_t> ids;
    std::unordered_set<std::string> names;
    for (const auto& stage : stages)
        if (!stage.id.is_valid() || stage.name.empty() || !stage.phase.is_valid() ||
            static_cast<uint8_t>(stage.execution_domain) > static_cast<uint8_t>(ExecutionDomain::kGpu) ||
            static_cast<uint8_t>(stage.time_policy) > static_cast<uint8_t>(TimePolicy::kShutdown) ||
            !ids.insert(stage.id.value()).second || !names.insert(stage.name).second)
            return foundation::invalid_argument("invalid or duplicate authoring stage");
    return foundation::Result::success();
}

foundation::Result validate_artifacts(std::span<const AuthoringArtifact> artifacts) {
    std::unordered_set<uint64_t> ids;
    std::unordered_set<std::string> names;
    for (const auto& artifact : artifacts)
        if (!artifact.id.is_valid() || artifact.name.empty() || !artifact.type_id.is_valid() || !artifact.version ||
            static_cast<uint8_t>(artifact.owner_scope) > static_cast<uint8_t>(PipelineScope::kEditor) ||
            (artifact.visible_to & ~kAllPipelineScopes) ||
            (artifact.visible_to & ((1u << static_cast<uint8_t>(artifact.owner_scope)) - 1u)) ||
            static_cast<uint8_t>(artifact.storage) > static_cast<uint8_t>(ArtifactStorage::kCompletionToken) ||
            static_cast<uint8_t>(artifact.lifetime) > static_cast<uint8_t>(ArtifactLifetime::kEngine) ||
            static_cast<uint8_t>(artifact.ownership) > static_cast<uint8_t>(OwnershipPolicy::kExternalBorrowed) ||
            !ids.insert(artifact.id.value()).second || !names.insert(artifact.name).second)
            return foundation::invalid_argument("invalid or duplicate authoring artifact");
    return foundation::Result::success();
}

// Referenced Feature/Pass schemas may be registered later. Check owned
// values here; schema/type/version resolution belongs to document validation.
bool valid_default_parameters(const ParameterBlock& parameters) {
    for (const auto& [id, value] : parameters) {
        if (!id || !finite_value(value))
            return false;
        if (const auto* asset = std::get_if<AssetReference>(&value); asset && !valid_asset_reference(*asset))
            return false;
    }
    return true;
}

foundation::Result validate_pipeline_declaration(const PipelineAuthoringDeclaration& declaration) {
    if (auto result = validate_stages(declaration.stages); !result)
        return result;
    if (auto result = validate_artifacts(declaration.artifacts); !result)
        return result;
    std::map<StageId, uint32_t> incoming;
    for (const auto& stage : declaration.stages)
        incoming.emplace(stage.id, 0);
    std::unordered_set<uint64_t> point_ids;
    std::unordered_set<std::string> point_names;
    constexpr auto kKnownFlags =
        static_cast<uint32_t>(FeaturePointFlags::kMayCreateArtifacts | FeaturePointFlags::kMayCreateGraphicsPass |
                              FeaturePointFlags::kMayCreateComputePass | FeaturePointFlags::kMayCreateCopyPass |
                              FeaturePointFlags::kMayUseDedicatedQueue | FeaturePointFlags::kMayWriteFinalOutput |
                              FeaturePointFlags::kMayPublishSnapshot);
    for (const auto& point : declaration.feature_points)
        if (!point.id.is_valid() || point.name.empty() || !incoming.contains(point.anchor_stage) ||
            static_cast<uint8_t>(point.position) > static_cast<uint8_t>(FeaturePointPosition::kAfter) ||
            !point.domains || (point.domains & ~kAllFeatureDomains) || !point.scopes ||
            (point.scopes & ~kAllPipelineScopes) || (static_cast<uint32_t>(point.flags) & ~kKnownFlags) ||
            !point_ids.insert(point.id.value()).second || !point_names.insert(point.name).second)
            return foundation::invalid_argument("invalid or duplicate authoring feature point");
    std::set<std::pair<StageId, StageId>> orderings;
    for (const auto& ordering : declaration.fixed_orderings) {
        if (!incoming.contains(ordering.before) || !incoming.contains(ordering.after) ||
            ordering.before == ordering.after || !orderings.emplace(ordering.before, ordering.after).second)
            return foundation::invalid_argument("invalid or duplicate stage ordering");
        ++incoming.at(ordering.after);
    }
    std::vector<StageId> ready;
    for (const auto& [stage, count] : incoming)
        if (!count)
            ready.push_back(stage);
    for (size_t index = 0; index < ready.size(); ++index)
        for (const auto& [before, after] : orderings)
            if (before == ready[index] && --incoming.at(after) == 0)
                ready.push_back(after);
    if (ready.size() != incoming.size())
        return foundation::invalid_argument("cyclic stage ordering");
    std::unordered_set<std::string> nodes;
    for (const auto& feature : declaration.default_features)
        if (feature.node_id.empty() || !nodes.insert(feature.node_id).second || !feature.type.is_valid() ||
            !feature.version.is_valid() || !point_ids.contains(feature.point.value()) ||
            !valid_default_parameters(feature.parameters))
            return foundation::invalid_argument("invalid or duplicate default feature");
    return foundation::Result::success();
}

foundation::Result validate_default_edges(const TypeCatalogEntry& entry,
                                          const FeatureAuthoringDeclaration& declaration) {
    using BindingKey = std::pair<DocumentEdgeKind, uint64_t>;
    using EndpointKey = std::pair<std::string, uint64_t>;
    std::map<BindingKey, ResourceClass> bindings;
    for (const auto& artifact : declaration.artifacts)
        bindings.emplace(BindingKey{DocumentEdgeKind::kArtifact, artifact.id.value()}, ResourceClass::kNone);
    for (const auto& port : entry.ports) {
        const auto kind = port.value_kind == PortValueKind::kArtifact      ? DocumentEdgeKind::kArtifact
                          : port.value_kind == PortValueKind::kRenderState ? DocumentEdgeKind::kRenderState
                                                                           : DocumentEdgeKind::kRenderResource;
        const auto [found, added] =
            bindings.emplace(BindingKey{kind, port.binding_id}, port.resource_constraints.resource_class);
        if (!added && found->second != port.resource_constraints.resource_class)
            return foundation::invalid_argument("conflicting resource classes for default binding");
    }
    std::unordered_set<std::string> nodes;
    for (const auto& pass : declaration.default_passes)
        nodes.insert(pass.node_id);
    std::map<std::string, const DefaultResourceNode*> resource_nodes;
    for (const auto& resource : declaration.default_resources) {
        nodes.insert(resource.node_id);
        resource_nodes.emplace(resource.node_id, &resource);
        const auto [found, added] =
            bindings.emplace(BindingKey{DocumentEdgeKind::kRenderResource, resource.resource_id.value()},
                             resource.constraints.resource_class);
        if (!added && found->second != resource.constraints.resource_class)
            return foundation::invalid_argument("conflicting resource classes for default binding");
    }
    std::unordered_set<std::string> edge_ids;
    std::set<std::tuple<DocumentEdgeKind, EndpointKey, EndpointKey>> connections;
    std::set<EndpointKey> destinations;
    std::map<BindingKey, EndpointKey> producers;
    std::map<EndpointKey, std::pair<PortDirection, BindingKey>> port_uses;
    for (const auto& edge : declaration.default_edges) {
        if (edge.edge_id.empty() || !edge_ids.insert(edge.edge_id).second ||
            static_cast<uint8_t>(edge.kind) > static_cast<uint8_t>(DocumentEdgeKind::kRenderState) ||
            !nodes.contains(edge.source.node_id) || !nodes.contains(edge.destination.node_id))
            return foundation::invalid_argument("invalid default edge identity, kind, or node reference");
        const EndpointKey source{edge.source.node_id, edge.source.port_id};
        const EndpointKey destination{edge.destination.node_id, edge.destination.port_id};
        if (!connections.emplace(edge.kind, source, destination).second)
            return foundation::invalid_argument("duplicate default connection");
        if (edge.kind == DocumentEdgeKind::kOrdering) {
            if (edge.binding_id || source.second || destination.second || source.first == destination.first ||
                resource_nodes.contains(source.first) || resource_nodes.contains(destination.first))
                return foundation::invalid_argument("ordering edge requires distinct pass nodes without data ports");
            const auto source_pass = std::find_if(declaration.default_passes.begin(), declaration.default_passes.end(),
                                                  [&](const auto& pass) { return pass.node_id == source.first; });
            const auto destination_pass =
                std::find_if(declaration.default_passes.begin(), declaration.default_passes.end(),
                             [&](const auto& pass) { return pass.node_id == destination.first; });
            if (source_pass == declaration.default_passes.end() ||
                destination_pass == declaration.default_passes.end() ||
                source_pass->part_id != destination_pass->part_id)
                return foundation::invalid_argument("ordering edge must stay within one Render Part");
            continue;
        }
        const BindingKey binding{edge.kind, edge.binding_id};
        if (!edge.binding_id || !bindings.contains(binding) || !source.second || !destination.second)
            return foundation::invalid_argument("default data edge requires declared binding and stable ports");
        for (const auto& [endpoint, direction] :
             {std::pair{source, PortDirection::kOutput}, std::pair{destination, PortDirection::kInput}}) {
            if (const auto resource = resource_nodes.find(endpoint.first); resource != resource_nodes.end()) {
                if (edge.kind != DocumentEdgeKind::kRenderResource ||
                    endpoint.second != resource->second->resource_id.value() || endpoint.second != edge.binding_id)
                    return foundation::invalid_argument("default resource endpoint does not match its binding");
            } else {
                const auto usage = std::pair{direction, binding};
                const auto [previous, added] = port_uses.emplace(endpoint, usage);
                if (!added && previous->second != usage)
                    return foundation::invalid_argument("default port has conflicting direction, kind, or binding");
            }
        }
        if (!destinations.insert(destination).second)
            return foundation::invalid_argument("default input has multiple connections");
        // Resource nodes relay a binding. Only a Pass output is its producer;
        // that same output may fan out to multiple input ports.
        if (!resource_nodes.contains(source.first)) {
            const auto [previous, added] = producers.emplace(binding, source);
            if (!added && previous->second != source)
                return foundation::invalid_argument("default binding has multiple producers");
        }
    }
    // Pass type/version/schema and declared port direction/cardinality require
    // the complete catalog in A4. Do not make registration order semantic here.
    return foundation::Result::success();
}

foundation::Result validate_feature_declaration(const TypeCatalogEntry& entry,
                                                const FeatureAuthoringDeclaration& declaration) {
    if (auto result = validate_stages(declaration.stages); !result)
        return result;
    if (auto result = validate_artifacts(declaration.artifacts); !result)
        return result;
    std::unordered_set<uint32_t> part_indices;
    std::map<std::string, FeatureDomain> parts;
    for (uint32_t part_position = 0; part_position < declaration.parts.size(); ++part_position) {
        const auto& part = declaration.parts[part_position];
        if (part.name.empty() || static_cast<uint8_t>(part.domain) > static_cast<uint8_t>(FeatureDomain::kStream) ||
            part.part_index != part_position || !part_indices.insert(part.part_index).second ||
            !parts.emplace(part.name, part.domain).second)
            return foundation::invalid_argument("invalid or duplicate authoring part");
    }
    std::unordered_set<uint64_t> allowed_passes;
    for (const auto type : declaration.allowed_pass_types)
        if (!type.is_valid() || !allowed_passes.insert(type.value()).second)
            return foundation::invalid_argument("invalid or duplicate allowed pass type");
    std::unordered_set<std::string> nodes;
    for (const auto& pass : declaration.default_passes) {
        if (pass.node_id.empty() || !nodes.insert(pass.node_id).second || !pass.type.is_valid() ||
            !pass.version.is_valid() || !allowed_passes.contains(pass.type.value()) ||
            !valid_default_parameters(pass.parameters))
            return foundation::invalid_argument("invalid or duplicate default pass");
        if (parts.empty()) {
            if (!pass.part_id.empty() || (entry.domains && !(entry.domains & to_mask(FeatureDomain::kRender))))
                return foundation::invalid_argument("default pass requires a render feature");
        } else if (!parts.contains(pass.part_id) || parts.at(pass.part_id) != FeatureDomain::kRender) {
            return foundation::invalid_argument("default pass requires a declared render part");
        }
    }
    std::unordered_set<uint64_t> resources;
    for (const auto& resource : declaration.default_resources) {
        if (resource.node_id.empty() || !nodes.insert(resource.node_id).second || !resource.resource_id.is_valid() ||
            !resources.insert(resource.resource_id.value()).second ||
            resource.constraints.resource_class == ResourceClass::kNone ||
            !valid_resource_constraints(resource.constraints))
            return foundation::invalid_argument("invalid or duplicate default resource");
    }
    return validate_default_edges(entry, declaration);
}

foundation::Result validate_declaration(const TypeCatalogEntry& entry) {
    switch (entry.key.kind) {
        case AuthoringTypeKind::kPipeline:
            if (const auto* value = std::get_if<PipelineAuthoringDeclaration>(&entry.declaration))
                return validate_pipeline_declaration(*value);
            break;
        case AuthoringTypeKind::kFeature:
            if (const auto* value = std::get_if<FeatureAuthoringDeclaration>(&entry.declaration))
                return validate_feature_declaration(entry, *value);
            break;
        case AuthoringTypeKind::kRenderPass:
            if (const auto* value = std::get_if<PassAuthoringDeclaration>(&entry.declaration)) {
                constexpr auto kQueues = (uint64_t{1} << (static_cast<uint8_t>(rhi::QueueClass::kPresent) + 1)) - 1;
                if (!value->type.is_valid() || value->type.value() != entry.key.id ||
                    (value->allowed_queue_mask & ~kQueues))
                    return foundation::invalid_argument("invalid pass declaration");
                return foundation::Result::success();
            }
            break;
    }
    return foundation::invalid_argument("catalog declaration does not match key kind");
}
foundation::Result validate_entry(const TypeCatalogEntry& entry) {
    if (!valid_type_kind(entry.key.kind) || !entry.key.id || !entry.version.is_valid() ||
        !entry.owner_module.is_valid() || entry.name.empty() || entry.display_name.empty() ||
        (entry.domains && (entry.domains & ~kAllFeatureDomains)) || (entry.scopes & ~kAllPipelineScopes))
        return foundation::invalid_argument("invalid catalog entry");
    if (auto result = validate_parameters(entry.parameters); !result)
        return result;
    if (auto result = validate_ports(entry.ports); !result)
        return result;
    return validate_declaration(entry);
}
}  // namespace
foundation::Result TypeCatalog::add(const TypeCatalogEntry& entry) {
    if (auto result = validate_entry(entry); !result)
        return result;
    if (entries_by_key_.contains(entry.key))
        return foundation::invalid_argument("catalog key is already registered");
    for (const auto& [key, existing] : entries_by_key_)
        if (key.kind == entry.key.kind && existing.name == entry.name)
            return foundation::invalid_argument("catalog type name is already registered");
    entries_by_key_.emplace(entry.key, entry);
    snapshot_dirty_ = true;
    return foundation::Result::success();
}
const TypeCatalogEntry* TypeCatalog::find(AuthoringTypeKey key) const noexcept {
    const auto found = entries_by_key_.find(key);
    return found == entries_by_key_.end() ? nullptr : &found->second;
}
std::span<const TypeCatalogEntry> TypeCatalog::entries() const {
    if (snapshot_dirty_)
        rebuild_entries_snapshot();
    return entries_snapshot_;
}
void TypeCatalog::rebuild_entries_snapshot() const {
    entries_snapshot_.clear();
    entries_snapshot_.reserve(entries_by_key_.size());
    for (const auto& [key, entry] : entries_by_key_) {
        (void)key;
        entries_snapshot_.push_back(entry);
    }
    snapshot_dirty_ = false;
}
foundation::Result validate_parameter_block(const ParameterBlock& values, std::span<const ParameterDescriptor> schema) {
    if (auto result = validate_parameters(schema); !result)
        return result;
    for (const auto& [id, value] : values) {
        const auto item =
            std::find_if(schema.begin(), schema.end(), [id](const auto& descriptor) { return descriptor.id == id; });
        if (item == schema.end() || !matches(value, item->kind) || !finite_value(value))
            return foundation::invalid_argument("undeclared or invalid parameter value");
        if (item->kind == ParameterKind::kEnum && std::find(item->enum_values.begin(), item->enum_values.end(),
                                                            std::get<std::string>(value)) == item->enum_values.end())
            return foundation::invalid_argument("unknown enum value");
        if (item->kind == ParameterKind::kFloat64) {
            const auto number = std::get<double>(value);
            if ((item->minimum && number < *item->minimum) || (item->maximum && number > *item->maximum))
                return foundation::invalid_argument("parameter value is outside bounds");
        }
    }
    return foundation::Result::success();
}
}  // namespace aeg::pipeline
