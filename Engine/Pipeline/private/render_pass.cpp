#include "render_pass.hpp"

namespace aeg::pipeline {
RenderPassRegistry& global_render_pass_registry() noexcept {
    static RenderPassRegistry registry;
    return registry;
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
    return foundation::Result::success();
}

foundation::Result RenderPassRegistry::unregister_pass(RenderPassTypeId type) {
    if (!factories_.erase(type))
        return foundation::not_found("render pass type is not registered");
    return foundation::Result::success();
}

foundation::ResultValue<std::shared_ptr<const RenderPass>> RenderPassRegistry::find_pass(RenderPassTypeId type) const {
    const auto it = factories_.find(type);
    if (it == factories_.end())
        return {foundation::not_found("render pass type is not registered"), {}};
    auto pass = it->second();
    if (!pass)
        return {foundation::invalid_argument("render pass factory returned null"), {}};
    return {foundation::Result::success(), std::shared_ptr<const RenderPass>(std::move(pass))};
}

foundation::ResultValue<RenderGraphPass> add_registered_render_pass(
    RenderGraphBuilder& builder, const RenderPassRegistry& registry,
    std::shared_ptr<const RenderPassConfiguration> configuration) {
    if (!configuration || !configuration->type.is_valid())
        return {foundation::invalid_argument("render pass configuration is invalid"), {}};
    if (!configuration->enabled)
        return {foundation::invalid_argument("render pass configuration is disabled"), {}};
    auto found = registry.find_pass(configuration->type);
    if (!found)
        return {found.result, {}};
    const auto owner = found.value;
    const auto& descriptor = owner->descriptor();
    const auto queue_bit = uint64_t{1} << static_cast<uint8_t>(configuration->queue);
    if (!(descriptor.allowed_queue_mask & queue_bit) ||
        (descriptor.graph_type == RenderGraphPassType::kGraphics && configuration->queue == rhi::QueueClass::kCompute))
        return {foundation::invalid_argument("render pass queue is not allowed"), {}};
    return builder.add_pass(
        descriptor.name, descriptor.graph_type, configuration->queue,
        [owner, configuration](RenderPassBuilder& setup) { return owner->setup(setup, *configuration); },
        [owner, configuration](RenderPassExecutionContext& execute) {
            return owner->execute(execute, *configuration);
        });
}

}  // namespace aeg::pipeline
