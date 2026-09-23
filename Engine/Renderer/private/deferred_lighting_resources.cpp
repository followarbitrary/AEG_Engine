/** @file deferred_lighting_resources.cpp @brief Frame-owned bindings for GBuffer shading. */
#include "deferred_lighting_resources.hpp"

#include <array>

#include "frame_context.hpp"
#include "shader_contract.hpp"
#include "shadow_rendering.hpp"

namespace aeg::renderer {
struct DeferredLightingResources::Impl final {
    explicit Impl(rhi::Device& device) : device(device), shaders(device) {}
    ~Impl() {
        if (pipeline.is_valid())
            (void)device.destroy_pipeline(pipeline);
        if (shadow_pipeline.is_valid())
            (void)device.destroy_pipeline(shadow_pipeline);
    }
    rhi::Device& device;
    shader::ShaderManager shaders;
    rhi::PipelineHandle pipeline{};
    rhi::BindingLayoutHandle inputs_layout{};
    rhi::PipelineHandle shadow_pipeline{};
    rhi::BindingLayoutHandle shadow_layout{};
};

DeferredLightingResources::DeferredLightingResources() = default;
DeferredLightingResources::~DeferredLightingResources() = default;

foundation::Result DeferredLightingResources::initialize(rhi::Device& device,
                                                         const std::filesystem::path& shader_package,
                                                         rhi::Format color_format,
                                                         const std::filesystem::path& shadow_package) try {
    if (impl_)
        return foundation::invalid_state("deferred lighting resources are already initialized");
    const auto program = shader::load_shader_program(shader_package);
    if (!program)
        return program.result;
    auto state = std::make_shared<Impl>(device);
    const auto layout = state->shaders.build_pipeline_layout(program.value);
    if (!layout)
        return layout.result;
    const auto inputs = state->shaders.binding_layout(layout.value, 0);
    if (!inputs)
        return inputs.result;
    rhi::GraphicsPipelineState graphics{};
    graphics.color_formats = std::span{&color_format, 1};
    graphics.cull_mode = rhi::CullMode::kNone;
    graphics.depth_test_enable = false;
    graphics.depth_write_enable = false;
    const auto pipeline = state->shaders.create_pipeline(
        program.value, layout.value,
        {rhi::PipelineType::kGraphics, graphics, rhi::DebugName{"Renderer.DeferredLighting"}});
    if (!pipeline)
        return pipeline.result;
    state->pipeline = pipeline.value;
    state->inputs_layout = inputs.value;
    if (!shadow_package.empty()) {
        const auto shadow_program = shader::load_shader_program(shadow_package);
        if (!shadow_program)
            return shadow_program.result;
        const auto shadow_layout = state->shaders.build_pipeline_layout(shadow_program.value);
        if (!shadow_layout)
            return shadow_layout.result;
        const auto shadow_inputs = state->shaders.binding_layout(shadow_layout.value, 0);
        if (!shadow_inputs)
            return shadow_inputs.result;
        const auto shadow_pipeline = state->shaders.create_pipeline(
            shadow_program.value, shadow_layout.value,
            {rhi::PipelineType::kGraphics, graphics, rhi::DebugName{"Renderer.DeferredShadowLighting"}});
        if (!shadow_pipeline)
            return shadow_pipeline.result;
        state->shadow_pipeline = shadow_pipeline.value;
        state->shadow_layout = shadow_inputs.value;
    }
    impl_ = std::move(state);
    return foundation::Result::success();
} catch (const std::bad_alloc&) {
    return foundation::out_of_memory("deferred lighting resource allocation failed");
}

foundation::ResultValue<RenderDeferredLightingResource> DeferredLightingResources::for_frame(
    render_graph::FrameContext& frame) const try {
    if (!impl_ || !frame.is_frame_active()) {
        return {foundation::invalid_state("deferred lighting requires initialized resources and an active frame"), {}};
    }
    const auto frame_number = frame.frame_number();
    RenderDeferredLightingResource resource;
    resource.pipeline = impl_->pipeline;
    resource.shadow_pipeline = impl_->shadow_pipeline;
    resource.prepare_inputs =
        [state = impl_, &frame,
         frame_number](const RenderDeferredLightingInputs& inputs) -> foundation::ResultValue<rhi::BindingSetHandle> {
        if (!frame.is_frame_active() || frame.frame_number() != frame_number) {
            return {foundation::invalid_state("deferred lighting bindings belong to a different frame"), {}};
        }
        const std::array views{inputs.albedo, inputs.normal, inputs.depth};
        for (const auto view : views) {
            if (!view.is_valid())
                return {foundation::invalid_argument("deferred lighting sampled view is invalid"), {}};
        }
        if (!inputs.lighting.buffer.is_valid() || inputs.lighting.size < sizeof(RenderLightingConstants)) {
            return {foundation::invalid_argument("deferred lighting uniform range is invalid"), {}};
        }
        if (const auto retained = frame.retain_until_complete(state); !retained)
            return {retained, {}};
        const bool shadowed = inputs.shadow_map.is_valid();
        if (shadowed && (!state->shadow_layout.is_valid() || !inputs.shadow_constants.buffer.is_valid() ||
                         inputs.shadow_constants.size < sizeof(RenderShadowConstants)))
            return {foundation::invalid_argument("deferred shadow input is invalid"), {}};
        const auto set = state->device.create_binding_set({shadowed ? state->shadow_layout : state->inputs_layout,
                                                           rhi::BindingSetLifetime::kPersistent,
                                                           rhi::DebugName{"Renderer.DeferredLighting.Inputs"}});
        if (!set)
            return set;
        std::array<rhi::BindingWrite, 6> writes{};
        for (uint16_t index = 0; index < views.size(); ++index) {
            writes[index].location = {0, index};
            writes[index].resource.type = rhi::BindingType::kSampledTexture;
            writes[index].resource.texture_view = views[index];
        }
        writes[3].location = {0, 3};
        writes[3].resource.type = rhi::BindingType::kUniformBuffer;
        writes[3].resource.buffer_range = inputs.lighting;
        if (shadowed) {
            writes[4].location = {0, 4};
            writes[4].resource.type = rhi::BindingType::kSampledTexture;
            writes[4].resource.texture_view = inputs.shadow_map;
            writes[5].location = {0, 5};
            writes[5].resource.type = rhi::BindingType::kUniformBuffer;
            writes[5].resource.buffer_range = inputs.shadow_constants;
        }
        if (const auto result =
                state->device.update_binding_set(set.value, std::span{writes.data(), shadowed ? size_t{6} : size_t{4}});
            !result) {
            (void)state->device.destroy_binding_set(set.value);
            return {result, {}};
        }
        if (const auto result = frame.defer_destroy_binding_set(set.value); !result) {
            (void)state->device.destroy_binding_set(set.value);
            return {result, {}};
        }
        return set;
    };
    return {foundation::Result::success(), std::move(resource)};
} catch (const std::bad_alloc&) {
    return {foundation::out_of_memory("deferred lighting frame binding allocation failed"), {}};
}
}  // namespace aeg::renderer
