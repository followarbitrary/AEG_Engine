/**
 * @file opaque_render_feature.cpp
 * @brief Implements the built-in RenderGraph opaque draw feature.
 */
#include "../public/opaque_render_feature.hpp"

#include <cstdint>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#include "../public/renderer.hpp"
#include "builtin_render_passes.hpp"

namespace aeg::renderer {
namespace {
using foundation::invalid_argument;
using foundation::invalid_state;

inline constexpr pipeline::StageId kOpaqueRenderFeatureStageId{"Renderer.OpaqueRenderFeature.Build"};
inline constexpr uint64_t kVertexResourceNamespace = 0x5a5e8f641b8d37c1ull;
inline constexpr uint64_t kIndexResourceNamespace = 0x7f6c2b94d8a513e9ull;
inline constexpr uint64_t kObjectResourceNamespace = 0x1ec7d5a2b9364f0bull;
inline constexpr uint32_t kPositionAttributeSize = sizeof(float) * 4u;
inline constexpr uint32_t kUvAttributeSize = sizeof(float) * 2u;
inline constexpr uint32_t kNormalAttributeSize = sizeof(float) * 3u;

[[nodiscard]] pipeline::RenderResourceId make_resource_id(uint64_t resource_namespace, uint64_t identity) noexcept {
    uint64_t value = resource_namespace;
    value ^= identity + 0x9e3779b97f4a7c15ull + (value << 6u) + (value >> 2u);
    value ^= value >> 30u;
    value *= 0xbf58476d1ce4e5b9ull;
    value ^= value >> 27u;
    value *= 0x94d049bb133111ebull;
    value ^= value >> 31u;
    return pipeline::RenderResourceId(value == 0 ? 1 : value);
}

[[nodiscard]] pipeline::RenderResourceId make_object_resource_id(RenderObjectId object, RenderViewId view) noexcept {
    return make_resource_id(kObjectResourceNamespace, object.value() ^ (view.value() * 0x9e3779b97f4a7c15ull));
}

[[nodiscard]] foundation::ResultValue<pipeline::RenderGraphBuffer> find_or_import_buffer(
    pipeline::RenderGraphBuilder& builder, pipeline::RenderResourceId id, rhi::BufferHandle buffer,
    const rhi::BufferDesc& desc) {
    const foundation::ResultValue<pipeline::RenderGraphBuffer> existing = builder.find_buffer(id);
    if (existing) {
        return existing;
    }
    if (existing.result.code != foundation::ResultCode::kNotFound) {
        return {existing.result, {}};
    }
    return builder.import_buffer(id, buffer, desc);
}

[[nodiscard]] foundation::Result end_rendering_after_failure(pipeline::RenderPassExecutionContext& context,
                                                             foundation::Result failure) {
    const foundation::Result end_result = context.end_rendering();
    return end_result ? failure : end_result;
}

struct OpaqueDrawItem final {
    pipeline::RenderGraphBuffer vertex_buffer{};
    pipeline::RenderGraphBuffer index_buffer{};
    pipeline::RenderGraphBuffer transform_buffer{};
    rhi::PipelineHandle pipeline{};
    rhi::BindingSetHandle material_binding_set{};
    uint32_t material_binding_group{1};
    pipeline::RenderGraphTexture material_texture{};
    pipeline::RenderGraphBuffer material_parameter_resource{};
    uint64_t material_parameter_offset{0};
    uint64_t material_parameter_size{0};
    rhi::BindingSetHandle binding_set{};
    uint32_t binding_group{0};
    uint64_t transform_offset{0};
    uint64_t transform_size{0};
    uint32_t index_count{0};
    rhi::IndexFormat index_format{rhi::IndexFormat::kUint16};
};

struct OpaquePassData final {
    pipeline::RenderGraphTexture color_target{};
    pipeline::RenderGraphTexture depth_target{};
    rhi::ResourceUsage color_target_final_usage{rhi::ResourceUsage::kColorAttachmentWrite};
    pipeline::RenderGraphBuffer lighting_resource{};
    rhi::BindingSetHandle lighting_binding_set{};
    uint32_t lighting_binding_group{2};
    uint64_t lighting_offset{0};
    uint64_t lighting_size{0};
    bool has_depth_target{false};
    bool preserve_color{false};
    pipeline::RenderGraphTexture shadow_map{};
    RenderShadowResource shadow_resource{};
    RenderLightCullingResource light_culling{};
    pipeline::RenderGraphBuffer light_clusters{};
    RenderExtent output_extent{};
    std::vector<OpaqueDrawItem> draws{};
};

class OpaqueRenderFeature final : public pipeline::RenderFeature {
public:
    [[nodiscard]] bool uses_render_state_output() const noexcept override { return true; }
    [[nodiscard]] pipeline::RenderStateBlendMode render_state_blend_mode() const noexcept override {
        return pipeline::RenderStateBlendMode::kOver;
    }

    [[nodiscard]] const pipeline::FeatureDescriptor& descriptor() const noexcept override {
        static const pipeline::FeatureDescriptor kDescriptor{
            .id = kOpaqueRenderFeatureId,
            .name = "Renderer.OpaqueRenderFeature",
            .domain = pipeline::FeatureDomain::kRender,
            .domain_mask = pipeline::to_mask(pipeline::FeatureDomain::kRender),
            .supported_scopes = pipeline::to_mask(pipeline::PipelineScope::kFrame),
            .version = {1, 2, 0},
        };
        return kDescriptor;
    }

    [[nodiscard]] foundation::Result build_render_graph(pipeline::RenderGraphBuilder& builder) const override {
        return build_render_graph_with_options(builder, false);
    }

    [[nodiscard]] foundation::Result build_render_graph_with_options(
        pipeline::RenderGraphBuilder& builder, bool preserve_depth, pipeline::RenderGraphTexture shadow_map = {},
        pipeline::RenderGraphBuffer light_clusters = {}, const RenderShadowResource* shadow_resource = nullptr,
        const RenderLightCullingResource* light_culling = nullptr,
        pipeline::RenderGraphTexture color_override = {},
        pipeline::RenderPassInstanceId authored_instance = {}) const {
        const RenderFeatureContext* context = render_feature_context(builder);
        if (context == nullptr || !context->is_valid()) {
            return invalid_state("opaque render feature requires a valid renderer context");
        }
        const auto add_opaque_pass = [&](std::string_view name, pipeline::RenderPassSetupFunction setup,
                                         pipeline::RenderPassExecuteFunction execute) {
            if (color_override.is_valid())
                return detail::add_builtin_render_pass(builder, *context, kForwardOpaquePassTypeId, std::move(setup),
                                                       std::move(execute), authored_instance);
            return builder.add_graphics_pass(name, std::move(setup), std::move(execute));
        };
        if (!context->scene_snapshot->instances.empty() && context->resource_resolver == nullptr) {
            return invalid_state("opaque render feature requires a render resource resolver");
        }
        if (!context->scene_snapshot->view.color_target.is_valid()) {
            return invalid_argument("opaque render feature requires a RenderView color target");
        }

        const pipeline::RenderGraphTexture state_output = builder.state_output();
        const pipeline::RenderGraphTexture state_input = builder.state_input();
        const std::span<const pipeline::RenderGraphTexture> state_inputs = builder.state_inputs();
        const bool preserve_state = !color_override.is_valid() && state_output.is_valid() && !state_inputs.empty();
        // RenderFeatureRuntime pre-composes multi-input state into state_output.
        // A serial input still needs the inexpensive copy seed below.
        const bool needs_state_seed = preserve_state && state_inputs.size() == 1 && state_input.is_valid();
        const foundation::ResultValue<pipeline::RenderGraphTexture> color_target =
            color_override.is_valid()
                ? foundation::ResultValue<pipeline::RenderGraphTexture>{foundation::Result::success(), color_override}
            : state_output.is_valid()
                ? foundation::ResultValue<pipeline::RenderGraphTexture>{foundation::Result::success(), state_output}
                : builder.find_texture(context->scene_snapshot->view.color_target);
        if (!color_target)
            return color_target.result;
        const rhi::ResourceUsage color_final_usage = color_override.is_valid() ? rhi::ResourceUsage::kSampledRead
                                                     : state_output.is_valid()
                                                         ? rhi::ResourceUsage::kCopySource
                                                         : context->scene_snapshot->view.color_target_final_usage;

        if (needs_state_seed) {
            const rhi::Extent3D extent{context->scene_snapshot->view.output_extent.width,
                                       context->scene_snapshot->view.output_extent.height, 1};
            const auto seed = builder.add_copy_pass(
                "Opaque.StateSeed",
                [state_input, state_output](pipeline::RenderPassBuilder& pass) {
                    if (const foundation::Result result = pass.read(state_input, rhi::ResourceUsage::kCopySource,
                                                                    rhi::PipelineStage::kCopy, rhi::ShaderStage::kAll);
                        !result) {
                        return result;
                    }
                    return pass.write(state_output, rhi::ResourceUsage::kCopyDestination, rhi::PipelineStage::kCopy,
                                      rhi::ShaderStage::kAll, {}, true);
                },
                [state_input, state_output, extent](pipeline::RenderPassExecutionContext& execution) {
                    return execution.copy_texture(
                        state_input, rhi::TextureSubresourceLayers{rhi::Aspect::kColor, 0, 0, 1}, rhi::Offset3D{},
                        state_output, rhi::TextureSubresourceLayers{rhi::Aspect::kColor, 0, 0, 1}, rhi::Offset3D{},
                        extent);
                });
            if (!seed) {
                return seed.result;
            }
        }

        pipeline::RenderGraphTexture depth_target{};
        const pipeline::RenderResourceId depth_target_id = context->scene_snapshot->view.depth_target;
        if (depth_target_id.is_valid()) {
            const foundation::ResultValue<pipeline::RenderGraphTexture> depth_target_result =
                builder.find_texture(depth_target_id);
            if (!depth_target_result) {
                return depth_target_result.result;
            }
            depth_target = depth_target_result.value;
        }

        // Empty frames still own and clear their targets; otherwise imported textures retain old geometry.
        if (context->scene_snapshot->instances.empty()) {
            const auto final_usage = color_final_usage;
            const auto clear = add_opaque_pass(
                "Opaque.ClearEmpty",
                [color = color_target.value, depth_target, final_usage, preserve_state, preserve_depth, shadow_map,
                 light_clusters](pipeline::RenderPassBuilder& pass) {
                    if (shadow_map.is_valid()) {
                        if (const auto result =
                                pass.read(shadow_map, rhi::ResourceUsage::kSampledRead,
                                          rhi::PipelineStage::kFragmentShader, rhi::ShaderStage::kFragment);
                            !result) {
                            return result;
                        }
                    }
                    if (light_clusters.is_valid()) {
                        if (const auto result =
                                pass.read(light_clusters, rhi::ResourceUsage::kStorageRead,
                                          rhi::PipelineStage::kFragmentShader, rhi::ShaderStage::kFragment);
                            !result) {
                            return result;
                        }
                    }
                    if (const auto result = pass.use_color_attachment(
                            color, preserve_state ? rhi::LoadOp::kLoad : rhi::LoadOp::kClear, rhi::StoreOp::kStore,
                            rhi::ClearColorValue{0, 0, 0, 1}, final_usage);
                        !result)
                        return result;
                    if (depth_target.is_valid())
                        return pass.use_depth_stencil_attachment(
                            depth_target, preserve_depth ? rhi::LoadOp::kLoad : rhi::LoadOp::kClear,
                            rhi::StoreOp::kStore, 1.0f, 0);
                    return foundation::Result::success();
                },
                [](pipeline::RenderPassExecutionContext& pass) {
                    if (const auto result = pass.begin_rendering(); !result)
                        return result;
                    return pass.end_rendering();
                });
            return clear ? foundation::Result::success() : clear.result;
        }

        // The graph owns callbacks past build(), so draw declarations need shared ownership.
        auto pass_data = std::make_shared<OpaquePassData>();
        pass_data->color_target = color_target.value;
        pass_data->depth_target = depth_target;
        pass_data->color_target_final_usage = color_final_usage;
        pass_data->has_depth_target = depth_target.is_valid();
        pass_data->preserve_color = preserve_state;
        pass_data->shadow_map = shadow_map;
        if (shadow_resource) {
            if (!shadow_resource->graph_buffer.is_valid() || !shadow_resource->prepare_inputs || !shadow_map.is_valid())
                return invalid_state("forward shadow inputs are incomplete");
            pass_data->shadow_resource = *shadow_resource;
        }
        pass_data->light_clusters = light_clusters;
        if (light_culling) {
            if (!light_culling->constants_graph.is_valid() || !light_culling->lights_graph.is_valid() ||
                !light_culling->prepare_forward || !light_clusters.is_valid() || !shadow_map.is_valid())
                return invalid_state("forward light culling inputs are incomplete");
            pass_data->light_culling = *light_culling;
        }
        pass_data->output_extent = context->scene_snapshot->view.output_extent;
        pass_data->draws.reserve(context->scene_snapshot->instances.size());

        for (const RenderInstance& instance : context->scene_snapshot->instances) {
            const foundation::ResultValue<RenderMeshResource> mesh =
                context->resource_resolver->resolve_mesh(instance.mesh);
            if (!mesh) {
                return mesh.result;
            }
            if (!mesh.value.is_valid()) {
                return invalid_state("opaque render feature received an invalid mesh resource");
            }
            const RenderMeshVertexLayout& vertex_layout = mesh.value.vertex_layout;
            if (vertex_layout.position_format != rhi::Format::kRgba32Float ||
                vertex_layout.position_offset > mesh.value.vertex_stride ||
                kPositionAttributeSize > mesh.value.vertex_stride - vertex_layout.position_offset) {
                return invalid_state("opaque render feature received an incompatible position vertex layout");
            }
            if (vertex_layout.uv_format != rhi::Format::kRg32Float ||
                vertex_layout.uv_offset > mesh.value.vertex_stride ||
                kUvAttributeSize > mesh.value.vertex_stride - vertex_layout.uv_offset) {
                return invalid_state("opaque render feature requires a float2 UV vertex attribute");
            }
            if (vertex_layout.normal_format != rhi::Format::kRgb32Float ||
                vertex_layout.normal_offset > mesh.value.vertex_stride ||
                kNormalAttributeSize > mesh.value.vertex_stride - vertex_layout.normal_offset) {
                return invalid_state("opaque render feature requires a float3 normal vertex attribute");
            }

            const foundation::ResultValue<RenderMaterialResource> material =
                context->resource_resolver->resolve_material(instance.material);
            if (!material) {
                return material.result;
            }
            if (!material.value.is_valid()) {
                return invalid_state("opaque render feature received an invalid material resource");
            }
            if (!material.value.binding_set.is_valid()) {
                return invalid_state("opaque render feature received an invalid material binding set");
            }
            if (!material.value.material_texture.is_valid()) {
                return invalid_state("opaque render feature material texture was not declared in RenderGraph");
            }
            if (!material.value.material_parameter_buffer.is_valid() ||
                !material.value.material_parameter_resource.is_valid() ||
                material.value.material_parameter_size < sizeof(float) * 4u ||
                material.value.material_parameter_buffer_desc.size == 0 ||
                !rhi::has_flag(material.value.material_parameter_buffer_desc.capabilities,
                               rhi::BufferCapability::kUniform) ||
                material.value.material_parameter_offset > material.value.material_parameter_buffer_desc.size ||
                material.value.material_parameter_size >
                    material.value.material_parameter_buffer_desc.size - material.value.material_parameter_offset) {
                return invalid_state("opaque render feature received an invalid material parameter buffer");
            }

            const foundation::ResultValue<RenderObjectResource> object =
                context->resource_resolver->resolve_object(instance, context->scene_snapshot->view);
            if (!object) {
                return object.result;
            }
            if (!object.value.is_valid()) {
                return invalid_state("opaque render feature received an invalid object resource");
            }

            const foundation::ResultValue<pipeline::RenderGraphBuffer> vertex_buffer =
                mesh.value.vertex_resource.is_valid()
                    ? foundation::ResultValue<pipeline::RenderGraphBuffer>{foundation::Result::success(),
                                                                           mesh.value.vertex_resource}
                    : find_or_import_buffer(builder, make_resource_id(kVertexResourceNamespace, instance.mesh.value()),
                                            mesh.value.vertex_buffer, mesh.value.vertex_buffer_desc);
            if (!vertex_buffer) {
                return vertex_buffer.result;
            }
            const foundation::ResultValue<pipeline::RenderGraphBuffer> index_buffer =
                mesh.value.index_resource.is_valid()
                    ? foundation::ResultValue<pipeline::RenderGraphBuffer>{foundation::Result::success(),
                                                                           mesh.value.index_resource}
                    : find_or_import_buffer(builder, make_resource_id(kIndexResourceNamespace, instance.mesh.value()),
                                            mesh.value.index_buffer, mesh.value.index_buffer_desc);
            if (!index_buffer) {
                return index_buffer.result;
            }
            const foundation::ResultValue<pipeline::RenderGraphBuffer> transform_buffer = find_or_import_buffer(
                builder, make_object_resource_id(instance.object, context->scene_snapshot->view.id),
                object.value.transform_buffer, object.value.transform_buffer_desc);
            if (!transform_buffer) {
                return transform_buffer.result;
            }

            if (shadow_resource && !material.value.shadow_pipeline.is_valid())
                return invalid_state("forward shadow pipeline is unavailable");
            if (light_culling && !material.value.clustered_pipeline.is_valid())
                return invalid_state("forward clustered pipeline is unavailable");
            pass_data->draws.push_back({
                .vertex_buffer = vertex_buffer.value,
                .index_buffer = index_buffer.value,
                .transform_buffer = transform_buffer.value,
                .pipeline = light_culling     ? material.value.clustered_pipeline
                            : shadow_resource ? material.value.shadow_pipeline
                                              : material.value.pipeline,
                .material_binding_set = material.value.binding_set,
                .material_binding_group = material.value.binding_group,
                .material_texture = material.value.material_texture,
                .material_parameter_resource = material.value.material_parameter_resource,
                .material_parameter_offset = material.value.material_parameter_offset,
                .material_parameter_size = material.value.material_parameter_size,
                .binding_set = object.value.binding_set,
                .binding_group = object.value.binding_group,
                .transform_offset = object.value.transform_offset,
                .transform_size = object.value.transform_size,
                .index_count = mesh.value.index_count,
                .index_format = mesh.value.index_format,
            });
        }

        const foundation::ResultValue<RenderLightingResource> lighting =
            context->resource_resolver->resolve_lighting(*context->scene_snapshot, context->scene_snapshot->view);
        if (!lighting) {
            return lighting.result;
        }
        if (!lighting.value.is_valid()) {
            return invalid_state("opaque render feature received an invalid frame lighting resource");
        }
        pass_data->lighting_resource = lighting.value.lighting_resource;
        pass_data->lighting_binding_set = lighting.value.binding_set;
        pass_data->lighting_binding_group = lighting.value.binding_group;
        pass_data->lighting_offset = lighting.value.lighting_offset;
        pass_data->lighting_size = lighting.value.lighting_size;

        const std::shared_ptr<const OpaquePassData> captured_data = std::move(pass_data);
        const foundation::ResultValue<pipeline::RenderGraphPass> pass = add_opaque_pass(
            "DrawOpaque",
            [captured_data, preserve_depth](pipeline::RenderPassBuilder& pass_builder) {
                for (const OpaqueDrawItem& draw : captured_data->draws) {
                    if (const foundation::Result result =
                            pass_builder.read(draw.vertex_buffer, rhi::ResourceUsage::kVertexInput,
                                              rhi::PipelineStage::kVertexInput, rhi::ShaderStage::kVertex);
                        !result) {
                        return result;
                    }
                    if (const foundation::Result result =
                            pass_builder.read(draw.index_buffer, rhi::ResourceUsage::kIndexInput,
                                              rhi::PipelineStage::kVertexInput, rhi::ShaderStage::kVertex);
                        !result) {
                        return result;
                    }
                    if (const foundation::Result result = pass_builder.read(
                            draw.transform_buffer, rhi::ResourceUsage::kUniformRead, rhi::PipelineStage::kVertexShader,
                            rhi::ShaderStage::kVertex,
                            pipeline::RenderGraphBufferRange{draw.transform_offset, draw.transform_size});
                        !result) {
                        return result;
                    }
                    if (draw.material_texture.is_valid()) {
                        if (const foundation::Result result =
                                pass_builder.read(draw.material_texture, rhi::ResourceUsage::kSampledRead,
                                                  rhi::PipelineStage::kFragmentShader, rhi::ShaderStage::kFragment);
                            !result) {
                            return result;
                        }
                    }
                    if (const foundation::Result result =
                            pass_builder.read(draw.material_parameter_resource, rhi::ResourceUsage::kUniformRead,
                                              rhi::PipelineStage::kFragmentShader, rhi::ShaderStage::kFragment,
                                              pipeline::RenderGraphBufferRange{draw.material_parameter_offset,
                                                                               draw.material_parameter_size});
                        !result) {
                        return result;
                    }
                }
                if (captured_data->light_clusters.is_valid()) {
                    if (const foundation::Result result =
                            pass_builder.read(captured_data->light_clusters, rhi::ResourceUsage::kStorageRead,
                                              rhi::PipelineStage::kFragmentShader, rhi::ShaderStage::kFragment);
                        !result) {
                        return result;
                    }
                }
                if (captured_data->light_culling.constants_graph.is_valid()) {
                    if (const auto result = pass_builder.read(
                            captured_data->light_culling.constants_graph, rhi::ResourceUsage::kUniformRead,
                            rhi::PipelineStage::kFragmentShader, rhi::ShaderStage::kFragment);
                        !result)
                        return result;
                    if (const auto result = pass_builder.read(
                            captured_data->light_culling.lights_graph, rhi::ResourceUsage::kStorageRead,
                            rhi::PipelineStage::kFragmentShader, rhi::ShaderStage::kFragment);
                        !result)
                        return result;
                }
                if (captured_data->shadow_map.is_valid()) {
                    if (const foundation::Result result =
                            pass_builder.read(captured_data->shadow_map, rhi::ResourceUsage::kSampledRead,
                                              rhi::PipelineStage::kFragmentShader, rhi::ShaderStage::kFragment);
                        !result) {
                        return result;
                    }
                }
                if (captured_data->shadow_resource.graph_buffer.is_valid()) {
                    if (const auto result = pass_builder.read(
                            captured_data->shadow_resource.graph_buffer, rhi::ResourceUsage::kUniformRead,
                            rhi::PipelineStage::kFragmentShader, rhi::ShaderStage::kFragment);
                        !result)
                        return result;
                }
                if (const foundation::Result result = pass_builder.read(
                        captured_data->lighting_resource, rhi::ResourceUsage::kUniformRead,
                        rhi::PipelineStage::kFragmentShader, rhi::ShaderStage::kFragment,
                        pipeline::RenderGraphBufferRange{captured_data->lighting_offset, captured_data->lighting_size});
                    !result) {
                    return result;
                }
                if (const foundation::Result result = pass_builder.use_color_attachment(
                        captured_data->color_target,
                        captured_data->preserve_color ? rhi::LoadOp::kLoad : rhi::LoadOp::kClear, rhi::StoreOp::kStore,
                        rhi::ClearColorValue{0.0f, 0.0f, 0.0f, 1.0f}, captured_data->color_target_final_usage);
                    !result) {
                    return result;
                }
                if (captured_data->has_depth_target) {
                    return pass_builder.use_depth_stencil_attachment(
                        captured_data->depth_target, preserve_depth ? rhi::LoadOp::kLoad : rhi::LoadOp::kClear,
                        rhi::StoreOp::kStore, 1.0f, 0);
                }
                return foundation::Result::success();
            },
            [captured_data](pipeline::RenderPassExecutionContext& execution) {
                if (const foundation::Result result = execution.begin_rendering(); !result) {
                    return result;
                }
                if (const foundation::Result result = execution.set_viewport({
                        0.0f,
                        0.0f,
                        static_cast<float>(captured_data->output_extent.width),
                        static_cast<float>(captured_data->output_extent.height),
                        0.0f,
                        1.0f,
                    });
                    !result) {
                    return end_rendering_after_failure(execution, result);
                }
                if (const foundation::Result result = execution.set_scissor({
                        0,
                        0,
                        captured_data->output_extent.width,
                        captured_data->output_extent.height,
                    });
                    !result) {
                    return end_rendering_after_failure(execution, result);
                }

                rhi::BindingSetHandle shadow_set{};
                if (captured_data->light_culling.constants_graph.is_valid()) {
                    const auto view = execution.texture_view(captured_data->shadow_map, rhi::TextureViewKind::kSampled,
                                                             {rhi::Aspect::kDepth, 0, 1, 0, 1});
                    const auto clusters = execution.buffer(captured_data->light_clusters);
                    if (!view || !clusters)
                        return end_rendering_after_failure(execution, !view ? view.result : clusters.result);
                    const auto prepared = captured_data->light_culling.prepare_forward(view.value, clusters.value);
                    if (!prepared)
                        return end_rendering_after_failure(execution, prepared.result);
                    if (!prepared.value.is_valid())
                        return end_rendering_after_failure(execution,
                                                           invalid_state("forward light culling binding is invalid"));
                    shadow_set = prepared.value;
                } else if (captured_data->shadow_resource.graph_buffer.is_valid()) {
                    const auto view = execution.texture_view(captured_data->shadow_map, rhi::TextureViewKind::kSampled,
                                                             {rhi::Aspect::kDepth, 0, 1, 0, 1});
                    if (!view)
                        return end_rendering_after_failure(execution, view.result);
                    const auto prepared = captured_data->shadow_resource.prepare_inputs(view.value);
                    if (!prepared)
                        return end_rendering_after_failure(execution, prepared.result);
                    if (!prepared.value.is_valid())
                        return end_rendering_after_failure(execution,
                                                           invalid_state("forward shadow input binding is invalid"));
                    shadow_set = prepared.value;
                }
                for (const OpaqueDrawItem& draw : captured_data->draws) {
                    if (const foundation::Result result = execution.bind_pipeline(draw.pipeline); !result) {
                        return end_rendering_after_failure(execution, result);
                    }
                    if (shadow_set.is_valid()) {
                        if (const auto result = execution.bind_binding_set(3, shadow_set); !result)
                            return end_rendering_after_failure(execution, result);
                    }
                    if (const foundation::Result result = execution.bind_binding_set(
                            captured_data->lighting_binding_group, captured_data->lighting_binding_set);
                        !result) {
                        return end_rendering_after_failure(execution, result);
                    }
                    if (const foundation::Result result =
                            execution.bind_binding_set(draw.material_binding_group, draw.material_binding_set);
                        !result) {
                        return end_rendering_after_failure(execution, result);
                    }
                    if (const foundation::Result result =
                            execution.bind_binding_set(draw.binding_group, draw.binding_set);
                        !result) {
                        return end_rendering_after_failure(execution, result);
                    }
                    const foundation::ResultValue<rhi::BufferHandle> vertex_buffer =
                        execution.buffer(draw.vertex_buffer);
                    if (!vertex_buffer) {
                        return end_rendering_after_failure(execution, vertex_buffer.result);
                    }
                    const foundation::ResultValue<rhi::BufferHandle> index_buffer = execution.buffer(draw.index_buffer);
                    if (!index_buffer) {
                        return end_rendering_after_failure(execution, index_buffer.result);
                    }
                    if (const foundation::Result result = execution.bind_vertex_buffer(0, vertex_buffer.value);
                        !result) {
                        return end_rendering_after_failure(execution, result);
                    }
                    if (const foundation::Result result =
                            execution.bind_index_buffer(index_buffer.value, 0, draw.index_format);
                        !result) {
                        return end_rendering_after_failure(execution, result);
                    }
                    if (const foundation::Result result =
                            execution.draw_indexed(rhi::DrawIndexedDesc{draw.index_count, 1, 0, 0, 0});
                        !result) {
                        return end_rendering_after_failure(execution, result);
                    }
                }
                return execution.end_rendering();
            });
        return pass ? foundation::Result::success() : pass.result;
    }

protected:
    [[nodiscard]] foundation::Result setup(pipeline::RenderGraphBuilder& builder) const override {
        const pipeline::PipelineStageHandle stage = builder.add_stage({
            .id = kOpaqueRenderFeatureStageId,
            .phase = pipeline::EnginePhase::kRenderBuild,
            .execution_domain = pipeline::ExecutionDomain::kRenderThread,
            .time_policy = pipeline::TimePolicy::kFrame,
            .parallelizable = false,
        });
        return stage ? foundation::Result::success() : invalid_state("opaque render feature stage declaration failed");
    }
};

struct GBufferDrawItem final {
    pipeline::RenderGraphBuffer vertex_buffer{};
    pipeline::RenderGraphBuffer index_buffer{};
    pipeline::RenderGraphBuffer transform_buffer{};
    rhi::PipelineHandle pipeline{};
    rhi::BindingSetHandle material_binding_set{};
    uint32_t material_binding_group{1};
    rhi::BindingSetHandle object_binding_set{};
    uint32_t object_binding_group{0};
    uint64_t transform_offset{0};
    uint64_t transform_size{0};
    pipeline::RenderGraphTexture material_texture{};
    pipeline::RenderGraphBuffer material_parameter_resource{};
    uint64_t material_parameter_offset{0};
    uint64_t material_parameter_size{0};
    uint32_t index_count{0};
    rhi::IndexFormat index_format{rhi::IndexFormat::kUint16};
};

struct GBufferPassData final {
    pipeline::RenderGraphTexture albedo{};
    pipeline::RenderGraphTexture normal{};
    pipeline::RenderGraphTexture material{};
    pipeline::RenderGraphTexture depth{};
    RenderExtent extent{};
    std::vector<GBufferDrawItem> draws{};
};

[[nodiscard]] foundation::Result build_gbuffer_impl(
    pipeline::RenderGraphBuilder& builder, const RenderFeatureContext& context, pipeline::RenderGraphTexture albedo,
    pipeline::RenderGraphTexture normal, pipeline::RenderGraphTexture material, pipeline::RenderGraphTexture depth,
    pipeline::RenderPassInstanceId authored_instance) {
    if (!builder.is_runtime() || builder.scope() != pipeline::PipelineScope::kFrame) {
        return invalid_state("GBuffer render graph requires a frame runtime builder");
    }
    if (!context.is_valid() || render_feature_context(builder) != &context) {
        return invalid_state("GBuffer render graph context does not match the runtime builder");
    }
    if (!albedo.is_valid() || !normal.is_valid() || !material.is_valid() || !depth.is_valid()) {
        return invalid_argument("GBuffer render graph requires valid MRT and depth targets");
    }
    if (!context.scene_snapshot->instances.empty() && context.resource_resolver == nullptr) {
        return invalid_state("GBuffer render graph requires a render resource resolver");
    }

    auto pass_data = std::make_shared<GBufferPassData>();
    pass_data->albedo = albedo;
    pass_data->normal = normal;
    pass_data->material = material;
    pass_data->depth = depth;
    pass_data->extent = context.scene_snapshot->view.output_extent;
    pass_data->draws.reserve(context.scene_snapshot->instances.size());

    for (const RenderInstance& instance : context.scene_snapshot->instances) {
        const auto mesh = context.resource_resolver->resolve_mesh(instance.mesh);
        if (!mesh)
            return mesh.result;
        if (!mesh.value.is_valid())
            return invalid_state("GBuffer received an invalid mesh resource");
        const auto& layout = mesh.value.vertex_layout;
        if (layout.position_format != rhi::Format::kRgba32Float || layout.position_offset > mesh.value.vertex_stride ||
            kPositionAttributeSize > mesh.value.vertex_stride - layout.position_offset ||
            layout.uv_format != rhi::Format::kRg32Float || layout.uv_offset > mesh.value.vertex_stride ||
            kUvAttributeSize > mesh.value.vertex_stride - layout.uv_offset ||
            layout.normal_format != rhi::Format::kRgb32Float || layout.normal_offset > mesh.value.vertex_stride ||
            kNormalAttributeSize > mesh.value.vertex_stride - layout.normal_offset) {
            return invalid_state("GBuffer received an incompatible vertex layout");
        }

        const auto material_resource = context.resource_resolver->resolve_material(instance.material);
        if (!material_resource)
            return material_resource.result;
        if (!material_resource.value.is_valid() || !material_resource.value.gbuffer_pipeline.is_valid()) {
            return invalid_state("GBuffer material is missing a GBuffer graphics pipeline");
        }
        if (!material_resource.value.material_texture.is_valid() ||
            !material_resource.value.material_parameter_resource.is_valid() ||
            material_resource.value.material_parameter_size < sizeof(float) * 4u ||
            material_resource.value.material_parameter_offset >
                material_resource.value.material_parameter_buffer_desc.size ||
            material_resource.value.material_parameter_size >
                material_resource.value.material_parameter_buffer_desc.size -
                    material_resource.value.material_parameter_offset) {
            return invalid_state("GBuffer material parameters are invalid");
        }

        const auto object = context.resource_resolver->resolve_object(instance, context.scene_snapshot->view);
        if (!object)
            return object.result;
        if (!object.value.is_valid())
            return invalid_state("GBuffer received an invalid object resource");

        const auto vertex_buffer =
            mesh.value.vertex_resource.is_valid()
                ? foundation::ResultValue<pipeline::RenderGraphBuffer>{foundation::Result::success(),
                                                                       mesh.value.vertex_resource}
                : find_or_import_buffer(builder, make_resource_id(kVertexResourceNamespace, instance.mesh.value()),
                                        mesh.value.vertex_buffer, mesh.value.vertex_buffer_desc);
        if (!vertex_buffer)
            return vertex_buffer.result;
        const auto index_buffer =
            mesh.value.index_resource.is_valid()
                ? foundation::ResultValue<pipeline::RenderGraphBuffer>{foundation::Result::success(),
                                                                       mesh.value.index_resource}
                : find_or_import_buffer(builder, make_resource_id(kIndexResourceNamespace, instance.mesh.value()),
                                        mesh.value.index_buffer, mesh.value.index_buffer_desc);
        if (!index_buffer)
            return index_buffer.result;
        const auto transform_buffer =
            find_or_import_buffer(builder, make_object_resource_id(instance.object, context.scene_snapshot->view.id),
                                  object.value.transform_buffer, object.value.transform_buffer_desc);
        if (!transform_buffer)
            return transform_buffer.result;

        pass_data->draws.push_back(
            {vertex_buffer.value, index_buffer.value, transform_buffer.value, material_resource.value.gbuffer_pipeline,
             material_resource.value.binding_set, material_resource.value.binding_group, object.value.binding_set,
             object.value.binding_group, object.value.transform_offset, object.value.transform_size,
             material_resource.value.material_texture, material_resource.value.material_parameter_resource,
             material_resource.value.material_parameter_offset, material_resource.value.material_parameter_size,
             mesh.value.index_count, mesh.value.index_format});
    }

    const auto pass = detail::add_builtin_render_pass(
        builder, context, kGBufferPassTypeId,
        [pass_data](pipeline::RenderPassBuilder& pass_builder) {
            for (const auto& draw : pass_data->draws) {
                if (const auto result = pass_builder.read(draw.vertex_buffer, rhi::ResourceUsage::kVertexInput,
                                                          rhi::PipelineStage::kVertexInput, rhi::ShaderStage::kVertex);
                    !result)
                    return result;
                if (const auto result = pass_builder.read(draw.index_buffer, rhi::ResourceUsage::kIndexInput,
                                                          rhi::PipelineStage::kVertexInput, rhi::ShaderStage::kVertex);
                    !result)
                    return result;
                if (const auto result = pass_builder.read(draw.transform_buffer, rhi::ResourceUsage::kUniformRead,
                                                          rhi::PipelineStage::kVertexShader, rhi::ShaderStage::kVertex,
                                                          {draw.transform_offset, draw.transform_size});
                    !result)
                    return result;
                if (const auto result =
                        pass_builder.read(draw.material_texture, rhi::ResourceUsage::kSampledRead,
                                          rhi::PipelineStage::kFragmentShader, rhi::ShaderStage::kFragment);
                    !result)
                    return result;
                if (const auto result =
                        pass_builder.read(draw.material_parameter_resource, rhi::ResourceUsage::kUniformRead,
                                          rhi::PipelineStage::kFragmentShader, rhi::ShaderStage::kFragment,
                                          {draw.material_parameter_offset, draw.material_parameter_size});
                    !result)
                    return result;
            }
            if (const auto result = pass_builder.use_color_attachment(
                    pass_data->albedo, rhi::LoadOp::kClear, rhi::StoreOp::kStore, {}, rhi::ResourceUsage::kSampledRead);
                !result)
                return result;
            if (const auto result = pass_builder.use_color_attachment(
                    pass_data->normal, rhi::LoadOp::kClear, rhi::StoreOp::kStore, {}, rhi::ResourceUsage::kSampledRead);
                !result)
                return result;
            if (const auto result =
                    pass_builder.use_color_attachment(pass_data->material, rhi::LoadOp::kClear, rhi::StoreOp::kStore,
                                                      {}, rhi::ResourceUsage::kSampledRead);
                !result)
                return result;
            return pass_builder.use_depth_stencil_attachment(
                pass_data->depth, rhi::LoadOp::kClear, rhi::StoreOp::kStore, 1.0f, 0, rhi::ResourceUsage::kSampledRead);
        },
         [pass_data](pipeline::RenderPassExecutionContext& execution) {
            if (const auto result = execution.begin_rendering(); !result)
                return result;
            if (const auto result = execution.set_viewport({0.0f, 0.0f, static_cast<float>(pass_data->extent.width),
                                                            static_cast<float>(pass_data->extent.height), 0.0f, 1.0f});
                !result)
                return end_rendering_after_failure(execution, result);
            if (const auto result = execution.set_scissor({0, 0, pass_data->extent.width, pass_data->extent.height});
                !result)
                return end_rendering_after_failure(execution, result);
            for (const auto& draw : pass_data->draws) {
                if (const auto result = execution.bind_pipeline(draw.pipeline); !result)
                    return end_rendering_after_failure(execution, result);
                if (const auto result =
                        execution.bind_binding_set(draw.material_binding_group, draw.material_binding_set);
                    !result)
                    return end_rendering_after_failure(execution, result);
                if (const auto result = execution.bind_binding_set(draw.object_binding_group, draw.object_binding_set);
                    !result)
                    return end_rendering_after_failure(execution, result);
                const auto vertex_buffer = execution.buffer(draw.vertex_buffer);
                if (!vertex_buffer)
                    return end_rendering_after_failure(execution, vertex_buffer.result);
                const auto index_buffer = execution.buffer(draw.index_buffer);
                if (!index_buffer)
                    return end_rendering_after_failure(execution, index_buffer.result);
                if (const auto result = execution.bind_vertex_buffer(0, vertex_buffer.value); !result)
                    return end_rendering_after_failure(execution, result);
                if (const auto result = execution.bind_index_buffer(index_buffer.value, 0, draw.index_format); !result)
                    return end_rendering_after_failure(execution, result);
                if (const auto result = execution.draw_indexed({draw.index_count, 1, 0, 0, 0}); !result)
                    return end_rendering_after_failure(execution, result);
            }
            return execution.end_rendering();
        },
        authored_instance);
    return pass ? foundation::Result::success() : pass.result;
}
}  // namespace

foundation::Result register_opaque_render_feature(pipeline::PipelineRegistry& registry) {
    return registry.register_feature([] { return std::make_unique<OpaqueRenderFeature>(); });
}

foundation::Result bind_opaque_render_feature(pipeline::PipelineRegistry& registry, int32_t priority) {
    return registry.bind_feature({
        .feature = kOpaqueRenderFeatureId,
        .pipeline = pipeline::PipelineId("RenderPipeline"),
        .scope = pipeline::PipelineScope::kFrame,
        .point = pipeline::FeaturePointId("RenderPipeline.Opaque"),
        .priority = priority,
        .enabled = true,
    });
}

foundation::Result build_opaque_render_graph(pipeline::RenderGraphBuilder& builder, const RenderFeatureContext& context,
                                             bool preserve_depth, pipeline::RenderGraphTexture shadow_map,
                                             pipeline::RenderGraphBuffer light_clusters,
                                             const RenderShadowResource* shadow_resource,
                                             const RenderLightCullingResource* light_culling,
                                             pipeline::RenderGraphTexture color_override,
                                             pipeline::RenderPassInstanceId authored_instance) {
    if (!context.is_valid()) {
        return invalid_state("opaque render graph helper requires a valid renderer context");
    }
    if (render_feature_context(builder) != &context) {
        return invalid_state("opaque render graph helper context does not match the runtime builder");
    }
    // The feature implementation reads the same typed context from the
    // runtime builder. The explicit argument is part of the public contract
    // and is validated above so callers cannot accidentally pass a stale
    // snapshot while reusing the draw path for a variant pipeline.
    OpaqueRenderFeature feature;
    return feature.build_render_graph_with_options(builder, preserve_depth, shadow_map, light_clusters, shadow_resource,
                                                   light_culling, color_override, authored_instance);
}

foundation::Result build_gbuffer_render_graph(pipeline::RenderGraphBuilder& builder,
                                              const RenderFeatureContext& context,
                                              pipeline::RenderGraphTexture albedo_target,
                                              pipeline::RenderGraphTexture normal_target,
                                              pipeline::RenderGraphTexture material_target,
                                               pipeline::RenderGraphTexture depth_target,
                                               pipeline::RenderPassInstanceId authored_instance) {
    return build_gbuffer_impl(builder, context, albedo_target, normal_target, material_target, depth_target,
                              authored_instance);
}

foundation::Result build_depth_render_graph(pipeline::RenderGraphBuilder& builder, const RenderFeatureContext& context,
                                            const RenderView& view, pipeline::RenderGraphTexture depth_target,
                                             std::string_view pass_name, std::span<const RenderInstance> instances,
                                             pipeline::RenderPassInstanceId authored_instance) {
    if (!builder.is_runtime() || builder.scope() != pipeline::PipelineScope::kFrame || !context.is_valid() ||
        render_feature_context(builder) != &context || !view.is_valid() ||
        view.world != context.scene_snapshot->world || !depth_target.is_valid() || pass_name.empty()) {
        return invalid_state("depth render graph context or target is invalid");
    }
    if (instances.empty())
        instances = context.scene_snapshot->instances;
    if (!instances.empty() && context.resource_resolver == nullptr) {
        return invalid_state("depth render graph requires a render resource resolver");
    }
    struct DepthDraw final {
        pipeline::RenderGraphBuffer vertex{}, index{}, transform{};
        rhi::PipelineHandle pipeline{};
        rhi::BindingSetHandle object_set{};
        uint32_t object_group{0};
        uint64_t transform_offset{0}, transform_size{0};
        uint32_t index_count{0};
        rhi::IndexFormat index_format{rhi::IndexFormat::kUint16};
    };
    struct DepthData final {
        pipeline::RenderGraphTexture depth{};
        RenderExtent extent{};
        std::vector<DepthDraw> draws{};
    };
    auto data = std::make_shared<DepthData>();
    data->depth = depth_target;
    data->extent = view.output_extent;
    data->draws.reserve(instances.size());
    for (const RenderInstance& instance : instances) {
        const auto mesh = context.resource_resolver->resolve_mesh(instance.mesh);
        const auto material = context.resource_resolver->resolve_material(instance.material);
        const auto object = context.resource_resolver->resolve_object(instance, view);
        if (!mesh || !material || !object)
            return !mesh ? mesh.result : !material ? material.result : object.result;
        if (!mesh.value.is_valid() || !material.value.depth_pipeline.is_valid() || !object.value.is_valid()) {
            return invalid_state("depth render graph received incomplete draw resources");
        }
        if (mesh.value.vertex_layout.position_format != rhi::Format::kRgba32Float ||
            mesh.value.vertex_layout.position_offset > mesh.value.vertex_stride ||
            kPositionAttributeSize > mesh.value.vertex_stride - mesh.value.vertex_layout.position_offset) {
            return invalid_state("depth render graph requires a float4 position attribute");
        }
        const auto vertex =
            mesh.value.vertex_resource.is_valid()
                ? foundation::ResultValue<pipeline::RenderGraphBuffer>{foundation::Result::success(),
                                                                       mesh.value.vertex_resource}
                : find_or_import_buffer(builder, make_resource_id(kVertexResourceNamespace, instance.mesh.value()),
                                        mesh.value.vertex_buffer, mesh.value.vertex_buffer_desc);
        const auto index =
            mesh.value.index_resource.is_valid()
                ? foundation::ResultValue<pipeline::RenderGraphBuffer>{foundation::Result::success(),
                                                                       mesh.value.index_resource}
                : find_or_import_buffer(builder, make_resource_id(kIndexResourceNamespace, instance.mesh.value()),
                                        mesh.value.index_buffer, mesh.value.index_buffer_desc);
        const auto transform = find_or_import_buffer(builder, make_object_resource_id(instance.object, view.id),
                                                     object.value.transform_buffer, object.value.transform_buffer_desc);
        if (!vertex || !index || !transform)
            return !vertex ? vertex.result : !index ? index.result : transform.result;
        data->draws.push_back({vertex.value, index.value, transform.value, material.value.depth_pipeline,
                               object.value.binding_set, object.value.binding_group, object.value.transform_offset,
                               object.value.transform_size, mesh.value.index_count, mesh.value.index_format});
    }
    const auto add_depth_pass = [&](pipeline::RenderPassSetupFunction setup,
                                    pipeline::RenderPassExecuteFunction execute) {
        const pipeline::RenderPassTypeId type(pass_name);
        if (type == kZPrepassPassTypeId || type == kShadowPassTypeId)
            return detail::add_builtin_render_pass(builder, context, type, std::move(setup), std::move(execute),
                                                   authored_instance);
        return builder.add_graphics_pass(pass_name, std::move(setup), std::move(execute));
    };
    const auto pass = add_depth_pass(
        [data](pipeline::RenderPassBuilder& pass) {
            for (const auto& draw : data->draws) {
                if (const auto r = pass.read(draw.vertex, rhi::ResourceUsage::kVertexInput,
                                             rhi::PipelineStage::kVertexInput, rhi::ShaderStage::kVertex);
                    !r)
                    return r;
                if (const auto r = pass.read(draw.index, rhi::ResourceUsage::kIndexInput,
                                             rhi::PipelineStage::kVertexInput, rhi::ShaderStage::kVertex);
                    !r)
                    return r;
                if (const auto r =
                        pass.read(draw.transform, rhi::ResourceUsage::kUniformRead, rhi::PipelineStage::kVertexShader,
                                  rhi::ShaderStage::kVertex, {draw.transform_offset, draw.transform_size});
                    !r)
                    return r;
            }
            return pass.use_depth_stencil_attachment(data->depth, rhi::LoadOp::kClear, rhi::StoreOp::kStore, 1.0f, 0,
                                                     rhi::ResourceUsage::kDepthStencilWrite);
        },
        [data](pipeline::RenderPassExecutionContext& execution) {
            if (const auto r = execution.begin_rendering(); !r)
                return r;
            const auto fail = [&execution](foundation::Result result) {
                return end_rendering_after_failure(execution, std::move(result));
            };
            if (const auto r = execution.set_viewport(
                    {0, 0, static_cast<float>(data->extent.width), static_cast<float>(data->extent.height), 0, 1});
                !r)
                return fail(r);
            if (const auto r = execution.set_scissor({0, 0, data->extent.width, data->extent.height}); !r)
                return fail(r);
            for (const auto& draw : data->draws) {
                if (const auto r = execution.bind_pipeline(draw.pipeline); !r)
                    return fail(r);
                if (const auto r = execution.bind_binding_set(draw.object_group, draw.object_set); !r)
                    return fail(r);
                const auto vertex = execution.buffer(draw.vertex);
                const auto index = execution.buffer(draw.index);
                if (!vertex || !index)
                    return fail(!vertex ? vertex.result : index.result);
                if (const auto r = execution.bind_vertex_buffer(0, vertex.value); !r)
                    return fail(r);
                if (const auto r = execution.bind_index_buffer(index.value, 0, draw.index_format); !r)
                    return fail(r);
                if (const auto r = execution.draw_indexed({draw.index_count, 1, 0, 0, 0}); !r)
                    return fail(r);
            }
            return execution.end_rendering();
        });
    return pass ? foundation::Result::success() : pass.result;
}
}  // namespace aeg::renderer
