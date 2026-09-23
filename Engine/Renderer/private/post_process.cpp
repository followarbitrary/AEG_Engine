/** @brief Frame-owned sky/postprocess constants, descriptors and restricted fullscreen commands. */
#include "post_process.hpp"

#include "builtin_render_passes.hpp"
#include "frame_context.hpp"
#include "shader_contract.hpp"

namespace aeg::renderer {
namespace {
struct PostProcessConstants final {
    math::Mat4 inverse_projection{}, inverse_view{};
    math::Vec4 sky_zenith{}, sky_horizon{}, sky_ground{}, parameters{}, viewport{};
};
static_assert(sizeof(PostProcessConstants) == 208);
}  // namespace

struct PostProcessResources::Impl final {
    explicit Impl(rhi::Device& value) : device(value), shaders(value) {}
    ~Impl() {
        if (pipeline.is_valid())
            (void)device.destroy_pipeline(pipeline);
    }
    rhi::Device& device;
    shader::ShaderManager shaders;
    rhi::PipelineHandle pipeline{};
    rhi::BindingLayoutHandle layout{};
};
PostProcessResources::PostProcessResources() = default;
PostProcessResources::~PostProcessResources() = default;

foundation::Result validate_post_process_settings(const RenderPostProcessSettings& settings) {
    for (const auto color : {settings.sky_zenith, settings.sky_horizon, settings.sky_ground})
        if (!math::is_finite(color) || color.x < 0 || color.y < 0 || color.z < 0)
            return foundation::invalid_argument("Sky colors must be finite and non-negative");
    if (!math::is_finite(settings.exposure) || settings.exposure < 0 || !math::is_finite(settings.gamma) ||
        settings.gamma <= 0 || !math::is_finite(1.0f / settings.gamma))
        return foundation::invalid_argument("Postprocess requires non-negative exposure and positive finite gamma");
    return foundation::Result::success();
}

foundation::Result PostProcessResources::initialize(rhi::Device& device, const std::filesystem::path& package,
                                                    rhi::Format output_format) try {
    if (impl_)
        return foundation::invalid_state("Postprocess resources already initialized");
    auto state = std::make_shared<Impl>(device);
    const auto program = shader::load_shader_program(package);
    if (!program)
        return program.result;
    const auto layout = state->shaders.build_pipeline_layout(program.value);
    if (!layout)
        return layout.result;
    const auto inputs = state->shaders.binding_layout(layout.value, 0);
    if (!inputs)
        return inputs.result;
    rhi::GraphicsPipelineState graphics{};
    graphics.color_formats = std::span{&output_format, 1};
    graphics.cull_mode = rhi::CullMode::kNone;
    graphics.depth_test_enable = false;
    graphics.depth_write_enable = false;
    const auto pipeline = state->shaders.create_pipeline(
        program.value, layout.value,
        {rhi::PipelineType::kGraphics, graphics, rhi::DebugName{"Renderer.SkyboxPostProcess"}});
    if (!pipeline)
        return pipeline.result;
    state->pipeline = pipeline.value;
    state->layout = inputs.value;
    impl_ = std::move(state);
    return foundation::Result::success();
} catch (const std::bad_alloc&) {
    return foundation::out_of_memory("Postprocess resource initialization failed");
}

foundation::ResultValue<RenderPostProcessResource> PostProcessResources::for_frame(
    render_graph::FrameContext& frame, const RenderView& view, const RenderPostProcessSettings& settings) const try {
    using namespace foundation;
    if (!impl_ || !frame.is_frame_active())
        return {invalid_state("Postprocess requires an active frame"), {}};
    if (const auto r = validate_post_process_settings(settings); !r)
        return {r, {}};
    PostProcessConstants constants{};
    if (!view.output_extent.is_valid() || !math::try_inverse(view.camera.projection, constants.inverse_projection) ||
        !math::try_inverse(view.camera.view, constants.inverse_view))
        return {invalid_argument("Postprocess view extent or camera is invalid"), {}};
    constants.sky_zenith = {settings.sky_zenith.x, settings.sky_zenith.y, settings.sky_zenith.z, 1};
    constants.sky_horizon = {settings.sky_horizon.x, settings.sky_horizon.y, settings.sky_horizon.z, 1};
    constants.sky_ground = {settings.sky_ground.x, settings.sky_ground.y, settings.sky_ground.z, 1};
    constants.parameters = {settings.exposure, 1.0f / settings.gamma, settings.tone_mapping ? 1.0f : 0.0f,
                            settings.sky_enabled ? 1.0f : 0.0f};
    constants.viewport = {float(view.output_extent.width), float(view.output_extent.height),
                          view.camera.clip_space_convention.reversed_z ? 1.0f : 0.0f, 0};
    RenderPostProcessResource resource{};
    resource.pipeline = impl_->pipeline;
    resource.constants_desc.size = sizeof(constants);
    resource.constants_desc.memory = rhi::MemoryClass::kUpload;
    resource.constants_desc.capabilities = rhi::BufferCapability::kUniform;
    resource.constants_desc.initial_usage = rhi::ResourceUsage::kUniformRead;
    const auto buffer = impl_->device.create_buffer(resource.constants_desc);
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
    if (const auto r = frame.retain_until_complete(impl_); !r)
        return {r, {}};
    resource.constants = buffer.value;
    resource.prepare_inputs = [state = impl_, &frame, number = frame.frame_number(), buffer = buffer.value](
                                  rhi::TextureViewHandle color,
                                  rhi::TextureViewHandle depth) -> ResultValue<rhi::BindingSetHandle> {
        if (!frame.is_frame_active() || frame.frame_number() != number)
            return {invalid_state("Postprocess bindings belong to another frame"), {}};
        if (!color.is_valid() || !depth.is_valid())
            return {invalid_argument("Postprocess inputs are invalid"), {}};
        const auto set = state->device.create_binding_set({state->layout, rhi::BindingSetLifetime::kPersistent});
        if (!set)
            return set;
        std::array<rhi::BindingWrite, 3> writes{};
        for (uint16_t index = 0; index < 2; ++index) {
            writes[index].location = {0, index};
            writes[index].resource.type = rhi::BindingType::kSampledTexture;
            writes[index].resource.texture_view = index == 0 ? color : depth;
        }
        writes[2].location = {0, 2};
        writes[2].resource.type = rhi::BindingType::kUniformBuffer;
        writes[2].resource.buffer_range = {buffer, 0, sizeof(PostProcessConstants)};
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
    return {Result::success(), std::move(resource)};
} catch (const std::bad_alloc&) {
    return {foundation::out_of_memory("Postprocess frame resource allocation failed"), {}};
}

foundation::Result build_post_process_render_graph(
    pipeline::RenderGraphBuilder& builder, const RenderFeatureContext& context, pipeline::RenderGraphTexture color,
    pipeline::RenderGraphTexture depth, pipeline::RenderGraphTexture output, rhi::ResourceUsage final_usage) {
    using namespace foundation;
    if (!builder.is_runtime() || !context.is_valid() || render_feature_context(builder) != &context ||
        !context.resource_resolver || !color.is_valid() || !depth.is_valid() || !output.is_valid() || color == output)
        return invalid_argument("Skybox/postprocess graph inputs are invalid");
    const auto resource = context.resource_resolver->resolve_post_process(context.scene_snapshot->view);
    if (!resource)
        return resource.result;
    if (!resource.value.pipeline.is_valid() || !resource.value.prepare_inputs || !resource.value.constants.is_valid() ||
        resource.value.constants_desc.size < sizeof(PostProcessConstants))
        return invalid_state("Skybox/postprocess resource is incomplete");
    const auto constants = builder.import_buffer(pipeline::RenderResourceId("Renderer.PostProcess.Constants"),
                                                 resource.value.constants, resource.value.constants_desc);
    if (!constants)
        return constants.result;
    const auto extent = context.scene_snapshot->view.output_extent;
    const auto pass = detail::add_builtin_render_pass(
        builder, context, kSkyboxPostProcessPassTypeId,
        [color, depth, output, constants = constants.value, final_usage](pipeline::RenderPassBuilder& pass) {
            for (auto texture : {color, depth})
                if (const auto r = pass.read(texture, rhi::ResourceUsage::kSampledRead,
                                             rhi::PipelineStage::kFragmentShader, rhi::ShaderStage::kFragment);
                    !r)
                    return r;
            if (const auto r = pass.read(constants, rhi::ResourceUsage::kUniformRead,
                                         rhi::PipelineStage::kFragmentShader, rhi::ShaderStage::kFragment);
                !r)
                return r;
            pass.set_side_effects();
            return pass.use_color_attachment(output, rhi::LoadOp::kClear, rhi::StoreOp::kStore, {}, final_usage);
        },
        [color, depth, resource = resource.value, extent](pipeline::RenderPassExecutionContext& command) {
            const auto color_view = command.texture_view(color);
            const auto depth_view =
                command.texture_view(depth, rhi::TextureViewKind::kSampled, {rhi::Aspect::kDepth, 0, 1, 0, 1});
            if (!color_view || !depth_view)
                return !color_view ? color_view.result : depth_view.result;
            const auto set = resource.prepare_inputs(color_view.value, depth_view.value);
            if (!set)
                return set.result;
            if (!set.value.is_valid())
                return invalid_state("Postprocess input binding is invalid");
            if (const auto r = command.begin_rendering(); !r)
                return r;
            const auto drawn = [&]() -> Result {
                if (const auto r = command.set_viewport({0, 0, float(extent.width), float(extent.height), 0, 1}); !r)
                    return r;
                if (const auto r = command.set_scissor({0, 0, extent.width, extent.height}); !r)
                    return r;
                if (const auto r = command.bind_pipeline(resource.pipeline); !r)
                    return r;
                if (const auto r = command.bind_binding_set(0, set.value); !r)
                    return r;
                return command.draw({3, 1, 0, 0});
            }();
            const auto ended = command.end_rendering();
            return drawn ? ended : drawn;
        });
    return pass ? Result::success() : pass.result;
}
}  // namespace aeg::renderer
