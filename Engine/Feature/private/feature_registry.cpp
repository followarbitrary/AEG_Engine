#include "feature_registry.hpp"
#include "feature_catalog.hpp"
#include "feature_clock_admission.hpp"

#include <algorithm>
#include <new>
#include <vector>
#include <string>

namespace aeg::feature {
namespace {
foundation::Result invalid(std::string message) {
    return foundation::Result::failure(foundation::ResultCode::kInvalidArgument, std::move(message));
}
}

FeatureRegistry::FeatureRegistry(const ClockHost& host)
    : clock_host_(host.state_ ? host.state_->context : detail::ClockHostContextPtr{}) {}

foundation::ResultValue<FeatureRegistry> FeatureRegistry::from_catalog(const FeatureCatalog& catalog) try {
    const auto host = detail::make_clock_host_context({});
    if (!host) return {host.result, FeatureRegistry{}};
    FeatureRegistry registry(host.value);
    for (const auto& entry : catalog.entries()) {
        FeatureDefinition definition{};
        definition.id = entry.definition_id;
        definition.version = entry.version;
        definition.owner_module = entry.owner_module;
        definition.contract = entry.contract;
        if (const auto approved = detail::admit_clock_request(registry, definition.contract); !approved)
            return {approved.result, FeatureRegistry{}};
        if (const auto registered = registry.register_definition(std::move(definition)); !registered)
            return {registered, FeatureRegistry{}};
    }
    // Compilation consumes only contracts. Never turn a catalog snapshot into
    // host approval that could be reused to install callbacks or activate owners.
    registry.clock_host_.reset();
    registry.clock_bindings_.clear();
    return {foundation::Result::success(), std::move(registry)};
} catch (const std::bad_alloc&) {
    return {foundation::out_of_memory("Feature catalog registry allocation failed"), FeatureRegistry{}};
}

foundation::Result FeatureRegistry::register_definition(FeatureDefinition definition) try {
    if (definition.id != definition.contract.definition_id || definition.version != definition.contract.version) {
        return invalid("definition identity does not match its contract");
    }
    if (const auto admitted = detail::validate_admitted_clock(*this, definition.contract); !admitted) return admitted;
    if (definitions_.contains(definition.id)) {
        return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                           "feature definition is already registered");
    }
    definitions_.emplace(definition.id, std::move(definition));
    return foundation::Result::success();
} catch (const std::bad_alloc&) {
    return foundation::out_of_memory("Feature registration allocation failed");
}

foundation::Result FeatureRegistry::replace_definition(FeatureDefinition definition) try {
    if (definition.id != definition.contract.definition_id || definition.version != definition.contract.version) {
        return invalid("definition identity does not match its contract");
    }
    if (const auto admitted = detail::validate_admitted_clock(*this, definition.contract); !admitted) return admitted;
    const auto it = definitions_.find(definition.id);
    if (it == definitions_.end()) {
        return foundation::Result::failure(foundation::ResultCode::kNotFound,
                                           "feature definition is not registered");
    }
    // A hot replacement may change executable code and contract details, but
    // it cannot transfer ownership to another module.  Module registration
    // validates ownership at the boundary; retaining this invariant here
    // keeps a later replace_definition call from bypassing that boundary.
    if (!definition.owner_module.is_valid() && it->second.owner_module.is_valid()) {
        definition.owner_module = it->second.owner_module;
    }
    if (it->second.owner_module.is_valid() &&
        definition.owner_module != it->second.owner_module) {
        return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                           "feature definition owner module cannot change during replacement");
    }
    it->second = std::move(definition);
    return foundation::Result::success();
} catch (const std::bad_alloc&) {
    return foundation::out_of_memory("Feature replacement allocation failed");
}

foundation::Result FeatureRegistry::unregister_definition(FeatureDefinitionId id) {
    if (!id.is_valid()) {
        return invalid("feature definition id is invalid");
    }
    const auto it = definitions_.find(id);
    if (it == definitions_.end()) {
        return foundation::Result::failure(foundation::ResultCode::kNotFound,
                                           "feature definition is not registered");
    }
    for (const auto& [candidate_id, candidate] : definitions_) {
        static_cast<void>(candidate_id);
        if (std::find(candidate.contract.children.begin(), candidate.contract.children.end(), id) !=
            candidate.contract.children.end()) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                               "feature definition is still referenced by a composite feature");
        }
    }
    definitions_.erase(it);
    return foundation::Result::success();
}

const FeatureDefinition* FeatureRegistry::find_definition(FeatureDefinitionId id) const noexcept {
    const auto it = definitions_.find(id);
    return it == definitions_.end() ? nullptr : &it->second;
}

uint64_t FeatureRegistry::contract_digest() const noexcept {
    std::vector<const FeatureDefinition*> definitions;
    definitions.reserve(definitions_.size());
    for (const auto& [id, definition] : definitions_) {
        static_cast<void>(id);
        definitions.push_back(&definition);
    }
    std::sort(definitions.begin(), definitions.end(), [](const auto* left, const auto* right) {
        return left->id.value() < right->id.value();
    });
    uint64_t digest = 14695981039346656037ull;
    const auto mix = [&digest](uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            digest ^= (value >> shift) & 0xffu;
            digest *= 1099511628211ull;
        }
    };
    for (const auto* definition : definitions) mix(feature_contract_digest(*definition));
    return digest == 0 ? 1 : digest;
}

foundation::ResultValue<ExpandedFeature> FeatureRegistry::expand(
    FeatureDefinitionId id, const OwnerPath& owner_path) const {
    std::vector<FeatureDefinitionId> stack;
    return expand_impl(id, owner_path, stack);
}

foundation::ResultValue<ExpandedFeature> FeatureRegistry::expand_impl(
    FeatureDefinitionId id, const OwnerPath& owner_path,
    std::vector<FeatureDefinitionId>& stack) const {
    const auto* definition = find_definition(id);
    if (definition == nullptr) {
        return {foundation::Result::failure(foundation::ResultCode::kNotFound,
                                             "feature definition was not found"), {}};
    }
    if (std::find(stack.begin(), stack.end(), id) != stack.end()) {
        return {invalid("recursive Feature composition is not allowed"), {}};
    }
    stack.push_back(id);
    ExpandedFeature result{};
    result.definition_id = id;
    result.version = definition->version;
    result.domains = definition->contract.domains;
    result.contract = definition->contract;
    result.owner_path = owner_path;
    result.owner_module = definition->owner_module;
    if (definition->contract.structure == FeatureStructure::kAtomic &&
        !definition->contract.children.empty()) {
        // The only non-Composite child list is the explicit service-owned
        // Process set.  It is deliberately resolved here, once all
        // definitions are available, instead of weakening the contract
        // validator with an order-dependent registry lookup.
        if (definition->contract.execution != ExecutionKind::kContinuous ||
            definition->contract.state != StatePolicy::kProcessState ||
            !definition->contract.allows_child_process) {
            stack.pop_back();
            return {invalid("atomic Feature children require an explicit Process service contract"), {}};
        }
        for (const auto child_id : definition->contract.children) {
            const auto* child_definition = find_definition(child_id);
            if (child_definition == nullptr || child_definition->contract.structure != FeatureStructure::kAtomic ||
                child_definition->contract.execution != ExecutionKind::kProcess) {
                stack.pop_back();
                return {invalid("Process service children must be registered atomic Process Features"), {}};
            }
        }
    }
    FeatureDomainMask child_domains = 0;
    for (const auto child_id : definition->contract.children) {
        auto child_path = owner_path;
        child_path.nodes.emplace_back(FeatureNodeId(child_id.value()));
        auto child = expand_impl(child_id, child_path, stack);
        if (!child) {
            stack.pop_back();
            return child;
        }
        child_domains = static_cast<FeatureDomainMask>(child_domains | child.value.domains);
        result.children.push_back(std::move(child.value));
    }
    if (definition->contract.structure == FeatureStructure::kComposite) {
        if (definition->contract.domains != child_domains) {
            stack.pop_back();
            return {invalid("composite domain set must equal its recursive child domain union"), {}};
        }
        result.domains = child_domains;
    }
    stack.pop_back();
    return {foundation::Result::success(), std::move(result)};
}
}  // namespace aeg::feature
