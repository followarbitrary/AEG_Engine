/**
 * @file editor_imgui_renderer_smoke.cpp
 * @brief Executes the fixed backend-neutral ImGui overlay and verifies pixels.
 */
#include "feature_clock_test_fixture.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <span>

#include "frame_context.hpp"
#include "imgui_renderer.hpp"
#include "../Application/EditorFramework/public/imgui_render_execution.hpp"
#include "imgui_texture_registry.hpp"
#include "pipeline.hpp"
#include "render_graph.hpp"

#include "../Engine/RenderGraph/private/render_graph_execution.hpp"
#include "renderer.hpp"
#include "renderer_variants_internal.hpp"
#include "renderer_feature_profiles.hpp"
#include "rhi.hpp"
#include "texture_uploader.hpp"

namespace {
using aeg::editor::ImGuiDrawCommand;
using aeg::editor::ImGuiDrawPacket;
using aeg::editor::ImGuiDrawVertex;
using aeg::editor::ImGuiRenderer;
using aeg::editor::ImGuiRenderTargetDesc;
using aeg::editor::ImGuiTextureBinding;
using aeg::editor::ImGuiTextureRegistry;
using aeg::foundation::Result;
using aeg::foundation::ResultCode;
using aeg::pipeline::PipelineRegistry;
using aeg::pipeline::RenderResourceId;
using aeg::render_graph::FrameAllocation;
using aeg::render_graph::FrameContext;
using aeg::render_graph::RenderGraph;
using aeg::render_graph::RenderGraphBuffer;
using aeg::render_graph::RenderGraphTexture;
using aeg::resource::GpuTexture;
using aeg::resource::TextureData;
using aeg::resource::TextureSubresourceData;
using aeg::resource::TextureUploader;
using aeg::rhi::BufferCapability;
using aeg::rhi::BufferDesc;
using aeg::rhi::ClearColorValue;
using aeg::rhi::Device;
using aeg::rhi::DeviceCreateDesc;
using aeg::rhi::Extent3D;
using aeg::rhi::Format;
using aeg::rhi::LoadOp;
using aeg::rhi::MemoryClass;
using aeg::rhi::Offset3D;
using aeg::rhi::PipelineStage;
using aeg::rhi::QueueClass;
using aeg::rhi::ResourceState;
using aeg::rhi::ResourceUsage;
using aeg::rhi::SamplerDesc;
using aeg::rhi::StoreOp;
using aeg::rhi::TextureCapability;
using aeg::rhi::TextureDesc;
using aeg::rhi::TextureSubresourceLayers;
using aeg::rhi::TextureViewDesc;
using aeg::rhi::TextureViewDimension;
using aeg::rhi::TextureViewKind;

constexpr uint32_t kTargetWidth = 64;
constexpr uint32_t kTargetHeight = 64;
constexpr uint64_t kReadbackSize = static_cast<uint64_t>(kTargetWidth) * kTargetHeight * 4;

bool check(const Result& result, const char* operation) {
    if (result) {
        return true;
    }
    (void)operation;
    return false;
}

template <typename T>
bool check(const aeg::foundation::ResultValue<T>& result, const char* operation) {
    return check(result.result, operation);
}

ResourceState make_copy_destination_state() {
    return ResourceState{
        ResourceUsage::kCopyDestination, aeg::rhi::ShaderStage::kAll, QueueClass::kCopy, false, PipelineStage::kCopy,
    };
}

ResourceState make_host_read_state() {
    return ResourceState{
        ResourceUsage::kHostRead, aeg::rhi::ShaderStage::kAll, QueueClass::kCopy, false, PipelineStage::kHost,
    };
}

BufferDesc make_readback_desc(uint64_t size) {
    BufferDesc desc{};
    desc.size = size;
    desc.capabilities = BufferCapability::kTransferSource | BufferCapability::kTransferDestination |
                        BufferCapability::kVertex | BufferCapability::kIndex | BufferCapability::kUniform;
    desc.memory = MemoryClass::kReadback;
    desc.initial_usage = ResourceUsage::kHostRead;
    desc.initial_stages = PipelineStage::kHost;
    desc.initial_queue = QueueClass::kCopy;
    desc.name = aeg::rhi::DebugName{"Editor.ImGui.Readback"};
    return desc;
}

Result destroy_target(Device& device, aeg::rhi::TextureViewHandle& view, aeg::rhi::TextureHandle& texture) {
    Result first_result = Result::success();
    if (view.is_valid()) {
        const Result result = device.destroy_texture_view(view);
        if (result || result.code == ResultCode::kNotFound) {
            view = {};
        } else if (first_result) {
            first_result = result;
        }
    }
    if (texture.is_valid()) {
        const Result result = device.destroy_texture(texture);
        if (result || result.code == ResultCode::kNotFound) {
            texture = {};
        } else if (first_result) {
            first_result = result;
        }
    }
    return first_result;
}
}  // namespace

int main() {
    const DeviceCreateDesc device_desc{
        aeg::rhi::BackendType::kVulkan,
        "AEG Editor ImGui Renderer Smoke",
        std::numeric_limits<uint32_t>::max(),
        true,
        true,
    };
    auto device_result = aeg::rhi::create_device(device_desc);
    if (!check(device_result, "create_device")) {
        return 1;
    }
    std::unique_ptr<Device> device = std::move(device_result.value);

    TextureDesc ui_texture_desc{};
    ui_texture_desc.extent = {1, 1, 1};
    ui_texture_desc.format = Format::kRgba8Unorm;
    ui_texture_desc.mip_levels = 1;
    ui_texture_desc.array_layers = 1;
    ui_texture_desc.capabilities = TextureCapability::kTransferDestination | TextureCapability::kSampled;
    ui_texture_desc.memory = MemoryClass::kDeviceLocal;
    ui_texture_desc.initial_usage = ResourceUsage::kUndefined;
    ui_texture_desc.initial_stages = PipelineStage::kAllCommands;
    ui_texture_desc.initial_queue = QueueClass::kAuto;
    ui_texture_desc.name = aeg::rhi::DebugName{"Editor.ImGui.WhiteTexture"};

    const std::array<std::byte, 4> white_pixel{
        std::byte{0xff},
        std::byte{0xff},
        std::byte{0xff},
        std::byte{0xff},
    };
    const TextureSubresourceData ui_subresource{white_pixel, 0, 0};
    const TextureData ui_texture_data{
        ui_texture_desc,
        std::span<const TextureSubresourceData>{&ui_subresource, 1},
    };

    FrameContext frame_context(*device, {.frames_in_flight = 2});
    RenderGraph upload_graph(*device);
    TextureUploader texture_uploader(*device);
    if (!check(frame_context.begin_frame(), "begin_texture_upload_frame")) {
        return 2;
    }
    const auto upload_result =
        texture_uploader.upload_texture(frame_context, upload_graph, ui_texture_data, "Editor.ImGui.WhiteTexture");
    if (!check(upload_result, "upload_texture")) {
        (void)frame_context.abort_frame();
        return 3;
    }
    GpuTexture ui_texture = upload_result.value.texture;
    const auto upload_execution = upload_graph.execute(frame_context);
    if (!check(upload_execution, "execute_texture_upload")) {
        (void)frame_context.abort_frame();
        (void)device->wait_idle();
        (void)texture_uploader.destroy_texture(ui_texture);
        return 4;
    }
    if (upload_execution.value.completion.is_valid() &&
        !check(device->wait(upload_execution.value.completion, std::numeric_limits<uint64_t>::max()),
               "wait_texture_upload")) {
        (void)frame_context.abort_frame();
        (void)device->wait_idle();
        (void)texture_uploader.destroy_texture(ui_texture);
        return 5;
    }
    if (!check(frame_context.end_frame(), "end_texture_upload_frame")) {
        (void)device->wait_idle();
        (void)texture_uploader.destroy_texture(ui_texture);
        return 6;
    }
    upload_graph.reset();

    const auto sampler_result = device->create_sampler(SamplerDesc{
        aeg::rhi::Filter::kNearest,
        aeg::rhi::Filter::kNearest,
        aeg::rhi::Filter::kNearest,
        aeg::rhi::AddressMode::kClampToEdge,
        aeg::rhi::AddressMode::kClampToEdge,
        aeg::rhi::AddressMode::kClampToEdge,
        0.0f,
        0.0f,
        1.0f,
        false,
        1.0f,
        aeg::rhi::DebugName{"Editor.ImGui.WhiteSampler"},
    });
    if (!check(sampler_result, "create_sampler")) {
        (void)device->wait_idle();
        (void)texture_uploader.destroy_texture(ui_texture);
        return 7;
    }
    aeg::rhi::SamplerHandle sampler = sampler_result.value;

    ImGuiTextureRegistry texture_registry;
    ImGuiTextureBinding texture_binding{};
    texture_binding.texture = ui_texture.texture;
    texture_binding.sampled_view = ui_texture.sampled_view;
    texture_binding.sampler = sampler;
    texture_binding.texture_desc = ui_texture_desc;
    texture_binding.initial_state = make_copy_destination_state();
    const auto texture_handle_result = texture_registry.register_texture(texture_binding);
    if (!check(texture_handle_result, "register_texture")) {
        (void)device->wait_idle();
        (void)device->destroy_sampler(sampler);
        (void)texture_uploader.destroy_texture(ui_texture);
        return 8;
    }

    TextureDesc target_desc{};
    target_desc.extent = {kTargetWidth, kTargetHeight, 1};
    target_desc.format = Format::kRgba8Unorm;
    target_desc.mip_levels = 1;
    target_desc.array_layers = 1;
    target_desc.capabilities = TextureCapability::kColorAttachment | TextureCapability::kTransferSource;
    target_desc.memory = MemoryClass::kDeviceLocal;
    target_desc.initial_usage = ResourceUsage::kUndefined;
    target_desc.initial_stages = PipelineStage::kAllCommands;
    target_desc.initial_queue = QueueClass::kAuto;
    target_desc.name = aeg::rhi::DebugName{"Editor.ImGui.Target"};
    const auto target_result = device->create_texture(target_desc);
    if (!check(target_result, "create_target_texture")) {
        (void)device->wait_idle();
        (void)device->destroy_sampler(sampler);
        (void)texture_uploader.destroy_texture(ui_texture);
        return 9;
    }
    aeg::rhi::TextureHandle target_texture = target_result.value;

    const auto target_view_result = device->create_texture_view(TextureViewDesc{
        target_texture,
        TextureViewKind::kColorAttachment,
        TextureViewDimension::kTex2D,
        target_desc.format,
        aeg::rhi::SubresourceRange{aeg::rhi::Aspect::kColor, 0, 1, 0, 1},
        {},
        aeg::rhi::DebugName{"Editor.ImGui.TargetView"},
    });
    if (!check(target_view_result, "create_target_view")) {
        (void)device->wait_idle();
        (void)device->destroy_texture(target_texture);
        (void)device->destroy_sampler(sampler);
        (void)texture_uploader.destroy_texture(ui_texture);
        return 10;
    }
    aeg::rhi::TextureViewHandle target_view = target_view_result.value;

    PipelineRegistry registry;
    if (!check(aeg::renderer::register_render_pipeline(registry), "register_render_pipeline")) {
        return 11;
    }
    ImGuiRenderer imgui_renderer(*device, texture_registry);
    if (!check(imgui_renderer.initialize(), "initialize_imgui_renderer")) {
        return 12;
    }

    RenderGraph graph(*device);
    const auto target_resource_result = graph.import_texture(target_texture, target_view, target_desc,
                                                             ResourceState{
                                                                 ResourceUsage::kUndefined,
                                                                 aeg::rhi::ShaderStage::kAll,
                                                                 QueueClass::kAuto,
                                                                 false,
                                                                 PipelineStage::kAllCommands,
                                                             });
    if (!check(target_resource_result, "import_target")) {
        return 14;
    }
    const RenderGraphTexture target_resource = target_resource_result.value;

    if (!check(frame_context.begin_frame(), "begin_imgui_frame")) {
        return 15;
    }
    const auto readback_allocation_result = frame_context.allocate_readback(kReadbackSize, 256);
    if (!check(readback_allocation_result, "allocate_readback")) {
        (void)frame_context.abort_frame();
        return 16;
    }
    const FrameAllocation readback_allocation = readback_allocation_result.value;
    const auto readback_resource_result = graph.import_buffer(
        readback_allocation.buffer, make_readback_desc(readback_allocation.buffer_size), make_host_read_state());
    if (!check(readback_resource_result, "import_readback")) {
        (void)frame_context.abort_frame();
        return 17;
    }
    const RenderGraphBuffer readback_resource = readback_resource_result.value;

    const auto clear_pass_result = graph.add_pass(
        "Editor.ImGui.ClearTarget", QueueClass::kGraphics,
        [target_resource](aeg::render_graph::PassBuilder& builder) {
            return builder.use_color_attachment(target_resource, LoadOp::kClear, StoreOp::kStore,
                                                ClearColorValue{0.0f, 0.0f, 0.0f, 1.0f},
                                                ResourceUsage::kColorAttachmentWrite);
        },
        [](aeg::render_graph::PassExecutionContext& context) {
            if (!context.begin_rendering()) {
                return Result::failure(ResultCode::kInvalidState, "clear pass begin rendering failed");
            }
            return context.end_rendering();
        });
    if (!check(clear_pass_result, "add_clear_pass")) {
        (void)frame_context.abort_frame();
        return 18;
    }

    ImGuiDrawPacket packet{};
    packet.display_width = static_cast<float>(kTargetWidth);
    packet.display_height = static_cast<float>(kTargetHeight);
    packet.framebuffer_scale_x = 1.0f;
    packet.framebuffer_scale_y = 1.0f;
    packet.frame_number = 1;
    packet.vertices = {
        ImGuiDrawVertex{8.0f, 8.0f, 0.0f, 0.0f, 0xffffffffu},
        ImGuiDrawVertex{56.0f, 8.0f, 1.0f, 0.0f, 0xffffffffu},
        ImGuiDrawVertex{32.0f, 56.0f, 0.5f, 1.0f, 0xffffffffu},
    };
    packet.indices = {0, 1, 2};
    packet.commands = {
        ImGuiDrawCommand{
            texture_handle_result.value,
            0.0f,
            0.0f,
            static_cast<float>(kTargetWidth),
            static_cast<float>(kTargetHeight),
            3,
            0,
            0,
        },
    };
    const ImGuiRenderTargetDesc imgui_target{
        RenderResourceId("SceneColor"),
        target_desc.format,
        {kTargetWidth, kTargetHeight},
        ResourceUsage::kCopySource,
    };
    ImGuiDrawPacket invalid_packet = packet;
    invalid_packet.commands[0].index_offset = 2;
    const Result invalid_packet_result =
        imgui_renderer.prepare_frame(frame_context, graph, invalid_packet, imgui_target);
    if (invalid_packet_result || invalid_packet_result.code != ResultCode::kInvalidArgument) {
        (void)frame_context.abort_frame();
        return 19;
    }
    ImGuiDrawPacket missing_texture_packet = packet;
    ++missing_texture_packet.commands[0].texture.generation;
    const auto missing_texture = imgui_renderer.prepare_frame(frame_context, graph, missing_texture_packet, imgui_target);
    if (missing_texture || missing_texture.code != ResultCode::kNotFound || imgui_renderer.has_prepared_frame()) return 74;
    auto invalid_texture_binding = texture_binding;
    invalid_texture_binding.texture_desc.extent.width = 0;
    const auto invalid_texture = texture_registry.register_texture(invalid_texture_binding);
    if (!check(invalid_texture, "register_invalid_texture_description")) return 74;
    auto invalid_texture_packet = packet;
    invalid_texture_packet.commands[0].texture = invalid_texture.value;
    const auto invalid_texture_preparation = imgui_renderer.prepare_frame(frame_context, graph, invalid_texture_packet, imgui_target);
    if (invalid_texture_preparation || invalid_texture_preparation.code != ResultCode::kInvalidArgument ||
        imgui_renderer.has_prepared_frame() || !check(texture_registry.unregister_texture(invalid_texture.value),
            "unregister_invalid_texture_description")) return 74;
    if (!check(imgui_renderer.prepare_frame(frame_context, graph, packet, imgui_target), "prepare_imgui_frame")) {
        (void)frame_context.abort_frame();
        return 19;
    }

    if (!check(aeg::editor::detail::ImGuiRenderExecution::build_render_graph(imgui_renderer, graph, target_resource),
               "build_imgui_overlay")) {
        (void)frame_context.abort_frame();
        return 20;
    }

    const auto readback_pass_result = graph.add_pass(
        "Editor.ImGui.Readback", QueueClass::kCopy,
        [target_resource, readback_resource, readback_allocation](aeg::render_graph::PassBuilder& builder) {
            if (const Result result = builder.read(target_resource, ResourceUsage::kCopySource, PipelineStage::kCopy,
                                                   aeg::rhi::ShaderStage::kAll);
                !result) {
                return result;
            }
            if (const Result result = builder.write(readback_resource, ResourceUsage::kCopyDestination,
                                                    PipelineStage::kCopy, aeg::rhi::ShaderStage::kAll,
                                                    aeg::render_graph::RenderGraphBufferRange{
                                                        readback_allocation.offset,
                                                        kReadbackSize,
                                                    },
                                                    true);
                !result) {
                return result;
            }
            builder.set_side_effects();
            return Result::success();
        },
        [target_resource, readback_resource, readback_allocation](aeg::render_graph::PassExecutionContext& context) {
            return context.copy_texture_to_buffer(
                target_resource, TextureSubresourceLayers{aeg::rhi::Aspect::kColor, 0, 0, 1}, Offset3D{0, 0, 0},
                Extent3D{kTargetWidth, kTargetHeight, 1}, readback_resource, readback_allocation.offset, 0, 0);
        });
    if (!check(readback_pass_result, "add_readback_pass")) {
        (void)frame_context.abort_frame();
        return 21;
    }

    if (!check(graph.compile(), "compile_imgui_graph") || aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).pass_count < 3 ||
        aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).culled_pass_count != 0 || aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).barrier_count == 0 ||
        aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).queue_dependency_count == 0) {
        (void)frame_context.abort_frame();
        return 22;
    }
    const auto& batches = aeg::render_graph::detail::RenderGraphExecution::plan(graph).batches;
    const bool has_clear_upload_imgui_batches =
            batches.size() >= 3 && batches[0].queue == QueueClass::kGraphics && batches[0].pass_indices.size() == 1 &&
            batches[1].queue == QueueClass::kCopy && batches[1].pass_indices.size() >= 3 &&
            batches[2].queue == QueueClass::kGraphics && !batches[2].pass_indices.empty() &&
            !batches[2].dependencies.empty();
    if (!has_clear_upload_imgui_batches) {
        (void)frame_context.abort_frame();
        return 22;
    }
    unsigned actual_overlays = 0;
    for (const auto& pass : aeg::render_graph::detail::RenderGraphExecution::plan(graph).pass_plans) {
        const auto actual = aeg::render_graph::detail::RenderGraphExecution::inspect_pass(graph, pass.pass);
        if (!check(actual, "inspect_overlay_pass")) return 75;
        if (actual.value.name != "Editor.ImGui.Draw") continue;
        if (!pass.live || pass.feature_work || actual.value.queue != QueueClass::kGraphics) return 75;
        ++actual_overlays;
    }
    if (actual_overlays != 1) return 75;

    const auto execution_result = graph.execute(frame_context);
    if (!check(execution_result, "execute_imgui_graph")) {
        return 23;
    }
    if (execution_result.value.draw_call_count == 0) return 75;
    if (execution_result.value.completion.is_valid() &&
        !check(device->wait(execution_result.value.completion, std::numeric_limits<uint64_t>::max()),
               "wait_imgui_graph")) {
        return 24;
    }

    std::array<std::byte, kReadbackSize> pixels{};
    if (!check(frame_context.read_readback(readback_allocation, pixels), "read_imgui_pixels")) {
        return 25;
    }
    bool has_colored_pixel = false;
    for (std::size_t pixel = 0; pixel < pixels.size(); pixel += 4) {
        if (pixels[pixel + 0] != std::byte{0} || pixels[pixel + 1] != std::byte{0} ||
            pixels[pixel + 2] != std::byte{0}) {
            has_colored_pixel = true;
            break;
        }
    }
    const std::size_t center_pixel =
        (static_cast<std::size_t>(kTargetHeight / 2) * kTargetWidth + kTargetWidth / 2) * 4;
    if (!has_colored_pixel || pixels[center_pixel + 0] == std::byte{0} || pixels[center_pixel + 1] == std::byte{0} ||
        pixels[center_pixel + 2] == std::byte{0}) {
        return 26;
    }
    for (const auto offset : {std::size_t{0}, std::size_t{(kTargetWidth - 1) * 4},
        std::size_t{(kTargetHeight - 1) * kTargetWidth * 4}, std::size_t{kReadbackSize - 4}})
        if (pixels[offset] != std::byte{0} || pixels[offset + 1] != std::byte{0} ||
            pixels[offset + 2] != std::byte{0} || pixels[offset + 3] != std::byte{255}) return 75;
    aeg::feature::FeatureRegistry scene_features{aeg::feature::test::clock_test_host()};
    if (!check(aeg::renderer::register_scene_render_profiles(scene_features), "register_scene_profiles")) return 73;
    for (const auto id : {"Renderer.Scene.Opaque", "Renderer.Scene.Forward", "Renderer.Scene.Deferred"})
        if (!scene_features.find_definition(aeg::feature::FeatureDefinitionId(id))) return 73;
    const bool retired_imgui_absent = !scene_features.find_definition(
        aeg::feature::FeatureDefinitionId("Editor.ImGuiRenderFeature"));
    std::cout << "ImGui GPU pixels verified; three real Scene definitions present, retired ImGui identity absent="
              << retired_imgui_absent << '\n';

    if (!check(frame_context.end_frame(), "end_imgui_frame")) {
        return 27;
    }
    // A failed next preparation must not expose the previous frame's packet
    // to render-graph construction when the application discards this frame.
    if (!check(frame_context.begin_frame(), "begin_rejected_imgui_frame")) return 31;
    const auto rejected_next_frame = imgui_renderer.prepare_frame(frame_context, graph, invalid_packet, imgui_target);
    if (rejected_next_frame || imgui_renderer.has_prepared_frame()) {
        (void)frame_context.abort_frame();
        return 32;
    }
    if (!check(frame_context.abort_frame(), "abort_rejected_imgui_frame")) return 33;
    {
        RenderGraph empty_graph(*device);
        const auto stale_target = empty_graph.create_texture(target_desc);
        if (!check(stale_target, "create_stale_overlay_target")) return 76;
        empty_graph.reset();
        const auto current_target = empty_graph.import_texture(target_texture, target_view, target_desc,
            {ResourceUsage::kCopySource, aeg::rhi::ShaderStage::kAll, QueueClass::kCopy, false, PipelineStage::kCopy});
        if (!check(current_target, "import_empty_overlay_target") ||
            !check(frame_context.begin_frame(), "begin_empty_overlay_frame")) return 76;
        auto empty_packet = packet;
        empty_packet.clear();
        if (!check(imgui_renderer.prepare_frame(frame_context, empty_graph, empty_packet, imgui_target),
                "prepare_empty_overlay") || !imgui_renderer.has_prepared_frame() ||
            !check(empty_graph.compile(), "compile_empty_overlay_baseline")) return 76;
        const auto baseline = aeg::render_graph::detail::RenderGraphExecution::compile_info(empty_graph);
        for (const auto rejected_target : {RenderGraphTexture{}, target_resource, stale_target.value}) {
            const auto rejected = aeg::editor::detail::ImGuiRenderExecution::build_render_graph(
                imgui_renderer, empty_graph, rejected_target);
            if (rejected || !check(empty_graph.compile(), "compile_rejected_overlay_target")) return 76;
            const auto restored = aeg::render_graph::detail::RenderGraphExecution::compile_info(empty_graph);
            if (restored.pass_count != baseline.pass_count || restored.resource_count != baseline.resource_count ||
                !imgui_renderer.has_prepared_frame()) return 76;
        }
        const auto empty_clear = empty_graph.add_pass("Editor.ImGui.EmptyClear", QueueClass::kGraphics,
            [target = current_target.value](aeg::render_graph::PassBuilder& pass) {
                return pass.use_color_attachment(target, LoadOp::kClear, StoreOp::kStore, {.2f, .4f, .6f, 1.f});
            }, [](aeg::render_graph::PassExecutionContext& command) {
                if (const auto result = command.begin_rendering(); !result) return result;
                return command.end_rendering();
            });
        if (!check(empty_clear, "add_empty_overlay_clear") ||
            !check(aeg::editor::detail::ImGuiRenderExecution::build_render_graph(imgui_renderer, empty_graph, current_target.value),
                "build_empty_overlay") || !check(empty_graph.compile(), "compile_empty_overlay_without_readback")) return 77;
        const auto empty_info = aeg::render_graph::detail::RenderGraphExecution::compile_info(empty_graph);
        if (empty_info.pass_count != 2 || empty_info.culled_pass_count != 0) return 77;
        unsigned empty_overlays = 0;
        uint32_t clear_order = std::numeric_limits<uint32_t>::max(), overlay_order = clear_order;
        bool loads_target = false;
        for (const auto& pass : aeg::render_graph::detail::RenderGraphExecution::plan(empty_graph).pass_plans) {
            const auto actual = aeg::render_graph::detail::RenderGraphExecution::inspect_pass(empty_graph, pass.pass);
            if (!check(actual, "inspect_empty_overlay") || !pass.live || pass.feature_work) return 77;
            if (pass.pass == empty_clear.value) clear_order = pass.topological_index;
            if (actual.value.name != "Editor.ImGui.Draw") continue;
            ++empty_overlays;
            overlay_order = pass.topological_index;
            for (const auto& barrier : pass.barriers)
                if (barrier.resource_kind == aeg::render_graph::RenderGraphResourceKind::kTexture &&
                    barrier.resource_index == current_target.value.index &&
                    barrier.resource_generation == current_target.value.generation &&
                    barrier.phase == aeg::render_graph::RenderGraphBarrierPhase::kBeforePass &&
                    barrier.access == aeg::render_graph::ResourceAccess::kReadWrite && !barrier.discard_contents)
                    loads_target = true;
        }
        if (empty_overlays != 1 || !loads_target || clear_order >= overlay_order) return 77;
        const auto allocation = frame_context.allocate_readback(kReadbackSize, 256);
        if (!check(allocation, "allocate_empty_overlay_readback")) return 78;
        const auto destination = empty_graph.import_buffer(allocation.value.buffer,
            make_readback_desc(allocation.value.buffer_size), make_host_read_state());
        if (!check(destination, "import_empty_overlay_readback")) return 78;
        const auto copied = empty_graph.add_pass("Editor.ImGui.EmptyReadback", QueueClass::kCopy,
            [target = current_target.value, buffer = destination.value, offset = allocation.value.offset](aeg::render_graph::PassBuilder& pass) {
                if (const auto result = pass.read(target, ResourceUsage::kCopySource, PipelineStage::kCopy); !result) return result;
                pass.set_side_effects();
                return pass.write(buffer, ResourceUsage::kCopyDestination, PipelineStage::kCopy,
                    aeg::rhi::ShaderStage::kAll, {offset, kReadbackSize});
            }, [target = current_target.value, buffer = destination.value, offset = allocation.value.offset](aeg::render_graph::PassExecutionContext& command) {
                return command.copy_texture_to_buffer(target, {aeg::rhi::Aspect::kColor, 0, 0, 1}, {},
                    {kTargetWidth, kTargetHeight, 1}, buffer, offset, 0, 0);
            });
        if (!check(copied, "add_empty_overlay_readback")) return 78;
        const auto empty_execution = empty_graph.execute(frame_context);
        if (!check(empty_execution, "execute_empty_overlay") || empty_execution.value.draw_call_count != 0 ||
            !check(device->wait_idle(), "wait_empty_overlay")) return 78;
        std::array<std::byte, kReadbackSize> empty_pixels{};
        if (!check(frame_context.read_readback(allocation.value, empty_pixels), "read_empty_overlay_pixels")) return 78;
        for (std::size_t offset = 0; offset < empty_pixels.size(); offset += 4)
            if (empty_pixels[offset] != std::byte{51} || empty_pixels[offset + 1] != std::byte{102} ||
                empty_pixels[offset + 2] != std::byte{153} || empty_pixels[offset + 3] != std::byte{255}) return 78;
        if (!check(frame_context.end_frame(), "end_empty_overlay_frame")) return 78;
        empty_graph.reset();
    }
    if (!check(imgui_renderer.release_prepared_frame(), "release_prepared_imgui_frame") ||
        !check(device->wait_idle(), "wait_before_imgui_resource_replacement") ||
        !check(texture_uploader.destroy_texture(ui_texture), "destroy_ui_texture_before_renderer_shutdown")) {
        return 28;
    }
    if (!check(imgui_renderer.shutdown(), "shutdown_imgui_renderer")) {
        return 29;
    }
    graph.reset();
    if (!check(texture_registry.unregister_texture(texture_handle_result.value), "unregister_texture")) {
        return 29;
    }
    if (!check(device->wait_idle(), "wait_idle_cleanup") ||
        !check(destroy_target(*device, target_view, target_texture), "destroy_target") ||
        !check(device->destroy_sampler(sampler), "destroy_sampler")) {
        return 30;
    }
    return retired_imgui_absent ? 0 : 73;
}
