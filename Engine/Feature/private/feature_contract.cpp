#include "feature_contract.hpp"

#include <algorithm>
#include <bit>
#include <vector>
#include <unordered_set>
#include <utility>

namespace aeg::feature {
namespace {
foundation::Result invalid(std::string message) {
    return foundation::Result::failure(foundation::ResultCode::kInvalidArgument, std::move(message));
}
}  // namespace

foundation::Result validate_clock_contract(const ClockContract& clock, FeatureStructure structure) {
    if (structure != FeatureStructure::kAtomic && structure != FeatureStructure::kComposite)
        return invalid("clock structure must be Atomic or Composite");
    if (!clock.id.is_valid() || clock.frequency_hz == 0 || clock.max_backlog == 0 ||
        clock.late_data_policy > LateDataPolicy::kDrop)
        return invalid("clock id, frequency and backlog policy are required");
    if (clock.capability_version != Version{1, 0, 0} || !clock.timing)
        return invalid("explicit supported clock capability v1.0.0 and timing are required");
    const auto& timing = *clock.timing;
    if (timing.relative_deadline_ns == 0 ||
        (timing.deadline_policy != DeadlinePolicy::kObserve &&
         timing.deadline_policy != DeadlinePolicy::kRejectCommit))
        return invalid("explicit clock deadline and known deadline policy are required");
    const uint64_t whole_period = 1'000'000'000ull / clock.frequency_hz;
    const auto remainder = 1'000'000'000ull % clock.frequency_hz;
    if (timing.phase_ns > whole_period || (timing.phase_ns == whole_period && remainder == 0))
        return invalid("clock phase must be strictly less than its nominal period");
    if ((structure == FeatureStructure::kAtomic && clock.work_budget_ns == 0) ||
        (structure == FeatureStructure::kComposite && clock.work_budget_ns != 0) ||
        clock.work_budget_ns > timing.relative_deadline_ns)
        return invalid("clock callback budget must match structure and fit its relative deadline");
    return foundation::Result::success();
}

uint64_t feature_contract_digest(const FeatureDefinition& definition) {
    uint64_t digest = 14695981039346656037ull;
    const auto mix = [&digest](uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            digest ^= (value >> shift) & 0xffu;
            digest *= 1099511628211ull;
        }
    };
    const auto& contract = definition.contract;
    mix(definition.id.value());
    mix(definition.owner_module.value());
    mix((static_cast<uint64_t>(definition.version.major) << 32u) |
        (static_cast<uint64_t>(definition.version.minor) << 16u) | definition.version.patch);
    mix(contract.definition_id.value());
    mix((static_cast<uint64_t>(contract.version.major) << 32u) |
        (static_cast<uint64_t>(contract.version.minor) << 16u) | contract.version.patch);
    mix(static_cast<uint64_t>(contract.structure));
    mix(contract.domains);
    mix(static_cast<uint64_t>(contract.execution));
    mix(static_cast<uint64_t>(contract.state));
    if (owns_state_payload(contract.state)) {
        mix(0x7374617465ull);
        mix((static_cast<uint64_t>(contract.state_payload.schema_version.major) << 32u) |
            (static_cast<uint64_t>(contract.state_payload.schema_version.minor) << 16u) |
            contract.state_payload.schema_version.patch);
        mix(contract.state_payload.max_size);
    }
    mix(contract.scope.id.value());
    mix(contract.clock.id.value());
    mix(contract.clock.frequency_hz);
    mix(contract.clock.max_backlog);
    mix(static_cast<uint64_t>(contract.clock.late_data_policy));
    mix(contract.clock.capability_version.major);
    mix(contract.clock.capability_version.minor);
    mix(contract.clock.capability_version.patch);
    mix(contract.clock.timing.has_value());
    if (contract.clock.timing) {
        mix(contract.clock.timing->phase_ns);
        mix(contract.clock.timing->relative_deadline_ns);
        mix(static_cast<uint64_t>(contract.clock.timing->deadline_policy));
    }
    mix(contract.clock.work_budget_ns);
    mix(contract.permissions.capability_mask);
    mix(contract.permissions.version);
    mix(static_cast<uint64_t>(contract.failure));
    mix(contract.retry.max_attempts);
    mix(contract.retry.backoff_ticks);
    mix(contract.has_continuation ? 1u : 0u);
    mix(contract.allows_child_process ? 1u : 0u);
    mix(contract.children.size());
    for (const auto child : contract.children) mix(child.value());
    std::vector<const CompositePortMapping*> mappings;
    mappings.reserve(contract.child_port_mappings.size());
    for (const auto& mapping : contract.child_port_mappings) mappings.push_back(&mapping);
    std::sort(mappings.begin(), mappings.end(), [](const auto* left, const auto* right) {
        if (left->parent_port.value() != right->parent_port.value())
            return left->parent_port.value() < right->parent_port.value();
        if (left->child_definition.value() != right->child_definition.value())
            return left->child_definition.value() < right->child_definition.value();
        if (left->child_port.value() != right->child_port.value())
            return left->child_port.value() < right->child_port.value();
        return static_cast<uint8_t>(left->direction) < static_cast<uint8_t>(right->direction);
    });
    mix(mappings.size());
    for (const auto* mapping : mappings) {
        mix(mapping->parent_port.value());
        mix(mapping->child_definition.value());
        mix(mapping->child_port.value());
        mix(static_cast<uint64_t>(mapping->direction));
    }

    std::vector<const PortContract*> ports;
    ports.reserve(contract.ports.size());
    for (const auto& port : contract.ports) ports.push_back(&port);
    std::sort(ports.begin(), ports.end(), [](const auto* left, const auto* right) {
        return left->id.value() < right->id.value();
    });
    mix(ports.size());
    for (const auto* port : ports) {
        mix(port->id.value());
        mix(static_cast<uint64_t>(port->direction));
        mix(port->schema.value());
        mix(port->version);
    }

    std::vector<const ParameterContract*> parameters;
    parameters.reserve(contract.parameters.size());
    for (const auto& parameter : contract.parameters) parameters.push_back(&parameter);
    std::sort(parameters.begin(), parameters.end(), [](const auto* left, const auto* right) {
        return left->id.value() < right->id.value();
    });
    mix(parameters.size());
    for (const auto* parameter : parameters) {
        mix(parameter->id.value());
        mix(parameter->version);
        mix(parameter->max_size);
        mix(parameter->required ? 1u : 0u);
        if (parameter->metadata) {
            mix(0x7479706564ull);
            mix(parameter_metadata_digest(*parameter->metadata));
        }
    }

    std::vector<const SideEffectContract*> effects;
    effects.reserve(contract.side_effects.size());
    for (const auto& effect : contract.side_effects) effects.push_back(&effect);
    std::sort(effects.begin(), effects.end(), [](const auto* left, const auto* right) {
        return left->id.value() < right->id.value();
    });
    mix(effects.size());
    for (const auto* effect : effects) {
        mix(effect->id.value());
        mix(effect->version);
        mix(static_cast<uint64_t>(effect->kind));
        mix(static_cast<uint64_t>(effect->commit));
    }
    return digest == 0 ? 1 : digest;
}

namespace {
foundation::Result validate_contract(const FeatureContract& contract, bool draft) {
    if (contract.execution != ExecutionKind::kCall && contract.execution != ExecutionKind::kProcess &&
        contract.execution != ExecutionKind::kContinuous)
        return invalid("Feature execution kind is unknown");
    if (contract.state != StatePolicy::kStateless && contract.state != StatePolicy::kInstanceState &&
        contract.state != StatePolicy::kProcessState && contract.state != StatePolicy::kExternalSnapshot)
        return invalid("Feature state policy is unknown");
    if (owns_state_payload(contract.state)) {
        if (contract.state_payload.schema_version.major == 0 || contract.state_payload.max_size == 0)
            return invalid("owned state requires a schema version and nonzero byte limit");
    } else if (contract.state_payload != StatePayloadContract{}) {
        return invalid("Feature without local state cannot declare a local payload contract");
    }
    if (!contract.definition_id.is_valid() || contract.version.major == 0) {
        return invalid("feature definition identity and version are required");
    }
    if (contract.domains == 0 || (contract.domains & ~kAllFeatureDomains) != 0) {
        return invalid("feature must declare one or more of the five leaf domains");
    }
    const auto domain_count = std::popcount(static_cast<unsigned int>(contract.domains));
    if (contract.structure == FeatureStructure::kAtomic && domain_count != 1) {
        return invalid("atomic feature must own exactly one leaf domain");
    }
    if (contract.structure == FeatureStructure::kAtomic && !contract.children.empty() &&
        !(contract.execution == ExecutionKind::kContinuous &&
          contract.state == StatePolicy::kProcessState && contract.allows_child_process)) {
        return invalid("atomic feature cannot declare children unless it is a child-process service");
    }
    if (contract.structure == FeatureStructure::kComposite && contract.children.empty() && !draft) {
        return invalid("composite feature must declare child definitions");
    }
    if (contract.structure == FeatureStructure::kAtomic && !contract.child_port_mappings.empty()) {
        return invalid("atomic feature cannot declare composite port mappings");
    }
    std::unordered_set<uint64_t> mapping_keys;
    for (const auto& mapping : contract.child_port_mappings) {
        if (!mapping.parent_port.is_valid() || !mapping.child_definition.is_valid() ||
            !mapping.child_port.is_valid()) {
            return invalid("composite port mappings require parent, child definition and child port ids");
        }
        uint64_t key = 14695981039346656037ull;
        key = (key ^ mapping.parent_port.value()) * 1099511628211ull;
        key = (key ^ mapping.child_definition.value()) * 1099511628211ull;
        key = (key ^ mapping.child_port.value()) * 1099511628211ull;
        key = (key ^ static_cast<uint64_t>(mapping.direction)) * 1099511628211ull;
        if (!mapping_keys.insert(key).second) return invalid("composite port mappings must be unique");
        if (std::find(contract.children.begin(), contract.children.end(), mapping.child_definition) ==
            contract.children.end()) {
            return invalid("composite port mapping references a non-child definition");
        }
        const auto parent_port = std::find_if(
            contract.ports.begin(), contract.ports.end(), [&mapping](const PortContract& port) {
                return port.id == mapping.parent_port && port.direction == mapping.direction;
            });
        if (parent_port == contract.ports.end())
            return invalid("composite port mapping parent port is undeclared or has the wrong direction");
    }
    std::unordered_set<FeatureDefinitionId, StableIdHash<FeatureDefinitionIdTag>> children;
    for (const auto child : contract.children) {
        if (!child.is_valid() || !children.insert(child).second) {
            return invalid("composite child definitions must be valid and unique");
        }
    }
    if (contract.scope.id.is_valid() == false) {
        return invalid("scope id is required");
    }
    if (const auto clock = validate_clock_contract(contract.clock, contract.structure); !clock) return clock;
    if (contract.permissions.version == 0) {
        return invalid("permission contract version is required");
    }
    std::unordered_set<PortId, StableIdHash<PortIdTag>> port_ids;
    for (const auto& port : contract.ports) {
        if (!port.id.is_valid() || !port.schema.is_valid() || port.version == 0) {
            return invalid("port id, schema and version are required");
        }
        if (!port_ids.insert(port.id).second) {
            return invalid("port ids must be unique within a Feature contract");
        }
    }
    if (const auto result = validate_feature_parameter_schema(contract.parameters); !result) return result;
    std::unordered_set<SideEffectId, StableIdHash<SideEffectIdTag>> effect_ids;
    for (const auto& effect : contract.side_effects) {
        if (!effect.id.is_valid() || effect.version == 0) {
            return invalid("side effect id and version are required");
        }
        if (!effect_ids.insert(effect.id).second) {
            return invalid("side effect ids must be unique within a Feature contract");
        }
    }
    if (contract.structure == FeatureStructure::kComposite &&
        (contract.execution != ExecutionKind::kCall || contract.state != StatePolicy::kStateless ||
         contract.has_continuation || contract.allows_child_process)) {
        return invalid("Composite coordination must be represented by a child Process Feature");
    }
    if (contract.execution == ExecutionKind::kCall && contract.state == StatePolicy::kProcessState) {
        return invalid("call execution cannot own process state");
    }
    if (contract.execution == ExecutionKind::kCall && contract.has_continuation) {
        return invalid("call execution cannot retain a continuation");
    }
    if (contract.execution == ExecutionKind::kProcess &&
        contract.state == StatePolicy::kStateless && !contract.has_continuation) {
        return invalid("process needs explicit continuation or external scheduling state");
    }
    if (contract.execution == ExecutionKind::kContinuous &&
        contract.state == StatePolicy::kProcessState && !contract.allows_child_process) {
        return invalid("continuous process state must explicitly own child processes");
    }
    if (contract.failure == FailurePolicy::kRetry && contract.retry.max_attempts < 2) {
        return invalid("retry policy requires at least two attempts");
    }
    return foundation::Result::success();
}

}  // namespace
foundation::Result validate_feature_contract(const FeatureContract& contract) { return validate_contract(contract, false); }
foundation::Result validate_feature_contract_draft(const FeatureContract& contract) { return validate_contract(contract, true); }

}  // namespace aeg::feature
