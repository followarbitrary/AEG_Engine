/**
 * @file renderer_authoring.cpp
 * @brief Engine-owned authoring catalog for deferred and forward rendering.
 */
#include <array>
#include <string>

#include "render_pipeline_contract.hpp"
#include "renderer_variants.hpp"

namespace aeg::renderer {
namespace {
constexpr pipeline::ModuleId kRendererModule{"Renderer"};
constexpr pipeline::Version kVersion{1, 0, 0};

[[nodiscard]] uint64_t usage_bit(rhi::ResourceUsage usage) {
    return uint64_t{1} << static_cast<uint8_t>(usage);
}

[[nodiscard]] pipeline::ResourceConstraints texture_constraints(const char* format, uint64_t capabilities) {
    return {
        .resource_class = pipeline::ResourceClass::kTexture,
        .formats = {format},
        .size_rule = "ViewExtent",
        .sample_count = 1,
        .capability_mask = capabilities,
        .usage_mask = usage_bit(rhi::ResourceUsage::kSampledRead) |
                      usage_bit(std::string_view(format) == "D32Float" ? rhi::ResourceUsage::kDepthStencilWrite
                                                                       : rhi::ResourceUsage::kColorAttachmentWrite)};
}

[[nodiscard]] pipeline::ResourceConstraints buffer_constraints(uint64_t capabilities) {
    return {.resource_class = pipeline::ResourceClass::kBuffer,
            .formats = {},
            .size_rule = "ViewExtent-derived cluster count",
            .sample_count = 0,
            .capability_mask = capabilities,
            .usage_mask = usage_bit(rhi::ResourceUsage::kStorageRead) | usage_bit(rhi::ResourceUsage::kStorageWrite)};
}

[[nodiscard]] std::vector<pipeline::PortDescriptor> pass_ports(pipeline::RenderPassTypeId type) {
    using namespace pipeline;
    std::vector<PortDescriptor> ports;
    const auto add = [&](const char* name, PortDirection direction, const char* format, bool buffer = false) {
        const uint64_t id = RenderResourceId(name).value();
        auto constraints =
            buffer
                ? buffer_constraints(static_cast<uint64_t>(rhi::BufferCapability::kStorage))
                : texture_constraints(format, static_cast<uint64_t>(rhi::TextureCapability::kSampled |
                                                                    (std::string_view(format) == "D32Float"
                                                                         ? rhi::TextureCapability::kDepthStencil
                                                                         : rhi::TextureCapability::kColorAttachment)));
        if (std::string_view(name) == "Renderer.SceneColor")
            constraints.formats.clear();
        ports.push_back({.id = id,
                         .name = name,
                         .direction = direction,
                         .value_kind = PortValueKind::kRenderResource,
                         .schema_id = id,
                         .required = direction == PortDirection::kInput,
                         .min_connections = direction == PortDirection::kInput ? 1u : 0u,
                         .binding_id = id,
                         .resource_constraints = std::move(constraints)});
    };
    using D = PortDirection;
    if (type == kShadowPassTypeId)
        add("Renderer.ShadowMap", D::kOutput, "D32Float");
    if (type == kGBufferPassTypeId) {
        add("Renderer.GBuffer.Albedo", D::kOutput, "Rgba16Float");
        add("Renderer.GBuffer.Normal", D::kOutput, "Rgba16Float");
        add("Renderer.GBuffer.Material", D::kOutput, "Rgba8Unorm");
        add("Renderer.SceneDepth", D::kOutput, "D32Float");
    }
    if (type == kDeferredLightingPassTypeId) {
        add("Renderer.GBuffer.Albedo", D::kInput, "Rgba16Float");
        add("Renderer.GBuffer.Normal", D::kInput, "Rgba16Float");
        add("Renderer.GBuffer.Material", D::kInput, "Rgba8Unorm");
        add("Renderer.SceneDepth", D::kInput, "D32Float");
        add("Renderer.ShadowMap", D::kInput, "D32Float");
        add("Renderer.SceneLighting", D::kOutput, "Rgba16Float");
    }
    if (type == kSkyboxPostProcessPassTypeId) {
        add("Renderer.SceneLighting", D::kInput, "Rgba16Float");
        add("Renderer.SceneDepth", D::kInput, "D32Float");
        add("Renderer.SceneColor", D::kOutput, "Rgba8Unorm");
    }
    if (type == kZPrepassPassTypeId)
        add("Renderer.SceneDepth", D::kOutput, "D32Float");
    if (type == kLightCullingPassTypeId) {
        add("Renderer.SceneDepth", D::kInput, "D32Float");
        add("Renderer.LightClusters", D::kOutput, "", true);
    }
    if (type == kForwardOpaquePassTypeId) {
        add("Renderer.ShadowMap", D::kInput, "D32Float");
        add("Renderer.SceneDepth", D::kInput, "D32Float");
        add("Renderer.LightClusters", D::kInput, "", true);
        add("Renderer.SceneLighting", D::kOutput, "Rgba16Float");
    }
    return ports;
}

[[nodiscard]] pipeline::TypeCatalogEntry pipeline_entry(pipeline::PipelineId id, const char* name) {
    const detail::Variant variant =
        id == kDeferredRenderPipelineId ? detail::Variant::kDeferred : detail::Variant::kForward;
    const bool deferred = id == kDeferredRenderPipelineId;
    pipeline::PipelineAuthoringDeclaration declaration{};
    pipeline::StageId previous_stage{};
    const auto append_stage = [&](pipeline::StageId stage_id, const char* stage_name, pipeline::PhaseId phase,
                                  pipeline::ExecutionDomain domain) {
        declaration.stages.push_back({.id = stage_id,
                                      .name = stage_name,
                                      .phase = phase,
                                      .execution_domain = domain,
                                      .time_policy = pipeline::TimePolicy::kFrame});
        if (previous_stage.is_valid()) {
            declaration.fixed_orderings.push_back({previous_stage, stage_id});
        }
        previous_stage = stage_id;
    };
    const std::string extract_stage = detail::stage_name(variant, "RenderExtract");
    append_stage(pipeline::StageId(extract_stage), extract_stage.c_str(), pipeline::PhaseId("RenderExtract"),
                 pipeline::ExecutionDomain::kCpuWorker);
    for (const size_t index : detail::pass_indices(variant)) {
        const std::string stage = detail::stage_name(variant, detail::kPassNodes[index]);
        append_stage(pipeline::StageId(stage), stage.c_str(), pipeline::PhaseId("RenderBuild"),
                     pipeline::ExecutionDomain::kRenderThread);
    }
    const std::string present_stage = detail::stage_name(variant, "BeforePresent");
    append_stage(pipeline::StageId(present_stage), present_stage.c_str(), pipeline::PhaseId("Present"),
                 pipeline::ExecutionDomain::kRenderThread);
    const auto add_point = [&](const char* suffix) {
        const std::string name = detail::stage_name(variant, suffix);
        declaration.feature_points.push_back({.id = pipeline::FeaturePointId(name),
                                              .name = name,
                                              .anchor_stage = pipeline::StageId(name),
                                              .position = pipeline::FeaturePointPosition::kBefore,
                                              .domains = detail::kDomain,
                                              .scopes = detail::kScope,
                                              .flags = detail::point_flags(variant, suffix)});
    };
    add_point("RenderExtract");
    for (const size_t index : detail::pass_indices(variant)) {
        add_point(detail::kPassNodes[index]);
    }
    add_point("BeforePresent");
    declaration.default_features.push_back({
        .node_id = deferred ? "DeferredFeature" : "ForwardFeature",
        .type = deferred ? kDeferredRenderFeatureId : kForwardRenderFeatureId,
        .version = kVersion,
        .point = pipeline::FeaturePointId(deferred ? "Renderer.DeferredRenderPipeline.BeforePresent"
                                                   : "Renderer.ForwardRenderPipeline.BeforePresent"),
    });
    return {.key = {pipeline::AuthoringTypeKind::kPipeline, id.value()},
            .version = kVersion,
            .owner_module = kRendererModule,
            .name = name,
            .display_name = name,
            .domains = pipeline::to_mask(pipeline::FeatureDomain::kRender),
            .scopes = pipeline::to_mask(pipeline::PipelineScope::kFrame),
            .declaration = std::move(declaration)};
}

[[nodiscard]] pipeline::TypeCatalogEntry feature_entry(pipeline::FeatureId id, const char* name,
                                                       std::vector<pipeline::RenderPassTypeId> allowed,
                                                       std::vector<pipeline::DefaultPassNode> passes,
                                                       std::vector<pipeline::DefaultResourceNode> resources) {
    pipeline::FeatureAuthoringDeclaration declaration{};
    declaration.allowed_pass_types = std::move(allowed);
    declaration.default_passes = std::move(passes);
    declaration.default_resources = std::move(resources);
    for (size_t index = 1; index < declaration.default_passes.size(); ++index) {
        declaration.default_edges.push_back({.edge_id = declaration.default_passes[index - 1].node_id + "_to_" +
                                                        declaration.default_passes[index].node_id,
                                             .kind = pipeline::DocumentEdgeKind::kOrdering,
                                             .source = {declaration.default_passes[index - 1].node_id, 0},
                                             .destination = {declaration.default_passes[index].node_id, 0},
                                             .binding_id = 0});
    }
    for (const auto& resource : declaration.default_resources) {
        const auto binding = resource.resource_id.value();
        for (const auto& pass : declaration.default_passes) {
            for (const auto& port : pass_ports(pass.type)) {
                if (port.binding_id != binding)
                    continue;
                const bool output = port.direction == pipeline::PortDirection::kOutput;
                declaration.default_edges.push_back(
                    {.edge_id = pass.node_id + "_" + resource.node_id,
                     .kind = pipeline::DocumentEdgeKind::kRenderResource,
                     .source = output ? pipeline::DocumentEndpoint{pass.node_id, port.id}
                                      : pipeline::DocumentEndpoint{resource.node_id, binding},
                     .destination = output ? pipeline::DocumentEndpoint{resource.node_id, binding}
                                           : pipeline::DocumentEndpoint{pass.node_id, port.id},
                     .binding_id = binding});
            }
        }
    }
    declaration.allow_add_passes = false;
    return {.key = {pipeline::AuthoringTypeKind::kFeature, id.value()},
            .version = kVersion,
            .owner_module = kRendererModule,
            .name = name,
            .display_name = name,
            .domains = pipeline::to_mask(pipeline::FeatureDomain::kRender),
            .scopes = pipeline::to_mask(pipeline::PipelineScope::kFrame),
            .declaration = std::move(declaration)};
}

[[nodiscard]] pipeline::TypeCatalogEntry pass_entry(pipeline::RenderPassTypeId id, const char* name,
                                                    pipeline::RenderGraphPassType graph_type, rhi::QueueClass queue) {
    static_cast<void>(graph_type);
    const uint64_t queue_mask = uint64_t{1} << static_cast<uint8_t>(queue);
    pipeline::TypeCatalogEntry entry{
        .key = {pipeline::AuthoringTypeKind::kRenderPass, id.value()},
        .version = kVersion,
        .owner_module = kRendererModule,
        .name = name,
        .display_name = name,
        .domains = pipeline::to_mask(pipeline::FeatureDomain::kRender),
        .scopes = pipeline::to_mask(pipeline::PipelineScope::kFrame),
        .declaration = pipeline::PassAuthoringDeclaration{.type = id, .allowed_queue_mask = queue_mask}};
    entry.ports = pass_ports(id);
    return entry;
}

[[nodiscard]] pipeline::DefaultPassNode pass_node(const char* node, pipeline::RenderPassTypeId type) {
    return {.node_id = node, .type = type, .version = kVersion, .part_id = {}, .parameters = {}, .enabled = true};
}

[[nodiscard]] pipeline::DefaultResourceNode resource_node(const char* node, pipeline::RenderResourceId id,
                                                          pipeline::ResourceConstraints constraints) {
    if (id == pipeline::RenderResourceId("Renderer.SceneColor")) {
        constraints.formats.clear();
        constraints.usage_mask |= usage_bit(rhi::ResourceUsage::kPresent);
    }
    return {.node_id = node, .resource_id = id, .constraints = std::move(constraints)};
}

}  // namespace

foundation::Result register_renderer_authoring_types(pipeline::TypeCatalog& catalog) {
    using TC = rhi::TextureCapability;
    using BC = rhi::BufferCapability;
    const uint64_t sampled = static_cast<uint64_t>(TC::kSampled);
    const uint64_t color_sampled = static_cast<uint64_t>(TC::kColorAttachment) | sampled;
    const uint64_t depth_sampled = static_cast<uint64_t>(TC::kDepthStencil) | sampled;

    const pipeline::TypeCatalogEntry entries[] = {
        pipeline_entry(kDeferredRenderPipelineId, "Renderer.DeferredRenderPipeline"),
        pipeline_entry(kForwardRenderPipelineId, "Renderer.ForwardRenderPipeline"),
        feature_entry(
            kDeferredRenderFeatureId, "Renderer.DeferredRenderFeature",
            {kShadowPassTypeId, kGBufferPassTypeId, kDeferredLightingPassTypeId, kSkyboxPostProcessPassTypeId},
            {pass_node("Shadow", kShadowPassTypeId), pass_node("GBuffer", kGBufferPassTypeId),
             pass_node("DeferredLighting", kDeferredLightingPassTypeId),
             pass_node("SkyboxPostProcess", kSkyboxPostProcessPassTypeId)},
            {resource_node("ShadowMap", pipeline::RenderResourceId("Renderer.ShadowMap"),
                           texture_constraints("D32Float", depth_sampled)),
             resource_node("GBufferAlbedo", pipeline::RenderResourceId("Renderer.GBuffer.Albedo"),
                           texture_constraints("Rgba16Float", color_sampled)),
             resource_node("GBufferNormal", pipeline::RenderResourceId("Renderer.GBuffer.Normal"),
                           texture_constraints("Rgba16Float", color_sampled)),
             resource_node("GBufferMaterial", pipeline::RenderResourceId("Renderer.GBuffer.Material"),
                           texture_constraints("Rgba8Unorm", color_sampled)),
             resource_node("SceneDepth", pipeline::RenderResourceId("Renderer.SceneDepth"),
                           texture_constraints("D32Float", depth_sampled)),
             resource_node("SceneColor", pipeline::RenderResourceId("Renderer.SceneColor"),
                           texture_constraints("Rgba8Unorm", color_sampled)),
             resource_node("SceneLighting", pipeline::RenderResourceId("Renderer.SceneLighting"),
                           texture_constraints("Rgba16Float", color_sampled))}),
        feature_entry(
            kForwardRenderFeatureId, "Renderer.ForwardRenderFeature",
            {kShadowPassTypeId, kZPrepassPassTypeId, kLightCullingPassTypeId, kForwardOpaquePassTypeId,
             kSkyboxPostProcessPassTypeId},
            {pass_node("Shadow", kShadowPassTypeId), pass_node("ZPrepass", kZPrepassPassTypeId),
             pass_node("LightCulling", kLightCullingPassTypeId), pass_node("ForwardOpaque", kForwardOpaquePassTypeId),
             pass_node("SkyboxPostProcess", kSkyboxPostProcessPassTypeId)},
            {resource_node("SceneDepth", pipeline::RenderResourceId("Renderer.SceneDepth"),
                           texture_constraints("D32Float", depth_sampled)),
             resource_node("ShadowMap", pipeline::RenderResourceId("Renderer.ShadowMap"),
                           texture_constraints("D32Float", depth_sampled)),
             resource_node("LightClusters", pipeline::RenderResourceId("Renderer.LightClusters"),
                           buffer_constraints(static_cast<uint64_t>(BC::kStorage))),
             resource_node("SceneColor", pipeline::RenderResourceId("Renderer.SceneColor"),
                           texture_constraints("Rgba8Unorm", color_sampled)),
             resource_node("SceneLighting", pipeline::RenderResourceId("Renderer.SceneLighting"),
                           texture_constraints("Rgba16Float", color_sampled))}),
        pass_entry(kShadowPassTypeId, "Renderer.ShadowPass", pipeline::RenderGraphPassType::kGraphics,
                   rhi::QueueClass::kGraphics),
        pass_entry(kGBufferPassTypeId, "Renderer.GBufferPass", pipeline::RenderGraphPassType::kGraphics,
                   rhi::QueueClass::kGraphics),
        pass_entry(kDeferredLightingPassTypeId, "Renderer.DeferredLightingPass",
                   pipeline::RenderGraphPassType::kGraphics, rhi::QueueClass::kGraphics),
        pass_entry(kSkyboxPostProcessPassTypeId, "Renderer.SkyboxPostProcessPass",
                   pipeline::RenderGraphPassType::kGraphics, rhi::QueueClass::kGraphics),
        pass_entry(kZPrepassPassTypeId, "Renderer.ZPrepassPass", pipeline::RenderGraphPassType::kGraphics,
                   rhi::QueueClass::kGraphics),
        pass_entry(kLightCullingPassTypeId, "Renderer.LightCullingPass", pipeline::RenderGraphPassType::kCompute,
                   rhi::QueueClass::kCompute),
        pass_entry(kForwardOpaquePassTypeId, "Renderer.ForwardOpaquePass", pipeline::RenderGraphPassType::kGraphics,
                   rhi::QueueClass::kGraphics),
    };
    for (const pipeline::TypeCatalogEntry& entry : entries) {
        if (const foundation::Result result = catalog.add(entry); !result) {
            return result;
        }
    }
    return foundation::Result::success();
}

}  // namespace aeg::renderer
