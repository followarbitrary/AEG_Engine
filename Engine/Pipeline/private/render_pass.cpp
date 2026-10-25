#include "render_pass_execution.hpp"
#include "render_pass_registry_execution.hpp"

#include <limits>

namespace aeg::pipeline {
namespace {
void advance_execution_epoch(uint64_t& epoch) noexcept {
    epoch = epoch == std::numeric_limits<uint64_t>::max() ? 1 : epoch + 1;
}
}  // namespace

uint64_t detail::RenderPassRegistryExecution::execution_epoch(const RenderPassRegistry& registry) noexcept {
    return registry.execution_epoch_;
}

foundation::ResultValue<std::shared_ptr<const RenderPass>> detail::RenderPassRegistryExecution::find_pass(
    const RenderPassRegistry& registry, RenderPassTypeId type) {
    const auto it = registry.factories_.find(type);
    if (it == registry.factories_.end())
        return {foundation::not_found("render pass type is not registered"), {}};
    auto pass = it->second();
    if (!pass)
        return {foundation::invalid_argument("render pass factory returned null"), {}};
    return {foundation::Result::success(), std::shared_ptr<const RenderPass>(std::move(pass))};
}

foundation::Result RenderPassRegistry::register_pass(RenderPassFactory factory) {
    if (!factory)
        return foundation::invalid_argument("render pass factory is empty");
    auto pass = factory();
    if (!pass)
        return foundation::invalid_argument("render pass factory returned null");
    const auto& d = pass->descriptor();
    if (!d.type.is_valid() || !d.owner_module.is_valid() || d.name.empty() || !d.version.is_valid() ||
        (d.allowed_queue_mask == 0))
        return foundation::invalid_argument("render pass descriptor is invalid");
    if (d.type != RenderPassTypeId(d.type.value()))
        return foundation::invalid_argument("render pass descriptor type is invalid");
    if (factories_.contains(d.type))
        return foundation::invalid_argument("render pass type is already registered");
    factories_.emplace(d.type, std::move(factory));
    advance_execution_epoch(execution_epoch_);
    return foundation::Result::success();
}

foundation::Result RenderPassRegistry::unregister_pass(RenderPassTypeId type) {
    if (!factories_.erase(type))
        return foundation::not_found("render pass type is not registered");
    advance_execution_epoch(execution_epoch_);
    return foundation::Result::success();
}

foundation::ResultValue<RenderGraphPass> add_registered_render_pass(
    RenderGraphBuilder& builder, const RenderPassRegistry& registry,
    std::shared_ptr<const RenderPassConfiguration> configuration) {
    if (!configuration || !configuration->type.is_valid() || configuration->queue > rhi::QueueClass::kCopy)
        return {foundation::invalid_argument("render pass configuration is invalid"), {}};
    if (!configuration->enabled)
        return {foundation::invalid_argument("render pass configuration is disabled"), {}};
    auto found = detail::RenderPassRegistryExecution::find_pass(registry, configuration->type);
    if (!found)
        return {found.result, {}};
    const auto owner = found.value;
    const auto& descriptor = owner->descriptor();
    const auto queue_bit = uint64_t{1} << static_cast<uint8_t>(configuration->queue);
    if (!(descriptor.allowed_queue_mask & queue_bit) ||
        (descriptor.graph_type == RenderGraphPassType::kGraphics && configuration->queue == rhi::QueueClass::kCompute))
        return {foundation::invalid_argument("render pass queue is not allowed"), {}};
    const auto added = builder.add_pass(
        descriptor.name, descriptor.graph_type, configuration->queue,
        [owner, configuration](RenderPassBuilder& setup) { return owner->setup(setup, *configuration); },
        [owner, configuration](RenderPassExecutionContext& execute) {
            return owner->execute(execute, *configuration);
        });
    if (!added) return added;
    if (const auto retained = builder.retain_execution_owner(owner); !retained) return {retained, {}};
    if (const auto retained = builder.retain_execution_owner(configuration); !retained) return {retained, {}};
    return added;
}

}  // namespace aeg::pipeline
