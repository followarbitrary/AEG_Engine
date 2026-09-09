/**
 * @file editor_imgui_renderer_smoke.cpp
 * @brief Executes the backend-neutral ImGui RenderFeature and verifies pixels.
 */
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>

#include "frame_context.hpp"
#include "imgui_renderer.hpp"
#include "imgui_texture_registry.hpp"
#include "pipeline.hpp"
#include "render_feature_runtime.hpp"
#include "render_graph.hpp"
#include "renderer.hpp"
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
using aeg::pipeline::PipelineCompileInput;
using aeg::pipeline::PipelineCompiler;
using aeg::pipeline::PipelineId;
using aeg::pipeline::PipelineInstanceId;
using aeg::pipeline::PipelineRegistry;
using aeg::pipeline::PipelineScope;
using aeg::pipeline::RenderResourceId;
using aeg::render_graph::FrameAllocation;
using aeg::render_graph::FrameContext;
using aeg::render_graph::RenderGraph;
using aeg::render_graph::RenderGraphBuffer;
using aeg::render_graph::RenderGraphFeatureRuntime;
using aeg::render_graph::RenderGraphFeatureRuntimeDesc;
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
    if (!check(imgui_renderer.initialize(), "initialize_imgui_renderer") ||
        !check(aeg::editor::register_imgui_render_feature(registry, imgui_renderer), "register_imgui_render_feature")) {
        return 12;
    }

    const auto compiled_plan = PipelineCompiler{}.compile(PipelineCompileInput{
        PipelineId("RenderPipeline"),
        PipelineScope::kFrame,
        0,
        &registry,
        PipelineInstanceId("Editor.ImGuiRendererSmoke"),
        nullptr,
    });
    if (!check(compiled_plan, "compile_render_pipeline")) {
        return 13;
    }
    aeg::pipeline::PipelinePlan plan = std::move(compiled_plan.value);

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
    if (!check(imgui_renderer.prepare_frame(frame_context, graph, packet, imgui_target), "prepare_imgui_frame")) {
        (void)frame_context.abort_frame();
        return 19;
    }

    const std::array<aeg::render_graph::RenderGraphFeatureTextureBinding, 1> feature_textures{{
        {RenderResourceId("SceneColor"), target_resource},
    }};
    const RenderGraphFeatureRuntimeDesc runtime_desc{
        {},
        feature_textures,
        nullptr,
        nullptr,
    };
    RenderGraphFeatureRuntime runtime(graph, plan, registry, runtime_desc);
    if (!check(runtime.build(), "build_imgui_feature")) {
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

    if (!check(graph.compile(), "compile_imgui_graph") || graph.compile_info().pass_count < 3 ||
        graph.compile_info().culled_pass_count != 0 || graph.compile_info().barrier_count == 0 ||
        graph.compile_info().queue_dependency_count == 0) {
        (void)frame_context.abort_frame();
        return 22;
    }

    const auto execution_result = graph.execute(frame_context);
    if (!check(execution_result, "execute_imgui_graph")) {
        return 23;
    }
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

    if (!check(frame_context.end_frame(), "end_imgui_frame")) {
        return 27;
    }
    if (!check(imgui_renderer.shutdown(), "shutdown_imgui_renderer")) {
        return 28;
    }
    graph.reset();
    if (!check(texture_registry.unregister_texture(texture_handle_result.value), "unregister_texture")) {
        return 29;
    }
    if (!check(device->wait_idle(), "wait_idle_cleanup") ||
        !check(destroy_target(*device, target_view, target_texture), "destroy_target") ||
        !check(device->destroy_sampler(sampler), "destroy_sampler") ||
        !check(texture_uploader.destroy_texture(ui_texture), "destroy_ui_texture")) {
        return 30;
    }
    return 0;
}
