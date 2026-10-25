/**
 * @file pipeline.cpp
 * @brief Implements pipeline definition collection, compilation, and scheduling.
 */
#include "pipeline.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <new>
#include <numeric>
#include <queue>
#include <shared_mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "render_pass_builder_backend.hpp"
#include "render_pass_execution_context_backend.hpp"
#include "render_graph_builder_backend.hpp"
#include "phase_registry_execution.hpp"
#include "pipeline_compiler_execution.hpp"
#include "pipeline_scope_host_execution.hpp"

namespace aeg::pipeline {

RenderPassBuilder::RenderPassBuilder(RenderPassBuilderBackend& backend, RenderGraphBuildFlags build_flags) noexcept
    : backend_(&backend), build_flags_(build_flags) {}

foundation::ResultValue<RenderGraphBuffer> RenderPassBuilder::find_buffer(RenderResourceId id) const {
    return backend_ == nullptr ? foundation::ResultValue<RenderGraphBuffer>{render_graph_builder_invalid_state(), {}}
                               : backend_->find_buffer(id);
}

foundation::ResultValue<RenderGraphTexture> RenderPassBuilder::find_texture(RenderResourceId id) const {
    return backend_ == nullptr ? foundation::ResultValue<RenderGraphTexture>{render_graph_builder_invalid_state(), {}}
                               : backend_->find_texture(id);
}

foundation::Result RenderPassBuilder::read(RenderGraphBuffer buffer, rhi::ResourceUsage usage,
                                           rhi::PipelineStageMask pipeline_stages,
                                           rhi::ShaderStageMask shader_stages, RenderGraphBufferRange range) {
    return backend_ == nullptr ? render_graph_builder_invalid_state()
                               : backend_->read_buffer(buffer, usage, pipeline_stages, shader_stages, range);
}

foundation::Result RenderPassBuilder::write(RenderGraphBuffer buffer, rhi::ResourceUsage usage,
                                            rhi::PipelineStageMask pipeline_stages,
                                            rhi::ShaderStageMask shader_stages, RenderGraphBufferRange range,
                                            bool discard_contents) {
    return backend_ == nullptr
               ? render_graph_builder_invalid_state()
               : backend_->write_buffer(buffer, usage, pipeline_stages, shader_stages, range, discard_contents);
}

foundation::Result RenderPassBuilder::read_write(RenderGraphBuffer buffer, rhi::ResourceUsage usage,
                                                 rhi::PipelineStageMask pipeline_stages,
                                                 rhi::ShaderStageMask shader_stages, RenderGraphBufferRange range) {
    return backend_ == nullptr ? render_graph_builder_invalid_state()
                               : backend_->read_write_buffer(buffer, usage, pipeline_stages, shader_stages, range);
}

foundation::Result RenderPassBuilder::read(RenderGraphTexture texture, rhi::ResourceUsage usage,
                                           rhi::PipelineStageMask pipeline_stages,
                                           rhi::ShaderStageMask shader_stages,
                                           rhi::SubresourceRange subresources) {
    if (usage == rhi::ResourceUsage::kPresent && !has_flag(build_flags_, RenderGraphBuildFlags::kMayWriteFinalOutput)) {
        return render_graph_builder_permission_denied(RenderGraphBuildFlags::kMayWriteFinalOutput);
    }
    return backend_ == nullptr ? render_graph_builder_invalid_state()
                               : backend_->read_texture(texture, usage, pipeline_stages, shader_stages, subresources);
}

foundation::Result RenderPassBuilder::write(RenderGraphTexture texture, rhi::ResourceUsage usage,
                                            rhi::PipelineStageMask pipeline_stages,
                                            rhi::ShaderStageMask shader_stages,
                                            rhi::SubresourceRange subresources, bool discard_contents) {
    if (usage == rhi::ResourceUsage::kPresent && !has_flag(build_flags_, RenderGraphBuildFlags::kMayWriteFinalOutput)) {
        return render_graph_builder_permission_denied(RenderGraphBuildFlags::kMayWriteFinalOutput);
    }
    return backend_ == nullptr
               ? render_graph_builder_invalid_state()
               : backend_->write_texture(texture, usage, pipeline_stages, shader_stages, subresources,
                                         discard_contents);
}

foundation::Result RenderPassBuilder::read_write(RenderGraphTexture texture, rhi::ResourceUsage usage,
                                                 rhi::PipelineStageMask pipeline_stages,
                                                 rhi::ShaderStageMask shader_stages,
                                                 rhi::SubresourceRange subresources) {
    if (usage == rhi::ResourceUsage::kPresent && !has_flag(build_flags_, RenderGraphBuildFlags::kMayWriteFinalOutput)) {
        return render_graph_builder_permission_denied(RenderGraphBuildFlags::kMayWriteFinalOutput);
    }
    return backend_ == nullptr
               ? render_graph_builder_invalid_state()
               : backend_->read_write_texture(texture, usage, pipeline_stages, shader_stages, subresources);
}

foundation::Result RenderPassBuilder::use_color_attachment(RenderGraphTexture texture, rhi::LoadOp load_op,
                                                           rhi::StoreOp store_op, rhi::ClearColorValue clear_color,
                                                           rhi::ResourceUsage final_usage,
                                                           rhi::SubresourceRange subresources) {
    if (final_usage == rhi::ResourceUsage::kPresent &&
        !has_flag(build_flags_, RenderGraphBuildFlags::kMayWriteFinalOutput)) {
        return render_graph_builder_permission_denied(RenderGraphBuildFlags::kMayWriteFinalOutput);
    }
    return backend_ == nullptr
               ? render_graph_builder_invalid_state()
               : backend_->use_color_attachment(texture, load_op, store_op, clear_color, final_usage, subresources);
}

foundation::Result RenderPassBuilder::use_depth_stencil_attachment(
    RenderGraphTexture texture, rhi::LoadOp depth_load_op, rhi::StoreOp depth_store_op, float clear_depth,
    uint32_t clear_stencil, rhi::ResourceUsage final_usage, rhi::SubresourceRange subresources) {
    if (final_usage == rhi::ResourceUsage::kPresent &&
        !has_flag(build_flags_, RenderGraphBuildFlags::kMayWriteFinalOutput)) {
        return render_graph_builder_permission_denied(RenderGraphBuildFlags::kMayWriteFinalOutput);
    }
    return backend_ == nullptr
               ? render_graph_builder_invalid_state()
               : backend_->use_depth_stencil_attachment(texture, depth_load_op, depth_store_op, clear_depth,
                                                        clear_stencil, final_usage, subresources);
}

void RenderPassBuilder::set_side_effects(bool enabled) noexcept {
    if (backend_ != nullptr) {
        backend_->set_side_effects(enabled);
    }
}

foundation::Result RenderPassBuilder::declare_render_program(const RenderProgramRequest& request,
                                                             const RenderPassConfiguration& configuration) {
    return backend_ == nullptr ? foundation::invalid_state("render-pass builder backend is not attached")
                               : backend_->declare_render_program(request, configuration);
}

RenderPassExecutionContext::RenderPassExecutionContext(RenderPassExecutionContextBackend& backend) noexcept
    : backend_(&backend) {}

RenderGraphPass RenderPassExecutionContext::pass() const noexcept {
    return backend_->pass();
}

foundation::ResultValue<RenderGraphBuffer> RenderPassExecutionContext::find_buffer(RenderResourceId id) const {
    return backend_->find_buffer(id);
}

foundation::ResultValue<RenderGraphTexture> RenderPassExecutionContext::find_texture(RenderResourceId id) const {
    return backend_->find_texture(id);
}

foundation::ResultValue<rhi::BufferHandle> RenderPassExecutionContext::buffer(RenderGraphBuffer buffer) const {
    return backend_->buffer(buffer);
}

foundation::ResultValue<rhi::TextureHandle> RenderPassExecutionContext::texture(RenderGraphTexture texture) const {
    return backend_->texture(texture);
}

foundation::ResultValue<rhi::BufferViewHandle> RenderPassExecutionContext::buffer_view(
    RenderGraphBuffer buffer, rhi::BufferViewKind kind, uint64_t offset, uint64_t size, rhi::Format format,
    uint32_t structure_stride) {
    return backend_->buffer_view(buffer, kind, offset, size, format, structure_stride);
}

foundation::ResultValue<rhi::TextureViewHandle> RenderPassExecutionContext::texture_view(
    RenderGraphTexture texture, rhi::TextureViewKind kind, rhi::SubresourceRange subresources, rhi::Format format,
    rhi::TextureViewDimension dimension) {
    return backend_->texture_view(texture, kind, subresources, format, dimension);
}

foundation::Result RenderPassExecutionContext::begin_rendering() {
    return backend_->begin_rendering();
}

foundation::Result RenderPassExecutionContext::end_rendering() {
    return backend_->end_rendering();
}

bool RenderPassExecutionContext::rendering_active() const noexcept {
    return backend_->rendering_active();
}

bool RenderPassExecutionContext::rendering_started() const noexcept {
    return backend_->rendering_started();
}

foundation::Result RenderPassExecutionContext::set_viewport(const rhi::Viewport& viewport) {
    return backend_->set_viewport(viewport);
}

foundation::Result RenderPassExecutionContext::set_scissor(const rhi::ScissorRect& scissor) {
    return backend_->set_scissor(scissor);
}

foundation::Result RenderPassExecutionContext::bind_pipeline(rhi::PipelineHandle pipeline) {
    return backend_->bind_pipeline(pipeline);
}

foundation::Result RenderPassExecutionContext::bind_binding_set(uint32_t group, rhi::BindingSetHandle set,
                                                                std::span<const uint32_t> dynamic_offsets) {
    return backend_->bind_binding_set(group, set, dynamic_offsets);
}

foundation::Result RenderPassExecutionContext::set_inline_constants(uint32_t offset,
                                                                     rhi::ShaderStageMask shader_stages,
                                                                     std::span<const std::byte> data) {
    return backend_->set_inline_constants(offset, shader_stages, data);
}

foundation::Result RenderPassExecutionContext::bind_vertex_buffer(uint32_t binding, rhi::BufferHandle buffer,
                                                                   uint64_t offset) {
    return backend_->bind_vertex_buffer(binding, buffer, offset);
}

foundation::Result RenderPassExecutionContext::bind_index_buffer(rhi::BufferHandle buffer, uint64_t offset,
                                                                  rhi::IndexFormat format) {
    return backend_->bind_index_buffer(buffer, offset, format);
}

foundation::Result RenderPassExecutionContext::draw(const rhi::DrawDesc& desc) {
    return backend_->draw(desc);
}

foundation::Result RenderPassExecutionContext::draw_indexed(const rhi::DrawIndexedDesc& desc) {
    return backend_->draw_indexed(desc);
}

foundation::Result RenderPassExecutionContext::dispatch(const rhi::DispatchDesc& desc) {
    return backend_->dispatch(desc);
}

foundation::Result RenderPassExecutionContext::dispatch_indirect(const rhi::DispatchIndirectDesc& desc) {
    return backend_->dispatch_indirect(desc);
}

foundation::Result RenderPassExecutionContext::copy_buffer(RenderGraphBuffer source, RenderGraphBuffer destination,
                                                            uint64_t source_offset, uint64_t destination_offset,
                                                            uint64_t size) {
    return backend_->copy_buffer(source, destination, source_offset, destination_offset, size);
}

foundation::Result RenderPassExecutionContext::copy_buffer_to_texture(
    RenderGraphBuffer source, uint64_t source_offset, uint32_t source_row_length, uint32_t source_image_height,
    RenderGraphTexture destination, rhi::TextureSubresourceLayers destination_subresources,
    rhi::Offset3D destination_offset, rhi::Extent3D extent) {
    return backend_->copy_buffer_to_texture(source, source_offset, source_row_length, source_image_height, destination,
                                             destination_subresources, destination_offset, extent);
}

foundation::Result RenderPassExecutionContext::copy_texture_to_buffer(
    RenderGraphTexture source, rhi::TextureSubresourceLayers source_subresources, rhi::Offset3D source_offset,
    rhi::Extent3D extent, RenderGraphBuffer destination, uint64_t destination_offset, uint32_t destination_row_length,
    uint32_t destination_image_height) {
    return backend_->copy_texture_to_buffer(source, source_subresources, source_offset, extent, destination,
                                             destination_offset, destination_row_length, destination_image_height);
}

foundation::Result RenderPassExecutionContext::copy_texture(
    RenderGraphTexture source, rhi::TextureSubresourceLayers source_subresources, rhi::Offset3D source_offset,
    RenderGraphTexture destination, rhi::TextureSubresourceLayers destination_subresources,
    rhi::Offset3D destination_offset, rhi::Extent3D extent) {
    return backend_->copy_texture(source, source_subresources, source_offset, destination, destination_subresources,
                                   destination_offset, extent);
}

foundation::Result RenderPassExecutionContext::clear_texture(RenderGraphTexture texture,
                                                              rhi::SubresourceRange subresources,
                                                              rhi::ClearColorValue color, float depth,
                                                              uint32_t stencil) {
    return backend_->clear_texture(texture, subresources, color, depth, stencil);
}

foundation::Result RenderPassExecutionContext::bind_render_program(const RenderProgramRequest& request,
                                                                    const RenderPassConfiguration& configuration) {
    return backend_ == nullptr ? foundation::invalid_state("render-pass execution backend is not attached")
                               : backend_->bind_render_program(request, configuration);
}

foundation::ResultValue<RenderGraphBuffer> RenderGraphBuilder::create_buffer(
    const rhi::BufferDesc& desc, RenderGraphResourceLifetime lifetime) {
    return create_buffer({}, desc, lifetime);
}

foundation::ResultValue<RenderGraphBuffer> RenderGraphBuilder::create_buffer(
    RenderResourceId id, const rhi::BufferDesc& desc, RenderGraphResourceLifetime lifetime) {
    if (runtime_backend_ == nullptr) {
        return {render_graph_builder_invalid_state(), {}};
    }
    if (!allows(RenderGraphBuildFlags::kMayCreateResources)) {
        return {render_graph_builder_permission_denied(RenderGraphBuildFlags::kMayCreateResources), {}};
    }
    return runtime_backend_->create_buffer(id, desc, lifetime);
}

foundation::ResultValue<RenderGraphTexture> RenderGraphBuilder::create_texture(
    const rhi::TextureDesc& desc, RenderGraphResourceLifetime lifetime) {
    return create_texture({}, desc, lifetime);
}

foundation::ResultValue<RenderGraphTexture> RenderGraphBuilder::create_texture(
    RenderResourceId id, const rhi::TextureDesc& desc, RenderGraphResourceLifetime lifetime) {
    if (runtime_backend_ == nullptr) {
        return {render_graph_builder_invalid_state(), {}};
    }
    if (!allows(RenderGraphBuildFlags::kMayCreateResources)) {
        return {render_graph_builder_permission_denied(RenderGraphBuildFlags::kMayCreateResources), {}};
    }
    return runtime_backend_->create_texture(id, desc, lifetime);
}

foundation::ResultValue<RenderGraphBuffer> RenderGraphBuilder::import_buffer(
    RenderResourceId id, rhi::BufferHandle buffer, const rhi::BufferDesc& desc,
    const rhi::ResourceState& initial_state) {
    if (runtime_backend_ == nullptr) {
        return {render_graph_builder_invalid_state(), {}};
    }
    if (!allows(RenderGraphBuildFlags::kMayCreateResources)) {
        return {render_graph_builder_permission_denied(RenderGraphBuildFlags::kMayCreateResources), {}};
    }
    return runtime_backend_->import_buffer(id, buffer, desc, initial_state);
}

foundation::ResultValue<RenderGraphTexture> RenderGraphBuilder::import_texture(
    RenderResourceId id, rhi::TextureHandle texture, rhi::TextureViewHandle default_view, const rhi::TextureDesc& desc,
    const rhi::ResourceState& initial_state) {
    if (runtime_backend_ == nullptr) {
        return {render_graph_builder_invalid_state(), {}};
    }
    if (!allows(RenderGraphBuildFlags::kMayCreateResources)) {
        return {render_graph_builder_permission_denied(RenderGraphBuildFlags::kMayCreateResources), {}};
    }
    return runtime_backend_->import_texture(id, texture, default_view, desc, initial_state);
}

foundation::ResultValue<RenderGraphTexture> RenderGraphBuilder::import_swapchain_image(
    RenderResourceId id, const rhi::FrameAcquireResult& acquired, const rhi::TextureDesc& desc,
    const rhi::ResourceState& initial_state) {
    if (runtime_backend_ == nullptr) {
        return {render_graph_builder_invalid_state(), {}};
    }
    if (!allows(RenderGraphBuildFlags::kMayCreateResources)) {
        return {render_graph_builder_permission_denied(RenderGraphBuildFlags::kMayCreateResources), {}};
    }
    return runtime_backend_->import_swapchain_image(id, acquired, desc, initial_state);
}

foundation::ResultValue<RenderGraphBuffer> RenderGraphBuilder::find_buffer(RenderResourceId id) const {
    if (runtime_backend_ == nullptr) {
        return {render_graph_builder_invalid_state(), {}};
    }
    return runtime_backend_->find_buffer(id);
}

foundation::ResultValue<RenderGraphTexture> RenderGraphBuilder::find_texture(RenderResourceId id) const {
    if (runtime_backend_ == nullptr) {
        return {render_graph_builder_invalid_state(), {}};
    }
    return runtime_backend_->find_texture(id);
}

foundation::Result RenderGraphBuilder::bind_buffer(RenderResourceId id, RenderGraphBuffer buffer) {
    if (runtime_backend_ == nullptr) {
        return render_graph_builder_invalid_state();
    }
    if (!allows(RenderGraphBuildFlags::kMayCreateResources)) {
        return render_graph_builder_permission_denied(RenderGraphBuildFlags::kMayCreateResources);
    }
    return runtime_backend_->bind_buffer(id, buffer);
}

foundation::Result RenderGraphBuilder::bind_texture(RenderResourceId id, RenderGraphTexture texture) {
    if (runtime_backend_ == nullptr) {
        return render_graph_builder_invalid_state();
    }
    if (!allows(RenderGraphBuildFlags::kMayCreateResources)) {
        return render_graph_builder_permission_denied(RenderGraphBuildFlags::kMayCreateResources);
    }
    return runtime_backend_->bind_texture(id, texture);
}

foundation::Result RenderGraphBuilder::export_buffer(RenderGraphBuffer buffer) {
    if (runtime_backend_ == nullptr) {
        return render_graph_builder_invalid_state();
    }
    if (!allows(RenderGraphBuildFlags::kMayWriteFinalOutput)) {
        return render_graph_builder_permission_denied(RenderGraphBuildFlags::kMayWriteFinalOutput);
    }
    return runtime_backend_->export_buffer(buffer);
}

foundation::Result RenderGraphBuilder::export_texture(RenderGraphTexture texture) {
    if (runtime_backend_ == nullptr) {
        return render_graph_builder_invalid_state();
    }
    if (!allows(RenderGraphBuildFlags::kMayWriteFinalOutput)) {
        return render_graph_builder_permission_denied(RenderGraphBuildFlags::kMayWriteFinalOutput);
    }
    return runtime_backend_->export_texture(texture);
}

foundation::ResultValue<RenderGraphPass> RenderGraphBuilder::add_pass(std::string_view name, RenderGraphPassType type,
                                                                       rhi::QueueClass queue,
                                                                       RenderPassSetupFunction setup,
                                                                       RenderPassExecuteFunction execute) {
    if (runtime_backend_ == nullptr) {
        return {render_graph_builder_invalid_state(), {}};
    }
    RenderGraphBuildFlags required_flag = RenderGraphBuildFlags::kNone;
    switch (type) {
        case RenderGraphPassType::kGraphics:
            required_flag = RenderGraphBuildFlags::kMayCreateGraphicsPass;
            break;
        case RenderGraphPassType::kCompute:
            required_flag = RenderGraphBuildFlags::kMayCreateComputePass;
            break;
        case RenderGraphPassType::kCopy:
            required_flag = RenderGraphBuildFlags::kMayCreateCopyPass;
            break;
    }
    if (!allows(required_flag)) {
        return {render_graph_builder_permission_denied(required_flag), {}};
    }
    if ((queue == rhi::QueueClass::kCompute || queue == rhi::QueueClass::kCopy) &&
        !allows(RenderGraphBuildFlags::kMayUseDedicatedQueue)) {
        return {render_graph_builder_permission_denied(RenderGraphBuildFlags::kMayUseDedicatedQueue), {}};
    }
    return runtime_backend_->add_pass(name, type, queue, std::move(setup), std::move(execute));
}

foundation::Result RenderGraphBuilder::add_dependency(RenderGraphPass source, RenderGraphPass destination) {
    return runtime_backend_ == nullptr ? render_graph_builder_invalid_state()
                                       : runtime_backend_->add_dependency(source, destination);
}

foundation::Result RenderGraphBuilder::remember_native_pass(RenderPassInstanceId instance, RenderGraphPass pass) {
    if (!instance.is_valid() || !pass.is_valid()) return foundation::invalid_argument("native pass identity or handle is invalid");
    try {
        if (!native_passes_.emplace(instance, pass).second)
            return foundation::invalid_argument("native pass instance is duplicated in this sequence");
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("native pass sequence allocation failed");
    }
}

foundation::ResultValue<RenderGraphPass> RenderGraphBuilder::native_pass(RenderPassInstanceId instance) const {
    if (!instance.is_valid()) return {foundation::invalid_argument("native pass instance is invalid"), {}};
    const auto found = native_passes_.find(instance);
    if (found == native_passes_.end()) return {foundation::not_found("native sequence pass is missing or disabled"), {}};
    return {foundation::Result::success(), found->second};
}

foundation::Result RenderGraphBuilder::retain_execution_owner(std::shared_ptr<const void> owner) {
    return runtime_backend_ ? runtime_backend_->retain_execution_owner(std::move(owner)) : render_graph_builder_invalid_state();
}

namespace {
inline constexpr uint32_t kKnownAccessFlags =
    static_cast<uint32_t>(AccessFlags::kOptional) | static_cast<uint32_t>(AccessFlags::kPreserveContents) |
    static_cast<uint32_t>(AccessFlags::kDiscardContents) | static_cast<uint32_t>(AccessFlags::kExclusive) |
    static_cast<uint32_t>(AccessFlags::kDeterministicAppend);

std::string append_message(std::string prefix, const foundation::Result& result) {
    if (!result.message.empty()) {
        prefix += ": ";
        prefix += result.message;
    }
    return prefix;
}

[[nodiscard]] bool is_valid_scope(PipelineScope scope) noexcept {
    return static_cast<uint8_t>(scope) <= static_cast<uint8_t>(PipelineScope::kEditor);
}

[[nodiscard]] bool is_valid_execution_domain(ExecutionDomain domain) noexcept {
    return static_cast<uint8_t>(domain) <= static_cast<uint8_t>(ExecutionDomain::kGpu);
}

[[nodiscard]] bool is_valid_time_policy(TimePolicy policy) noexcept {
    return static_cast<uint8_t>(policy) <= static_cast<uint8_t>(TimePolicy::kShutdown);
}

[[nodiscard]] bool is_valid_engine_phase(EnginePhase phase) noexcept {
    return static_cast<uint8_t>(phase) <= static_cast<uint8_t>(EnginePhase::kShutdown);
}

[[nodiscard]] bool is_valid_phase_order_relation(PhaseOrderRelation relation) noexcept {
    return static_cast<uint8_t>(relation) <= static_cast<uint8_t>(PhaseOrderRelation::kAfter);
}

[[nodiscard]] bool is_valid_artifact_storage(ArtifactStorage storage) noexcept {
    return static_cast<uint8_t>(storage) <= static_cast<uint8_t>(ArtifactStorage::kCompletionToken);
}

[[nodiscard]] bool is_valid_artifact_lifetime(ArtifactLifetime lifetime) noexcept {
    return static_cast<uint8_t>(lifetime) <= static_cast<uint8_t>(ArtifactLifetime::kEngine);
}

[[nodiscard]] bool is_valid_ownership_policy(OwnershipPolicy ownership) noexcept {
    return static_cast<uint8_t>(ownership) <= static_cast<uint8_t>(OwnershipPolicy::kExternalBorrowed);
}

[[nodiscard]] bool is_valid_artifact_access(ArtifactAccess access) noexcept {
    return static_cast<uint8_t>(access) <= static_cast<uint8_t>(ArtifactAccess::kPublishSnapshot);
}

[[nodiscard]] int scope_rank(PipelineScope scope) noexcept {
    return static_cast<int>(scope);
}

[[nodiscard]] PipelineScopeMask lower_scope_mask(PipelineScope scope) noexcept {
    return (1u << static_cast<uint8_t>(scope)) - 1u;
}

[[nodiscard]] bool can_scope_read(PipelineScope observer, PipelineScope owner) noexcept {
    return scope_rank(observer) >= scope_rank(owner);
}

[[nodiscard]] bool descriptors_match(const ArtifactDescriptor& left, const ArtifactDescriptor& right) noexcept {
    return left.id == right.id && left.type == right.type && left.owner_scope == right.owner_scope &&
           left.storage == right.storage && left.lifetime == right.lifetime && left.ownership == right.ownership &&
           left.version == right.version && left.visible_to == right.visible_to &&
           left.external_input == right.external_input;
}

[[nodiscard]] bool artifact_is_exposed(const PipelinePlan& plan, ArtifactId artifact,
                                       PipelineScope target_scope) noexcept {
    return std::any_of(plan.exposures.begin(), plan.exposures.end(), [&](const auto& exposure) {
        return exposure.artifact == artifact && exposure.target_scope == target_scope;
    });
}

[[nodiscard]] const ArtifactDescriptor* find_plan_artifact(const PipelinePlan& plan, ArtifactId id) noexcept {
    for (const ArtifactDescriptor& artifact : plan.artifacts) {
        if (artifact.id == id) {
            return &artifact;
        }
    }
    return nullptr;
}

[[nodiscard]] bool plan_contracts_preserved(const PipelinePlan& previous, const PipelinePlan& next) noexcept {
    return std::all_of(previous.artifacts.begin(), previous.artifacts.end(), [&](const ArtifactDescriptor& descriptor) {
        const ArtifactDescriptor* replacement = find_plan_artifact(next, descriptor.id);
        return replacement != nullptr && descriptors_match(descriptor, *replacement);
    });
}

[[nodiscard]] bool is_writer(ArtifactAccess access) noexcept {
    return access == ArtifactAccess::kWrite || access == ArtifactAccess::kReadWrite ||
           access == ArtifactAccess::kAppend || access == ArtifactAccess::kPublishSnapshot;
}

[[nodiscard]] bool is_reader(ArtifactAccess access) noexcept {
    return access == ArtifactAccess::kRead || access == ArtifactAccess::kReadWrite ||
           access == ArtifactAccess::kConsume;
}

[[nodiscard]] bool is_append_writer(ArtifactAccess access) noexcept {
    return access == ArtifactAccess::kAppend;
}

using foundation::invalid_argument;
using foundation::invalid_state;
using foundation::not_found;
using foundation::unsupported;

[[nodiscard]] foundation::Result validate_artifact_descriptor(const ArtifactDescriptor& descriptor) {
    if (!descriptor.id.is_valid() || !descriptor.type.is_valid() || descriptor.version == 0 ||
        !is_valid_scope(descriptor.owner_scope) || !is_valid_artifact_storage(descriptor.storage) ||
        !is_valid_artifact_lifetime(descriptor.lifetime) || !is_valid_ownership_policy(descriptor.ownership) ||
        (descriptor.visible_to & ~kAllPipelineScopes) != 0 ||
        (descriptor.visible_to & lower_scope_mask(descriptor.owner_scope)) != 0) {
        return invalid_argument("artifact contract is invalid");
    }
    return foundation::Result::success();
}

using PlanArtifactIndex = std::unordered_map<ArtifactId, const ArtifactDescriptor*, StableIdHash<ArtifactIdTag>>;

[[nodiscard]] foundation::Result build_plan_artifact_index(const PipelinePlan& plan, PlanArtifactIndex& index) {
    index.reserve(plan.artifacts.size());
    for (const ArtifactDescriptor& descriptor : plan.artifacts) {
        if (const foundation::Result result = validate_artifact_descriptor(descriptor); !result) {
            return result;
        }
        if (!index.emplace(descriptor.id, &descriptor).second) {
            return invalid_argument("pipeline plan contains duplicate artifact ids");
        }
    }
    for (const PipelineExposureRecord& exposure : plan.exposures) {
        const auto artifact = index.find(exposure.artifact);
        if (artifact == index.end() || !is_valid_scope(exposure.target_scope) ||
            !can_scope_read(exposure.target_scope, artifact->second->owner_scope) ||
            exposure.target_scope == artifact->second->owner_scope) {
            return invalid_argument("pipeline plan contains an invalid artifact exposure");
        }
    }
    return foundation::Result::success();
}

}  // namespace

struct PhaseRegistry::Impl final {
    struct Record final {
        PhaseDescriptor descriptor{};
        std::string name_storage{};
        bool builtin{false};
        uint32_t builtin_order{kInvalidPipelineHandleIndex};
        uint32_t lower_builtin_order{kInvalidPipelineHandleIndex};
        uint32_t upper_builtin_order{kInvalidPipelineHandleIndex};
    };

    mutable std::shared_mutex mutex;
    std::unordered_map<PhaseId, Record, StableIdHash<PhaseIdTag>> phases{};
    uint64_t revision{1};
};

namespace {
struct BuiltinPhaseInfo final {
    EnginePhase phase;
    const char* name;
};

inline constexpr std::array kBuiltinPhaseInfos{
    BuiltinPhaseInfo{EnginePhase::kStartup, "Startup"},
    BuiltinPhaseInfo{EnginePhase::kFrameBegin, "FrameBegin"},
    BuiltinPhaseInfo{EnginePhase::kInput, "Input"},
    BuiltinPhaseInfo{EnginePhase::kPreUpdate, "PreUpdate"},
    BuiltinPhaseInfo{EnginePhase::kFixedUpdate, "FixedUpdate"},
    BuiltinPhaseInfo{EnginePhase::kVariableUpdate, "VariableUpdate"},
    BuiltinPhaseInfo{EnginePhase::kWorldCommit, "WorldCommit"},
    BuiltinPhaseInfo{EnginePhase::kRenderExtract, "RenderExtract"},
    BuiltinPhaseInfo{EnginePhase::kRenderBuild, "RenderBuild"},
    BuiltinPhaseInfo{EnginePhase::kRenderExecute, "RenderExecute"},
    BuiltinPhaseInfo{EnginePhase::kPresent, "Present"},
    BuiltinPhaseInfo{EnginePhase::kFrameEnd, "FrameEnd"},
    BuiltinPhaseInfo{EnginePhase::kShutdown, "Shutdown"},
};

[[nodiscard]] uint64_t next_phase_revision(uint64_t revision) noexcept {
    return revision == std::numeric_limits<uint64_t>::max() ? 1 : revision + 1;
}
}  // namespace

PhaseRegistry::PhaseRegistry() : impl_(std::make_unique<Impl>()) {
    for (uint32_t index = 0; index < kBuiltinPhaseInfos.size(); ++index) {
        const BuiltinPhaseInfo& info = kBuiltinPhaseInfos[index];
        Impl::Record record{};
        record.name_storage = info.name;
        record.descriptor.id = PhaseId(info.name);
        record.descriptor.name = record.name_storage;
        record.descriptor.anchor_phase = {};
        record.descriptor.relation = PhaseOrderRelation::kAfter;
        record.builtin = true;
        record.builtin_order = index;
        const auto [iterator, inserted] = impl_->phases.emplace(record.descriptor.id, std::move(record));
        if (inserted) {
            iterator->second.descriptor.name = iterator->second.name_storage;
        }
    }
}

PhaseRegistry::~PhaseRegistry() = default;

PipelineRegistry::PipelineRegistry() = default;

PipelineRegistry::~PipelineRegistry() = default;

foundation::ResultValue<std::vector<PhaseId>> PhaseRegistry::ordered_phases_locked() const {
    if (impl_ == nullptr) {
        return {invalid_state("phase registry is not initialized"), {}};
    }

    try {
        std::vector<PhaseId> ids;
        ids.reserve(impl_->phases.size());
        for (const auto& [id, record] : impl_->phases) {
            static_cast<void>(record);
            ids.push_back(id);
        }

        const auto phase_less = [&](PhaseId left, PhaseId right) {
            const auto left_record = impl_->phases.find(left);
            const auto right_record = impl_->phases.find(right);
            if (left_record->second.builtin != right_record->second.builtin) {
                return left_record->second.builtin;
            }
            if (left_record->second.builtin &&
                left_record->second.builtin_order != right_record->second.builtin_order) {
                return left_record->second.builtin_order < right_record->second.builtin_order;
            }
            return left < right;
        };
        std::sort(ids.begin(), ids.end(), phase_less);

        std::unordered_map<PhaseId, uint32_t, StableIdHash<PhaseIdTag>> indices;
        indices.reserve(ids.size());
        for (uint32_t index = 0; index < ids.size(); ++index) {
            indices.emplace(ids[index], index);
        }

        std::vector<std::vector<uint32_t>> outgoing(ids.size());
        std::vector<uint32_t> indegree(ids.size(), 0);
        std::unordered_set<uint64_t> edge_set;
        edge_set.reserve(ids.size() * 2);
        const auto add_edge = [&](PhaseId source, PhaseId destination) {
            const auto source_iterator = indices.find(source);
            const auto destination_iterator = indices.find(destination);
            if (source_iterator == indices.end() || destination_iterator == indices.end() || source == destination) {
                return;
            }
            const uint64_t key = (static_cast<uint64_t>(source_iterator->second) << 32u) | destination_iterator->second;
            if (edge_set.insert(key).second) {
                outgoing[source_iterator->second].push_back(destination_iterator->second);
                ++indegree[destination_iterator->second];
            }
        };

        for (size_t index = 1; index < kBuiltinPhaseInfos.size(); ++index) {
            add_edge(PhaseId(kBuiltinPhaseInfos[index - 1].name), PhaseId(kBuiltinPhaseInfos[index].name));
        }

        for (const PhaseId id : ids) {
            const Impl::Record& record = impl_->phases.find(id)->second;
            if (record.builtin) {
                continue;
            }
            const PhaseId anchor = record.descriptor.anchor_phase;
            const auto anchor_iterator = impl_->phases.find(anchor);
            if (anchor_iterator == impl_->phases.end()) {
                return {not_found("phase anchor is no longer registered"), {}};
            }
            if (record.descriptor.relation == PhaseOrderRelation::kBefore) {
                add_edge(id, anchor);
            } else if (record.descriptor.relation == PhaseOrderRelation::kAfter) {
                add_edge(anchor, id);
            } else {
                return {invalid_argument("phase ordering relation is invalid"), {}};
            }

            if (record.lower_builtin_order != kInvalidPipelineHandleIndex) {
                add_edge(PhaseId(kBuiltinPhaseInfos[record.lower_builtin_order].name), id);
            }
            if (record.upper_builtin_order != kInvalidPipelineHandleIndex) {
                add_edge(id, PhaseId(kBuiltinPhaseInfos[record.upper_builtin_order].name));
            }
        }

        const auto ready_compare = [&](uint32_t left, uint32_t right) { return phase_less(ids[right], ids[left]); };
        std::priority_queue<uint32_t, std::vector<uint32_t>, decltype(ready_compare)> ready(ready_compare);
        for (uint32_t index = 0; index < indegree.size(); ++index) {
            if (indegree[index] == 0) {
                ready.push(index);
            }
        }

        std::vector<PhaseId> ordered;
        ordered.reserve(ids.size());
        while (!ready.empty()) {
            const uint32_t source = ready.top();
            ready.pop();
            ordered.push_back(ids[source]);
            for (const uint32_t destination : outgoing[source]) {
                if (--indegree[destination] == 0) {
                    ready.push(destination);
                }
            }
        }

        if (ordered.size() != ids.size()) {
            return {invalid_argument("phase registry contains a cyclic ordering"), {}};
        }
        return {foundation::Result::success(), std::move(ordered)};
    } catch (const std::bad_alloc&) {
        return {foundation::Result::failure(foundation::ResultCode::kOutOfMemory, "phase ordering allocation failed"),
                {}};
    }
}

foundation::Result PhaseRegistry::register_phase(const PhaseDescriptor& descriptor) {
    if (impl_ == nullptr) {
        return invalid_state("phase registry is not initialized");
    }
    if (!descriptor.id.is_valid() || descriptor.name.empty() || !descriptor.anchor_phase.is_valid() ||
        !is_valid_phase_order_relation(descriptor.relation)) {
        return invalid_argument("phase descriptor is invalid");
    }
    if (descriptor.id == descriptor.anchor_phase) {
        return invalid_argument("phase cannot be anchored to itself");
    }

    std::unique_lock lock(impl_->mutex);
    if (impl_->phases.find(descriptor.id) != impl_->phases.end()) {
        return invalid_state("phase is already registered");
    }
    if (impl_->phases.find(descriptor.anchor_phase) == impl_->phases.end()) {
        return not_found("phase anchor is not registered");
    }

    try {
        const Impl::Record& anchor = impl_->phases.find(descriptor.anchor_phase)->second;
        Impl::Record record{};
        record.descriptor = descriptor;
        record.name_storage.assign(descriptor.name.begin(), descriptor.name.end());
        record.descriptor.name = record.name_storage;
        if (anchor.builtin) {
            if (descriptor.relation == PhaseOrderRelation::kBefore) {
                record.upper_builtin_order = anchor.builtin_order;
                if (anchor.builtin_order > 0) {
                    record.lower_builtin_order = anchor.builtin_order - 1;
                }
            } else {
                record.lower_builtin_order = anchor.builtin_order;
                if (anchor.builtin_order + 1 < kBuiltinPhaseInfos.size()) {
                    record.upper_builtin_order = anchor.builtin_order + 1;
                }
            }
        } else {
            record.lower_builtin_order = anchor.lower_builtin_order;
            record.upper_builtin_order = anchor.upper_builtin_order;
        }
        const auto [iterator, inserted] = impl_->phases.emplace(descriptor.id, std::move(record));
        if (!inserted) {
            return invalid_state("phase is already registered");
        }
        iterator->second.descriptor.name = iterator->second.name_storage;

        const foundation::ResultValue<std::vector<PhaseId>> ordered = ordered_phases_locked();
        if (!ordered) {
            impl_->phases.erase(iterator);
            return ordered.result;
        }
        impl_->revision = next_phase_revision(impl_->revision);
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                           "phase registration allocation failed");
    }
}

foundation::Result PhaseRegistry::unregister_phase(PhaseId id) {
    if (impl_ == nullptr) {
        return invalid_state("phase registry is not initialized");
    }
    if (!id.is_valid()) {
        return invalid_argument("phase id is invalid");
    }

    std::unique_lock lock(impl_->mutex);
    const auto iterator = impl_->phases.find(id);
    if (iterator == impl_->phases.end()) {
        return not_found("phase is not registered");
    }
    if (iterator->second.builtin) {
        return unsupported("built-in phases cannot be unregistered");
    }
    for (const auto& [candidate_id, candidate] : impl_->phases) {
        static_cast<void>(candidate_id);
        if (!candidate.builtin && candidate.descriptor.anchor_phase == id) {
            return invalid_state("phase is used as another phase anchor");
        }
    }
    impl_->phases.erase(iterator);
    impl_->revision = next_phase_revision(impl_->revision);
    return foundation::Result::success();
}

bool PhaseRegistry::contains(PhaseId id) const noexcept {
    if (impl_ == nullptr || !id.is_valid()) {
        return false;
    }
    std::shared_lock lock(impl_->mutex);
    return impl_->phases.find(id) != impl_->phases.end();
}

foundation::ResultValue<PhaseDescriptor> PhaseRegistry::find_phase(PhaseId id) const {
    if (impl_ == nullptr) {
        return {invalid_state("phase registry is not initialized"), {}};
    }
    if (!id.is_valid()) {
        return {invalid_argument("phase id is invalid"), {}};
    }
    std::shared_lock lock(impl_->mutex);
    const auto iterator = impl_->phases.find(id);
    if (iterator == impl_->phases.end()) {
        return {not_found("phase is not registered"), {}};
    }
    PhaseDescriptor descriptor = iterator->second.descriptor;
    descriptor.name = iterator->second.name_storage;
    return {foundation::Result::success(), descriptor};
}

foundation::ResultValue<std::vector<PhaseId>> PhaseRegistry::ordered_phases() const {
    foundation::ResultValue<PhaseRegistrySnapshot> snapshot_result =
        detail::PhaseRegistryExecution::snapshot(*this);
    if (!snapshot_result) {
        return {snapshot_result.result, {}};
    }
    return {foundation::Result::success(), std::move(snapshot_result.value.ordered_phases)};
}

foundation::ResultValue<PhaseRegistrySnapshot> detail::PhaseRegistryExecution::snapshot(
    const PhaseRegistry& registry) {
    if (registry.impl_ == nullptr) {
        return {invalid_state("phase registry is not initialized"), {}};
    }
    std::shared_lock lock(registry.impl_->mutex);
    const foundation::ResultValue<std::vector<PhaseId>> ordered = registry.ordered_phases_locked();
    if (!ordered) {
        return {ordered.result, {}};
    }

    PhaseRegistrySnapshot result{};
    result.revision = registry.impl_->revision;
    result.ordered_phases = std::move(ordered.value);
    return {foundation::Result::success(), std::move(result)};
}

uint64_t detail::PhaseRegistryExecution::revision(const PhaseRegistry& registry) noexcept {
    if (registry.impl_ == nullptr) {
        return 0;
    }
    std::shared_lock lock(registry.impl_->mutex);
    return registry.impl_->revision;
}

struct ArtifactRuntimeEntry final {
    ArtifactDescriptor descriptor{};
    mutable std::shared_mutex mutex;
    std::atomic<uint32_t> active_writer_count{0};
    std::unordered_set<PlanRevision> plan_revisions{};
    bool has_manual_contract{false};
    std::shared_ptr<const void> owner{};
    const void* data{nullptr};
    std::size_t size{0};
    std::size_t alignment{alignof(std::max_align_t)};
    bool writable{false};
    bool has_value{false};
    ArtifactRevision revision{0};
};

struct ArtifactWriter::Impl final {
    std::shared_ptr<ArtifactRuntimeEntry> entry{};
    std::unique_lock<std::shared_mutex> lock{};
    void* data{nullptr};
    std::size_t size{0};
    std::size_t alignment{0};
    bool committed{false};
};

struct ArtifactRegistry::Impl final {
    explicit Impl(PipelineScope registry_scope) noexcept : scope(registry_scope) {}

    PipelineScope scope{PipelineScope::kEngine};
    mutable std::shared_mutex mutex;
    std::unordered_map<ArtifactId, std::shared_ptr<ArtifactRuntimeEntry>, StableIdHash<ArtifactIdTag>> entries{};
};

struct ArtifactScopeBridge::Impl final {
    Impl(PipelineScope bridge_source_scope, PipelineScope bridge_target_scope,
         const ArtifactRegistry& bridge_source_registry) noexcept
        : source_scope(bridge_source_scope),
          target_scope(bridge_target_scope),
          source_registry(&bridge_source_registry) {}

    PipelineScope source_scope{PipelineScope::kEngine};
    PipelineScope target_scope{PipelineScope::kFrame};
    const ArtifactRegistry* source_registry{nullptr};
    mutable std::shared_mutex mutex;
    std::vector<ArtifactId> exposed_artifacts{};
    std::unordered_map<ArtifactId, ArtifactView, StableIdHash<ArtifactIdTag>> snapshots{};
};

ArtifactWriter::ArtifactWriter(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

ArtifactWriter::~ArtifactWriter() {
    static_cast<void>(commit());
}

ArtifactWriter::ArtifactWriter(ArtifactWriter&& other) noexcept : impl_(std::move(other.impl_)) {}

ArtifactWriter& ArtifactWriter::operator=(ArtifactWriter&& other) noexcept {
    if (this != &other) {
        static_cast<void>(commit());
        impl_ = std::move(other.impl_);
    }
    return *this;
}

bool ArtifactWriter::is_valid() const noexcept {
    return impl_ != nullptr && impl_->entry != nullptr && !impl_->committed && impl_->lock.owns_lock();
}

ArtifactId ArtifactWriter::id() const noexcept {
    return impl_ == nullptr || impl_->entry == nullptr ? ArtifactId{} : impl_->entry->descriptor.id;
}

ArtifactTypeId ArtifactWriter::type() const noexcept {
    return impl_ == nullptr || impl_->entry == nullptr ? ArtifactTypeId{} : impl_->entry->descriptor.type;
}

PipelineScope ArtifactWriter::owner_scope() const noexcept {
    return impl_ == nullptr || impl_->entry == nullptr ? PipelineScope::kEngine : impl_->entry->descriptor.owner_scope;
}

ArtifactStorage ArtifactWriter::storage() const noexcept {
    return impl_ == nullptr || impl_->entry == nullptr ? ArtifactStorage::kValue : impl_->entry->descriptor.storage;
}

uint32_t ArtifactWriter::version() const noexcept {
    return impl_ == nullptr || impl_->entry == nullptr ? 0 : impl_->entry->descriptor.version;
}

ArtifactRevision ArtifactWriter::revision() const noexcept {
    return impl_ == nullptr || impl_->entry == nullptr ? 0 : impl_->entry->revision;
}

void* ArtifactWriter::data() const noexcept {
    return is_valid() ? impl_->data : nullptr;
}

std::size_t ArtifactWriter::size() const noexcept {
    return impl_ == nullptr ? 0 : impl_->size;
}

std::size_t ArtifactWriter::alignment() const noexcept {
    return impl_ == nullptr ? 0 : impl_->alignment;
}

foundation::Result ArtifactWriter::commit() {
    if (impl_ == nullptr) {
        return invalid_state("artifact writer is not initialized");
    }
    if (impl_->committed) {
        return invalid_state("artifact writer has already been committed");
    }
    if (!impl_->lock.owns_lock() || impl_->entry == nullptr) {
        return invalid_state("artifact writer lease is not held");
    }

    ++impl_->entry->revision;
    impl_->committed = true;
    impl_->lock.unlock();
    impl_->entry->active_writer_count.fetch_sub(1, std::memory_order_release);
    return foundation::Result::success();
}

ArtifactRegistry::ArtifactRegistry(PipelineScope scope) : impl_(std::make_unique<Impl>(scope)) {}

ArtifactRegistry::~ArtifactRegistry() = default;

PipelineScope ArtifactRegistry::scope() const noexcept {
    return impl_ == nullptr ? PipelineScope::kEngine : impl_->scope;
}

bool ArtifactRegistry::contains(ArtifactId id) const noexcept {
    if (impl_ == nullptr || !id.is_valid()) {
        return false;
    }
    std::shared_lock lock(impl_->mutex);
    return impl_->entries.find(id) != impl_->entries.end();
}

foundation::Result ArtifactRegistry::install_contract(const ArtifactDescriptor& descriptor) {
    if (impl_ == nullptr) {
        return invalid_state("artifact registry is not initialized");
    }
    if (const foundation::Result result = validate_artifact_descriptor(descriptor); !result) {
        return result;
    }

    std::unique_lock lock(impl_->mutex);
    const auto iterator = impl_->entries.find(descriptor.id);
    if (iterator != impl_->entries.end()) {
        if (!descriptors_match(iterator->second->descriptor, descriptor)) {
            return invalid_state("artifact contract conflicts with an installed contract");
        }
        iterator->second->has_manual_contract = true;
        return foundation::Result::success();
    }

    auto entry = std::make_shared<ArtifactRuntimeEntry>();
    entry->descriptor = descriptor;
    entry->has_manual_contract = true;
    impl_->entries.emplace(descriptor.id, std::move(entry));
    return foundation::Result::success();
}

foundation::Result ArtifactRegistry::install_plan(const PipelinePlan& plan) {
    if (impl_ == nullptr) {
        return invalid_state("artifact registry is not initialized");
    }
    if (plan.revision == 0 || !plan.pipeline.is_valid()) {
        return invalid_argument("pipeline plan is invalid");
    }
    if (plan.scope != impl_->scope) {
        return invalid_state("artifact registry scope does not match pipeline plan scope");
    }
    PlanArtifactIndex artifact_index;
    if (const foundation::Result result = build_plan_artifact_index(plan, artifact_index); !result) {
        return result;
    }

    std::unique_lock lock(impl_->mutex);
    for (const ArtifactDescriptor& descriptor : plan.artifacts) {
        const auto iterator = impl_->entries.find(descriptor.id);
        if (iterator != impl_->entries.end() && !descriptors_match(iterator->second->descriptor, descriptor)) {
            return invalid_state("pipeline plan artifact conflicts with an installed contract");
        }
    }
    for (const ArtifactDescriptor& descriptor : plan.artifacts) {
        const auto iterator = impl_->entries.find(descriptor.id);
        if (iterator == impl_->entries.end()) {
            auto entry = std::make_shared<ArtifactRuntimeEntry>();
            entry->descriptor = descriptor;
            entry->plan_revisions.insert(plan.revision);
            impl_->entries.emplace(descriptor.id, std::move(entry));
            continue;
        }
        iterator->second->plan_revisions.insert(plan.revision);
    }
    return foundation::Result::success();
}

foundation::Result ArtifactRegistry::replace_plan(const PipelinePlan& previous, const PipelinePlan& next) {
    if (impl_ == nullptr) {
        return invalid_state("artifact registry is not initialized");
    }
    if (previous.revision == 0 || next.revision == 0 || !previous.pipeline.is_valid() || !next.pipeline.is_valid()) {
        return invalid_argument("pipeline plan is invalid");
    }
    if (previous.scope != impl_->scope || next.scope != impl_->scope) {
        return invalid_state("artifact registry scope does not match pipeline plan scope");
    }
    if (previous.pipeline != next.pipeline) {
        return invalid_argument("pipeline plan replacement must keep the same pipeline");
    }
    if (previous.revision == next.revision) {
        return foundation::Result::success();
    }

    PlanArtifactIndex previous_artifacts;
    if (const foundation::Result result = build_plan_artifact_index(previous, previous_artifacts); !result) {
        return result;
    }
    PlanArtifactIndex next_artifacts;
    if (const foundation::Result result = build_plan_artifact_index(next, next_artifacts); !result) {
        return result;
    }

    struct StagedEntry final {
        ArtifactId id{};
        std::shared_ptr<ArtifactRuntimeEntry> entry{};
    };
    std::vector<StagedEntry> staged_entries;

    std::unique_lock lock(impl_->mutex);
    for (const auto& [id, descriptor] : previous_artifacts) {
        const auto iterator = impl_->entries.find(id);
        if (iterator == impl_->entries.end() ||
            iterator->second->plan_revisions.find(previous.revision) == iterator->second->plan_revisions.end() ||
            !descriptors_match(iterator->second->descriptor, *descriptor)) {
            return invalid_state("previous pipeline plan is not installed in the artifact registry");
        }
    }

    for (const auto& [id, descriptor] : next_artifacts) {
        const auto iterator = impl_->entries.find(id);
        if (iterator == impl_->entries.end()) {
            continue;
        }
        ArtifactRuntimeEntry& entry = *iterator->second;
        if (descriptors_match(entry.descriptor, *descriptor)) {
            if (entry.plan_revisions.find(next.revision) == entry.plan_revisions.end()) {
                entry.plan_revisions.reserve(entry.plan_revisions.size() + 1);
            }
            continue;
        }

        const auto previous_iterator = previous_artifacts.find(id);
        if (previous_iterator == previous_artifacts.end() ||
            !descriptors_match(entry.descriptor, *previous_iterator->second) || entry.plan_revisions.size() != 1 ||
            entry.plan_revisions.find(previous.revision) == entry.plan_revisions.end() || entry.has_manual_contract) {
            return invalid_state("next pipeline plan artifact conflicts with an installed contract");
        }
        if (iterator->second->active_writer_count.load(std::memory_order_acquire) != 0) {
            return invalid_state("pipeline plan replacement is blocked by an artifact runtime lease");
        }

        auto replacement = std::make_shared<ArtifactRuntimeEntry>();
        replacement->descriptor = *descriptor;
        replacement->plan_revisions.reserve(1);
        replacement->plan_revisions.insert(next.revision);
        staged_entries.push_back(StagedEntry{id, std::move(replacement)});
    }

    for (const auto& [id, descriptor] : previous_artifacts) {
        const auto next_iterator = next_artifacts.find(id);
        if (next_iterator != next_artifacts.end() && descriptors_match(*descriptor, *next_iterator->second)) {
            continue;
        }
        const auto iterator = impl_->entries.find(id);
        if (iterator == impl_->entries.end()) {
            continue;
        }
        ArtifactRuntimeEntry& entry = *iterator->second;
        if (entry.plan_revisions.size() == 1 && !entry.has_manual_contract &&
            entry.active_writer_count.load(std::memory_order_acquire) != 0) {
            return invalid_state("pipeline plan retirement is blocked by an artifact runtime lease");
        }
    }

    try {
        impl_->entries.reserve(impl_->entries.size() + staged_entries.size());
    } catch (const std::bad_alloc&) {
        return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                           "artifact plan replacement allocation failed");
    }

    for (const auto& [id, descriptor] : previous_artifacts) {
        const auto iterator = impl_->entries.find(id);
        if (iterator == impl_->entries.end()) {
            continue;
        }
        ArtifactRuntimeEntry& entry = *iterator->second;
        entry.plan_revisions.erase(previous.revision);
        const auto next_iterator = next_artifacts.find(id);
        const bool descriptor_preserved =
            next_iterator != next_artifacts.end() && descriptors_match(*descriptor, *next_iterator->second);
        if (entry.plan_revisions.empty() && !entry.has_manual_contract && !descriptor_preserved) {
            impl_->entries.erase(iterator);
        }
    }

    for (const auto& [id, descriptor] : next_artifacts) {
        const auto staged = std::find_if(staged_entries.begin(), staged_entries.end(),
                                         [id](const StagedEntry& candidate) { return candidate.id == id; });
        if (staged != staged_entries.end()) {
            impl_->entries.emplace(id, staged->entry);
            continue;
        }
        const auto iterator = impl_->entries.find(id);
        if (iterator == impl_->entries.end()) {
            auto entry = std::make_shared<ArtifactRuntimeEntry>();
            entry->descriptor = *descriptor;
            entry->plan_revisions.insert(next.revision);
            impl_->entries.emplace(id, std::move(entry));
            continue;
        }
        iterator->second->plan_revisions.insert(next.revision);
    }
    return foundation::Result::success();
}

foundation::Result ArtifactRegistry::retire_plan(const PipelinePlan& plan) {
    if (impl_ == nullptr) {
        return invalid_state("artifact registry is not initialized");
    }
    if (plan.revision == 0 || !plan.pipeline.is_valid()) {
        return invalid_argument("pipeline plan is invalid");
    }
    if (plan.scope != impl_->scope) {
        return invalid_state("artifact registry scope does not match pipeline plan scope");
    }
    PlanArtifactIndex artifact_index;
    if (const foundation::Result result = build_plan_artifact_index(plan, artifact_index); !result) {
        return result;
    }

    std::unique_lock lock(impl_->mutex);
    for (const auto& [id, descriptor] : artifact_index) {
        const auto iterator = impl_->entries.find(id);
        if (iterator == impl_->entries.end() ||
            iterator->second->plan_revisions.find(plan.revision) == iterator->second->plan_revisions.end()) {
            continue;
        }
        ArtifactRuntimeEntry& entry = *iterator->second;
        if (!descriptors_match(entry.descriptor, *descriptor)) {
            return invalid_state("retired pipeline plan artifact conflicts with its installed contract");
        }
        if (entry.plan_revisions.size() == 1 && !entry.has_manual_contract &&
            entry.active_writer_count.load(std::memory_order_acquire) != 0) {
            return invalid_state("pipeline plan retirement is blocked by an artifact runtime lease");
        }
    }

    for (const auto& [id, descriptor] : artifact_index) {
        static_cast<void>(descriptor);
        const auto iterator = impl_->entries.find(id);
        if (iterator == impl_->entries.end()) {
            continue;
        }
        ArtifactRuntimeEntry& entry = *iterator->second;
        if (entry.plan_revisions.erase(plan.revision) == 0) {
            continue;
        }
        if (entry.plan_revisions.empty() && !entry.has_manual_contract &&
            entry.active_writer_count.load(std::memory_order_acquire) == 0) {
            impl_->entries.erase(iterator);
        }
    }
    return foundation::Result::success();
}

foundation::Result ArtifactRegistry::unregister_artifact(ArtifactId id) {
    if (impl_ == nullptr) {
        return invalid_state("artifact registry is not initialized");
    }
    if (!id.is_valid()) {
        return invalid_argument("artifact id is invalid");
    }
    std::unique_lock lock(impl_->mutex);
    const auto iterator = impl_->entries.find(id);
    if (iterator == impl_->entries.end()) {
        return not_found("artifact contract is not installed");
    }
    if (!iterator->second->plan_revisions.empty()) {
        return invalid_state("artifact contract is still referenced by an installed pipeline plan");
    }
    if (iterator->second->active_writer_count.load(std::memory_order_acquire) != 0) {
        return invalid_state("artifact is still referenced by a runtime lease");
    }
    impl_->entries.erase(iterator);
    return foundation::Result::success();
}

foundation::ResultValue<ArtifactDescriptor> ArtifactRegistry::find_descriptor(ArtifactId id) const {
    if (impl_ == nullptr) {
        return {invalid_state("artifact registry is not initialized"), {}};
    }
    if (!id.is_valid()) {
        return {invalid_argument("artifact id is invalid"), {}};
    }
    std::shared_lock lock(impl_->mutex);
    const auto iterator = impl_->entries.find(id);
    if (iterator == impl_->entries.end()) {
        return {not_found("artifact contract is not installed"), {}};
    }
    return {foundation::Result::success(), iterator->second->descriptor};
}

foundation::ResultValue<ArtifactView> ArtifactRegistry::read(ArtifactId id, ArtifactTypeId expected_type,
                                                             uint32_t required_version) const {
    if (impl_ == nullptr) {
        return {invalid_state("artifact registry is not initialized"), {}};
    }
    if (!id.is_valid()) {
        return {invalid_argument("artifact id is invalid"), {}};
    }

    std::shared_ptr<ArtifactRuntimeEntry> entry;
    {
        std::shared_lock registry_lock(impl_->mutex);
        const auto iterator = impl_->entries.find(id);
        if (iterator == impl_->entries.end()) {
            return {not_found("artifact contract is not installed"), {}};
        }
        entry = iterator->second;
    }

    std::shared_lock lock(entry->mutex);
    if (expected_type.is_valid() && expected_type != entry->descriptor.type) {
        return {invalid_argument("artifact type does not match its contract"), {}};
    }
    if (required_version != 0 && required_version != entry->descriptor.version) {
        return {invalid_argument("artifact version does not match its contract"), {}};
    }
    if (!entry->has_value) {
        return {
            foundation::Result::failure(foundation::ResultCode::kNotReady, "artifact has no published runtime value"),
            {}};
    }
    return {foundation::Result::success(),
            ArtifactView(entry->descriptor.id, entry->descriptor.type, entry->descriptor.owner_scope,
                         entry->descriptor.storage, entry->descriptor.version, entry->revision, entry->data,
                         entry->size, entry->alignment, entry->owner)};
}

foundation::ResultValue<ArtifactWriter> ArtifactRegistry::write(ArtifactId id, ArtifactTypeId expected_type,
                                                                uint32_t required_version) {
    if (impl_ == nullptr) {
        return {invalid_state("artifact registry is not initialized"), {}};
    }
    if (!id.is_valid()) {
        return {invalid_argument("artifact id is invalid"), {}};
    }

    std::shared_ptr<ArtifactRuntimeEntry> entry;
    {
        std::shared_lock registry_lock(impl_->mutex);
        const auto iterator = impl_->entries.find(id);
        if (iterator == impl_->entries.end()) {
            return {not_found("artifact contract is not installed"), {}};
        }
        entry = iterator->second;
    }

    if (entry->descriptor.owner_scope != impl_->scope) {
        return {unsupported("only the artifact owner scope may write the artifact"), {}};
    }
    if (entry->descriptor.storage == ArtifactStorage::kImmutableSnapshot ||
        entry->descriptor.ownership == OwnershipPolicy::kSharedImmutable) {
        return {unsupported("immutable artifact cannot be written through a writer lease"), {}};
    }

    std::unique_lock lock(entry->mutex);
    if (expected_type.is_valid() && expected_type != entry->descriptor.type) {
        return {invalid_argument("artifact type does not match its contract"), {}};
    }
    if (required_version != 0 && required_version != entry->descriptor.version) {
        return {invalid_argument("artifact version does not match its contract"), {}};
    }
    if (!entry->has_value) {
        return {
            foundation::Result::failure(foundation::ResultCode::kNotReady, "artifact has no writable runtime value"),
            {}};
    }
    if (!entry->writable) {
        return {unsupported("artifact payload is immutable"), {}};
    }

    auto writer = std::make_unique<ArtifactWriter::Impl>();
    writer->entry = std::move(entry);
    writer->lock = std::move(lock);
    writer->data = const_cast<void*>(writer->entry->data);
    writer->size = writer->entry->size;
    writer->alignment = writer->entry->alignment;
    writer->entry->active_writer_count.fetch_add(1, std::memory_order_relaxed);
    return {foundation::Result::success(), ArtifactWriter(std::move(writer))};
}

foundation::Result ArtifactRegistry::publish(ArtifactId id, ArtifactPayload payload) {
    if (impl_ == nullptr) {
        return invalid_state("artifact registry is not initialized");
    }
    if (!id.is_valid()) {
        return invalid_argument("artifact id is invalid");
    }
    if (!payload.type.is_valid()) {
        return invalid_argument("artifact payload type is invalid");
    }
    if (payload.size != 0 && payload.data == nullptr) {
        return invalid_argument("artifact payload data is null");
    }
    if (payload.alignment == 0) {
        return invalid_argument("artifact payload alignment is zero");
    }

    std::shared_ptr<ArtifactRuntimeEntry> entry;
    {
        std::shared_lock registry_lock(impl_->mutex);
        const auto iterator = impl_->entries.find(id);
        if (iterator == impl_->entries.end()) {
            return not_found("artifact contract is not installed");
        }
        entry = iterator->second;
    }
    if (entry->descriptor.owner_scope != impl_->scope) {
        return unsupported("only the artifact owner scope may publish the artifact");
    }
    if (payload.type != entry->descriptor.type) {
        return invalid_argument("artifact payload type does not match its contract");
    }
    if ((entry->descriptor.storage == ArtifactStorage::kImmutableSnapshot ||
         entry->descriptor.ownership == OwnershipPolicy::kSharedImmutable) &&
        payload.writable) {
        return unsupported("immutable artifact payload cannot be published as writable");
    }

    std::unique_lock lock(entry->mutex);
    entry->owner = std::move(payload.owner);
    entry->data = payload.data;
    entry->size = payload.size;
    entry->alignment = payload.alignment;
    entry->writable = payload.writable;
    entry->has_value = true;
    ++entry->revision;
    return foundation::Result::success();
}

foundation::Result ArtifactRegistry::release_lifetime(ArtifactLifetime lifetime) {
    if (impl_ == nullptr) {
        return invalid_state("artifact registry is not initialized");
    }
    if (!is_valid_artifact_lifetime(lifetime)) {
        return invalid_argument("artifact lifetime is invalid");
    }

    std::vector<std::shared_ptr<ArtifactRuntimeEntry>> entries;
    {
        std::shared_lock registry_lock(impl_->mutex);
        entries.reserve(impl_->entries.size());
        for (const auto& [id, entry] : impl_->entries) {
            static_cast<void>(id);
            if (static_cast<uint8_t>(entry->descriptor.lifetime) <= static_cast<uint8_t>(lifetime)) {
                entries.push_back(entry);
            }
        }
    }
    for (const std::shared_ptr<ArtifactRuntimeEntry>& entry : entries) {
        std::unique_lock lock(entry->mutex);
        if (!entry->has_value) {
            continue;
        }
        entry->owner.reset();
        entry->data = nullptr;
        entry->size = 0;
        entry->alignment = alignof(std::max_align_t);
        entry->writable = false;
        entry->has_value = false;
        ++entry->revision;
    }
    return foundation::Result::success();
}

ArtifactScopeBridge::ArtifactScopeBridge(PipelineScope source_scope, PipelineScope target_scope,
                                         const ArtifactRegistry& source_registry)
    : impl_(std::make_unique<Impl>(source_scope, target_scope, source_registry)) {}

ArtifactScopeBridge::~ArtifactScopeBridge() = default;

PipelineScope ArtifactScopeBridge::source_scope() const noexcept {
    return impl_ == nullptr ? PipelineScope::kEngine : impl_->source_scope;
}

PipelineScope ArtifactScopeBridge::target_scope() const noexcept {
    return impl_ == nullptr ? PipelineScope::kFrame : impl_->target_scope;
}

bool ArtifactScopeBridge::is_bound_to(const ArtifactRegistry& source_registry) const noexcept {
    return impl_ != nullptr && impl_->source_registry == &source_registry;
}

foundation::Result ArtifactScopeBridge::expose(ArtifactId id) {
    if (impl_ == nullptr || impl_->source_registry == nullptr) {
        return invalid_state("artifact scope bridge is not initialized");
    }
    if (!is_valid_scope(impl_->source_scope) || !is_valid_scope(impl_->target_scope) ||
        impl_->source_registry->scope() != impl_->source_scope ||
        !can_scope_read(impl_->target_scope, impl_->source_scope) || impl_->target_scope == impl_->source_scope) {
        return invalid_argument("artifact scope bridge direction is invalid");
    }

    const foundation::ResultValue<ArtifactDescriptor> descriptor = impl_->source_registry->find_descriptor(id);
    if (!descriptor) {
        return descriptor.result;
    }
    if (descriptor.value.owner_scope != impl_->source_scope) {
        return unsupported("scope bridge can only expose source-owned artifacts");
    }
    if (descriptor.value.storage != ArtifactStorage::kImmutableSnapshot) {
        return unsupported("scope bridge requires immutable snapshot artifacts");
    }

    std::unique_lock lock(impl_->mutex);
    if (std::find(impl_->exposed_artifacts.begin(), impl_->exposed_artifacts.end(), id) ==
        impl_->exposed_artifacts.end()) {
        impl_->exposed_artifacts.push_back(id);
    }
    return foundation::Result::success();
}

foundation::Result ArtifactScopeBridge::capture() {
    if (impl_ == nullptr || impl_->source_registry == nullptr) {
        return invalid_state("artifact scope bridge is not initialized");
    }

    std::vector<ArtifactId> exposed_artifacts;
    {
        std::shared_lock lock(impl_->mutex);
        exposed_artifacts = impl_->exposed_artifacts;
    }

    std::unordered_map<ArtifactId, ArtifactView, StableIdHash<ArtifactIdTag>> snapshots;
    snapshots.reserve(exposed_artifacts.size());
    for (const ArtifactId id : exposed_artifacts) {
        const foundation::ResultValue<ArtifactView> value = impl_->source_registry->read(id);
        if (!value) {
            return foundation::Result::failure(value.result.code,
                                               append_message("scope bridge snapshot capture failed", value.result));
        }
        if (value.value.owner_scope() != impl_->source_scope ||
            value.value.storage() != ArtifactStorage::kImmutableSnapshot) {
            return unsupported("scope bridge captured a non-source immutable artifact");
        }
        snapshots.emplace(id, value.value);
    }

    {
        std::unique_lock lock(impl_->mutex);
        impl_->snapshots = std::move(snapshots);
    }
    return foundation::Result::success();
}

foundation::ResultValue<ArtifactView> ArtifactScopeBridge::read(ArtifactId id, ArtifactTypeId expected_type,
                                                                uint32_t required_version) const {
    if (impl_ == nullptr) {
        return {invalid_state("artifact scope bridge is not initialized"), {}};
    }
    if (!id.is_valid()) {
        return {invalid_argument("artifact id is invalid"), {}};
    }

    std::shared_lock lock(impl_->mutex);
    const auto iterator = impl_->snapshots.find(id);
    if (iterator == impl_->snapshots.end()) {
        return {foundation::Result::failure(foundation::ResultCode::kNotReady,
                                            "scope bridge has no captured value for the artifact"),
                {}};
    }
    const ArtifactView& value = iterator->second;
    if (expected_type.is_valid() && expected_type != value.type()) {
        return {invalid_argument("bridged artifact type does not match its contract"), {}};
    }
    if (required_version != 0 && required_version != value.version()) {
        return {invalid_argument("bridged artifact version does not match its contract"), {}};
    }
    return {foundation::Result::success(), value};
}

foundation::Result PipelineExecutionContext::begin_plan(const PipelinePlan& plan) noexcept {
    if (active_plan_ != nullptr) {
        return invalid_state("pipeline execution context is already executing a plan");
    }
    if (plan.revision == 0 || !plan.pipeline.is_valid()) {
        return invalid_state("pipeline plan is invalid");
    }
    if (plan.scope != scope_) {
        return invalid_state("pipeline plan scope does not match execution context scope");
    }
    active_plan_ = &plan;
    return foundation::Result::success();
}

void PipelineExecutionContext::end_plan() noexcept {
    active_plan_ = nullptr;
}

PipelineBuilder::PipelineBuilder(const PipelineDescriptor& descriptor) : descriptor_(descriptor) {}

void PipelineBuilder::set_failure(foundation::ResultCode code, std::string message) {
    if (status_) {
        status_ = foundation::Result::failure(code, std::move(message));
    }
}

bool PipelineBuilder::validate_stage(PipelineStageHandle stage) const noexcept {
    return stage.is_valid() && stage.generation == kBuilderGeneration && stage.index < stages_.size();
}

bool PipelineBuilder::validate_artifact(PipelineArtifactHandle artifact) const noexcept {
    return artifact.is_valid() && artifact.generation == kBuilderGeneration && artifact.index < artifacts_.size();
}

uint32_t PipelineBuilder::find_stage(StageId id) const noexcept {
    for (uint32_t index = 0; index < stages_.size(); ++index) {
        if (stages_[index].descriptor.id == id) {
            return index;
        }
    }
    return kInvalidPipelineHandleIndex;
}

uint32_t PipelineBuilder::find_artifact(ArtifactId id) const noexcept {
    for (uint32_t index = 0; index < artifacts_.size(); ++index) {
        if (artifacts_[index].descriptor.id == id) {
            return index;
        }
    }
    return kInvalidPipelineHandleIndex;
}

PipelineStageHandle PipelineBuilder::add_stage(const StageDescriptor& descriptor) {
    if (!status_) {
        return {};
    }
    if (!descriptor.id.is_valid()) {
        set_failure(foundation::ResultCode::kInvalidArgument, "stage id is invalid");
        return {};
    }
    if (!is_valid_engine_phase(descriptor.phase)) {
        set_failure(foundation::ResultCode::kInvalidArgument, "stage compatibility phase is invalid");
        return {};
    }
    if (!is_valid_execution_domain(descriptor.execution_domain)) {
        set_failure(foundation::ResultCode::kInvalidArgument, "stage execution domain is invalid");
        return {};
    }
    if (!is_valid_time_policy(descriptor.time_policy)) {
        set_failure(foundation::ResultCode::kInvalidArgument, "stage time policy is invalid");
        return {};
    }
    if (find_stage(descriptor.id) != kInvalidPipelineHandleIndex) {
        set_failure(foundation::ResultCode::kInvalidArgument,
                    "duplicate stage id=" + std::to_string(descriptor.id.value()));
        return {};
    }

    const uint32_t index = static_cast<uint32_t>(stages_.size());
    StageDescriptor normalized = descriptor;
    if (!normalized.phase_id.is_valid()) {
        normalized.phase_id = to_phase_id(normalized.phase);
    }
    stages_.push_back(PipelineStageRecord{normalized});
    return PipelineStageHandle{index, kBuilderGeneration};
}

PipelineArtifactHandle PipelineBuilder::declare_artifact(const ArtifactDescriptor& descriptor) {
    if (!status_) {
        return {};
    }
    if (!descriptor.id.is_valid() || !descriptor.type.is_valid()) {
        set_failure(foundation::ResultCode::kInvalidArgument, "artifact id or type is invalid");
        return {};
    }
    if (descriptor.version == 0) {
        set_failure(foundation::ResultCode::kInvalidArgument, "artifact version must be non-zero");
        return {};
    }
    if (!is_valid_scope(descriptor.owner_scope)) {
        set_failure(foundation::ResultCode::kInvalidArgument, "artifact owner scope is invalid");
        return {};
    }
    if (!is_valid_artifact_storage(descriptor.storage) || !is_valid_artifact_lifetime(descriptor.lifetime) ||
        !is_valid_ownership_policy(descriptor.ownership)) {
        set_failure(foundation::ResultCode::kInvalidArgument,
                    "artifact storage, lifetime, or ownership policy is invalid");
        return {};
    }
    if ((descriptor.visible_to & ~kAllPipelineScopes) != 0) {
        set_failure(foundation::ResultCode::kInvalidArgument, "artifact visibility contains unknown scopes");
        return {};
    }
    if ((descriptor.visible_to & ((1u << static_cast<uint8_t>(descriptor.owner_scope)) - 1u)) != 0) {
        set_failure(foundation::ResultCode::kInvalidArgument,
                    "artifact visibility cannot include a lower pipeline scope");
        return {};
    }
    if (find_artifact(descriptor.id) != kInvalidPipelineHandleIndex) {
        set_failure(foundation::ResultCode::kInvalidArgument,
                    "duplicate artifact id=" + std::to_string(descriptor.id.value()));
        return {};
    }

    const uint32_t index = static_cast<uint32_t>(artifacts_.size());
    artifacts_.push_back(PipelineArtifactRecord{descriptor});
    return PipelineArtifactHandle{index, kBuilderGeneration};
}

foundation::Result PipelineBuilder::add_binding(PipelineStageHandle stage, const ArtifactBinding& binding,
                                                ArtifactAccess expected_access) {
    if (!status_) {
        return status_;
    }
    if (!validate_stage(stage)) {
        set_failure(foundation::ResultCode::kInvalidArgument, "stage handle is invalid");
        return status_;
    }
    if (binding.access != expected_access) {
        set_failure(foundation::ResultCode::kInvalidArgument, "artifact access does not match builder method");
        return status_;
    }
    if (!binding.artifact.is_valid()) {
        set_failure(foundation::ResultCode::kInvalidArgument, "artifact binding id is invalid");
        return status_;
    }
    const uint32_t artifact_index = find_artifact(binding.artifact);
    if (artifact_index == kInvalidPipelineHandleIndex) {
        set_failure(foundation::ResultCode::kNotFound, "artifact binding references an undeclared artifact");
        return status_;
    }
    if (binding.required_version == 0 || binding.required_version != artifacts_[artifact_index].descriptor.version) {
        set_failure(foundation::ResultCode::kInvalidArgument,
                    "artifact binding version does not match its declaration");
        return status_;
    }
    if ((static_cast<uint32_t>(binding.flags) & ~kKnownAccessFlags) != 0) {
        set_failure(foundation::ResultCode::kInvalidArgument, "artifact access flags contain unknown bits");
        return status_;
    }
    if (has_flag(binding.flags, AccessFlags::kPreserveContents) &&
        has_flag(binding.flags, AccessFlags::kDiscardContents)) {
        set_failure(foundation::ResultCode::kInvalidArgument,
                    "preserve-contents and discard-contents are mutually exclusive");
        return status_;
    }
    if (has_flag(binding.flags, AccessFlags::kOptional) && expected_access != ArtifactAccess::kRead &&
        expected_access != ArtifactAccess::kConsume) {
        set_failure(foundation::ResultCode::kInvalidArgument, "optional access is only valid for read or consume");
        return status_;
    }
    if (has_flag(binding.flags, AccessFlags::kDeterministicAppend) && expected_access != ArtifactAccess::kAppend) {
        set_failure(foundation::ResultCode::kInvalidArgument, "deterministic append is only valid for append access");
        return status_;
    }
    if (has_flag(binding.flags, AccessFlags::kDiscardContents) && expected_access != ArtifactAccess::kWrite &&
        expected_access != ArtifactAccess::kReadWrite) {
        set_failure(foundation::ResultCode::kInvalidArgument,
                    "discard-contents is only valid for write or read/write access");
        return status_;
    }

    for (PipelineBindingRecord& existing : bindings_) {
        if (existing.stage_index == stage.index && existing.binding.artifact == binding.artifact) {
            if (existing.binding.access != binding.access ||
                existing.binding.required_version != binding.required_version) {
                set_failure(foundation::ResultCode::kInvalidArgument,
                            "stage declares conflicting access for one artifact");
                return status_;
            }
            const AccessFlags merged_flags = existing.binding.flags | binding.flags;
            if (has_flag(merged_flags, AccessFlags::kPreserveContents) &&
                has_flag(merged_flags, AccessFlags::kDiscardContents)) {
                set_failure(foundation::ResultCode::kInvalidArgument,
                            "merged artifact access flags are mutually exclusive");
                return status_;
            }
            existing.binding.flags = merged_flags;
            return foundation::Result::success();
        }
    }

    bindings_.push_back(PipelineBindingRecord{stage.index, binding});
    return foundation::Result::success();
}

foundation::Result PipelineBuilder::reads(PipelineStageHandle stage, const ArtifactBinding& binding) {
    return add_binding(stage, binding, ArtifactAccess::kRead);
}

foundation::Result PipelineBuilder::writes(PipelineStageHandle stage, const ArtifactBinding& binding) {
    return add_binding(stage, binding, ArtifactAccess::kWrite);
}

foundation::Result PipelineBuilder::reads_writes(PipelineStageHandle stage, const ArtifactBinding& binding) {
    return add_binding(stage, binding, ArtifactAccess::kReadWrite);
}

foundation::Result PipelineBuilder::publish_snapshot(PipelineStageHandle stage, const ArtifactBinding& binding) {
    return add_binding(stage, binding, ArtifactAccess::kPublishSnapshot);
}

foundation::Result PipelineBuilder::runs_before(PipelineStageHandle stage, StageId other) {
    if (!status_) {
        return status_;
    }
    if (!validate_stage(stage) || !other.is_valid()) {
        set_failure(foundation::ResultCode::kInvalidArgument, "ordering relation contains an invalid stage");
        return status_;
    }
    if (stages_[stage.index].descriptor.id == other) {
        set_failure(foundation::ResultCode::kInvalidArgument, "stage cannot run before itself");
        return status_;
    }
    orderings_.push_back(PipelineOrderingRecord{stage.index, other, true});
    return foundation::Result::success();
}

foundation::Result PipelineBuilder::runs_after(PipelineStageHandle stage, StageId other) {
    if (!status_) {
        return status_;
    }
    if (!validate_stage(stage) || !other.is_valid()) {
        set_failure(foundation::ResultCode::kInvalidArgument, "ordering relation contains an invalid stage");
        return status_;
    }
    if (stages_[stage.index].descriptor.id == other) {
        set_failure(foundation::ResultCode::kInvalidArgument, "stage cannot run after itself");
        return status_;
    }
    orderings_.push_back(PipelineOrderingRecord{stage.index, other, false});
    return foundation::Result::success();
}

foundation::Result PipelineBuilder::expose_artifact(ArtifactId artifact, PipelineScope target_scope) {
    if (!status_) {
        return status_;
    }
    if (!artifact.is_valid() || find_artifact(artifact) == kInvalidPipelineHandleIndex) {
        set_failure(foundation::ResultCode::kNotFound, "exposed artifact is not declared");
        return status_;
    }
    if (!is_valid_scope(target_scope)) {
        set_failure(foundation::ResultCode::kInvalidArgument, "exposure target scope is invalid");
        return status_;
    }
    const ArtifactDescriptor& descriptor = artifacts_[find_artifact(artifact)].descriptor;
    if (!can_scope_read(target_scope, descriptor.owner_scope) || target_scope == descriptor.owner_scope) {
        set_failure(foundation::ResultCode::kInvalidArgument,
                    "artifact exposure must point to a higher pipeline scope");
        return status_;
    }
    for (const PipelineExposureRecord& existing : exposures_) {
        if (existing.artifact == artifact && existing.target_scope == target_scope) {
            return foundation::Result::success();
        }
    }
    exposures_.push_back(PipelineExposureRecord{artifact, target_scope});
    return foundation::Result::success();
}

foundation::ResultValue<PipelineDefinition> PipelineBuilder::finish() {
    if (finished_) {
        return {invalid_state("pipeline builder has already been finalized"), {}};
    }
    finished_ = true;

    if (!status_) {
        return {status_, {}};
    }
    if (!descriptor_.id.is_valid() || descriptor_.name.empty()) {
        return {invalid_argument("pipeline id or name is invalid"), {}};
    }
    if (!descriptor_.version.is_valid()) {
        return {invalid_argument("pipeline version is invalid"), {}};
    }
    if (descriptor_.supported_scopes == 0 || (descriptor_.supported_scopes & ~kAllPipelineScopes) != 0) {
        return {invalid_argument("pipeline supported scopes are invalid"), {}};
    }
    if (stages_.empty()) {
        return {invalid_argument("pipeline must declare at least one stage"), {}};
    }
    for (const PipelineOrderingRecord& ordering : orderings_) {
        if (ordering.source_stage_index >= stages_.size() ||
            find_stage(ordering.target_stage) == kInvalidPipelineHandleIndex) {
            return {not_found("ordering relation references an undeclared stage"), {}};
        }
    }
    for (const PipelineArtifactRecord& artifact : artifacts_) {
        const ArtifactDescriptor& descriptor = artifact.descriptor;
        if (descriptor.visible_to != 0 &&
            (descriptor.visible_to & ((1u << static_cast<uint8_t>(descriptor.owner_scope)) - 1u)) != 0) {
            return {invalid_argument("artifact visibility includes a lower pipeline scope"), {}};
        }
    }
    for (const PipelineBindingRecord& binding : bindings_) {
        if (binding.stage_index >= stages_.size()) {
            return {invalid_state("artifact binding references an invalid stage"), {}};
        }
        const uint32_t artifact_index = find_artifact(binding.binding.artifact);
        if (artifact_index == kInvalidPipelineHandleIndex || !is_valid_artifact_access(binding.binding.access)) {
            return {invalid_state("artifact binding contains an invalid artifact reference"), {}};
        }
        const ArtifactDescriptor& descriptor = artifacts_[artifact_index].descriptor;
        if (binding.binding.access == ArtifactAccess::kPublishSnapshot &&
            descriptor.storage != ArtifactStorage::kImmutableSnapshot) {
            return {invalid_argument("publish-snapshot access requires immutable snapshot storage"), {}};
        }
    }
    for (const PipelineExposureRecord& exposure : exposures_) {
        const uint32_t artifact_index = find_artifact(exposure.artifact);
        if (artifact_index == kInvalidPipelineHandleIndex || !is_valid_scope(exposure.target_scope)) {
            return {invalid_state("artifact exposure contains an invalid reference"), {}};
        }
        const ArtifactDescriptor& descriptor = artifacts_[artifact_index].descriptor;
        if (!can_scope_read(exposure.target_scope, descriptor.owner_scope) ||
            exposure.target_scope == descriptor.owner_scope) {
            return {invalid_argument("artifact exposure must point to a higher pipeline scope"), {}};
        }
    }

    PipelineDefinition definition;
    definition.descriptor = descriptor_;
    definition.stages = std::move(stages_);
    definition.artifacts = std::move(artifacts_);
    definition.bindings = std::move(bindings_);
    definition.orderings = std::move(orderings_);
    definition.exposures = std::move(exposures_);
    return {foundation::Result::success(), std::move(definition)};
}

foundation::Result PipelineRegistry::register_pipeline(PipelineDefinition definition) {
    try {
        // Replay records through the actual builder validation. A caller can
        // construct or edit aggregate records without ever using a builder.
        PipelineBuilder builder(definition.descriptor);
        for (const auto& stage : definition.stages)
            if (!builder.add_stage(stage.descriptor)) return builder.status();
        for (const auto& artifact : definition.artifacts)
            if (!builder.declare_artifact(artifact.descriptor)) return builder.status();
        for (const auto& binding : definition.bindings) {
            if (binding.stage_index >= builder.stages_.size() || !is_valid_artifact_access(binding.binding.access))
                return invalid_argument("artifact binding stage index or access is invalid");
            if (const auto result = builder.add_binding(
                    {binding.stage_index, PipelineBuilder::kBuilderGeneration}, binding.binding, binding.binding.access);
                !result) return result;
        }
        for (const auto& ordering : definition.orderings) {
            const PipelineStageHandle stage{ordering.source_stage_index, PipelineBuilder::kBuilderGeneration};
            const auto result = ordering.source_runs_before ? builder.runs_before(stage, ordering.target_stage)
                                                            : builder.runs_after(stage, ordering.target_stage);
            if (!result) return result;
        }
        for (const auto& exposure : definition.exposures)
            if (const auto result = builder.expose_artifact(exposure.artifact, exposure.target_scope); !result) return result;
        auto validated = builder.finish();
        if (!validated) return validated.result;
        auto owner = std::make_shared<const PipelineDefinition>(std::move(validated.value));
        const auto id = owner->descriptor.id;
        std::unique_lock lock(mutex_);
        if (registrations_.contains(id)) return invalid_state("pipeline is already registered");
        registrations_.emplace(id, Registration{std::move(owner)});
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("pipeline definition registration allocation failed");
    }
}

foundation::Result PipelineRegistry::unregister_pipeline(PipelineId id) {
    if (!id.is_valid()) {
        return invalid_argument("pipeline id is invalid");
    }
    std::unique_lock lock(mutex_);
    const auto iterator = registrations_.find(id);
    if (iterator == registrations_.end()) {
        return not_found("pipeline is not registered");
    }
    registrations_.erase(iterator);
    return foundation::Result::success();
}

foundation::ResultValue<std::shared_ptr<const PipelineDefinition>> PipelineRegistry::find_definition(
    PipelineId id) const {
    const foundation::ResultValue<Registration> registration = acquire_registration(id);
    if (!registration) {
        return {registration.result, {}};
    }
    return {foundation::Result::success(), registration.value.definition};
}

foundation::ResultValue<PipelineRegistry::Registration> PipelineRegistry::acquire_registration(PipelineId id) const {
    if (!id.is_valid()) {
        return {invalid_argument("pipeline id is invalid"), {}};
    }
    std::shared_lock lock(mutex_);
    const auto iterator = registrations_.find(id);
    if (iterator == registrations_.end()) {
        return {not_found("pipeline is not registered"), {}};
    }
    return {foundation::Result::success(), iterator->second};
}

foundation::ResultValue<PipelinePlan> detail::PipelineCompilerExecution::compile(const PipelineCompileInput& input) {
    if (input.registry == nullptr) {
        return {invalid_argument("pipeline compile registry is null"), {}};
    }
    if (!input.pipeline.is_valid()) {
        return {invalid_argument("pipeline id is invalid"), {}};
    }
    if (!is_valid_scope(input.scope)) {
        return {invalid_argument("pipeline compile scope is invalid"), {}};
    }

    const PhaseRegistry* phase_registry =
        input.phase_registry != nullptr ? input.phase_registry : &input.registry->phase_registry();
    if (phase_registry == nullptr) {
        return {invalid_state("pipeline compile phase registry is null"), {}};
    }

    const foundation::ResultValue<PipelineRegistry::Registration> registration =
        input.registry->acquire_registration(input.pipeline);
    if (!registration) {
        return {registration.result, {}};
    }

    const PipelineDefinition& base_definition = *registration.value.definition;
    if ((base_definition.descriptor.supported_scopes & to_mask(input.scope)) == 0) {
        return {unsupported("pipeline does not support the requested scope"), {}};
    }
    if ((base_definition.descriptor.required_capabilities & ~input.available_capabilities) != 0) {
        return {unsupported("pipeline required capabilities are unavailable"), {}};
    }

    const PipelineDefinition& definition = base_definition;

    const foundation::ResultValue<PhaseRegistrySnapshot> phase_snapshot =
        detail::PhaseRegistryExecution::snapshot(*phase_registry);
    if (!phase_snapshot) {
        return {phase_snapshot.result, {}};
    }
    std::unordered_map<PhaseId, uint32_t, StableIdHash<PhaseIdTag>> phase_ranks;
    phase_ranks.reserve(phase_snapshot.value.ordered_phases.size());
    for (uint32_t rank = 0; rank < phase_snapshot.value.ordered_phases.size(); ++rank) {
        phase_ranks.emplace(phase_snapshot.value.ordered_phases[rank], rank);
    }
    std::vector<PhaseId> resolved_stage_phases;
    resolved_stage_phases.reserve(definition.stages.size());
    for (const PipelineStageRecord& stage_record : definition.stages) {
        const StageDescriptor& stage = stage_record.descriptor;
        const PhaseId phase = stage.phase_id.is_valid() ? stage.phase_id : to_phase_id(stage.phase);
        if (!phase.is_valid() || phase_ranks.find(phase) == phase_ranks.end()) {
            return {not_found("pipeline stage references an unregistered phase"), {}};
        }
        resolved_stage_phases.push_back(phase);
    }

    const auto definition_artifact_exposed_to = [&](ArtifactId artifact, PipelineScope target_scope) {
        return std::any_of(definition.exposures.begin(), definition.exposures.end(),
                           [&](const PipelineExposureRecord& exposure) {
                               return exposure.artifact == artifact && exposure.target_scope == target_scope;
                           });
    };
    for (const PipelineBindingRecord& binding : definition.bindings) {
        const ArtifactDescriptor* artifact = nullptr;
        for (const PipelineArtifactRecord& candidate : definition.artifacts) {
            if (candidate.descriptor.id == binding.binding.artifact) {
                artifact = &candidate.descriptor;
                break;
            }
        }
        if (artifact == nullptr) {
            return {not_found("pipeline binding references an undeclared artifact"), {}};
        }
        if (!can_scope_read(input.scope, artifact->owner_scope)) {
            return {unsupported("pipeline scope cannot read an artifact owned by a higher scope"), {}};
        }
        if (is_writer(binding.binding.access) && artifact->owner_scope != input.scope) {
            return {unsupported("only the artifact owner scope may declare a write access"), {}};
        }
        if (is_reader(binding.binding.access) && input.scope != artifact->owner_scope &&
            (artifact->visible_to & to_mask(input.scope)) == 0 &&
            !definition_artifact_exposed_to(artifact->id, input.scope)) {
            return {unsupported("artifact is not exposed to the requested pipeline scope"), {}};
        }
        if (binding.binding.required_version != artifact->version) {
            return {invalid_argument("pipeline binding version does not match the artifact contract"), {}};
        }
        if (binding.binding.access == ArtifactAccess::kPublishSnapshot &&
            artifact->storage != ArtifactStorage::kImmutableSnapshot) {
            return {invalid_argument("publish-snapshot access requires immutable snapshot storage"), {}};
        }
    }

    PipelinePlan plan;
    plan.pipeline = definition.descriptor.id;
    plan.instance = input.instance;
    plan.definition_version = definition.descriptor.version;
    plan.scope = input.scope;
    plan.phase_revision = phase_snapshot.value.revision;
    plan.definition_owner = registration.value.definition;
    plan.artifacts.reserve(definition.artifacts.size());
    for (const PipelineArtifactRecord& artifact : definition.artifacts) {
        plan.artifacts.push_back(artifact.descriptor);
    }
    plan.exposures = definition.exposures;

    std::vector<uint32_t> stage_order(definition.stages.size());
    std::iota(stage_order.begin(), stage_order.end(), 0);
    std::sort(stage_order.begin(), stage_order.end(), [&](uint32_t left, uint32_t right) {
        return definition.stages[left].descriptor.id < definition.stages[right].descriptor.id;
    });

    std::vector<std::vector<uint32_t>> stage_nodes(definition.stages.size());
    std::vector<std::vector<ArtifactBinding>> stage_bindings(definition.stages.size());
    for (const PipelineBindingRecord& binding : definition.bindings) {
        if (binding.stage_index >= stage_bindings.size()) {
            return {invalid_state("pipeline binding references an invalid stage"), {}};
        }
        stage_bindings[binding.stage_index].push_back(binding.binding);
    }
    for (const uint32_t original_stage_index : stage_order) {
        const StageDescriptor& stage = definition.stages[original_stage_index].descriptor;
        stage_nodes[original_stage_index].push_back(static_cast<uint32_t>(plan.nodes.size()));
        plan.nodes.push_back(PipelineNode{
            .stage = stage.id,
            .phase = stage.phase,
            .execution_domain = stage.execution_domain,
            .time_policy = stage.time_policy,
            .stage_parallelizable = stage.parallelizable,
            .artifact_bindings = stage_bindings[original_stage_index],
            .phase_id = resolved_stage_phases[original_stage_index],
            .definition_stage_index = original_stage_index,
        });
    }

    std::unordered_map<uint64_t, uint32_t> dependency_indices;
    const auto add_dependency = [&](uint32_t source_node, uint32_t destination_node, PipelineDependencyKind kind,
                                    ArtifactId artifact) {
        if (source_node == destination_node) {
            return;
        }
        const uint64_t key = (static_cast<uint64_t>(source_node) << 32u) | destination_node;
        if (dependency_indices.find(key) != dependency_indices.end()) {
            return;
        }
        dependency_indices.emplace(key, static_cast<uint32_t>(plan.dependencies.size()));
        plan.dependencies.push_back(PipelineDependency{source_node, destination_node, kind, artifact});
    };

    const auto add_stage_dependency = [&](uint32_t source_stage, uint32_t destination_stage,
                                          PipelineDependencyKind kind, ArtifactId artifact) {
        if (source_stage == destination_stage) {
            return;
        }
        for (const uint32_t source_node : stage_nodes[source_stage]) {
            for (const uint32_t destination_node : stage_nodes[destination_stage]) {
                add_dependency(source_node, destination_node, kind, artifact);
            }
        }
    };

    std::vector<std::vector<uint32_t>> stages_by_phase(phase_snapshot.value.ordered_phases.size());
    for (uint32_t stage_index = 0; stage_index < definition.stages.size(); ++stage_index) {
        const auto phase_iterator = phase_ranks.find(resolved_stage_phases[stage_index]);
        if (phase_iterator == phase_ranks.end()) {
            return {not_found("pipeline stage references an unregistered phase"), {}};
        }
        stages_by_phase[phase_iterator->second].push_back(stage_index);
    }

    uint32_t previous_phase = kInvalidPipelineHandleIndex;
    for (uint32_t phase_index = 0; phase_index < stages_by_phase.size(); ++phase_index) {
        if (stages_by_phase[phase_index].empty()) {
            continue;
        }
        if (previous_phase != kInvalidPipelineHandleIndex) {
            for (const uint32_t source_stage : stages_by_phase[previous_phase]) {
                for (const uint32_t destination_stage : stages_by_phase[phase_index]) {
                    add_stage_dependency(source_stage, destination_stage, PipelineDependencyKind::kOrdering, {});
                }
            }
        }
        previous_phase = phase_index;
    }

    struct BindingReference {
        uint32_t stage_index{kInvalidPipelineHandleIndex};
        const ArtifactBinding* binding{nullptr};
    };

    std::vector<std::vector<BindingReference>> artifact_bindings(definition.artifacts.size());
    for (const PipelineBindingRecord& binding_record : definition.bindings) {
        const uint32_t artifact_index = [&] {
            for (uint32_t index = 0; index < definition.artifacts.size(); ++index) {
                if (definition.artifacts[index].descriptor.id == binding_record.binding.artifact) {
                    return index;
                }
            }
            return kInvalidPipelineHandleIndex;
        }();
        if (artifact_index == kInvalidPipelineHandleIndex) {
            return {not_found("binding references an undeclared artifact"), {}};
        }
        artifact_bindings[artifact_index].push_back(
            BindingReference{binding_record.stage_index, &binding_record.binding});
    }

    for (uint32_t artifact_index = 0; artifact_index < definition.artifacts.size(); ++artifact_index) {
        const ArtifactDescriptor& artifact = definition.artifacts[artifact_index].descriptor;
        std::vector<BindingReference> writers;
        std::vector<BindingReference> readers;
        for (const BindingReference& reference : artifact_bindings[artifact_index]) {
            if (is_writer(reference.binding->access)) {
                writers.push_back(reference);
            }
            if (is_reader(reference.binding->access)) {
                readers.push_back(reference);
            }
        }

        if (writers.empty()) {
            const bool all_optional =
                !readers.empty() && std::all_of(readers.begin(), readers.end(), [](const BindingReference& reference) {
                    return has_flag(reference.binding->flags, AccessFlags::kOptional);
                });
            if (!readers.empty() && !artifact.external_input && !all_optional) {
                return {invalid_argument("artifact has readers but no producer"), {}};
            }
            continue;
        }

        if (writers.size() > 1) {
            const bool all_append = std::all_of(writers.begin(), writers.end(), [](const BindingReference& reference) {
                return is_append_writer(reference.binding->access);
            });
            const bool deterministic_append =
                all_append && std::all_of(writers.begin(), writers.end(), [](const BindingReference& reference) {
                    return has_flag(reference.binding->flags, AccessFlags::kDeterministicAppend);
                });
            if (!deterministic_append) {
                return {invalid_argument("artifact has multiple unordered producers"), {}};
            }
            std::sort(writers.begin(), writers.end(), [&](const BindingReference& left, const BindingReference& right) {
                return definition.stages[left.stage_index].descriptor.id <
                       definition.stages[right.stage_index].descriptor.id;
            });
            for (size_t index = 1; index < writers.size(); ++index) {
                add_stage_dependency(writers[index - 1].stage_index, writers[index].stage_index,
                                     PipelineDependencyKind::kArtifact, artifact.id);
            }
            for (const BindingReference& writer : writers) {
                for (const BindingReference& reader : readers) {
                    add_stage_dependency(writer.stage_index, reader.stage_index, PipelineDependencyKind::kArtifact,
                                         artifact.id);
                }
            }
            continue;
        }

        const BindingReference& writer = writers.front();
        for (const BindingReference& reader : readers) {
            add_stage_dependency(writer.stage_index, reader.stage_index, PipelineDependencyKind::kArtifact,
                                 artifact.id);
        }
    }

    for (const PipelineOrderingRecord& ordering : definition.orderings) {
        uint32_t target_stage = kInvalidPipelineHandleIndex;
        for (uint32_t index = 0; index < definition.stages.size(); ++index) {
            if (definition.stages[index].descriptor.id == ordering.target_stage) {
                target_stage = index;
                break;
            }
        }
        if (target_stage == kInvalidPipelineHandleIndex) {
            return {not_found("explicit ordering references an undeclared stage"), {}};
        }
        if (ordering.source_runs_before) {
            add_stage_dependency(ordering.source_stage_index, target_stage, PipelineDependencyKind::kOrdering, {});
        } else {
            add_stage_dependency(target_stage, ordering.source_stage_index, PipelineDependencyKind::kOrdering, {});
        }
    }

    std::vector<std::vector<uint32_t>> outgoing(plan.nodes.size());
    std::vector<uint32_t> indegree(plan.nodes.size(), 0);
    for (const PipelineDependency& dependency : plan.dependencies) {
        if (dependency.source_node >= plan.nodes.size() || dependency.destination_node >= plan.nodes.size()) {
            return {invalid_state("compiled dependency references an invalid node"), {}};
        }
        outgoing[dependency.source_node].push_back(dependency.destination_node);
        ++indegree[dependency.destination_node];
    }

    std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<>> ready;
    for (uint32_t node_index = 0; node_index < indegree.size(); ++node_index) {
        if (indegree[node_index] == 0) {
            ready.push(node_index);
        }
    }

    std::vector<uint32_t> remaining_indegree = indegree;
    while (!ready.empty()) {
        std::vector<uint32_t> current_batch;
        while (!ready.empty()) {
            current_batch.push_back(ready.top());
            ready.pop();
        }

        PipelineExecutionBatch batch;
        batch.node_indices = current_batch;
        plan.topological_order.insert(plan.topological_order.end(), current_batch.begin(), current_batch.end());
        plan.batches.push_back(std::move(batch));

        for (const uint32_t source_node : current_batch) {
            for (const uint32_t destination_node : outgoing[source_node]) {
                if (--remaining_indegree[destination_node] == 0) {
                    ready.push(destination_node);
                }
            }
        }
    }

    if (plan.topological_order.size() != plan.nodes.size()) {
        return {invalid_argument("pipeline dependency graph contains a cycle"), {}};
    }

    if (detail::PhaseRegistryExecution::revision(*phase_registry) != plan.phase_revision) {
        return {foundation::Result::failure(foundation::ResultCode::kOutOfDate,
                                            "phase registry changed while compiling the pipeline"),
                {}};
    }

    static std::atomic<PlanRevision> next_revision{1};
    plan.revision = next_revision.fetch_add(1, std::memory_order_relaxed);
    return {foundation::Result::success(), std::move(plan)};
}

foundation::Result PipelineScheduler::execute(const PipelinePlan& plan, PipelineExecutionContext& context) const {
    return execute_internal(plan, context, nullptr);
}

foundation::Result PipelineScheduler::execute_phase(const PipelinePlan& plan, PipelineExecutionContext& context,
                                                    PhaseId phase) const {
    return execute_internal(plan, context, &phase);
}

foundation::Result PipelineScheduler::execute_internal(const PipelinePlan& plan, PipelineExecutionContext& context,
                                                       const PhaseId* phase) const {
    context.visited_nodes_.clear();
    if (phase != nullptr && !phase->is_valid()) {
        return invalid_argument("pipeline execution phase id is invalid");
    }
    if (const foundation::Result result = context.begin_plan(plan); !result) {
        return result;
    }
    struct PlanExecutionGuard final {
        PipelineExecutionContext& context;
        ~PlanExecutionGuard() { context.end_plan(); }
    } plan_guard{context};

    try {
        if (plan.topological_order.size() != plan.nodes.size()) {
            return invalid_state("pipeline topological order does not cover its Stage nodes");
        }
        std::vector<uint32_t> positions(plan.nodes.size(), kInvalidPipelineHandleIndex);
        std::vector<uint32_t> visited_nodes;
        visited_nodes.reserve(plan.nodes.size());
        std::size_t ordered_index = 0;
        for (const PipelineExecutionBatch& batch : plan.batches) {
            for (const uint32_t node_index : batch.node_indices) {
                if (node_index >= plan.nodes.size()) {
                    return invalid_state("pipeline batch references an invalid Stage node");
                }
                if (positions[node_index] != kInvalidPipelineHandleIndex ||
                    ordered_index >= plan.topological_order.size() ||
                    plan.topological_order[ordered_index] != node_index) {
                    return invalid_state("pipeline Stage batches do not match its topological order");
                }
                positions[node_index] = static_cast<uint32_t>(ordered_index);
                ++ordered_index;
                const PipelineNode& node = plan.nodes[node_index];
                if (!node.stage.is_valid() || !is_valid_engine_phase(node.phase) ||
                    !is_valid_execution_domain(node.execution_domain) || !is_valid_time_policy(node.time_policy)) {
                    return invalid_argument("pipeline Stage metadata is invalid");
                }
                const PhaseId node_phase = node.phase_id.is_valid() ? node.phase_id : to_phase_id(node.phase);
                if (phase == nullptr || node_phase == *phase) {
                    visited_nodes.push_back(node_index);
                }
            }
        }
        if (ordered_index != plan.nodes.size()) {
            return invalid_state("pipeline Stage batches do not cover its nodes");
        }
        for (const PipelineDependency& dependency : plan.dependencies) {
            if (dependency.source_node >= plan.nodes.size() || dependency.destination_node >= plan.nodes.size() ||
                positions[dependency.source_node] >= positions[dependency.destination_node]) {
                return invalid_state("pipeline Stage order does not satisfy its dependencies");
            }
        }
        context.visited_nodes_.swap(visited_nodes);
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("pipeline Stage observation allocation failed");
    }
}

foundation::Result PipelineScopeHost::bind_pipeline(PipelineId pipeline) {
    std::unique_lock plan_lock(plan_mutex_);
    if (!is_valid_scope(scope_)) {
        return invalid_state("pipeline scope host was created with an invalid scope");
    }
    if (active_) {
        return invalid_state("cannot bind a pipeline while the scope host is active");
    }
    if (!pipeline.is_valid()) {
        return invalid_argument("pipeline id is invalid");
    }
    pipeline_ = pipeline;
    if (active_plan_ != nullptr) {
        retired_plans_.push_back(std::move(active_plan_));
    }
    pending_plan_.reset();
    active_plan_.reset();
    plan_lock.unlock();
    return collect_retired_plans();
}

foundation::Result PipelineScopeHost::rebuild(const PipelineRegistry& registry, CapabilityMask available_capabilities) {
    PipelineId pipeline;
    {
        std::shared_lock plan_lock(plan_mutex_);
        pipeline = pipeline_;
    }
    if (!pipeline.is_valid()) {
        return invalid_state("scope host has no bound pipeline");
    }
    foundation::ResultValue<PipelinePlan> compiled = detail::PipelineCompilerExecution::compile({
        .pipeline = pipeline,
        .scope = scope_,
        .available_capabilities = available_capabilities,
        .registry = &registry,
        .instance = instance_,
    });
    if (!compiled) {
        return compiled.result;
    }

    PipelinePlanHandle plan;
    try {
        plan = std::make_shared<const PipelinePlan>(std::move(compiled.value));
    } catch (const std::bad_alloc&) {
        return foundation::Result::failure(foundation::ResultCode::kOutOfMemory, "pipeline plan allocation failed");
    }

    std::unique_lock plan_lock(plan_mutex_);
    if (pipeline_ != pipeline) {
        return invalid_state("bound pipeline changed during pipeline rebuild");
    }
    if (active_) {
        pending_plan_ = std::move(plan);
        return foundation::Result::success();
    }
    if (active_plan_ != nullptr) {
        if (active_plan_.use_count() > 1 && !plan_contracts_preserved(*active_plan_, *plan)) {
            return invalid_state("pipeline rebuild is blocked by an externally referenced active plan");
        }
        if (const foundation::Result result = artifacts_.replace_plan(*active_plan_, *plan); !result) {
            return result;
        }
    } else if (const foundation::Result result = artifacts_.install_plan(*plan); !result) {
        return result;
    }
    if (active_plan_ != nullptr) {
        retired_plans_.push_back(std::move(active_plan_));
    }
    pending_plan_.reset();
    active_plan_ = std::move(plan);
    return foundation::Result::success();
}

namespace {
foundation::Result rebuild_runtime_schedule(PipelinePlan& plan) {
    std::vector<std::vector<uint32_t>> outgoing(plan.nodes.size());
    std::vector<uint32_t> indegree(plan.nodes.size(), 0);
    for (const PipelineDependency& dependency : plan.dependencies) {
        if (dependency.source_node >= plan.nodes.size() || dependency.destination_node >= plan.nodes.size()) {
            return invalid_state("runtime ordering references an invalid pipeline node");
        }
        outgoing[dependency.source_node].push_back(dependency.destination_node);
        ++indegree[dependency.destination_node];
    }
    plan.topological_order.clear();
    plan.batches.clear();
    std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<>> ready;
    for (uint32_t index = 0; index < indegree.size(); ++index)
        if (indegree[index] == 0)
            ready.push(index);
    while (!ready.empty()) {
        std::vector<uint32_t> current_batch;
        while (!ready.empty()) {
            current_batch.push_back(ready.top());
            ready.pop();
        }
        PipelineExecutionBatch batch;
        batch.node_indices = current_batch;
        plan.topological_order.insert(plan.topological_order.end(), current_batch.begin(), current_batch.end());
        plan.batches.push_back(std::move(batch));
        for (const uint32_t source : current_batch)
            for (const uint32_t destination : outgoing[source]) {
                if (--indegree[destination] == 0)
                    ready.push(destination);
            }
    }
    if (plan.topological_order.size() != plan.nodes.size()) {
        return invalid_argument("runtime ordering would create a pipeline cycle");
    }
    return foundation::Result::success();
}
}  // namespace

foundation::Result PipelineScopeHost::add_runtime_ordering(uint32_t source_node, uint32_t destination_node) {
    std::unique_lock plan_lock(plan_mutex_);
    const PipelinePlanHandle base = pending_plan_ != nullptr ? pending_plan_ : active_plan_;
    if (base == nullptr)
        return invalid_state("scope host has no compiled pipeline plan");
    if (source_node >= base->nodes.size() || destination_node >= base->nodes.size() ||
        source_node == destination_node) {
        return invalid_argument("runtime ordering nodes are invalid");
    }
    if (std::any_of(base->dependencies.begin(), base->dependencies.end(), [&](const PipelineDependency& edge) {
            return edge.source_node == source_node && edge.destination_node == destination_node;
        }))
        return foundation::Result::success();
    auto next = std::make_shared<PipelinePlan>(*base);
    next->dependencies.push_back(
        PipelineDependency{source_node, destination_node, PipelineDependencyKind::kOrdering, {}, true});
    if (const foundation::Result result = rebuild_runtime_schedule(*next); !result)
        return result;
    next->revision = base->revision + 1;
    pending_plan_ = std::move(next);
    return foundation::Result::success();
}

foundation::Result PipelineScopeHost::remove_runtime_ordering(uint32_t source_node, uint32_t destination_node) {
    std::unique_lock plan_lock(plan_mutex_);
    const PipelinePlanHandle base = pending_plan_ != nullptr ? pending_plan_ : active_plan_;
    if (base == nullptr)
        return invalid_state("scope host has no compiled pipeline plan");
    auto next = std::make_shared<PipelinePlan>(*base);
    const auto edge =
        std::find_if(next->dependencies.begin(), next->dependencies.end(), [&](const PipelineDependency& value) {
            return value.source_node == source_node && value.destination_node == destination_node;
        });
    if (edge == next->dependencies.end())
        return not_found("pipeline connection does not exist");
    next->dependencies.erase(edge);
    if (const foundation::Result result = rebuild_runtime_schedule(*next); !result)
        return result;
    next->revision = base->revision + 1;
    pending_plan_ = std::move(next);
    return foundation::Result::success();
}

bool PipelineScopeHost::is_active() const noexcept {
    std::shared_lock plan_lock(plan_mutex_);
    return active_;
}

const PipelinePlan* detail::PipelineScopeHostExecution::active_plan(const PipelineScopeHost& host) noexcept {
    std::shared_lock plan_lock(host.plan_mutex_);
    return host.active_plan_.get();
}

PipelinePlanHandle detail::PipelineScopeHostExecution::active_plan_snapshot(const PipelineScopeHost& host) noexcept {
    std::shared_lock plan_lock(host.plan_mutex_);
    return host.active_plan_;
}

bool detail::PipelineScopeHostExecution::has_pending_plan(const PipelineScopeHost& host) noexcept {
    std::shared_lock plan_lock(host.plan_mutex_);
    return host.pending_plan_ != nullptr;
}

std::size_t detail::PipelineScopeHostExecution::retired_plan_count(const PipelineScopeHost& host) noexcept {
    std::shared_lock plan_lock(host.plan_mutex_);
    return host.retired_plans_.size();
}

const ArtifactScopeBridge* detail::PipelineScopeHostExecution::scope_bridge(
    const PipelineScopeHost& host) noexcept {
    return host.scope_bridge_;
}

void detail::PipelineScopeHostExecution::set_scope_bridge(
    PipelineScopeHost& host, const ArtifactScopeBridge* scope_bridge) noexcept {
    host.scope_bridge_ = scope_bridge;
}

foundation::Result PipelineScopeHost::apply_pending_plan_internal() const {
    std::unique_lock plan_lock(plan_mutex_);
    if (pending_plan_ == nullptr) {
        return foundation::Result::failure(foundation::ResultCode::kNotReady,
                                           "scope host has no pending pipeline plan");
    }
    if (active_plan_ != nullptr) {
        if (active_plan_.use_count() > 1 && !plan_contracts_preserved(*active_plan_, *pending_plan_)) {
            return invalid_state("pending pipeline plan is blocked by an externally referenced active plan");
        }
        if (const foundation::Result result = artifacts_.replace_plan(*active_plan_, *pending_plan_); !result) {
            return result;
        }
    } else if (const foundation::Result result = artifacts_.install_plan(*pending_plan_); !result) {
        return result;
    }
    if (active_plan_ != nullptr) {
        retired_plans_.push_back(std::move(active_plan_));
    }
    active_plan_ = std::move(pending_plan_);
    return foundation::Result::success();
}

foundation::Result PipelineScopeHost::apply_pending_plan() {
    return apply_pending_plan_internal();
}

foundation::Result PipelineScopeHost::collect_retired_plans() {
    std::unique_lock plan_lock(plan_mutex_);
    foundation::Result first_error = foundation::Result::success();
    for (auto iterator = retired_plans_.begin(); iterator != retired_plans_.end();) {
        if (iterator->use_count() != 1) {
            ++iterator;
            continue;
        }
        const foundation::Result result = artifacts_.retire_plan(**iterator);
        if (!result) {
            if (first_error) {
                first_error = result;
            }
            ++iterator;
            continue;
        }
        iterator = retired_plans_.erase(iterator);
    }
    return first_error;
}

foundation::Result PipelineScopeHost::activate(const PipelineActivationContext&) {
    std::unique_lock plan_lock(plan_mutex_);
    if (active_) {
        return invalid_state("pipeline scope host is already active");
    }
    if (!pipeline_.is_valid() || active_plan_ == nullptr) {
        return invalid_state("scope host must have a bound and compiled pipeline");
    }
    active_ = true;
    return foundation::Result::success();
}

foundation::Result PipelineScopeHost::deactivate(const PipelineDeactivationContext&) {
    std::unique_lock plan_lock(plan_mutex_);
    if (!active_) {
        return invalid_state("pipeline scope host is not active");
    }
    active_ = false;
    return foundation::Result::success();
}

foundation::Result PipelineScopeHost::execute(PipelineScheduler& scheduler, PipelineExecutionContext& context) const {
    context.visited_nodes_.clear();
    const foundation::Result pending_result = apply_pending_plan_internal();
    if (!pending_result && pending_result.code != foundation::ResultCode::kNotReady) {
        return pending_result;
    }

    std::shared_lock plan_lock(plan_mutex_);
    if (!active_ || active_plan_ == nullptr) {
        return invalid_state("pipeline scope host is not active or has no compiled plan");
    }
    if (context.scope() != scope_) {
        return invalid_state("pipeline execution context scope does not match the host scope");
    }
    ArtifactRegistry* artifact_registry = context.artifact_registry();
    const ArtifactScopeBridge* scope_bridge = context.scope_bridge();
    if (artifact_registry == nullptr) {
        artifact_registry = &artifacts_;
    }
    if (scope_bridge == nullptr) {
        scope_bridge = scope_bridge_;
    }
    context.set_artifact_runtime(artifact_registry, scope_bridge);
    const PipelinePlanHandle plan = active_plan_;
    const foundation::Result result = scheduler.execute(*plan, context);
    return result;
}

foundation::Result PipelineScopeHost::execute_phase(PipelineScheduler& scheduler, PipelineExecutionContext& context,
                                                    PhaseId phase, PipelinePlanHandle pinned_plan) const {
    context.visited_nodes_.clear();
    if (!phase.is_valid()) {
        return invalid_argument("pipeline execution phase id is invalid");
    }

    if (pinned_plan == nullptr) {
        const foundation::Result pending_result = apply_pending_plan_internal();
        if (!pending_result && pending_result.code != foundation::ResultCode::kNotReady) {
            return pending_result;
        }
    }

    std::shared_lock plan_lock(plan_mutex_);
    if (!active_) {
        return invalid_state("pipeline scope host is not active");
    }
    if (context.scope() != scope_) {
        return invalid_state("pipeline execution context scope does not match the host scope");
    }

    const PipelinePlanHandle plan = pinned_plan != nullptr ? std::move(pinned_plan) : active_plan_;
    if (plan == nullptr || plan->revision == 0 || !plan->pipeline.is_valid()) {
        return invalid_state("pipeline scope host has no valid execution plan");
    }
    if (plan->scope != scope_ || plan->instance != instance_ || plan->pipeline != pipeline_) {
        return invalid_state("pinned pipeline plan does not belong to the scope host");
    }

    ArtifactRegistry* artifact_registry = context.artifact_registry();
    const ArtifactScopeBridge* scope_bridge = context.scope_bridge();
    if (artifact_registry == nullptr) {
        artifact_registry = &artifacts_;
    }
    if (scope_bridge == nullptr) {
        scope_bridge = scope_bridge_;
    }
    context.set_artifact_runtime(artifact_registry, scope_bridge);
    return scheduler.execute_phase(*plan, context, phase);
}
}  // namespace aeg::pipeline
