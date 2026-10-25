/**
 * @file render_pipeline_graph.cpp
 * @brief Resource contracts and graph expansion for built-in renderer variants.
 */
#include "render_pipeline_graph.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "builtin_render_passes.hpp"
#include "render_feature_graph_builders.hpp"
#include "light_culling.hpp"
#include "post_process.hpp"
#include "renderer_variants_internal.hpp"
#include "shadow_rendering.hpp"

namespace aeg::renderer {
namespace {

using foundation::invalid_argument;
using foundation::invalid_state;
using foundation::Result;
using foundation::ResultValue;
using pipeline::RenderGraphBuffer;
using pipeline::RenderGraphBuilder;
using pipeline::RenderGraphTexture;
using pipeline::RenderResourceId;
using rhi::BufferCapability;
using rhi::BufferDesc;
using rhi::Format;
using rhi::MemoryClass;
using rhi::PipelineStage;
using rhi::QueueClass;
using rhi::ResourceUsage;
using rhi::ShaderStage;
using rhi::TextureCapability;
using rhi::TextureDesc;

inline constexpr RenderResourceId kShadowMap{"Renderer.ShadowMap"};
inline constexpr RenderResourceId kGBufferAlbedo{"Renderer.GBuffer.Albedo"};
inline constexpr RenderResourceId kGBufferNormal{"Renderer.GBuffer.Normal"};
inline constexpr RenderResourceId kGBufferMaterial{"Renderer.GBuffer.Material"};
inline constexpr RenderResourceId kLightClusters{"Renderer.LightClusters"};

[[nodiscard]] TextureDesc make_texture_desc(rhi::Extent3D extent, Format format, TextureCapability capabilities,
                                            const char* name) {
    TextureDesc desc{};
    desc.extent = extent;
    desc.format = format;
    desc.capabilities = capabilities;
    desc.memory = MemoryClass::kDeviceLocal;
    desc.initial_usage = ResourceUsage::kUndefined;
    desc.name = rhi::DebugName{name};
    return desc;
}

[[nodiscard]] Result validate_context(const RenderGraphBuilder& builder, const RenderFeatureContext& context) {
    if (!builder.is_runtime()) {
        return invalid_state("built-in renderer graph requires a frame runtime builder");
    }
    if (context.registered_native || context.operation_source) {
        return invalid_argument("fixed native topology cannot substitute for a captured Scene operation");
    }
    if (!context.is_valid()) {
        return invalid_argument("built-in renderer graph requires a valid RenderSceneSnapshot");
    }
    if (!context.scene_snapshot->view.color_target.is_valid() ||
        !context.scene_snapshot->view.depth_target.is_valid()) {
        return invalid_argument("built-in renderer graph requires color and depth targets");
    }
    const rhi::Extent3D extent{context.scene_snapshot->view.output_extent.width,
                               context.scene_snapshot->view.output_extent.height, 1};
    if (extent.width == 0 || extent.height == 0 || extent.depth == 0) {
        return invalid_argument("built-in renderer graph requires a non-empty output extent");
    }
    return Result::success();
}

[[nodiscard]] ResultValue<RenderGraphTexture> find_target(RenderGraphBuilder& builder, RenderResourceId id,
                                                          const char* name) {
    const auto target = builder.find_texture(id);
    if (!target) {
        return {target.result, {}};
    }
    if (!target.value.is_valid()) {
        return {invalid_state(std::string(name) + " target resolved to an invalid graph handle"), {}};
    }
    return target;
}

[[nodiscard]] Result validate_native_sequence(const NativePassSequence& configuration) {
    std::unordered_map<uint64_t, size_t> instances;
    for (const auto& pass : configuration.passes) {
        if (!pass.enabled) continue;
        if (!pass.instance.is_valid() || !instances.emplace(pass.instance.value(), instances.size()).second)
            return invalid_argument("native render pass instance is missing or duplicated");
    }
    std::vector<std::vector<size_t>> outgoing(instances.size());
    std::vector<size_t> indegree(instances.size());
    for (const auto& pass : configuration.passes) {
        if (!pass.enabled) continue;
        const auto destination = instances.find(pass.instance.value())->second;
        for (const auto source_instance : pass.ordered_after) {
            const auto source = instances.find(source_instance.value());
            if (source == instances.end())
                return invalid_argument("native render pass ordering references an inactive pass");
            if (source->second == destination)
                return invalid_argument("native render pass cannot be ordered after itself");
            outgoing[source->second].push_back(destination);
            ++indegree[destination];
        }
    }
    std::vector<size_t> ready;
    for (size_t index = 0; index < indegree.size(); ++index)
        if (indegree[index] == 0) ready.push_back(index);
    size_t visited = 0;
    while (!ready.empty()) {
        const auto source = ready.back(); ready.pop_back(); ++visited;
        for (const auto destination : outgoing[source])
            if (--indegree[destination] == 0) ready.push_back(destination);
    }
    return visited == instances.size() ? Result::success()
        : invalid_argument("native render pass ordering contains a cycle");
}

[[nodiscard]] Result apply_native_ordering(RenderGraphBuilder& builder,
                                           const NativePassSequence& configuration) {
    for (const auto& pass : configuration.passes) {
        if (!pass.enabled) continue;
        const auto destination = builder.native_pass(pass.instance);
        if (!destination) return destination.result;
        for (const auto source_instance : pass.ordered_after) {
            const auto source = builder.native_pass(source_instance);
            if (!source) return source.result;
            if (const auto result = builder.add_dependency(source.value, destination.value); !result) return result;
        }
    }
    return Result::success();
}

[[nodiscard]] ResultValue<RenderGraphTexture> create_lighting_target(RenderGraphBuilder& builder,
                                                                     rhi::Extent3D extent) {
    return builder.create_texture(
        RenderResourceId("Renderer.SceneLighting"),
        make_texture_desc(extent, kSceneLightingFormat,
                          TextureCapability::kSampled | TextureCapability::kColorAttachment, "Renderer.SceneLighting"));
}

[[nodiscard]] Result add_empty_culling_pass(RenderGraphBuilder& builder, const RenderFeatureContext& context,
                                            std::function<Result(pipeline::RenderPassBuilder&)> setup,
                                            pipeline::RenderPassInstanceId authored_instance = {}) {
    // An empty scene has no shaded pixels and needs no light-list dispatch.
    const auto pass =
        detail::add_builtin_render_pass(builder, context, kLightCullingPassTypeId, std::move(setup),
                                        [](pipeline::RenderPassExecutionContext&) { return Result::success(); },
                                        authored_instance);
    return pass ? Result::success() : pass.result;
}

[[nodiscard]] Result build_deferred(RenderGraphBuilder& builder, const RenderFeatureContext& context) {
    if (const Result result = validate_context(builder, context); !result) {
        return result;
    }
    if (const auto* sequence = context.native_sequence; sequence != nullptr) {
        if (const auto result = validate_native_sequence(*sequence); !result) return result;
    }
    const auto color = find_target(builder, context.scene_snapshot->view.color_target, "scene color");
    if (!color) {
        return color.result;
    }
    const auto depth = find_target(builder, context.scene_snapshot->view.depth_target, "scene depth");
    if (!depth) {
        return depth.result;
    }

    const rhi::Extent3D extent{context.scene_snapshot->view.output_extent.width,
                               context.scene_snapshot->view.output_extent.height, 1};
    const auto lighting_color = create_lighting_target(builder, extent);
    if (!lighting_color)
        return lighting_color.result;
    const auto shadow_view = make_directional_shadow_view(*context.scene_snapshot);
    if (!shadow_view)
        return shadow_view.result;
    const rhi::Extent3D shadow_extent{shadow_view.value.view.output_extent.width,
                                      shadow_view.value.view.output_extent.height, 1};
    const auto shadow = builder.create_texture(
        kShadowMap,
        make_texture_desc(shadow_extent, Format::kD32Float,
                          TextureCapability::kSampled | TextureCapability::kDepthStencil,
                          "Renderer.ShadowMap"));
    if (!shadow) {
        return shadow.result;
    }
    const auto albedo = builder.create_texture(
        kGBufferAlbedo, make_texture_desc(extent, Format::kRgba16Float,
                                          TextureCapability::kSampled | TextureCapability::kColorAttachment,
                                          "Renderer.GBuffer.Albedo"));
    if (!albedo) {
        return albedo.result;
    }
    const auto normal = builder.create_texture(
        kGBufferNormal, make_texture_desc(extent, Format::kRgba16Float,
                                          TextureCapability::kSampled | TextureCapability::kColorAttachment,
                                          "Renderer.GBuffer.Normal"));
    if (!normal) {
        return normal.result;
    }
    const auto material = builder.create_texture(
        kGBufferMaterial, make_texture_desc(extent, Format::kRgba8Unorm,
                                            TextureCapability::kSampled | TextureCapability::kColorAttachment,
                                            "Renderer.GBuffer.Material"));
    if (!material) {
        return material.result;
    }

    const auto shadow_resource = prepare_shadow_resources(builder, context, shadow_view.value,
                                                         RenderResourceId("Renderer.ShadowConstants"));
    if (!shadow_resource)
        return shadow_resource.result;
    const auto add_lighting_pass = [&](pipeline::RenderPassInstanceId authored_instance = {}) -> Result {
        return build_deferred_lighting_render_graph(builder, context, albedo.value, normal.value, depth.value,
            lighting_color.value, shadow_view.value.enabled ? shadow.value : RenderGraphTexture{},
            shadow_view.value.enabled ? &shadow_resource.value : nullptr, authored_instance);
    };

    if (const auto* sequence = context.native_sequence; sequence != nullptr) {
        bool has_final_output = false;
        for (const auto& pass : sequence->passes) {
            if (!pass.enabled)
                continue;
            if (pass.type == kShadowPassTypeId) {
                if (const auto result = build_shadow_render_graph(builder, context, shadow_view.value, shadow.value,
                                                                  pass.instance);
                    !result)
                    return result;
            } else if (pass.type == kGBufferPassTypeId) {
                if (const auto result =
                        build_gbuffer_render_graph(builder, context, albedo.value, normal.value, material.value,
                                                   depth.value, pass.instance);
                    !result)
                    return result;
            } else if (pass.type == kDeferredLightingPassTypeId) {
                if (const auto result = add_lighting_pass(pass.instance); !result)
                    return result;
            } else if (pass.type == kSkyboxPostProcessPassTypeId) {
                if (const auto result = build_post_process_render_graph(
                        builder, context, lighting_color.value, depth.value, color.value,
                        context.scene_snapshot->view.color_target_final_usage,
                        pass.instance);
                    !result)
                    return result;
                has_final_output = true;
            } else {
                return invalid_argument("Deferred renderer pass type is not supported by its fixed native adapter");
            }
        }
        if (!has_final_output) return invalid_argument("Deferred renderer graph has no final output pass");
        return apply_native_ordering(builder, *sequence);
    }

    if (const auto result = build_shadow_render_graph(builder, context, shadow_view.value, shadow.value); !result)
        return result;
    if (const Result result =
            build_gbuffer_render_graph(builder, context, albedo.value, normal.value, material.value, depth.value);
        !result) {
        return result;
    }
    if (const Result result = add_lighting_pass(); !result)
        return result;
    return build_post_process_render_graph(builder, context, lighting_color.value, depth.value, color.value,
                                           context.scene_snapshot->view.color_target_final_usage);
}

[[nodiscard]] Result build_forward(RenderGraphBuilder& builder, const RenderFeatureContext& context) {
    if (const Result result = validate_context(builder, context); !result) {
        return result;
    }
    if (const auto* sequence = context.native_sequence; sequence != nullptr) {
        if (const auto result = validate_native_sequence(*sequence); !result) return result;
    }
    const auto color = find_target(builder, context.scene_snapshot->view.color_target, "scene color");
    if (!color) {
        return color.result;
    }
    const auto depth = find_target(builder, context.scene_snapshot->view.depth_target, "scene depth");
    if (!depth) {
        return depth.result;
    }
    const rhi::Extent3D extent{context.scene_snapshot->view.output_extent.width,
                               context.scene_snapshot->view.output_extent.height, 1};
    const auto lighting_color = create_lighting_target(builder, extent);
    if (!lighting_color)
        return lighting_color.result;
    const auto shadow_view = make_directional_shadow_view(*context.scene_snapshot);
    if (!shadow_view)
        return shadow_view.result;
    const rhi::Extent3D shadow_extent{shadow_view.value.view.output_extent.width,
                                      shadow_view.value.view.output_extent.height, 1};
    const auto shadow = builder.create_texture(
        kShadowMap,
        make_texture_desc(shadow_extent, Format::kD32Float,
                          TextureCapability::kSampled | TextureCapability::kDepthStencil,
                          "Renderer.ShadowMap"));
    if (!shadow)
        return shadow.result;
    const bool has_geometry = !context.scene_snapshot->instances.empty();
    RenderLightCullingResource light_culling{};
    if (has_geometry) {
        if (!context.resource_resolver)
            return invalid_state("forward rendering requires a resource resolver");
        const auto resolved =
            context.resource_resolver->resolve_light_culling(*context.scene_snapshot, shadow_view.value);
        if (!resolved)
            return resolved.result;
        light_culling = resolved.value;
        if (light_culling.cluster_bytes == 0)
            return invalid_state("forward light cluster buffer is empty");
    }
    BufferDesc cluster_desc{};
    cluster_desc.size = has_geometry ? light_culling.cluster_bytes : sizeof(uint32_t);
    cluster_desc.capabilities = BufferCapability::kStorage;
    cluster_desc.memory = MemoryClass::kDeviceLocal;
    cluster_desc.initial_queue = QueueClass::kCompute;
    cluster_desc.name = rhi::DebugName{"Renderer.LightClusters"};
    const auto clusters = builder.create_buffer(kLightClusters, cluster_desc);
    if (!clusters) {
        return clusters.result;
    }

    if (const auto* sequence = context.native_sequence; sequence != nullptr) {
        bool has_final_output = false;
        for (const auto& pass : sequence->passes) {
            if (!pass.enabled)
                continue;
            if (pass.type == kShadowPassTypeId) {
                if (const auto result = build_shadow_render_graph(builder, context, shadow_view.value, shadow.value,
                                                                  pass.instance);
                    !result)
                    return result;
            } else if (pass.type == kZPrepassPassTypeId) {
                if (const auto result = build_depth_render_graph(builder, context, context.scene_snapshot->view,
                                                                 depth.value, "Renderer.ZPrepassPass", {},
                                                                 pass.instance);
                    !result)
                    return result;
            } else if (pass.type == kLightCullingPassTypeId) {
                if (has_geometry) {
                    if (const auto result = build_light_culling_render_graph(builder, context, depth.value,
                                                                               clusters.value, light_culling,
                                                                               pass.instance);
                        !result)
                        return result;
                } else if (const Result result = add_empty_culling_pass(
                               builder, context,
                               [clusters = clusters.value, depth = depth.value](pipeline::RenderPassBuilder& pass) {
                                   pass.set_side_effects();
                                   if (const auto result = pass.read(depth, ResourceUsage::kSampledRead,
                                                                     PipelineStage::kCompute, ShaderStage::kCompute);
                                       !result)
                                       return result;
                                    return pass.write(clusters, ResourceUsage::kStorageWrite, PipelineStage::kCompute,
                                                      ShaderStage::kCompute);
                                },
                                pass.instance);
                           !result) {
                    return result;
                }
            } else if (pass.type == kForwardOpaquePassTypeId) {
                if (const Result result = build_opaque_render_graph(
                        builder, context, true, shadow_view.value.enabled ? shadow.value : RenderGraphTexture{}, clusters.value, nullptr,
                         has_geometry ? &light_culling : nullptr, lighting_color.value, pass.instance);
                    !result)
                    return result;
            } else if (pass.type == kSkyboxPostProcessPassTypeId) {
                if (const auto result = build_post_process_render_graph(
                        builder, context, lighting_color.value, depth.value, color.value,
                        context.scene_snapshot->view.color_target_final_usage,
                        pass.instance);
                    !result)
                    return result;
                has_final_output = true;
            } else {
                return invalid_argument("Forward renderer pass type is not supported by its fixed native adapter");
            }
        }
        if (!has_final_output) return invalid_argument("Forward renderer graph has no final output pass");
        return apply_native_ordering(builder, *sequence);
    }

    if (const auto result = build_shadow_render_graph(builder, context, shadow_view.value, shadow.value); !result)
        return result;
    if (const Result result = build_depth_render_graph(builder, context, context.scene_snapshot->view, depth.value,
                                                       "Renderer.ZPrepassPass");
        !result) {
        return result;
    }
    if (has_geometry) {
        if (const auto result =
                build_light_culling_render_graph(builder, context, depth.value, clusters.value, light_culling);
            !result)
            return result;
    } else if (const Result result = add_empty_culling_pass(
                   builder, context,
                   [clusters = clusters.value, depth = depth.value](pipeline::RenderPassBuilder& pass) {
                       pass.set_side_effects();
                       if (const auto result = pass.read(depth, ResourceUsage::kSampledRead, PipelineStage::kCompute,
                                                         ShaderStage::kCompute);
                           !result)
                           return result;
                       return pass.write(clusters, ResourceUsage::kStorageWrite, PipelineStage::kCompute,
                                         ShaderStage::kCompute);
                   });
               !result) {
        return result;
    }
    // Reuse the renderer's validated mesh/material/object resolver and
    // indexed-draw command path. The forward pass keeps the Z-prepass result
    // by loading the depth attachment instead of clearing it again.
    if (const Result result = build_opaque_render_graph(builder, context, true,
                                                        shadow_view.value.enabled ? shadow.value : RenderGraphTexture{}, clusters.value, nullptr,
                                                        has_geometry ? &light_culling : nullptr, lighting_color.value);
        !result) {
        return result;
    }
    return build_post_process_render_graph(builder, context, lighting_color.value, depth.value, color.value,
                                           context.scene_snapshot->view.color_target_final_usage);
}

}  // namespace

Result build_deferred_lighting_render_graph(RenderGraphBuilder& builder, const RenderFeatureContext& context,
    RenderGraphTexture albedo, RenderGraphTexture normal, RenderGraphTexture depth, RenderGraphTexture color,
    RenderGraphTexture shadow, const RenderShadowResource* shadow_resources,
    pipeline::RenderPassInstanceId authored_instance, const DeferredLightingDrawOptions* draw_options) {
    if (!builder.is_runtime() || !context.is_valid())
        return invalid_state("DeferredLighting requires a frame builder and valid scene");
    if (shadow.is_valid() != (shadow_resources != nullptr) ||
        (shadow_resources && !shadow_resources->graph_buffer.is_valid()))
        return invalid_argument("DeferredLighting shadow texture/constants pair is invalid");
    const bool draw_fullscreen = draw_options || !context.scene_snapshot->instances.empty();
    const auto shadow_resource = shadow_resources ? *shadow_resources : RenderShadowResource{};
    const rhi::Extent3D extent{context.scene_snapshot->view.output_extent.width,
                               context.scene_snapshot->view.output_extent.height, 1};
    const bool preserve_color = draw_options && draw_options->preserve_color;
    const auto clear_color = draw_options ? draw_options->clear_color : rhi::ClearColorValue{};
    RenderDeferredLightingResource deferred{};
    RenderLightingResource lighting{};
    if (draw_fullscreen) {
        if (!context.resource_resolver) return invalid_state("DeferredLighting requires a resource resolver");
        const auto resolved = context.resource_resolver->resolve_deferred_lighting(context.scene_snapshot->view);
        if (!resolved) return resolved.result;
        if (!resolved.value.is_valid()) return invalid_state("DeferredLighting resources are invalid");
        if (!resolved.value.profile_contract.is_valid(RendererProfile::kDeferredLighting))
            return invalid_state("DeferredLighting shader/PSO contract is invalid");
        deferred = resolved.value;
        const auto& contract = shadow_resources ? deferred.shadow_pipeline_contract : deferred.attachment_contract;
        if (draw_options && contract != draw_options->attachment_contract)
            return invalid_argument("DeferredLighting PSO attachment contract is incompatible");
        if (shadow_resources) {
            if (!deferred.shadow_pipeline.is_valid()) return invalid_state("DeferredLighting shadow PSO is unavailable");
            if (!deferred.shadow_profile_contract.is_valid(RendererProfile::kDeferredLighting))
                return invalid_state("DeferredLighting shadow shader/PSO contract is invalid");
            deferred.pipeline = deferred.shadow_pipeline;
        }
        const auto& profile = shadow_resources ? deferred.shadow_profile_contract : deferred.profile_contract;
        if (profile.attachment != contract)
            return invalid_state("DeferredLighting selected profile attachment contract is inconsistent");
        const auto resolved_lighting =
            context.resource_resolver->resolve_lighting(*context.scene_snapshot, context.scene_snapshot->view);
        if (!resolved_lighting) return resolved_lighting.result;
        if (!resolved_lighting.value.is_valid()) return invalid_state("DeferredLighting constants are invalid");
        lighting = resolved_lighting.value;
    }
    const auto setup = [albedo, normal, shadow, depth, color, lighting, draw_fullscreen, shadow_resource,
                        preserve_color, clear_color](pipeline::RenderPassBuilder& pass) {
        if (draw_fullscreen) {
            for (const RenderGraphTexture input : {albedo, normal, depth}) {
                if (const Result result = pass.read(input, ResourceUsage::kSampledRead,
                                                    PipelineStage::kFragmentShader, ShaderStage::kFragment);
                    !result)
                    return result;
            }
            if (shadow_resource.graph_buffer.is_valid()) {
                if (const auto result = pass.read(shadow, ResourceUsage::kSampledRead,
                                                  PipelineStage::kFragmentShader, ShaderStage::kFragment); !result)
                    return result;
                if (const auto result = pass.read(shadow_resource.graph_buffer, ResourceUsage::kUniformRead,
                                                  PipelineStage::kFragmentShader, ShaderStage::kFragment,
                                                  {0, sizeof(RenderShadowConstants)});
                    !result)
                    return result;
            }
            if (const auto result = pass.read(lighting.lighting_resource, ResourceUsage::kUniformRead,
                                              PipelineStage::kFragmentShader, ShaderStage::kFragment,
                                              {lighting.lighting_offset, lighting.lighting_size});
                !result)
                return result;
        }
        return pass.use_color_attachment(color, preserve_color ? rhi::LoadOp::kLoad : rhi::LoadOp::kClear,
                                         rhi::StoreOp::kStore, clear_color,
                                         ResourceUsage::kColorAttachmentWrite);
    };
    const auto execute = [albedo, normal, depth, deferred, lighting, extent, draw_fullscreen, shadow,
                          shadow_resource](pipeline::RenderPassExecutionContext& execution) -> Result {
        rhi::BindingSetHandle inputs{};
        if (draw_fullscreen) {
            const auto albedo_view = execution.texture_view(albedo);
            if (!albedo_view)
                return albedo_view.result;
            const auto normal_view = execution.texture_view(normal);
            if (!normal_view)
                return normal_view.result;
            const auto depth_view = execution.texture_view(
                depth, rhi::TextureViewKind::kSampled, {rhi::Aspect::kDepth, 0, 1, 0, 1});
            if (!depth_view)
                return depth_view.result;
            const auto light_buffer = execution.buffer(lighting.lighting_resource);
            if (!light_buffer)
                return light_buffer.result;
            RenderDeferredLightingInputs bindings{
                albedo_view.value,
                normal_view.value,
                depth_view.value,
                {light_buffer.value, lighting.lighting_offset, lighting.lighting_size}};
            if (shadow_resource.graph_buffer.is_valid()) {
                const auto shadow_view = execution.texture_view(
                    shadow, rhi::TextureViewKind::kSampled, {rhi::Aspect::kDepth, 0, 1, 0, 1});
                const auto buffer = execution.buffer(shadow_resource.graph_buffer);
                if (!shadow_view || !buffer)
                    return !shadow_view ? shadow_view.result : buffer.result;
                bindings.shadow_map = shadow_view.value;
                bindings.shadow_constants = {buffer.value, 0, sizeof(RenderShadowConstants)};
            }
            const auto prepared = deferred.prepare_inputs(bindings);
            if (!prepared)
                return prepared.result;
            if (!prepared.value.is_valid())
                return invalid_state("deferred lighting input binding is invalid");
            inputs = prepared.value;
        }
        if (const auto result = execution.begin_rendering(); !result)
            return result;
        const auto draw = [&]() -> Result {
            if (!draw_fullscreen)
                return Result::success();
            if (const auto result = execution.set_viewport(
                    {0, 0, float(extent.width), float(extent.height), 0, 1});
                !result)
                return result;
            if (const auto result = execution.set_scissor({0, 0, extent.width, extent.height}); !result)
                return result;
            if (const auto result = execution.bind_pipeline(deferred.pipeline); !result)
                return result;
            if (const auto result = execution.bind_binding_set(0, inputs); !result)
                return result;
            return execution.draw({3, 1, 0, 0});
        }();
        const auto end = execution.end_rendering();
        return draw ? end : draw;
    };
    const auto lighting_pass = draw_options && !context.registered_native
        ? builder.add_graphics_pass("Renderer.DeferredLighting", setup, execute)
        : detail::add_builtin_render_pass(builder, context, kDeferredLightingPassTypeId, setup, execute, authored_instance);
    return lighting_pass ? Result::success() : lighting_pass.result;
}

Result build_deferred_render_graph(pipeline::RenderGraphBuilder& builder, const RenderFeatureContext& context) {
    return build_deferred(builder, context);
}

Result build_forward_render_graph(pipeline::RenderGraphBuilder& builder, const RenderFeatureContext& context) {
    return build_forward(builder, context);
}

}  // namespace aeg::renderer
