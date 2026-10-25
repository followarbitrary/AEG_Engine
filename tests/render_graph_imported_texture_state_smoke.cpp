/**
 * @file render_graph_imported_texture_state_smoke.cpp
 * @brief Verifies imported texture state continuity across independent graphs.
 */
#include <cassert>
#include <limits>
#include <memory>

#include "frame_context.hpp"
#include "render_graph.hpp"

#include "../Engine/RenderGraph/private/render_graph_execution.hpp"
#include "rhi.hpp"

namespace {
using aeg::foundation::Result;
using aeg::render_graph::FrameContext;
using aeg::render_graph::RenderGraph;
using aeg::render_graph::RenderGraphTexture;
using aeg::rhi::ClearColorValue;
using aeg::rhi::BufferCapability;
using aeg::rhi::BufferDesc;
using aeg::rhi::Device;
using aeg::rhi::DeviceCreateDesc;
using aeg::rhi::Format;
using aeg::rhi::LoadOp;
using aeg::rhi::MemoryClass;
using aeg::rhi::PipelineStage;
using aeg::rhi::QueueClass;
using aeg::rhi::ResourceState;
using aeg::rhi::ResourceUsage;
using aeg::rhi::StoreOp;
using aeg::rhi::TextureCapability;
using aeg::rhi::TextureDesc;
using aeg::rhi::TextureHandle;
using aeg::rhi::TextureViewDesc;
using aeg::rhi::TextureViewKind;

[[nodiscard]] TextureDesc make_texture_desc() {
    TextureDesc desc{};
    desc.extent = {4, 4, 1};
    desc.format = Format::kRgba8Unorm;
    desc.mip_levels = 1;
    desc.array_layers = 1;
    desc.capabilities = TextureCapability::kColorAttachment | TextureCapability::kSampled;
    desc.memory = MemoryClass::kDeviceLocal;
    desc.initial_usage = ResourceUsage::kUndefined;
    desc.initial_stages = PipelineStage::kAllCommands;
    desc.initial_queue = QueueClass::kAuto;
    desc.name = aeg::rhi::DebugName{"RenderGraph.ImportedTextureState"};
    return desc;
}

[[nodiscard]] ResourceState make_undefined_state() {
    return ResourceState{
        ResourceUsage::kUndefined,
        aeg::rhi::ShaderStage::kAll,
        QueueClass::kAuto,
        false,
        PipelineStage::kAllCommands,
    };
}
}

int main() {
    const DeviceCreateDesc device_desc{
        aeg::rhi::BackendType::kVulkan,
        "AEG RenderGraph Imported Texture State Smoke",
        std::numeric_limits<uint32_t>::max(),
        true,
        true,
    };
    auto device_result = aeg::rhi::create_device(device_desc);
    assert(device_result);
    std::unique_ptr<Device> device = std::move(device_result.value);

    const TextureDesc texture_desc = make_texture_desc();
    const auto texture_result = device->create_texture(texture_desc);
    assert(texture_result);
    const aeg::rhi::TextureHandle texture = texture_result.value;

    BufferDesc buffer_desc{};
    buffer_desc.size = 64;
    buffer_desc.capabilities = BufferCapability::kTransferSource | BufferCapability::kTransferDestination;
    buffer_desc.initial_usage = ResourceUsage::kCopySource;
    buffer_desc.initial_stages = PipelineStage::kCopy;
    buffer_desc.initial_queue = QueueClass::kGraphics;
    buffer_desc.name = aeg::rhi::DebugName{"RenderGraph.ImportedTextureState.Buffer"};
    const auto buffer_result = device->create_buffer(buffer_desc);
    assert(buffer_result);
    const aeg::rhi::BufferHandle buffer = buffer_result.value;

    const auto buffer_state_result = device->query_buffer_state(buffer);
    assert(buffer_state_result);
    assert(buffer_state_result.value.segments.size() == 1);
    assert(buffer_state_result.value.segments.front().offset == 0);
    assert(buffer_state_result.value.segments.front().size == buffer_desc.size);
    assert(buffer_state_result.value.segments.front().state.usage == ResourceUsage::kCopySource);

    const auto invalid_texture_state_result = device->query_texture_state(TextureHandle{});
    assert(!invalid_texture_state_result);
    assert(invalid_texture_state_result.result.code == aeg::foundation::ResultCode::kNotFound);

    const auto view_result = device->create_texture_view(TextureViewDesc{
        texture,
        TextureViewKind::kColorAttachment,
        aeg::rhi::TextureViewDimension::kTex2D,
        texture_desc.format,
        aeg::rhi::SubresourceRange{aeg::rhi::Aspect::kColor, 0, 1, 0, 1},
        {},
        aeg::rhi::DebugName{"RenderGraph.ImportedTextureState.View"},
    });
    assert(view_result);
    aeg::rhi::TextureViewHandle view = view_result.value;

    FrameContext frame_context(*device, {.frames_in_flight = 2});
    RenderGraph first_graph(*device);
    const auto first_resource_result = first_graph.import_texture(
        texture, view, texture_desc, make_undefined_state());
    assert(first_resource_result);
    const RenderGraphTexture first_resource = first_resource_result.value;
    const auto first_pass_result = first_graph.add_pass(
        "RenderGraph.ImportedTextureState.Write", QueueClass::kGraphics,
        [first_resource](aeg::render_graph::PassBuilder& builder) -> Result {
            const Result result = builder.use_color_attachment(
                first_resource, LoadOp::kClear, StoreOp::kStore,
                ClearColorValue{0.25f, 0.5f, 0.75f, 1.0f}, ResourceUsage::kSampledRead);
            if (result) {
                builder.set_side_effects();
            }
            return result;
        },
        [](aeg::render_graph::PassExecutionContext& context) {
            if (const Result result = context.begin_rendering(); !result) {
                return result;
            }
            return context.end_rendering();
        });
    assert(first_pass_result);

    assert(frame_context.begin_frame());
    const auto first_execution_result = first_graph.execute(frame_context);
    assert(first_execution_result);
    assert(first_execution_result.value.completion.is_valid());
    assert(device->wait(first_execution_result.value.completion, std::numeric_limits<uint64_t>::max()));
    assert(frame_context.end_frame());

    const auto first_texture_state_result = device->query_texture_state(texture);
    assert(first_texture_state_result);
    assert(first_texture_state_result.value.subresource_states.size() == 3);
    assert(first_texture_state_result.value.subresource_states.front().usage == ResourceUsage::kSampledRead);
    bool first_completion_is_tracked = false;
    for (const aeg::rhi::GpuSyncPoint point: first_texture_state_result.value.last_use.points()) {
        first_completion_is_tracked = first_completion_is_tracked || point == first_execution_result.value.completion;
    }
    assert(first_completion_is_tracked);

    RenderGraph second_graph(*device);
    const auto second_resource_result = second_graph.import_texture(
        texture, view, texture_desc, make_undefined_state());
    assert(second_resource_result);
    const RenderGraphTexture second_resource = second_resource_result.value;
    const auto second_pass_result = second_graph.add_pass(
        "RenderGraph.ImportedTextureState.Read", QueueClass::kGraphics,
        [second_resource](aeg::render_graph::PassBuilder& builder) -> Result {
            const Result result = builder.read(
                second_resource, ResourceUsage::kSampledRead, PipelineStage::kFragmentShader,
                aeg::rhi::ShaderStage::kFragment);
            if (result) {
                builder.set_side_effects();
            }
            return result;
        },
        [](aeg::render_graph::PassExecutionContext&) { return Result::success(); });
    assert(second_pass_result);

    assert(frame_context.begin_frame());
    const auto second_execution_result = second_graph.execute(frame_context);
    assert(second_execution_result);
    assert(second_execution_result.value.completion.is_valid());
    assert(device->wait(second_execution_result.value.completion, std::numeric_limits<uint64_t>::max()));
    assert(frame_context.end_frame());

    RenderGraph ordinary_present_graph(*device);
    const auto ordinary_present_resource_result = ordinary_present_graph.import_texture(
        texture, view, texture_desc, make_undefined_state());
    assert(ordinary_present_resource_result);
    const RenderGraphTexture ordinary_present_resource = ordinary_present_resource_result.value;
    const auto ordinary_present_pass_result = ordinary_present_graph.add_pass(
        "RenderGraph.ImportedTextureState.OrdinaryPresent", QueueClass::kGraphics,
        [ordinary_present_resource](aeg::render_graph::PassBuilder& builder) {
            return builder.use_color_attachment(
                ordinary_present_resource, LoadOp::kClear, StoreOp::kStore,
                ClearColorValue{0.0f, 0.0f, 0.0f, 1.0f}, ResourceUsage::kPresent);
        },
        [](aeg::render_graph::PassExecutionContext& context) {
            if (const Result result = context.begin_rendering(); !result) {
                return result;
            }
            return context.end_rendering();
        });
    assert(ordinary_present_pass_result);
    assert(ordinary_present_graph.compile());
    assert(aeg::render_graph::detail::RenderGraphExecution::plan(ordinary_present_graph).batches.size() == 1);
    assert(!aeg::render_graph::detail::RenderGraphExecution::plan(ordinary_present_graph).batches.front().submits_frame);
    assert(!aeg::render_graph::detail::RenderGraphExecution::plan(ordinary_present_graph).batches.front().waits_for_acquire);

    first_graph.reset();
    second_graph.reset();
    ordinary_present_graph.reset();
    assert(device->wait_idle());
    assert(device->destroy_texture_view(view));
    assert(device->destroy_texture(texture));
    assert(device->destroy_buffer(buffer));
    return 0;
}
