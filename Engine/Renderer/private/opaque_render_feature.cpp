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
    RenderExtent output_extent{};
    std::vector<OpaqueDrawItem> draws{};
};

class OpaqueRenderFeature final : public pipeline::RenderFeature {
public:
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
        const RenderFeatureContext* context = render_feature_context(builder);
        if (context == nullptr || !context->is_valid()) {
            return invalid_state("opaque render feature requires a valid renderer context");
        }
        if (context->scene_snapshot->instances.empty()) {
            return foundation::Result::success();
        }
        if (context->resource_resolver == nullptr) {
            return invalid_state("opaque render feature requires a render resource resolver");
        }
        if (!context->scene_snapshot->view.color_target.is_valid()) {
            return invalid_argument("opaque render feature requires a RenderView color target");
        }

        const foundation::ResultValue<pipeline::RenderGraphTexture> color_target =
            builder.find_texture(context->scene_snapshot->view.color_target);
        if (!color_target) {
            return color_target.result;
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

        // The graph owns callbacks past build(), so draw declarations need shared ownership.
        auto pass_data = std::make_shared<OpaquePassData>();
        pass_data->color_target = color_target.value;
        pass_data->depth_target = depth_target;
        pass_data->color_target_final_usage = context->scene_snapshot->view.color_target_final_usage;
        pass_data->has_depth_target = depth_target.is_valid();
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

            pass_data->draws.push_back({
                .vertex_buffer = vertex_buffer.value,
                .index_buffer = index_buffer.value,
                .transform_buffer = transform_buffer.value,
                .pipeline = material.value.pipeline,
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
        const foundation::ResultValue<pipeline::RenderGraphPass> pass = builder.add_graphics_pass(
            "DrawOpaque",
            [captured_data](pipeline::RenderPassBuilder& pass_builder) {
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
                if (const foundation::Result result = pass_builder.read(
                        captured_data->lighting_resource, rhi::ResourceUsage::kUniformRead,
                        rhi::PipelineStage::kFragmentShader, rhi::ShaderStage::kFragment,
                        pipeline::RenderGraphBufferRange{captured_data->lighting_offset, captured_data->lighting_size});
                    !result) {
                    return result;
                }
                if (const foundation::Result result = pass_builder.use_color_attachment(
                        captured_data->color_target, rhi::LoadOp::kClear, rhi::StoreOp::kStore,
                        rhi::ClearColorValue{0.0f, 0.0f, 0.0f, 1.0f}, captured_data->color_target_final_usage);
                    !result) {
                    return result;
                }
                if (captured_data->has_depth_target) {
                    return pass_builder.use_depth_stencil_attachment(captured_data->depth_target, rhi::LoadOp::kClear,
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

                for (const OpaqueDrawItem& draw : captured_data->draws) {
                    if (const foundation::Result result = execution.bind_pipeline(draw.pipeline); !result) {
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
}  // namespace aeg::renderer
