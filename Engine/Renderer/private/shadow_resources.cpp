/** @brief Host-owned shadow descriptor allocation outside restricted pass commands. */
#include "frame_context.hpp"
#include "shadow_rendering.hpp"

namespace aeg::renderer {
struct ShadowResources::Impl final {
    explicit Impl(rhi::Device& d) : device(d) {}
    rhi::Device& device;
    rhi::BindingLayoutHandle layout{};
};
ShadowResources::ShadowResources() = default;
ShadowResources::~ShadowResources() = default;

foundation::Result ShadowResources::initialize(rhi::Device& device, rhi::BindingLayoutHandle forward_layout) try {
    if (impl_)
        return foundation::invalid_state("shadow resources already initialized");
    if (!forward_layout.is_valid())
        return foundation::invalid_argument("forward shadow layout is invalid");
    auto state = std::make_shared<Impl>(device);
    state->layout = forward_layout;
    impl_ = std::move(state);
    return foundation::Result::success();
} catch (const std::bad_alloc&) {
    return foundation::out_of_memory("shadow resource allocation failed");
}

foundation::ResultValue<RenderShadowResource> ShadowResources::for_frame(render_graph::FrameContext& frame,
                                                                         const RenderShadowConstants& constants) const
    try {
    if (!impl_ || !frame.is_frame_active())
        return {foundation::invalid_state("shadow requires resources and an active frame"), {}};
    if (!math::is_finite(constants.clip_to_shadow) || !math::is_finite(constants.parameters) ||
        constants.parameters.x <= 0 || constants.parameters.y <= 0 || constants.parameters.z < 0)
        return {foundation::invalid_argument("shadow constants are invalid"), {}};
    if (const auto r = frame.retain_until_complete(impl_); !r)
        return {r, {}};
    rhi::BufferDesc desc{};
    desc.size = sizeof(constants);
    desc.capabilities = rhi::BufferCapability::kUniform;
    desc.memory = rhi::MemoryClass::kUpload;
    desc.initial_usage = rhi::ResourceUsage::kUniformRead;
    desc.initial_queue = rhi::QueueClass::kGraphics;
    const auto buffer = impl_->device.create_buffer(desc);
    if (!buffer)
        return {buffer.result, {}};
    if (const auto r = impl_->device.write_buffer(buffer.value, 0, std::as_bytes(std::span{&constants, 1})); !r) {
        (void)impl_->device.destroy_buffer(buffer.value);
        return {r, {}};
    }
    if (const auto r = frame.defer_destroy_buffer(buffer.value); !r) {
        (void)impl_->device.destroy_buffer(buffer.value);
        return {r, {}};
    }
    RenderShadowResource resource{buffer.value, desc};
    resource.prepare_inputs = [state = impl_, &frame, number = frame.frame_number(), buffer = buffer.value](
                                  rhi::TextureViewHandle map) -> foundation::ResultValue<rhi::BindingSetHandle> {
        if (!frame.is_frame_active() || frame.frame_number() != number)
            return {foundation::invalid_state("shadow binding belongs to another frame"), {}};
        if (!map.is_valid())
            return {foundation::invalid_argument("shadow map view is invalid"), {}};
        const auto set = state->device.create_binding_set({state->layout, rhi::BindingSetLifetime::kPersistent});
        if (!set)
            return set;
        std::array<rhi::BindingWrite, 2> writes{};
        writes[0].location = {3, 0};
        writes[0].resource.type = rhi::BindingType::kSampledTexture;
        writes[0].resource.texture_view = map;
        writes[1].location = {3, 1};
        writes[1].resource.type = rhi::BindingType::kUniformBuffer;
        writes[1].resource.buffer_range = {buffer, 0, sizeof(RenderShadowConstants)};
        if (const auto r = state->device.update_binding_set(set.value, writes); !r) {
            (void)state->device.destroy_binding_set(set.value);
            return {r, {}};
        }
        if (const auto r = frame.defer_destroy_binding_set(set.value); !r) {
            (void)state->device.destroy_binding_set(set.value);
            return {r, {}};
        }
        return set;
    };
    return {foundation::Result::success(), std::move(resource)};
} catch (const std::bad_alloc&) {
    return {foundation::out_of_memory("shadow frame allocation failed"), {}};
}
}  // namespace aeg::renderer
