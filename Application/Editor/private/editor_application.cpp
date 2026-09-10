/**
 * @file editor_application.cpp
 * @brief Minimal editor application assembled from the public engine contracts.
 */
#include <imgui.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "application_platform.hpp"
#include "editor_imgui.hpp"
#include "engine_frame_orchestrator.hpp"
#include "frame_context.hpp"
#include "imgui_renderer.hpp"
#include "imgui_texture_registry.hpp"
#include "input_system.hpp"
#include "pipeline.hpp"
#include "render_graph.hpp"
#include "renderer.hpp"
#include "resource_uploader.hpp"
#include "rhi.hpp"
#include "texture_uploader.hpp"

namespace {
using aeg::editor::EditorImGuiFrameDesc;
using aeg::editor::EditorImGuiHost;
using aeg::editor::ImGuiDrawPacket;
using aeg::editor::ImGuiFontAtlasData;
using aeg::editor::ImGuiRenderTargetDesc;
using aeg::editor::ImGuiRenderer;
using aeg::editor::ImGuiTextureBinding;
using aeg::editor::ImGuiTextureHandle;
using aeg::editor::ImGuiTextureRegistry;
using aeg::foundation::Result;
using aeg::foundation::ResultCode;
using aeg::input::InputSystem;
using aeg::pipeline::PipelineRegistry;
using aeg::pipeline::PipelineScheduler;
using aeg::pipeline::PipelineScope;
using aeg::pipeline::PipelineScopeHost;
using aeg::platform::IWindow;
using aeg::platform::IWindowSystem;
using aeg::platform::WindowEvent;
using aeg::platform::WindowEventType;
using aeg::render_graph::FrameAllocation;
using aeg::render_graph::FrameContext;
using aeg::render_graph::RenderGraph;
using aeg::resource::GpuTexture;
using aeg::resource::TextureData;
using aeg::resource::TextureSubresourceData;
using aeg::resource::TextureUploader;
using aeg::rhi::Device;
using aeg::rhi::DeviceCreateDesc;
using aeg::rhi::ClearColorValue;
using aeg::rhi::Format;
using aeg::rhi::MemoryClass;
using aeg::rhi::PipelineStage;
using aeg::rhi::PresentMode;
using aeg::rhi::QueueClass;
using aeg::rhi::ResourceState;
using aeg::rhi::ResourceUsage;
using aeg::rhi::SamplerDesc;
using aeg::rhi::SurfaceDesc;
using aeg::rhi::SurfaceHandle;
using aeg::rhi::SwapchainDesc;
using aeg::rhi::SwapchainHandle;
using aeg::rhi::SwapchainInfo;
using aeg::rhi::TextureCapability;
using aeg::rhi::TextureDesc;

bool report_result(const Result& result, std::string_view operation) {
    if (result) {
        return true;
    }
    std::fprintf(stderr, "%.*s failed [%s]: %s\n", static_cast<int>(operation.size()), operation.data(),
                 aeg::foundation::result_code_name(result.code), result.message.c_str());
    return false;
}

template <typename T>
bool report_result(const aeg::foundation::ResultValue<T>& result, std::string_view operation) {
    return report_result(result.result, operation);
}

class MainEditorPanel final : public aeg::editor::EditorPanel {
public:
    explicit MainEditorPanel(const uint64_t& frame_count) noexcept : frame_count_(frame_count) {}

    void build_ui() override {
        ImGui::Begin("AEG Editor");
        ImGui::TextUnformatted("AEG Engine Editor");
        ImGui::Text("Frame: %llu", static_cast<unsigned long long>(frame_count_));
        ImGui::TextUnformatted("ImGui is rendered through RHI and RenderGraph.");
        ImGui::End();
    }

private:
    const uint64_t& frame_count_;
};

class EditorApplication final {
public:
    EditorApplication() = default;
    ~EditorApplication() { cleanup(); }

    EditorApplication(const EditorApplication&) = delete;
    EditorApplication& operator=(const EditorApplication&) = delete;

    [[nodiscard]] bool initialize() {
        auto window_system_result = aeg::application::create_win32_window_system();
        if (!report_result(window_system_result, "create_win32_window_system")) {
            return false;
        }
        window_system_ = std::move(window_system_result.value);

        aeg::platform::WindowDesc window_desc{};
        window_desc.title = "AEG Engine Editor";
        window_desc.extent = {1280, 720};
        window_desc.visible = true;
        const auto window_result = window_system_->create_window(window_desc);
        if (!report_result(window_result, "create_editor_window")) {
            return false;
        }
        window_ = window_result.value;

        const DeviceCreateDesc device_desc{
            aeg::rhi::BackendType::kVulkan,
            "AEG Editor",
            std::numeric_limits<uint32_t>::max(),
            true,
            true,
        };
        auto device_result = aeg::rhi::create_device(device_desc);
        if (!report_result(device_result, "create_device")) {
            return false;
        }
        device_ = std::move(device_result.value);

        const aeg::platform::WindowSurfaceBinding surface_binding = window_->surface_binding();
        if (!surface_binding.is_valid()) {
            std::fprintf(stderr, "editor window has no valid surface binding\n");
            return false;
        }
        SurfaceDesc surface_desc{};
        surface_desc.native_display_handle = surface_binding.display_handle;
        surface_desc.native_window_handle = surface_binding.window_handle;
        surface_desc.name = aeg::rhi::DebugName{"Editor.Surface"};
        const auto surface_result = device_->create_surface(surface_desc);
        if (!report_result(surface_result, "create_surface")) {
            return false;
        }
        surface_ = surface_result.value;

        if (!create_swapchain()) {
            return false;
        }

        frame_context_ = std::make_unique<FrameContext>(*device_);
        render_graph_ = std::make_unique<RenderGraph>(*device_);
        texture_uploader_ = std::make_unique<TextureUploader>(*device_);

        aeg::editor::EditorImGuiDesc imgui_desc{};
        imgui_desc.platform_services = &window_system_->platform_services();
        if (!report_result(imgui_host_.initialize(imgui_desc), "initialize_editor_imgui")) {
            return false;
        }
        if (!upload_font_texture()) {
            return false;
        }
        if (!create_font_sampler()) {
            return false;
        }
        if (!register_font_texture()) {
            return false;
        }
        if (!report_result(imgui_host_.set_font_texture(font_texture_handle_), "set_imgui_font_texture")) {
            return false;
        }

        imgui_renderer_ = std::make_unique<ImGuiRenderer>(*device_, texture_registry_);
        if (!report_result(imgui_renderer_->initialize(), "initialize_imgui_renderer")) {
            return false;
        }
        if (!report_result(aeg::renderer::register_render_pipeline(registry_), "register_render_pipeline")) {
            return false;
        }
        if (!report_result(aeg::editor::register_imgui_render_feature(registry_, *imgui_renderer_),
                           "register_imgui_render_feature")) {
            return false;
        }

        if (!report_result(frame_host_.bind_pipeline(aeg::pipeline::PipelineId("RenderPipeline")),
                           "bind_editor_render_pipeline")) {
            return false;
        }
        orchestrator_ = std::make_unique<aeg::engine::EngineFrameOrchestrator>(
            registry_, scheduler_, *frame_context_, *render_graph_);
        if (!report_result(orchestrator_->register_pipeline(frame_host_), "register_editor_frame_pipeline")) {
            return false;
        }
        if (!report_result(orchestrator_->initialize(), "initialize_engine_frame_orchestrator")) {
            return false;
        }

        auto panel = std::make_unique<MainEditorPanel>(frame_count_);
        if (!report_result(imgui_host_.register_panel(std::move(panel)), "register_main_editor_panel")) {
            return false;
        }
        last_frame_time_ = std::chrono::steady_clock::now();
        initialized_ = true;
        return true;
    }

    [[nodiscard]] int run(uint32_t frame_limit, bool regression_mode) {
        regression_mode_ = regression_mode;
        while (!close_requested()) {
            if (frame_limit != 0 && frame_count_ >= frame_limit) {
                break;
            }

            Result event_result = Result::success();
            const Result poll_result = window_system_->poll_events([this, &event_result](const WindowEvent& event) {
                if (event.window != window_->id()) {
                    return;
                }
                if (event.type == WindowEventType::kWindowResized) {
                    resize_pending_ = true;
                }
                if (event_result) {
                    event_result = input_system_.submit_event(event);
                }
                if (event_result) {
                    event_result = imgui_host_.submit_event(event);
                }
            });
            if (!report_result(poll_result, "poll_editor_events") || !report_result(event_result, "submit_editor_event")) {
                return 1;
            }
            if (close_requested()) {
                break;
            }

            if (regression_mode_ &&
                (frame_count_ == 10 || frame_count_ == 30 || frame_count_ == 50)) {
                const aeg::platform::WindowExtent extent =
                        frame_count_ == 10 ? aeg::platform::WindowExtent{1600, 900}
                        : frame_count_ == 30 ? aeg::platform::WindowExtent{800, 600}
                                              : aeg::platform::WindowExtent{1280, 720};
                if (!report_result(window_->set_extent(extent), "resize_regression_window")) {
                    return 1;
                }
                resize_pending_ = true;
            }

            if (resize_pending_ && !recreate_swapchain()) {
                return 1;
            }

            if (!report_result(input_system_.begin_frame(), "begin_input_frame")) {
                return 1;
            }
            const auto finish_input_frame = [this]() noexcept { input_system_.end_frame(); };

            if (window_->state().minimized) {
                finish_input_frame();
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                continue;
            }

            const auto now = std::chrono::steady_clock::now();
            float delta_seconds = std::chrono::duration<float>(now - last_frame_time_).count();
            last_frame_time_ = now;
            if (!(delta_seconds > 0.0f) || delta_seconds > 0.25f) {
                delta_seconds = 1.0f / 60.0f;
            }

            const aeg::platform::WindowState window_state = window_->state();
            const EditorImGuiFrameDesc imgui_frame_desc{
                window_->id(),
                window_state.extent,
                delta_seconds,
                1.0f,
                1.0f,
            };
            bool imgui_frame_active = false;
            if (!report_result(imgui_host_.begin_frame(imgui_frame_desc), "begin_imgui_frame")) {
                finish_input_frame();
                return 1;
            }
            imgui_frame_active = true;
            if (!report_result(imgui_host_.build_panels(), "build_editor_panels")) {
                static_cast<void>(imgui_host_.end_frame());
                finish_input_frame();
                return 1;
            }
            if (!report_result(imgui_host_.end_frame(), "end_imgui_frame")) {
                finish_input_frame();
                return 1;
            }
            imgui_frame_active = false;

            ImGuiDrawPacket draw_packet;
            if (!report_result(imgui_host_.capture_draw_packet(draw_packet), "capture_imgui_draw_packet")) {
                finish_input_frame();
                return 1;
            }

            const Result frame_result = run_engine_frame(draw_packet);
            if (imgui_frame_active) {
                static_cast<void>(imgui_host_.end_frame());
            }
            finish_input_frame();
            if (!frame_result) {
                if (frame_result.code == ResultCode::kOutOfDate || frame_result.code == ResultCode::kSurfaceLost) {
                    resize_pending_ = true;
                    continue;
                }
                report_result(frame_result, "run_editor_frame");
                return 1;
            }
            ++frame_count_;
        }
        return 0;
    }

private:
    [[nodiscard]] bool close_requested() const noexcept {
        return window_ == nullptr || window_->state().close_requested;
    }

    [[nodiscard]] bool create_swapchain() {
        const aeg::platform::WindowState state = window_->state();
        SwapchainDesc desc{};
        desc.surface = surface_;
        desc.extent = {state.extent.width, state.extent.height};
        desc.format = Format::kUnknown;
        desc.present_mode = PresentMode::kVsync;
        desc.min_image_count = 2;
        desc.frames_in_flight = 2;
        desc.image_capabilities = TextureCapability::kColorAttachment | TextureCapability::kTransferSource;
        desc.name = aeg::rhi::DebugName{"Editor.Swapchain"};
        const auto swapchain_result = device_->create_swapchain(desc);
        if (!report_result(swapchain_result, "create_swapchain")) {
            return false;
        }
        swapchain_ = swapchain_result.value;
        return refresh_swapchain_info();
    }

    [[nodiscard]] bool refresh_swapchain_info() {
        const auto info_result = device_->get_swapchain_info(swapchain_);
        if (!report_result(info_result, "get_swapchain_info")) {
            return false;
        }
        swapchain_info_ = info_result.value;
        return swapchain_info_.format != Format::kUnknown && swapchain_info_.extent.width != 0 &&
               swapchain_info_.extent.height != 0;
    }

    [[nodiscard]] bool recreate_swapchain() {
        if (!report_result(device_->wait_idle(), "wait_idle_before_swapchain_recreate")) {
            return false;
        }
        const aeg::platform::WindowState state = window_->state();
        if (state.minimized || !state.extent.is_valid()) {
            return true;
        }
        SwapchainDesc desc{};
        desc.surface = surface_;
        desc.extent = {state.extent.width, state.extent.height};
        desc.present_mode = PresentMode::kVsync;
        desc.min_image_count = 2;
        desc.frames_in_flight = 2;
        desc.image_capabilities = TextureCapability::kColorAttachment | TextureCapability::kTransferSource;
        if (!report_result(device_->recreate_swapchain(swapchain_, desc), "recreate_swapchain")) {
            return false;
        }
        if (!refresh_swapchain_info()) {
            return false;
        }
        render_graph_->reset();
        resize_pending_ = false;
        return true;
    }

    [[nodiscard]] bool upload_font_texture() {
        ImGuiFontAtlasData atlas;
        if (!report_result(imgui_host_.capture_font_atlas(atlas), "capture_imgui_font_atlas") || !atlas.is_valid()) {
            std::fprintf(stderr, "captured ImGui font atlas is invalid\n");
            return false;
        }
        font_texture_desc_ = {};
        font_texture_desc_.extent = {atlas.width, atlas.height, 1};
        font_texture_desc_.format = Format::kRgba8Unorm;
        font_texture_desc_.mip_levels = 1;
        font_texture_desc_.array_layers = 1;
        font_texture_desc_.capabilities = TextureCapability::kTransferDestination | TextureCapability::kSampled;
        font_texture_desc_.memory = MemoryClass::kDeviceLocal;
        font_texture_desc_.initial_usage = ResourceUsage::kUndefined;
        font_texture_desc_.initial_stages = PipelineStage::kAllCommands;
        font_texture_desc_.initial_queue = QueueClass::kAuto;
        font_texture_desc_.name = aeg::rhi::DebugName{"Editor.ImGui.FontTexture"};

        const TextureSubresourceData subresource{
            std::span<const std::byte>{atlas.rgba8_pixels.data(), atlas.rgba8_pixels.size()},
            atlas.row_pitch,
            atlas.row_pitch * atlas.height,
        };
        const TextureData texture_data{
            font_texture_desc_,
            std::span<const TextureSubresourceData>{&subresource, 1},
        };

        RenderGraph upload_graph(*device_);
        if (!report_result(frame_context_->begin_frame(), "begin_font_upload_frame")) {
            return false;
        }
        const auto upload_result = texture_uploader_->upload_texture(
            *frame_context_, upload_graph, texture_data, "Editor.ImGui.FontTexture");
        if (!report_result(upload_result, "upload_imgui_font_texture")) {
            static_cast<void>(frame_context_->abort_frame());
            return false;
        }
        font_texture_ = upload_result.value.texture;

        const auto execution_result = upload_graph.execute(*frame_context_);
        if (!report_result(execution_result, "execute_font_upload")) {
            static_cast<void>(frame_context_->abort_frame());
            return false;
        }
        if (execution_result.value.completion.is_valid() &&
            !report_result(device_->wait(execution_result.value.completion, std::numeric_limits<uint64_t>::max()),
                           "wait_font_upload")) {
            static_cast<void>(frame_context_->abort_frame());
            return false;
        }
        if (!report_result(frame_context_->end_frame(), "end_font_upload_frame")) {
            return false;
        }
        return true;
    }

    [[nodiscard]] bool create_font_sampler() {
        const SamplerDesc desc{
            aeg::rhi::Filter::kLinear,
            aeg::rhi::Filter::kLinear,
            aeg::rhi::Filter::kLinear,
            aeg::rhi::AddressMode::kClampToEdge,
            aeg::rhi::AddressMode::kClampToEdge,
            aeg::rhi::AddressMode::kClampToEdge,
            0.0f,
            0.0f,
            1.0f,
            false,
            1.0f,
            aeg::rhi::DebugName{"Editor.ImGui.FontSampler"},
        };
        const auto sampler_result = device_->create_sampler(desc);
        if (!report_result(sampler_result, "create_imgui_font_sampler")) {
            return false;
        }
        font_sampler_ = sampler_result.value;
        return true;
    }

    [[nodiscard]] bool register_font_texture() {
        const ResourceState upload_state{
            ResourceUsage::kCopyDestination,
            aeg::rhi::ShaderStage::kAll,
            QueueClass::kCopy,
            false,
            PipelineStage::kCopy,
        };
        const ImGuiTextureBinding binding{
            font_texture_.sampled_view,
            font_sampler_,
            font_texture_.texture,
            font_texture_desc_,
            upload_state,
        };
        const auto texture_result = texture_registry_.register_texture(binding);
        if (!report_result(texture_result, "register_imgui_font_texture")) {
            return false;
        }
        font_texture_handle_ = texture_result.value;
        return true;
    }

    [[nodiscard]] Result run_engine_frame(const ImGuiDrawPacket& draw_packet) {
        const ImGuiRenderTargetDesc target{
            aeg::engine::kSwapchainImageResourceId,
            swapchain_info_.format,
            swapchain_info_.extent,
            regression_mode_ ? ResourceUsage::kCopySource : ResourceUsage::kPresent,
        };
        aeg::engine::EngineFrameDesc frame_desc{};
        frame_desc.fixed_tick = frame_count_;
        frame_desc.acquire_swapchain_image = true;
        frame_desc.swapchain = swapchain_;
        frame_desc.swapchain_image_desc = make_swapchain_image_desc();
        frame_desc.present_acquired_frame = true;
        frame_desc.user_data = this;
        FrameAllocation readback_allocation{};
        bool has_readback = false;
        frame_desc.render_feature_preparation = [this, &draw_packet, target, &readback_allocation, &has_readback](
                                                    const aeg::engine::EngineFramePreparationContext& context) {
            if (context.acquired_frame == nullptr) {
                return aeg::foundation::invalid_state("ImGui preparation requires an acquired swapchain image");
            }
            if (!context.swapchain_resource.is_valid()) {
                return aeg::foundation::invalid_state("ImGui preparation requires an imported swapchain resource");
            }
            const auto clear_result = context.render_graph.add_pass(
                    "Editor.ClearSwapchain", QueueClass::kGraphics,
                    [resource = context.swapchain_resource](aeg::render_graph::PassBuilder& builder) {
                        return builder.use_color_attachment(resource, aeg::rhi::LoadOp::kClear,
                                                            aeg::rhi::StoreOp::kStore,
                                                            ClearColorValue{0.0f, 0.0f, 0.0f, 1.0f},
                                                            ResourceUsage::kColorAttachmentWrite);
                    },
                    [](aeg::render_graph::PassExecutionContext& pass) {
                        if (const Result result = pass.begin_rendering(); !result) {
                            return result;
                        }
                        return pass.end_rendering();
                    });
            if (!clear_result) {
                return clear_result.result;
            }
            if (const Result result =
                        imgui_renderer_->prepare_frame(context.frame_context, context.render_graph, draw_packet, target);
                !result) {
                return result;
            }
            if (!regression_mode_) {
                return Result::success();
            }

            const uint64_t pixel_count = static_cast<uint64_t>(target.extent.width) * target.extent.height;
            const auto allocation_result = context.frame_context.allocate_readback(pixel_count * 4u, 256);
            if (!allocation_result) {
                return allocation_result.result;
            }
            readback_allocation = allocation_result.value;
            has_readback = true;

            aeg::rhi::BufferDesc readback_desc{};
            readback_desc.size = readback_allocation.buffer_size;
            readback_desc.capabilities = aeg::rhi::BufferCapability::kTransferSource |
                                         aeg::rhi::BufferCapability::kTransferDestination |
                                         aeg::rhi::BufferCapability::kVertex |
                                         aeg::rhi::BufferCapability::kIndex |
                                         aeg::rhi::BufferCapability::kUniform;
            readback_desc.memory = aeg::rhi::MemoryClass::kReadback;
            readback_desc.initial_usage = ResourceUsage::kHostRead;
            readback_desc.initial_stages = PipelineStage::kHost;
            readback_desc.initial_queue = QueueClass::kCopy;
            readback_desc.name = aeg::rhi::DebugName{"Editor.FramebufferReadback"};
            const auto readback_resource_result = context.render_graph.import_buffer(
                    readback_allocation.buffer, readback_desc,
                    ResourceState{ResourceUsage::kHostRead, aeg::rhi::ShaderStage::kAll, QueueClass::kCopy, false,
                                  PipelineStage::kHost});
            if (!readback_resource_result) {
                return readback_resource_result.result;
            }
            const auto readback_pass_result = context.render_graph.add_pass(
                    "Editor.FramebufferReadback", QueueClass::kCopy,
                    [swapchain_resource = context.swapchain_resource,
                     readback_resource = readback_resource_result.value,
                     readback_allocation, pixel_count](aeg::render_graph::PassBuilder& builder) {
                        if (const Result result = builder.read_to(
                                    swapchain_resource, ResourceUsage::kCopySource, PipelineStage::kCopy,
                                    aeg::rhi::ShaderStage::kAll, ResourceUsage::kCopySource);
                            !result) {
                            return result;
                        }
                        if (const Result result = builder.write_to(
                                    readback_resource, ResourceUsage::kCopyDestination, PipelineStage::kCopy,
                                    aeg::rhi::ShaderStage::kAll,
                                    ResourceUsage::kHostRead,
                                    aeg::render_graph::RenderGraphBufferRange{readback_allocation.offset,
                                                                              pixel_count * 4u},
                                    true);
                            !result) {
                            return result;
                        }
                        builder.set_side_effects();
                        return Result::success();
                    },
                    [swapchain_resource = context.swapchain_resource,
                     readback_resource = readback_resource_result.value, readback_allocation, target](
                            aeg::render_graph::PassExecutionContext& pass) {
                        return pass.copy_texture_to_buffer(
                                swapchain_resource,
                                aeg::rhi::TextureSubresourceLayers{aeg::rhi::Aspect::kColor, 0, 0, 1},
                                aeg::rhi::Offset3D{0, 0, 0},
                                aeg::rhi::Extent3D{target.extent.width, target.extent.height, 1}, readback_resource,
                                readback_allocation.offset, 0, 0);
                    });
            if (!readback_pass_result) {
                return readback_pass_result.result;
            }
            const auto present_pass_result = context.render_graph.add_pass(
                    "Editor.PresentSwapchain", QueueClass::kGraphics,
                    [resource = context.swapchain_resource](aeg::render_graph::PassBuilder& builder) {
                        return builder.use_color_attachment(resource, aeg::rhi::LoadOp::kLoad,
                                                            aeg::rhi::StoreOp::kStore, ClearColorValue{},
                                                            ResourceUsage::kPresent);
                    },
                    [](aeg::render_graph::PassExecutionContext& pass) {
                        if (const Result result = pass.begin_rendering(); !result) {
                            return result;
                        }
                        return pass.end_rendering();
                    });
            return present_pass_result ? Result::success() : present_pass_result.result;
        };
        const Result frame_result = orchestrator_->run_frame(frame_desc);
        if (!frame_result || !has_readback) {
            return frame_result;
        }
        if (const Result wait_result = device_->wait_idle(); !wait_result) {
            return wait_result;
        }
        std::vector<std::byte> pixels(static_cast<size_t>(readback_allocation.size));
        if (const Result read_result = frame_context_->read_readback(readback_allocation, pixels); !read_result) {
            return read_result;
        }
        constexpr uint32_t kClearProbeExtent = 8;
        const auto pixel_is_clear = [&pixels, width = target.extent.width](uint32_t x, uint32_t y) {
            const size_t offset = (static_cast<size_t>(y) * width + x) * 4u;
            return pixels[offset] == std::byte{0} && pixels[offset + 1] == std::byte{0} &&
                   pixels[offset + 2] == std::byte{0};
        };
        bool clear_probes_are_clean = true;
        for (uint32_t y = 0; y < kClearProbeExtent; ++y) {
            for (uint32_t x = 0; x < kClearProbeExtent; ++x) {
                clear_probes_are_clean = clear_probes_are_clean &&
                                         pixel_is_clear(x, y) &&
                                         pixel_is_clear(target.extent.width - 1u - x, y) &&
                                         pixel_is_clear(x, target.extent.height - 1u - y) &&
                                         pixel_is_clear(target.extent.width - 1u - x,
                                                        target.extent.height - 1u - y);
            }
        }
        if (!clear_probes_are_clean) {
            return aeg::foundation::invalid_state("editor framebuffer regression check failed");
        }
        return frame_result;
    }

    [[nodiscard]] TextureDesc make_swapchain_image_desc() const noexcept {
        TextureDesc desc{};
        desc.extent = {swapchain_info_.extent.width, swapchain_info_.extent.height, 1};
        desc.format = swapchain_info_.format;
        desc.mip_levels = 1;
        desc.array_layers = 1;
        desc.capabilities = TextureCapability::kColorAttachment | TextureCapability::kTransferSource;
        desc.memory = MemoryClass::kDeviceLocal;
        desc.initial_usage = ResourceUsage::kPresent;
        desc.initial_stages = PipelineStage::kAllCommands;
        desc.initial_queue = QueueClass::kGraphics;
        desc.name = aeg::rhi::DebugName{"Editor.SwapchainImage"};
        return desc;
    }

    void cleanup() noexcept {
        if (orchestrator_ != nullptr) {
            static_cast<void>(orchestrator_->shutdown());
            orchestrator_.reset();
        }
        if (imgui_renderer_ != nullptr) {
            static_cast<void>(imgui_renderer_->shutdown());
            imgui_renderer_.reset();
        }
        if (imgui_host_.is_initialized()) {
            static_cast<void>(imgui_host_.shutdown());
        }
        if (font_texture_handle_.is_valid()) {
            static_cast<void>(texture_registry_.unregister_texture(font_texture_handle_));
            font_texture_handle_ = {};
        }
        if (device_ != nullptr) {
            static_cast<void>(device_->wait_idle());
            if (font_sampler_.is_valid()) {
                static_cast<void>(device_->destroy_sampler(font_sampler_));
                font_sampler_ = {};
            }
            if (texture_uploader_ != nullptr && font_texture_.is_valid()) {
                static_cast<void>(texture_uploader_->destroy_texture(font_texture_));
            }
        }
        render_graph_.reset();
        frame_context_.reset();
        texture_uploader_.reset();
        if (device_ != nullptr && swapchain_.is_valid()) {
            static_cast<void>(device_->destroy_swapchain(swapchain_));
            swapchain_ = {};
        }
        if (device_ != nullptr && surface_.is_valid()) {
            static_cast<void>(device_->destroy_surface(surface_));
            surface_ = {};
        }
        device_.reset();
        if (window_system_ != nullptr && window_ != nullptr) {
            static_cast<void>(window_system_->destroy_window(window_->id()));
            window_.reset();
        }
        window_system_.reset();
        initialized_ = false;
    }

    std::unique_ptr<IWindowSystem> window_system_{};
    std::shared_ptr<IWindow> window_{};
    std::unique_ptr<Device> device_{};
    SurfaceHandle surface_{};
    SwapchainHandle swapchain_{};
    SwapchainInfo swapchain_info_{};
    std::unique_ptr<FrameContext> frame_context_{};
    std::unique_ptr<RenderGraph> render_graph_{};
    std::unique_ptr<TextureUploader> texture_uploader_{};
    GpuTexture font_texture_{};
    TextureDesc font_texture_desc_{};
    aeg::rhi::SamplerHandle font_sampler_{};
    ImGuiTextureRegistry texture_registry_{};
    ImGuiTextureHandle font_texture_handle_{};
    EditorImGuiHost imgui_host_{};
    std::unique_ptr<ImGuiRenderer> imgui_renderer_{};
    PipelineRegistry registry_{};
    PipelineScheduler scheduler_{{1, 1, 1, 1}};
    PipelineScopeHost frame_host_{PipelineScope::kFrame,
                                  aeg::pipeline::PipelineInstanceId("Editor.Frame")};
    std::unique_ptr<aeg::engine::EngineFrameOrchestrator> orchestrator_{};
    InputSystem input_system_{};
    std::chrono::steady_clock::time_point last_frame_time_{};
    uint64_t frame_count_{0};
    bool resize_pending_{false};
    bool regression_mode_{false};
    bool initialized_{false};
};

uint32_t parse_frame_limit(int argc, char** argv) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string_view{argv[index]} != "--frames") {
            continue;
        }
        const unsigned long long value = std::strtoull(argv[index + 1], nullptr, 10);
        return value > std::numeric_limits<uint32_t>::max() ? 0 : static_cast<uint32_t>(value);
    }
    return 0;
}

bool has_argument(int argc, char** argv, std::string_view argument) {
    for (int index = 1; index < argc; ++index) {
        if (std::string_view{argv[index]} == argument) {
            return true;
        }
    }
    return false;
}
}  // namespace

int main(int argc, char** argv) {
    EditorApplication application;
    if (!application.initialize()) {
        return 1;
    }
    return application.run(parse_frame_limit(argc, argv), has_argument(argc, argv, "--stress-resize"));
}
