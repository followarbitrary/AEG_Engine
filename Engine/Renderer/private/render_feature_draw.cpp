#include "render_feature_draw.hpp"
#include "light_culling_origin.hpp"
#include "render_feature_graph_builders.hpp"
#include "shadow_rendering.hpp"
#include "light_culling.hpp"
#include "frame_context.hpp"
#include "render_feature_history.hpp"

#include <type_traits>

namespace aeg::renderer {
namespace {
pipeline::RenderResourceId scope_resource_id(feature::RenderResourceId id, RenderViewId view,
                                             bool canonical_view) {
    return canonical_view ? pipeline::RenderResourceId(id.value())
                          : make_view_scoped_render_resource_id(pipeline::RenderResourceId(id.value()), view);
}

void scope_texture_reference(feature::RenderTextureReference& reference, RenderViewId view,
                             bool canonical_view) {
    if (reference.external)
        reference.id = feature::RenderResourceId(scope_resource_id(reference.id, view, canonical_view).value());
}

void scope_scene_reference(feature::RenderSceneReference& reference, RenderViewId view, bool canonical_view) {
    reference.id = feature::RenderResourceId(scope_resource_id(reference.id, view, canonical_view).value());
}

void scope_light_list_declaration(feature::RenderLightListDeclaration& declaration, RenderViewId view,
                                  bool canonical_view) {
    if (declaration.id.is_valid())
        declaration.id = feature::RenderResourceId(scope_resource_id(declaration.id, view, canonical_view).value());
}

void scope_light_list_reference(feature::RenderLightListReference& reference, RenderViewId view,
                                bool canonical_view) {
    if (reference.id.is_valid())
        reference.id = feature::RenderResourceId(scope_resource_id(reference.id, view, canonical_view).value());
}

void scope_color_attachment(feature::RenderColorAttachment& attachment, RenderViewId view, bool canonical_view) {
    scope_texture_reference(attachment.target, view, canonical_view);
}

void scope_depth_attachment(feature::RenderDepthAttachment& attachment, RenderViewId view, bool canonical_view) {
    scope_texture_reference(attachment.target, view, canonical_view);
}

void scope_operation(feature::RenderOperation& operation, RenderViewId view, bool canonical_view) {
    std::visit([&](auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, feature::RenderClearColor>) {
            scope_texture_reference(value.target, view, canonical_view);
        } else if constexpr (std::is_same_v<T, feature::RenderCopyTexture>) {
            scope_texture_reference(value.source, view, canonical_view);
            scope_texture_reference(value.destination, view, canonical_view);
        } else if constexpr (std::is_same_v<T, feature::RenderDrawScene>) {
            scope_scene_reference(value.scene, view, canonical_view);
            scope_texture_reference(value.target, view, canonical_view);
            if (value.depth) scope_depth_attachment(*value.depth, view, canonical_view);
            if (value.directional_shadow) scope_texture_reference(*value.directional_shadow, view, canonical_view);
        } else if constexpr (std::is_same_v<T, feature::RenderDrawDepthScene>) {
            scope_scene_reference(value.scene, view, canonical_view);
            scope_depth_attachment(value.depth, view, canonical_view);
        } else if constexpr (std::is_same_v<T, feature::RenderDrawGBufferScene>) {
            scope_scene_reference(value.scene, view, canonical_view);
            scope_color_attachment(value.albedo, view, canonical_view);
            scope_color_attachment(value.normal, view, canonical_view);
            scope_color_attachment(value.material, view, canonical_view);
            scope_depth_attachment(value.depth, view, canonical_view);
        } else if constexpr (std::is_same_v<T, feature::RenderDeferredLightingScene>) {
            scope_scene_reference(value.scene, view, canonical_view);
            scope_texture_reference(value.albedo, view, canonical_view);
            scope_texture_reference(value.normal, view, canonical_view);
            scope_texture_reference(value.depth, view, canonical_view);
            scope_color_attachment(value.hdr, view, canonical_view);
            if (value.directional_shadow) scope_texture_reference(*value.directional_shadow, view, canonical_view);
        } else if constexpr (std::is_same_v<T, feature::RenderPostProcessScene>) {
            scope_scene_reference(value.scene, view, canonical_view);
            scope_texture_reference(value.hdr, view, canonical_view);
            scope_texture_reference(value.depth, view, canonical_view);
            scope_color_attachment(value.output, view, canonical_view);
        } else if constexpr (std::is_same_v<T, feature::RenderLightCullingScene>) {
            scope_scene_reference(value.scene, view, canonical_view);
            scope_texture_reference(value.camera_depth, view, canonical_view);
            scope_light_list_declaration(value.output, view, canonical_view);
            if (value.directional_shadow) scope_texture_reference(*value.directional_shadow, view, canonical_view);
        } else if constexpr (std::is_same_v<T, feature::RenderDrawClusteredScene>) {
            scope_scene_reference(value.scene, view, canonical_view);
            scope_light_list_reference(value.light_list, view, canonical_view);
            scope_color_attachment(value.hdr, view, canonical_view);
            scope_depth_attachment(value.depth, view, canonical_view);
            if (value.directional_shadow) scope_texture_reference(*value.directional_shadow, view, canonical_view);
        }
    }, operation);
}
foundation::ResultValue<const RenderFeatureSceneBinding*> select_scene(
    std::span<const RenderFeatureSceneBinding> scenes, const feature::RenderSceneReference& scene,
    const feature::RenderResourceOwner& owner, uint64_t frame_index) {
    const RenderFeatureSceneBinding* selected = nullptr;
    for (const auto& binding : scenes) {
        if (binding.id != scene.id || (binding.owner.is_valid() && binding.owner != owner.instance_id)) continue;
        if (selected) return {foundation::invalid_argument("scene draw binding is ambiguous"), {}};
        selected = &binding;
    }
    if (!selected || selected->version != scene.version || !selected->snapshot ||
        !selected->snapshot->is_valid() || selected->snapshot->frame_number != frame_index)
        return {foundation::invalid_state("scene draw binding is missing, stale or owned by another instance"), {}};
    return {foundation::Result::success(), selected};
}
bool compatible_attachment(const rhi::TextureDesc& attachment, const RenderSceneSnapshot& snapshot,
                           rhi::TextureCapability capability) {
    return rhi::has_flag(attachment.capabilities, capability) && attachment.dimension == rhi::TextureDimension::kTex2D &&
            attachment.samples == rhi::SampleCount::kX1 && attachment.mip_levels == 1 && attachment.array_layers == 1 &&
            attachment.extent.depth == 1 && attachment.extent.width == snapshot.view.output_extent.width &&
            attachment.extent.height == snapshot.view.output_extent.height;
}
bool compatible_attachment(const rhi::TextureDesc& attachment, const RenderView& view,
                           rhi::TextureCapability capability) {
    return rhi::has_flag(attachment.capabilities, capability) && attachment.dimension == rhi::TextureDimension::kTex2D &&
        attachment.samples == rhi::SampleCount::kX1 && attachment.mip_levels == 1 && attachment.array_layers == 1 &&
        attachment.extent.depth == 1 && attachment.extent.width == view.output_extent.width &&
        attachment.extent.height == view.output_extent.height;
}
bool compatible_depth(const render_graph::RenderSceneDrawTargets& targets, const RenderSceneSnapshot& snapshot) {
    return targets.depth.is_valid() && targets.depth_description.format == rhi::Format::kD32Float &&
        compatible_attachment(targets.depth_description, snapshot, rhi::TextureCapability::kDepthStencil);
}
bool compatible_depth(const render_graph::RenderSceneDrawTargets& targets, const RenderView& view) {
    return targets.depth.is_valid() && targets.depth_description.format == rhi::Format::kD32Float &&
        rhi::has_flag(targets.depth_description.capabilities, rhi::TextureCapability::kDepthStencil) &&
        targets.depth_description.dimension == rhi::TextureDimension::kTex2D &&
        targets.depth_description.samples == rhi::SampleCount::kX1 && targets.depth_description.mip_levels == 1 &&
        targets.depth_description.array_layers == 1 && targets.depth_description.extent.depth == 1 &&
        targets.depth_description.extent.width == view.output_extent.width &&
        targets.depth_description.extent.height == view.output_extent.height;
}
struct RenderLightListBundle final : render_graph::RenderLightListLease {
    RenderLightListBundle(render_graph::RenderFeatureBuildScope scope, render_graph::RenderLightListProduction production)
        : RenderLightListLease(std::move(scope), std::move(production)) {}
    std::shared_ptr<const RenderSceneSnapshot> snapshot{};
    feature::RenderLightCullingScene intent{};
    render_graph::RenderSceneDepthProduction camera_depth{};
    std::optional<render_graph::RenderSceneDepthProduction> directional_depth{};
    RenderShadowView shadow{};
    RenderLightCullingResource resource{};
    pipeline::RenderGraphBuffer clusters{};
};
bool valid_build_scope(const render_graph::RenderFeatureBuildScope& scope,
                       pipeline::RenderGraphTexture depth) {
    return scope.identity && scope.device && scope.frame && scope.graph_id != 0 &&
        depth.is_valid() && depth.graph_id == scope.graph_id && scope.frame->is_frame_active() &&
        scope.frame->frame_number() == scope.frame_index && &scope.frame->device() == scope.device;
}
bool compatible_light_depth(const render_graph::RenderSceneDrawTargets& targets, const RenderSceneSnapshot& snapshot) {
    return compatible_depth(targets, snapshot) &&
        rhi::has_flag(targets.depth_description.capabilities, rhi::TextureCapability::kSampled);
}
bool valid_depth_production(const render_graph::RenderSceneDepthProduction* production,
    const render_graph::RenderFeatureBuildScope& scope, const feature::RenderSceneReference& scene,
    const feature::RenderTextureReference& reference, pipeline::RenderGraphTexture texture,
    feature::RenderDepthSceneProjection projection, const RenderSceneSnapshot& snapshot) {
    return production && production->matches(scope, scene, reference, texture, projection, &snapshot);
}
}
foundation::Result RenderFeatureDrawLowerer::validate_native_source(
    const render_graph::RenderSceneDrawTargets& targets, const feature::RenderResourceOwner& owner,
    uint64_t frame_index, render_graph::RenderSceneOperationKind kind) const {
    if (!registered_native_) return foundation::Result::success();
    if (!pass_host_.is_valid()) return foundation::invalid_state("registered scene draw has a null or retired native pass host");
    const auto* source = targets.source;
    if (!source || source->kind != kind || source->owner != owner ||
        source->work.domain != feature::FeatureDomain::kRender || source->work.kind != render_graph::RenderWorkItemKind::kExecute ||
        !source->work.id.is_valid() || !source->work.feature_definition_id.is_valid() || !source->work.owner_module.is_valid() ||
        source->work.instance_id != owner.instance_id || source->work.node_id != owner.node_id ||
        !source->work.invocation.invocation_id.is_valid() || !source->work.invocation.generation.is_valid() ||
        source->work.frame_index != frame_index || owner.definition_digest == 0 || owner.resource_epoch == 0)
        return foundation::invalid_state("registered scene draw has no matching actual Feature operation source");
    return foundation::Result::success();
}
foundation::ResultValue<std::shared_ptr<const render_graph::RenderLightListLease>> RenderFeatureDrawLowerer::lower(
    pipeline::RenderGraphBuilder& builder, const feature::RenderLightCullingScene& draw,
    const render_graph::RenderSceneDrawTargets& targets, const feature::RenderResourceOwner& owner,
    const render_graph::RenderFeatureBuildScope& scope, const render_graph::RenderLightListProduction& production) const {
    if (const auto result = validate_native_source(targets, owner, scope.frame_index,
            render_graph::RenderSceneOperationKind::kLightCullingScene); !result) return {result, {}};
    const auto binding = select_scene(scenes_, draw.scene, owner, scope.frame_index);
    if (!binding) return {binding.result, {}};
    const auto* selected = binding.value;
    const auto& snapshot = *selected->snapshot;
    RenderShadowView shadow_view{};
    if (draw.directional_shadow) {
        const auto fitted = make_directional_shadow_view(snapshot);
        if (!fitted) return {fitted.result, {}};
        shadow_view = fitted.value;
    }
    if (!builder.is_runtime() || !valid_build_scope(scope, targets.depth) || production.owner != owner ||
        !compatible_light_depth(targets, snapshot) ||
        !valid_depth_production(targets.depth_production, scope, draw.scene, draw.camera_depth, targets.depth,
            feature::RenderDepthSceneProjection::kCamera, snapshot))
        return {foundation::invalid_state("light culling requires current camera depth from the selected scene snapshot"), {}};
    if (draw.directional_shadow.has_value() != targets.shadow.is_valid() ||
        (draw.directional_shadow && (targets.shadow.graph_id != scope.graph_id ||
            targets.shadow_description.format != rhi::Format::kD32Float ||
            !compatible_attachment(targets.shadow_description,
                shadow_view.enabled ? shadow_view.view : snapshot.view, rhi::TextureCapability::kSampled) ||
            !valid_depth_production(targets.shadow_production, scope, draw.scene, *draw.directional_shadow, targets.shadow,
                feature::RenderDepthSceneProjection::kDirectionalShadow, snapshot))))
        return {foundation::invalid_state("light culling shadow depth does not belong to this scene production"), {}};
    if (!selected->resolver) return {foundation::invalid_state("light culling requires a native resource resolver"), {}};
    auto bundle = std::make_shared<RenderLightListBundle>(scope, production);
    bundle->snapshot = selected->snapshot;
    bundle->intent = draw;
    bundle->camera_depth = *targets.depth_production;
    if (draw.directional_shadow) {
        bundle->directional_depth = *targets.shadow_production;
        bundle->shadow = shadow_view;
    }
    const auto resource = selected->resolver->resolve_light_culling(snapshot, bundle->shadow);
    if (!resource) return {resource.result, {}};
    if (!light_culling_resource_has_origin(resource.value, *scope.frame, *scope.device, snapshot) ||
        !resource.value.pipeline.is_valid() || !resource.value.constants.is_valid() || !resource.value.lights.is_valid() ||
        !resource.value.prepare_compute || !resource.value.prepare_forward ||
        resource.value.shadow_enabled != bundle->shadow.enabled)
        return {foundation::invalid_state("light culling resource has another frame, device, snapshot or native payload"), {}};
    bundle->resource = resource.value;
    rhi::BufferDesc clusters_desc{};
    clusters_desc.size = resource.value.cluster_bytes;
    clusters_desc.capabilities = rhi::BufferCapability::kStorage;
    const auto clusters = builder.create_buffer(clusters_desc);
    if (!clusters) return {clusters.result, {}};
    bundle->clusters = clusters.value;
    const RenderFeatureContext context{selected->snapshot.get(), selected->resolver, nullptr, pass_host_,
        registered_native_, registered_native_ ? targets.source : nullptr};
    const auto result = build_light_culling_render_graph(builder, context, targets.depth, bundle->clusters, bundle->resource);
    return {result, result ? std::move(bundle) : nullptr};
}
foundation::Result RenderFeatureDrawLowerer::lower(pipeline::RenderGraphBuilder& builder,
    const feature::RenderDrawClusteredScene& draw, const render_graph::RenderSceneDrawTargets& targets,
    const feature::RenderResourceOwner& owner, const render_graph::RenderFeatureBuildScope& scope,
    const render_graph::RenderLightListLease& light_list) const {
    if (const auto result = validate_native_source(targets, owner, scope.frame_index,
            render_graph::RenderSceneOperationKind::kDrawClusteredScene); !result) return result;
    const auto* bundle = dynamic_cast<const RenderLightListBundle*>(&light_list);
    if (!bundle || bundle->scope() != scope || !valid_build_scope(scope, targets.depth))
        return foundation::invalid_state("clustered draw light list belongs to another build attempt");
    const auto binding = select_scene(scenes_, draw.scene, owner, scope.frame_index);
    if (!binding) return binding.result;
    const auto* selected = binding.value;
    const auto& snapshot = *selected->snapshot;
    if (selected->snapshot.get() != bundle->snapshot.get() || draw.scene.id != bundle->intent.scene.id ||
        draw.scene.version != bundle->intent.scene.version ||
        draw.light_list != feature::RenderLightListReference{bundle->intent.output.id, bundle->intent.output.version,
            bundle->production().invocation.invocation_id, bundle->production().invocation.generation} ||
        draw.depth.load != feature::RenderAttachmentLoad::kLoad || !compatible_light_depth(targets, snapshot) ||
        !valid_depth_production(targets.depth_production, scope, draw.scene, draw.depth.target, targets.depth,
            feature::RenderDepthSceneProjection::kCamera, snapshot) ||
        !bundle->camera_depth.same_contents(*targets.depth_production) ||
        !bundle->camera_depth.matches(scope, draw.scene, draw.depth.target, targets.depth,
            feature::RenderDepthSceneProjection::kCamera, &snapshot))
        return foundation::invalid_state("clustered draw camera depth or snapshot differs from its producer");
    if (draw.directional_shadow.has_value() != bundle->directional_depth.has_value() ||
        draw.directional_shadow.has_value() != targets.shadow.is_valid() ||
        (draw.directional_shadow && (!compatible_attachment(targets.shadow_description,
                bundle->shadow.enabled ? bundle->shadow.view : snapshot.view,
                rhi::TextureCapability::kSampled) ||
            !valid_depth_production(targets.shadow_production, scope, draw.scene,
                *draw.directional_shadow, targets.shadow, feature::RenderDepthSceneProjection::kDirectionalShadow, snapshot) ||
            !bundle->directional_depth->same_contents(*targets.shadow_production) ||
            !bundle->directional_depth->matches(scope, draw.scene, *draw.directional_shadow, targets.shadow,
                feature::RenderDepthSceneProjection::kDirectionalShadow, &snapshot))))
        return foundation::invalid_state("clustered draw shadow depth differs from its producer");
    if (!targets.color.is_valid() || targets.color_description.format != rhi::Format::kRgba16Float ||
        !compatible_attachment(targets.color_description, snapshot, rhi::TextureCapability::kColorAttachment) ||
        !light_culling_resource_has_origin(bundle->resource, *scope.frame, *scope.device, snapshot))
        return foundation::invalid_argument("clustered HDR target or native light-list origin is incompatible");
    const auto& clear = draw.hdr.clear_color;
    const OpaqueDrawOptions options{draw.hdr.load == feature::RenderAttachmentLoad::kLoad,
        {clear[0], clear[1], clear[2], clear[3]},
        {targets.color_description.format, targets.color_description.samples, targets.depth_description.format},
        targets.depth, true, draw.depth.clear_depth};
    const RenderFeatureContext context{selected->snapshot.get(), selected->resolver, nullptr, pass_host_,
        registered_native_, registered_native_ ? targets.source : nullptr};
    return build_opaque_render_graph(builder, context, true,
        bundle->shadow.enabled ? targets.shadow : pipeline::RenderGraphTexture{}, bundle->clusters,
        nullptr, &bundle->resource, targets.color, {}, &options);
}
foundation::ResultValue<std::shared_ptr<const void>> RenderFeatureDrawLowerer::lower(
    pipeline::RenderGraphBuilder& builder, const feature::RenderPostProcessScene& draw,
    const render_graph::RenderSceneDrawTargets& targets, const feature::RenderResourceOwner& owner,
    uint64_t frame_index) const {
    if (const auto result = validate_native_source(targets, owner, frame_index,
            render_graph::RenderSceneOperationKind::kPostProcessScene); !result) return {result, {}};
    const auto binding = select_scene(scenes_, draw.scene, owner, frame_index);
    if (!binding) return {binding.result, {}};
    const auto* selected = binding.value;
    const auto& snapshot = *selected->snapshot;
    if (!targets.color.is_valid() ||
        !compatible_attachment(targets.color_description, snapshot, rhi::TextureCapability::kColorAttachment) ||
        !targets.additional_colors[0].is_valid() ||
        targets.additional_color_descriptions[0].format != rhi::Format::kRgba16Float ||
        !compatible_attachment(targets.additional_color_descriptions[0], snapshot, rhi::TextureCapability::kSampled) ||
        !targets.depth.is_valid() || targets.depth_description.format != rhi::Format::kD32Float ||
        !compatible_attachment(targets.depth_description, snapshot, rhi::TextureCapability::kSampled))
        return {foundation::invalid_argument("PostProcess output/HDR/depth format, capability or shape is incompatible"), {}};
    const auto& clear = draw.output.clear_color;
    const PostProcessDrawOptions options{draw.output.load == feature::RenderAttachmentLoad::kLoad,
        {clear[0], clear[1], clear[2], clear[3]},
        {targets.color_description.format, targets.color_description.samples, rhi::Format::kUnknown}, draw.settings};
    const RenderFeatureContext context{selected->snapshot.get(), selected->resolver, nullptr, pass_host_,
        registered_native_, registered_native_ ? targets.source : nullptr};
    const auto result = build_post_process_render_graph(builder, context, targets.additional_colors[0],
        targets.depth, targets.color, rhi::ResourceUsage::kColorAttachmentWrite, {}, &options);
    return {result, result ? selected->snapshot : nullptr};
}
foundation::ResultValue<std::shared_ptr<const void>> RenderFeatureDrawLowerer::lower(
    pipeline::RenderGraphBuilder& builder, const feature::RenderDeferredLightingScene& draw,
    const render_graph::RenderSceneDrawTargets& targets, const feature::RenderResourceOwner& owner,
    uint64_t frame_index) const {
    if (const auto result = validate_native_source(targets, owner, frame_index,
            render_graph::RenderSceneOperationKind::kDeferredLightingScene); !result) return {result, {}};
    const auto binding = select_scene(scenes_, draw.scene, owner, frame_index);
    if (!binding) return {binding.result, {}};
    const auto* selected = binding.value;
    const auto& snapshot = *selected->snapshot;
    RenderShadowView shadow_view{};
    if (draw.directional_shadow) {
        const auto fitted = make_directional_shadow_view(snapshot);
        if (!fitted) return {fitted.result, {}};
        shadow_view = fitted.value;
    }
    if (!targets.color.is_valid() || targets.color_description.format != rhi::Format::kRgba16Float ||
        !compatible_attachment(targets.color_description, snapshot, rhi::TextureCapability::kColorAttachment))
        return {foundation::invalid_argument("DeferredLighting HDR output format, capability or shape is incompatible"), {}};
    for (size_t index = 0; index < targets.additional_colors.size(); ++index)
        if (!targets.additional_colors[index].is_valid() ||
            targets.additional_color_descriptions[index].format != rhi::Format::kRgba16Float ||
            !compatible_attachment(targets.additional_color_descriptions[index], snapshot, rhi::TextureCapability::kSampled))
            return {foundation::invalid_argument("DeferredLighting GBuffer input format, capability or shape is incompatible"), {}};
    if (!targets.depth.is_valid() || targets.depth_description.format != rhi::Format::kD32Float ||
        !compatible_attachment(targets.depth_description, snapshot, rhi::TextureCapability::kSampled) ||
        draw.directional_shadow.has_value() != targets.shadow.is_valid() ||
        (draw.directional_shadow && (targets.shadow_description.format != rhi::Format::kD32Float ||
            !compatible_attachment(targets.shadow_description,
                shadow_view.enabled ? shadow_view.view : snapshot.view, rhi::TextureCapability::kSampled))))
        return {foundation::invalid_argument("DeferredLighting depth/shadow input format, capability or shape is incompatible"), {}};
    const RenderFeatureContext context{selected->snapshot.get(), selected->resolver, nullptr, pass_host_,
        registered_native_, registered_native_ ? targets.source : nullptr};
    RenderShadowResource shadow_resource{};
    if (draw.directional_shadow) {
        if (shadow_view.enabled) {
            const auto prepared = prepare_shadow_resources(builder, context, shadow_view);
            if (!prepared) return {prepared.result, {}};
            shadow_resource = prepared.value;
        }
    }
    const auto& clear = draw.hdr.clear_color;
    const DeferredLightingDrawOptions options{draw.hdr.load == feature::RenderAttachmentLoad::kLoad,
        {clear[0], clear[1], clear[2], clear[3]},
        {targets.color_description.format, targets.color_description.samples, rhi::Format::kUnknown}};
    const auto result = build_deferred_lighting_render_graph(builder, context, targets.additional_colors[0],
        targets.additional_colors[1], targets.depth, targets.color,
        shadow_resource.graph_buffer.is_valid() ? targets.shadow : pipeline::RenderGraphTexture{},
        shadow_resource.graph_buffer.is_valid() ? &shadow_resource : nullptr, {}, &options);
    return {result, result ? selected->snapshot : nullptr};
}
foundation::ResultValue<std::shared_ptr<const void>> RenderFeatureDrawLowerer::lower(
    pipeline::RenderGraphBuilder& builder, const feature::RenderDrawGBufferScene& draw,
    const render_graph::RenderSceneDrawTargets& targets, const feature::RenderResourceOwner& owner,
    uint64_t frame_index) const {
    if (const auto result = validate_native_source(targets, owner, frame_index,
            render_graph::RenderSceneOperationKind::kDrawGBufferScene); !result) return {result, {}};
    const auto binding = select_scene(scenes_, draw.scene, owner, frame_index);
    if (!binding) return {binding.result, {}};
    const auto* selected = binding.value;
    const auto& snapshot = *selected->snapshot;
    const std::array colors{targets.color, targets.additional_colors[0], targets.additional_colors[1]};
    const std::array descriptions{targets.color_description, targets.additional_color_descriptions[0],
        targets.additional_color_descriptions[1]};
    const std::array formats{rhi::Format::kRgba16Float, rhi::Format::kRgba16Float, rhi::Format::kRgba8Unorm};
    for (size_t index = 0; index < colors.size(); ++index)
        if (!colors[index].is_valid() || descriptions[index].format != formats[index] ||
            !compatible_attachment(descriptions[index], snapshot, rhi::TextureCapability::kColorAttachment))
            return {foundation::invalid_argument("GBuffer MRT format, capability or shape is incompatible"), {}};
    if (!compatible_depth(targets, snapshot))
        return {foundation::invalid_argument("GBuffer depth format, capability or shape is incompatible"), {}};
    GBufferDrawOptions options{};
    const std::array attachments{&draw.albedo, &draw.normal, &draw.material};
    for (size_t index = 0; index < attachments.size(); ++index) {
        const auto& attachment = *attachments[index];
        options.preserve_colors[index] = attachment.load == feature::RenderAttachmentLoad::kLoad;
        const auto& clear = attachment.clear_color;
        options.clear_colors[index] = {clear[0], clear[1], clear[2], clear[3]};
    }
    options.preserve_depth = draw.depth.load == feature::RenderAttachmentLoad::kLoad;
    options.clear_depth = draw.depth.clear_depth;
    options.attachment_contract = {descriptions[0].format, descriptions[0].samples, targets.depth_description.format,
        {descriptions[1].format, descriptions[2].format}};
    const RenderFeatureContext context{selected->snapshot.get(), selected->resolver, nullptr, pass_host_,
        registered_native_, registered_native_ ? targets.source : nullptr};
    const auto result = build_gbuffer_render_graph(builder, context, colors[0], colors[1], colors[2], targets.depth, {}, &options);
    return {result, result ? selected->snapshot : nullptr};
}
foundation::ResultValue<std::shared_ptr<const void>> RenderFeatureDrawLowerer::lower(
    pipeline::RenderGraphBuilder& builder, const feature::RenderDrawScene& draw,
    const render_graph::RenderSceneDrawTargets& targets,
    const feature::RenderResourceOwner& owner, uint64_t frame_index) const {
    if (const auto result = validate_native_source(targets, owner, frame_index,
            render_graph::RenderSceneOperationKind::kDrawScene); !result) return {result, {}};
    const auto binding = select_scene(scenes_, draw.scene, owner, frame_index);
    if (!binding) return {binding.result, {}};
    const auto* selected = binding.value;
    const auto& snapshot = *selected->snapshot;
    const auto& desc = targets.color_description;
    RenderShadowView shadow_view{};
    if (draw.directional_shadow) {
        const auto fitted = make_directional_shadow_view(snapshot);
        if (!fitted) return {fitted.result, {}};
        shadow_view = fitted.value;
    }
    if (!targets.color.is_valid() || !compatible_attachment(desc, snapshot, rhi::TextureCapability::kColorAttachment))
        return {foundation::invalid_argument("scene draw target attachment, samples, layers or extent is incompatible"), {}};
    if (draw.depth.has_value() != targets.depth.is_valid() ||
        (draw.depth && !compatible_depth(targets, snapshot)))
        return {foundation::invalid_argument("scene draw depth binding format, capability or shape is incompatible"), {}};
    if (draw.directional_shadow.has_value() != targets.shadow.is_valid() ||
        (draw.directional_shadow && (targets.shadow_description.format != rhi::Format::kD32Float ||
            !compatible_attachment(targets.shadow_description,
                shadow_view.enabled ? shadow_view.view : snapshot.view, rhi::TextureCapability::kSampled))))
        return {foundation::invalid_argument("scene shadow input format, capability or shape is incompatible"), {}};
    const RenderFeatureContext context{selected->snapshot.get(), selected->resolver, nullptr, pass_host_,
        registered_native_, registered_native_ ? targets.source : nullptr};
    const OpaqueDrawOptions options{
        draw.load == feature::RenderAttachmentLoad::kLoad,
        {draw.clear_color[0], draw.clear_color[1], draw.clear_color[2], draw.clear_color[3]},
        {desc.format, desc.samples, draw.depth ? targets.depth_description.format : rhi::Format::kUnknown},
        targets.depth, draw.depth && draw.depth->load == feature::RenderAttachmentLoad::kLoad,
        draw.depth ? draw.depth->clear_depth : 1.f};
    RenderShadowResource shadow_resource{};
    if (draw.directional_shadow) {
        if (shadow_view.enabled) {
            const auto prepared = prepare_shadow_resources(builder, context, shadow_view);
            if (!prepared) return {prepared.result, {}};
            shadow_resource = prepared.value;
        }
    }
    const auto result = build_opaque_render_graph(builder, context, false,
        shadow_resource.graph_buffer.is_valid() ? targets.shadow : pipeline::RenderGraphTexture{}, {},
        shadow_resource.graph_buffer.is_valid() ? &shadow_resource : nullptr, nullptr, targets.color, {}, &options);
    return {result, result ? selected->snapshot : nullptr};
}
foundation::ResultValue<std::shared_ptr<const void>> RenderFeatureDrawLowerer::lower(
    pipeline::RenderGraphBuilder& builder, const feature::RenderDrawDepthScene& draw,
    const render_graph::RenderSceneDrawTargets& targets,
    const feature::RenderResourceOwner& owner, uint64_t frame_index) const {
    if (const auto result = validate_native_source(targets, owner, frame_index,
            render_graph::RenderSceneOperationKind::kDrawDepthScene); !result) return {result, {}};
    if (registered_native_ && targets.source->depth_projection != draw.projection)
        return {foundation::invalid_state("registered depth operation source has another projection"), {}};
    const auto binding = select_scene(scenes_, draw.scene, owner, frame_index);
    if (!binding) return {binding.result, {}};
    const auto* selected = binding.value;
    const auto& snapshot = *selected->snapshot;
    auto view = snapshot.view;
    std::span<const RenderInstance> instances(snapshot.instances);
    std::string_view pass_name = "Renderer.DepthScene";
    if (draw.projection == feature::RenderDepthSceneProjection::kDirectionalShadow) {
        const auto shadow = make_directional_shadow_view(snapshot);
        if (!shadow) return {shadow.result, {}};
        pass_name = "Renderer.ShadowScene";
        if (shadow.value.enabled) {
            view = shadow.value.view;
            instances = snapshot.shadow_casters.empty() ? std::span<const RenderInstance>(snapshot.instances)
                                                        : std::span<const RenderInstance>(snapshot.shadow_casters);
        } else {
            instances = {};
        }
    } else if (draw.projection != feature::RenderDepthSceneProjection::kCamera) {
        return {foundation::invalid_argument("depth scene projection is invalid"), {}};
    }
    if (targets.color.is_valid() || !compatible_depth(targets, view))
        return {foundation::invalid_argument("depth-only scene target format, capability or shape is incompatible"), {}};
    const RenderFeatureContext context{selected->snapshot.get(), selected->resolver, nullptr, pass_host_,
        registered_native_, registered_native_ ? targets.source : nullptr};
    const DepthDrawOptions options{draw.depth.load == feature::RenderAttachmentLoad::kLoad, draw.depth.clear_depth,
        {rhi::Format::kUnknown, targets.depth_description.samples, targets.depth_description.format}};
    const auto result = build_depth_render_graph(builder, context, view, targets.depth,
        pass_name, instances, {}, &options);
    return {result, result ? selected->snapshot : nullptr};
}

feature::RenderFeatureOutput scope_render_feature_output(const feature::RenderFeatureOutput& output,
                                                         RenderViewId view, bool canonical_view) {
    auto scoped = output;
    scoped.view_key = view.value();
    if (scoped.intent.final_output)
        scope_texture_reference(*scoped.intent.final_output, view, canonical_view);
    if (scoped.intent.depth_output)
        scope_texture_reference(*scoped.intent.depth_output, view, canonical_view);
    for (auto& operation : scoped.intent.operations)
        scope_operation(operation, view, canonical_view);
    return scoped;
}

foundation::Result build_render_feature_outputs(
    render_graph::RenderGraph& graph, render_graph::FrameContext& frame,
    std::shared_ptr<const std::vector<feature::RenderFeatureOutput>> outputs,
    std::span<const feature::RenderResourceOwner> live_owners,
    std::span<const render_graph::RenderGraphFeatureTextureBinding> textures,
    std::span<const RenderFeatureSceneBinding> scenes, render_graph::RenderFeatureHistory& history,
    uint64_t history_key, std::optional<render_graph::RenderPassExecutionHost> pass_host) {
    if (const auto result = history.reconcile(render_graph::RenderFeatureHistoryKey{history_key}, live_owners); !result)
        return result;
    auto draw_lowerer = pass_host ? RenderFeatureDrawLowerer(scenes, *pass_host) : RenderFeatureDrawLowerer(scenes);
    const render_graph::RenderGraphFeatureRuntimeDesc desc{.textures = textures,
        .draw_lowerer = &draw_lowerer, .current_frame_owner = &frame, .history_key = history_key};
    return render_graph::build_render_graph_features(graph, std::move(outputs), desc, frame.frame_number(), &history);
}

}
