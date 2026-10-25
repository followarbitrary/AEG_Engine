/** @brief Frame-owned tile culling uniforms, descriptors, and restricted graph dispatch. */
#include "light_culling.hpp"
#include "light_culling_origin.hpp"
#include "render_feature_graph_builders.hpp"

#include <limits>

#include "builtin_render_passes.hpp"
#include "frame_context.hpp"
#include "../../Shader/private/shader_pipeline_layout_access.hpp"
#include "shader_contract.hpp"

namespace aeg::renderer {
namespace {
struct LocalLight final {
    math::Vec4 position_range{}, color_intensity{}, direction_spot{}, cone{};
};
static_assert(sizeof(LocalLight) == 64);
struct CullingConstants final {
    math::Mat4 inverse_projection{}, clip_to_shadow{};
    math::Vec4 shadow_parameters{};
    std::array<uint32_t, 4> tiles{};     // X, Y, local light count, uint entries per tile
    std::array<uint32_t, 4> viewport{};  // width, height, shadows enabled, reversed Z
};
static_assert(sizeof(CullingConstants) == 176);

rhi::BufferDesc upload_desc(uint64_t size, rhi::BufferCapability capability) {
    rhi::BufferDesc desc{};
    desc.size = size;
    desc.capabilities = capability;
    desc.memory = rhi::MemoryClass::kUpload;
    desc.initial_usage = capability == rhi::BufferCapability::kUniform ? rhi::ResourceUsage::kUniformRead
                                                                       : rhi::ResourceUsage::kStorageRead;
    // The first GPU consumer is the culling dispatch, not a graphics command.
    desc.initial_queue = rhi::QueueClass::kCompute;
    return desc;
}
}  // namespace

struct LightCullingResources::Impl final {
    explicit Impl(rhi::Device& d) : device(d), shaders(d) {}
    ~Impl() {
        if (pipeline.is_valid())
            (void)device.destroy_pipeline(pipeline);
    }
    rhi::Device& device;
    shader::ShaderManager shaders;
    rhi::PipelineHandle pipeline{};
    rhi::BindingLayoutHandle compute_layout{}, forward_layout{}, unshadowed_forward_layout{};
    RendererProfileShaderContract shader_contract{};
};
LightCullingResources::LightCullingResources() = default;
LightCullingResources::~LightCullingResources() = default;

foundation::Result LightCullingResources::initialize(rhi::Device& device, const std::filesystem::path& compute_package,
                                                     rhi::BindingLayoutHandle forward_layout,
                                                     rhi::BindingLayoutHandle unshadowed_forward_layout) try {
    if (impl_)
        return foundation::invalid_state("light culling resources already initialized");
    auto state = std::make_shared<Impl>(device);
    const auto program = shader::load_shader_program(compute_package);
    if (!program)
        return program.result;
    const auto layout = state->shaders.build_pipeline_layout(program.value);
    if (!layout)
        return layout.result;
    const auto inputs = state->shaders.binding_layout(layout.value, 0);
    if (!inputs)
        return inputs.result;
    const auto pipeline = state->shaders.create_pipeline(
        program.value, layout.value, {rhi::PipelineType::kCompute, {}, rhi::DebugName{"Renderer.LightCulling"}});
    if (!pipeline)
        return pipeline.result;
    state->pipeline = pipeline.value;
    state->shader_contract = {
        kRendererProfileContractVersion, RendererProfile::kLightCulling,
        shader::detail::ShaderPipelineLayoutAccess::contract_hash(layout.value),
        shader::detail::ShaderPipelineLayoutAccess::cache_epoch(layout.value)};
    state->compute_layout = inputs.value;
    state->forward_layout = forward_layout;
    state->unshadowed_forward_layout = unshadowed_forward_layout;
    impl_ = std::move(state);
    return foundation::Result::success();
} catch (const std::bad_alloc&) {
    return foundation::out_of_memory("light culling initialization failed");
}

foundation::ResultValue<RenderLightCullingResource> LightCullingResources::for_frame(
    render_graph::FrameContext& frame, const RenderSceneSnapshot& scene, const RenderShadowView& shadow) const try {
    using namespace foundation;
    if (!impl_ || !frame.is_frame_active() || &frame.device() != &impl_->device)
        return {invalid_state("light culling requires resources and an active frame"), {}};
    if (!scene.is_valid())
        return {invalid_argument("light culling scene is invalid"), {}};
    CullingConstants constants{};
    if (!math::try_inverse(scene.view.camera.projection, constants.inverse_projection))
        return {invalid_argument("light culling camera is not invertible"), {}};
    constants.clip_to_shadow = shadow.constants.clip_to_shadow;
    const auto shadow_extent = shadow.enabled ? shadow.view.output_extent : scene.view.output_extent;
    constants.shadow_parameters = {1.0f / shadow_extent.width, 1.0f / shadow_extent.height,
                                   shadow.constants.parameters.z, 0};
    constants.viewport = {scene.view.output_extent.width, scene.view.output_extent.height, shadow.enabled ? 1u : 0u,
                          scene.view.camera.clip_space_convention.reversed_z ? 1u : 0u};
    std::vector<LocalLight> lights;
    for (const auto& light : scene.lights) {
        if (light.type == RenderLightType::kDirectional)
            continue;
        if (light.type != RenderLightType::kPoint && light.type != RenderLightType::kSpot)
            return {invalid_argument("unknown local light type"), {}};
        if (!math::is_finite(light.position) || !math::is_finite(light.color) || !math::is_finite(light.range) ||
            !math::is_finite(light.intensity) || light.range < 0 || light.intensity < 0 || light.color.x < 0 ||
            light.color.y < 0 || light.color.z < 0 || light.color.x > 1 || light.color.y > 1 || light.color.z > 1)
            return {invalid_argument("local light has invalid position, color, range or intensity"), {}};
        if (light.intensity == 0 || light.range == 0 || (light.visibility_mask & scene.view.visibility_mask) == 0)
            continue;
        math::Vec3 direction{0, 0, -1};
        if (light.type == RenderLightType::kSpot &&
            (!math::try_normalize(light.direction, direction) || !math::is_finite(light.inner_cone_cosine) ||
             !math::is_finite(light.outer_cone_cosine) || light.outer_cone_cosine < -1 || light.inner_cone_cosine > 1 ||
             light.outer_cone_cosine > light.inner_cone_cosine))
            return {invalid_argument("spot light cone or direction is invalid"), {}};
        if (lights.size() == kMaxLocalLights)
            return {unsupported("local light budget exceeds 1024 lights"), {}};
        math::Vec3 view_position{}, view_direction{};
        if (!math::try_transform_point(scene.view.camera.view, light.position, view_position) ||
            !math::try_normalize(math::transform_vector(scene.view.camera.view, direction), view_direction))
            return {invalid_argument("local light view transform is invalid"), {}};
        lights.push_back(
            {{view_position.x, view_position.y, view_position.z, light.range},
             {light.color.x, light.color.y, light.color.z, light.intensity},
             {view_direction.x, view_direction.y, view_direction.z, light.type == RenderLightType::kSpot ? 1.0f : 0.0f},
             {light.inner_cone_cosine, light.outer_cone_cosine, 0, 0}});
    }
    const uint32_t count = static_cast<uint32_t>(lights.size());
    constants.tiles = {(scene.view.output_extent.width - 1) / kLightCullingTileSize + 1,
                       (scene.view.output_extent.height - 1) / kLightCullingTileSize + 1, count, count + 1};
    if (lights.empty())
        lights.emplace_back();  // valid descriptor even when the logical light count is zero
    RenderLightCullingResource resource{};
    resource.pipeline = impl_->pipeline;
    resource.shader_contract = impl_->shader_contract;
    resource.tile_count_x = constants.tiles[0];
    resource.tile_count_y = constants.tiles[1];
    const uint64_t tile_count = uint64_t(resource.tile_count_x) * resource.tile_count_y;
    const uint64_t tile_bytes = uint64_t(count + 1) * sizeof(uint32_t);
    if (tile_count > std::numeric_limits<uint64_t>::max() / tile_bytes)
        return {invalid_argument("light culling output size overflows"), {}};
    resource.cluster_bytes = tile_count * tile_bytes;
    // Both shaders use uint32 byte offsets; the last four-byte element must
    // fit even when CPU size arithmetic still fits in uint64.
    if (resource.cluster_bytes > uint64_t(std::numeric_limits<uint32_t>::max()) + 1)
        return {unsupported("light culling output exceeds GPU uint32 byte addressing"), {}};
    const uint64_t light_bytes = lights.size() * sizeof(LocalLight);
    const uint64_t storage_limit = impl_->device.capabilities().max_storage_buffer_bytes;
    if (storage_limit == 0 || resource.cluster_bytes > storage_limit || light_bytes > storage_limit)
        return {unsupported("light culling buffers exceed the device storage descriptor range"), {}};
    if (const auto r = frame.retain_until_complete(impl_); !r)
        return {r, {}};
    resource.constants_desc = upload_desc(sizeof(constants), rhi::BufferCapability::kUniform);
    resource.shadow_enabled = shadow.enabled;
    resource.lights_desc = upload_desc(light_bytes, rhi::BufferCapability::kStorage);
    const auto allocate = [&](const rhi::BufferDesc& desc,
                              std::span<const std::byte> bytes) -> ResultValue<rhi::BufferHandle> {
        const auto buffer = impl_->device.create_buffer(desc);
        if (!buffer)
            return buffer;
        if (const auto r = impl_->device.write_buffer(buffer.value, 0, bytes); !r) {
            (void)impl_->device.destroy_buffer(buffer.value);
            return {r, {}};
        }
        if (const auto r = frame.defer_destroy_buffer(buffer.value); !r) {
            (void)impl_->device.destroy_buffer(buffer.value);
            return {r, {}};
        }
        return buffer;
    };
    const auto constant_buffer = allocate(resource.constants_desc, std::as_bytes(std::span{&constants, 1}));
    if (!constant_buffer)
        return {constant_buffer.result, {}};
    resource.constants = constant_buffer.value;
    const auto light_buffer = allocate(resource.lights_desc, std::as_bytes(std::span{lights}));
    if (!light_buffer)
        return {light_buffer.result, {}};
    resource.lights = light_buffer.value;
    const auto prepare = [state = impl_, &frame, number = frame.frame_number(), constant_buffer = resource.constants,
                          light_buffer = resource.lights, light_bytes = resource.lights_desc.size,
                          cluster_bytes = resource.cluster_bytes, shadow_enabled = resource.shadow_enabled](
                             bool forward, rhi::TextureViewHandle texture,
                             rhi::BufferHandle clusters) -> ResultValue<rhi::BindingSetHandle> {
        if (!frame.is_frame_active() || frame.frame_number() != number)
            return {invalid_state("light culling binding belongs to another frame"), {}};
        const bool needs_texture = !forward || shadow_enabled;
        if (texture.is_valid() != needs_texture || !clusters.is_valid())
            return {invalid_argument("light culling input handles are invalid"), {}};
        const auto layout = !forward ? state->compute_layout
            : shadow_enabled ? state->forward_layout : state->unshadowed_forward_layout;
        if (!layout.is_valid())
            return {invalid_state("forward light-list layout is unavailable"), {}};
        const auto set = state->device.create_binding_set({layout, rhi::BindingSetLifetime::kPersistent});
        if (!set)
            return set;
        const uint16_t group = forward ? 3 : 0;
        std::array<rhi::BindingWrite, 4> writes{};
        writes[0].location = {group, 0};
        writes[0].resource.type = rhi::BindingType::kSampledTexture;
        writes[0].resource.texture_view = texture;
        writes[1].location = {group, 1};
        writes[1].resource.type = rhi::BindingType::kUniformBuffer;
        writes[1].resource.buffer_range = {constant_buffer, 0, sizeof(CullingConstants)};
        writes[2].location = {group, 2};
        writes[2].resource.type = rhi::BindingType::kStorageBuffer;
        writes[2].resource.buffer_range = {light_buffer, 0, light_bytes};
        writes[3].location = {group, 3};
        writes[3].resource.type = rhi::BindingType::kStorageBuffer;
        writes[3].resource.buffer_range = {clusters, 0, cluster_bytes};
        const auto active_writes = std::span<const rhi::BindingWrite>{writes}.subspan(needs_texture ? 0 : 1);
        if (const auto r = state->device.update_binding_set(set.value, active_writes); !r) {
            (void)state->device.destroy_binding_set(set.value);
            return {r, {}};
        }
        if (const auto r = frame.defer_destroy_binding_set(set.value); !r) {
            (void)state->device.destroy_binding_set(set.value);
            return {r, {}};
        }
        return set;
    };
    resource.prepare_compute = [prepare](rhi::TextureViewHandle texture, rhi::BufferHandle clusters) {
        return prepare(false, texture, clusters);
    };
    resource.prepare_forward = [prepare](rhi::TextureViewHandle texture, rhi::BufferHandle clusters) {
        return prepare(true, texture, clusters);
    };
    resource.origin_token = std::make_shared<const LightCullingOrigin>(LightCullingOrigin{
        &frame, &impl_->device, &scene, frame.frame_number(), resource.pipeline, resource.constants, resource.lights,
        resource.constants_desc, resource.lights_desc, resource.tile_count_x, resource.tile_count_y,
        resource.cluster_bytes, resource.shadow_enabled});
    return {Result::success(), std::move(resource)};
} catch (const std::bad_alloc&) {
    return {foundation::out_of_memory("light culling frame allocation failed"), {}};
}

bool light_culling_resource_has_origin(const RenderLightCullingResource& resource,
                                       const render_graph::FrameContext& frame, const rhi::Device& device,
                                       const RenderSceneSnapshot& scene) noexcept {
    const auto origin = std::static_pointer_cast<const LightCullingOrigin>(resource.origin_token);
    if (!origin)
        return false;
    const auto same_description = [](const rhi::BufferDesc& a, const rhi::BufferDesc& b) {
        return a.size == b.size && a.capabilities == b.capabilities && a.memory == b.memory &&
            a.flags == b.flags && a.initial_usage == b.initial_usage && a.initial_stages == b.initial_stages &&
            a.initial_queue == b.initial_queue;
    };
    return origin->frame == &frame && origin->device == &device && &frame.device() == &device &&
        frame.is_frame_active() && origin->frame_number == frame.frame_number() && origin->scene == &scene &&
        scene.frame_number == frame.frame_number() && origin->pipeline == resource.pipeline &&
        origin->constants == resource.constants && origin->lights == resource.lights &&
        same_description(origin->constants_desc, resource.constants_desc) &&
        same_description(origin->lights_desc, resource.lights_desc) && origin->tile_count_x == resource.tile_count_x &&
        origin->tile_count_y == resource.tile_count_y && origin->cluster_bytes == resource.cluster_bytes &&
        origin->shadow_enabled == resource.shadow_enabled;
}

foundation::Result build_light_culling_render_graph(pipeline::RenderGraphBuilder& builder,
                                                    const RenderFeatureContext& context,
                                                    pipeline::RenderGraphTexture depth,
                                                    pipeline::RenderGraphBuffer clusters,
                                                     RenderLightCullingResource& resource,
                                                     pipeline::RenderPassInstanceId authored_instance) {
    using namespace foundation;
    if (!builder.is_runtime() || !context.is_valid() ||
        (!context.registered_native && context.pass_execution.is_valid() && render_feature_context(builder) != &context) ||
        (!context.pass_execution.is_valid() && authored_instance.is_valid()) ||
        !depth.is_valid() || !clusters.is_valid() || !resource.pipeline.is_valid() || !resource.prepare_compute ||
        resource.tile_count_x == 0 || resource.tile_count_y == 0 ||
        !resource.shader_contract.is_valid(RendererProfile::kLightCulling))
        return invalid_argument("light culling graph inputs are invalid");
    const auto constants = find_or_import_buffer(builder,
        make_resource_id(pipeline::RenderResourceId("Renderer.LightCulling.Constants").value(), resource.constants.value),
        resource.constants, resource.constants_desc);
    const auto lights = find_or_import_buffer(builder,
        make_resource_id(pipeline::RenderResourceId("Renderer.LightCulling.Lights").value(), resource.lights.value),
        resource.lights, resource.lights_desc);
    if (!constants || !lights)
        return !constants ? constants.result : lights.result;
    resource.constants_graph = constants.value;
    resource.lights_graph = lights.value;
    const auto setup = [resource, depth, clusters](pipeline::RenderPassBuilder& pass) {
            if (const auto r = pass.read(depth, rhi::ResourceUsage::kSampledRead, rhi::PipelineStage::kCompute,
                                         rhi::ShaderStage::kCompute);
                !r)
                return r;
            if (const auto r = pass.read(resource.constants_graph, rhi::ResourceUsage::kUniformRead,
                                         rhi::PipelineStage::kCompute, rhi::ShaderStage::kCompute, {0, sizeof(CullingConstants)});
                !r)
                return r;
            if (const auto r = pass.read(resource.lights_graph, rhi::ResourceUsage::kStorageRead,
                                         rhi::PipelineStage::kCompute, rhi::ShaderStage::kCompute, {0, resource.lights_desc.size});
                !r)
                return r;
            return pass.write(clusters, rhi::ResourceUsage::kStorageWrite, rhi::PipelineStage::kCompute,
                              rhi::ShaderStage::kCompute, {0, resource.cluster_bytes});
        };
    const auto execute = [resource, depth, clusters](pipeline::RenderPassExecutionContext& command) {
            const auto view =
                command.texture_view(depth, rhi::TextureViewKind::kSampled, {rhi::Aspect::kDepth, 0, 1, 0, 1});
            const auto buffer = command.buffer(clusters);
            if (!view || !buffer)
                return !view ? view.result : buffer.result;
            const auto set = resource.prepare_compute(view.value, buffer.value);
            if (!set)
                return set.result;
            if (!set.value.is_valid())
                return invalid_state("light culling binding is invalid");
            if (const auto r = command.bind_pipeline(resource.pipeline); !r)
                return r;
            if (const auto r = command.bind_binding_set(0, set.value); !r)
                return r;
            return command.dispatch({resource.tile_count_x, resource.tile_count_y, 1});
        };
    const auto pass = (context.registered_native || context.pass_execution.is_valid())
                          ? detail::add_builtin_render_pass(builder, context, kLightCullingPassTypeId, setup, execute,
                                                            authored_instance)
                          : builder.add_compute_pass("Renderer.LightCulling", setup, execute,
                                                     builder.allows(pipeline::RenderGraphBuildFlags::kMayUseDedicatedQueue)
                                                         ? rhi::QueueClass::kCompute
                                                         : rhi::QueueClass::kGraphics);
    return pass ? Result::success() : pass.result;
}
}  // namespace aeg::renderer
