#include "../public/pipeline_composition_runtime.hpp"

#include <unordered_set>
namespace aeg::engine {
foundation::Result PipelineCompositionRuntime::stage(const pipeline::PreparedPipelineDocument& prepared,
                                                     std::span<const PipelineHostBinding> bindings) {
    pipeline::ExecutionInstanceId legacy_identity;
    legacy_identity.graph_asset_id = prepared.configurations.empty() || !prepared.configurations.front() ||
                                             prepared.configurations.front()->source.empty()
                                         ? "legacy"
                                         : prepared.configurations.front()->source;
    legacy_identity.graph_version = prepared.graph_version.empty() ? "legacy" : prepared.graph_version;
    legacy_identity.instance_path = "runtime";
    legacy_identity.view_id = "default";
    legacy_identity.world_id = "default";
    legacy_identity.target_variant = "runtime";
    return stage(prepared, bindings, legacy_identity);
}

foundation::Result PipelineCompositionRuntime::stage(const pipeline::PreparedPipelineDocument& prepared,
                                                     std::span<const PipelineHostBinding> bindings,
                                                     const pipeline::ExecutionInstanceId& execution) {
    if (!execution.is_valid())
        return foundation::invalid_argument("execution instance identity is incomplete");
    if (prepared.configurations.size() != bindings.size())
        return foundation::invalid_argument("composition host count does not match prepared configurations");
    std::unordered_set<std::string> names;
    for (std::size_t index = 0; index < bindings.size(); ++index) {
        const auto& binding = bindings[index];
        if (binding.name.empty() || !binding.host || !names.insert(binding.name).second)
            return foundation::invalid_argument("composition host binding is invalid or duplicated");
        const auto& configuration = prepared.configurations[index];
        if (!configuration || !configuration->instance.is_valid() || !configuration->pipeline.is_valid())
            return foundation::invalid_argument("composition contains an invalid host configuration");
        if (binding.host->instance_id().is_valid() && binding.host->instance_id() != configuration->instance)
            return foundation::invalid_argument("composition configuration instance does not match host");
    }
    pending_hosts_.assign(bindings.begin(), bindings.end());
    pending_configurations_ = prepared.configurations;
    pending_attempt_ = {execution, next_attempt_++};
    pending_ = true;
    return foundation::Result::success();
}
foundation::Result PipelineCompositionRuntime::apply_pending(bool at_safe_boundary) {
    if (!at_safe_boundary)
        return foundation::invalid_state("composition requires a safe frame boundary");
    if (!pending_)
        return foundation::Result::success();
    for (std::size_t index = 0; index < pending_hosts_.size(); ++index) {
        if (const auto result = pending_hosts_[index].host->set_configuration(pending_configurations_[index]); !result)
            return result;
    }
    active_.swap(pending_hosts_);
    pending_hosts_.clear();
    pending_ = false;
    active_configurations_.swap(pending_configurations_);
    pending_configurations_.clear();
    active_attempt_ = pending_attempt_;
    pending_attempt_ = {};
    return foundation::Result::success();
}

void PipelineCompositionRuntime::discard_pending() noexcept {
    pending_hosts_.clear();
    pending_configurations_.clear();
    pending_attempt_ = {};
    pending_ = false;
}

foundation::Result PipelineCompositionRuntime::stop_and_retire(const pipeline::PipelineDeactivationContext& context) {
    for (const auto& binding : active_) {
        if (binding.host == nullptr || !binding.host->is_active())
            continue;
        if (const auto result = binding.host->deactivate(context); !result)
            return result;
    }
    active_.clear();
    pending_hosts_.clear();
    active_configurations_.clear();
    pending_configurations_.clear();
    pending_attempt_ = {};
    active_attempt_ = {};
    pending_ = false;
    return foundation::Result::success();
}
}  // namespace aeg::engine
