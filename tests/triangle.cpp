/**
 * @file triangle.cpp
 * @brief Runs the Windows triangle rendering example through the public engine APIs.
 *
 * The sample exercises device creation, shader contracts, indexed mesh upload,
 * texture binding, offscreen rendering, render-graph execution, and frame
 * presentation in one small application.
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <logging.hpp>
#include <material.hpp>
#include <memory>
#include <math.hpp>
#include <new>
#include <render_target.hpp>
#include <render_graph.hpp>
#include <resource_uploader.hpp>
#include <rhi.hpp>
#include <shader_contract.hpp>
#include <span>
#include <string_view>
#include <texture_uploader.hpp>
#include <vector>

#include "presentation_fullscreen_texture_blit_shaders.hpp"

namespace {
    using aeg::rhi::BufferCapability;
    using aeg::rhi::BufferDesc;
    using aeg::rhi::BufferHandle;
    using aeg::rhi::BindingLayoutHandle;
    using aeg::rhi::BindingResource;
    using aeg::rhi::BindingSetDesc;
    using aeg::rhi::BindingSetHandle;
    using aeg::rhi::BindingSetLifetime;
    using aeg::rhi::BindingType;
    using aeg::rhi::BindingWrite;
    using aeg::rhi::BufferRange;
    using aeg::rhi::Device;
    using aeg::rhi::DeviceCreateDesc;
    using aeg::rhi::DebugName;
    using aeg::rhi::DrawDesc;
    using aeg::rhi::DrawIndexedDesc;
    using aeg::rhi::Filter;
    using aeg::rhi::Format;
    using aeg::rhi::FrameAcquireResult;
    using aeg::rhi::GraphicsPipelineState;
    using aeg::rhi::LoadOp;
    using aeg::rhi::MemoryClass;
    using aeg::rhi::PipelineDesc;
    using aeg::rhi::PipelineHandle;
    using aeg::rhi::PipelineStage;
    using aeg::rhi::PipelineType;
    using aeg::rhi::PresentMode;
    using aeg::rhi::PrimitiveTopology;
    using aeg::rhi::QueueClass;
    using aeg::rhi::ResourceUsage;
    using aeg::foundation::Result;
    using aeg::foundation::ResultCode;
    using aeg::rhi::ShaderStage;
    using aeg::rhi::ShaderStageDesc;
    using aeg::rhi::SurfaceDesc;
    using aeg::rhi::SurfaceHandle;
    using aeg::rhi::SwapchainDesc;
    using aeg::rhi::SwapchainHandle;
    using aeg::rhi::SwapchainInfo;
    using aeg::rhi::TextureCapability;
    using aeg::rhi::TextureDesc;
    using aeg::rhi::VertexAttributeDesc;
    using aeg::rhi::VertexBindingDesc;
    using aeg::rhi::VertexInputRate;
    using aeg::rhi::Viewport;
    using aeg::math::ClipSpaceConvention;
    using aeg::math::Mat4;
    using aeg::math::Transform;
    using aeg::math::Vec3;
    using aeg::render_graph::FrameContext;
    using aeg::render_graph::PassExecutionContext;
    using aeg::render_graph::PassBuilder;
    using aeg::render_graph::RenderGraph;
    using aeg::render_graph::RenderGraphBuffer;
    using aeg::render_graph::RenderGraphTexture;
    using aeg::resource::GpuMesh;
    using aeg::resource::MeshData;
    using aeg::resource::GpuTexture;
    using aeg::resource::OffscreenRenderTarget;
    using aeg::resource::OffscreenRenderTargetDesc;
    using aeg::resource::ResourceUploader;
    using aeg::resource::TextureData;
    using aeg::resource::TextureMaterial;
    using aeg::resource::TextureMaterialDesc;
    using aeg::resource::TextureSubresourceData;
    using aeg::resource::TextureUploader;
    using aeg::shader::ShaderManager;
    using aeg::shader::ShaderPipelineDesc;
    using aeg::shader::ShaderPipelineLayout;
    using aeg::shader::ShaderProgram;

    bool check_result(const Result &result, std::string_view operation) {
        if (result) {
            return true;
        }
        AEG_LOG_ERROR(aeg::logging::Category::kTest, "{} failed [{}]: {}", operation,
                      aeg::foundation::result_code_name(result.code), result.message);
        return false;
    }

    template<typename T>
    bool check_result(const aeg::foundation::ResultValue<T> &result, std::string_view operation) {
        return check_result(result.result, operation);
    }

    struct LoggingGuard {
        ~LoggingGuard() { aeg::logging::shutdown(); }
    };

    struct Vertex {
        float position[2];
        float color[4];
        float uv[2];
    };

    constexpr std::array<Vertex, 3> kTriangleVertices{
        Vertex{{0.0f, -0.75f}, {1.0f, 1.0f, 1.0f, 1.0f}, {0.5f, 1.0f}},
        Vertex{{0.75f, 0.65f}, {1.0f, 1.0f, 1.0f, 1.0f}, {1.0f, 0.0f}},
        Vertex{{-0.75f, 0.65f}, {1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, 0.0f}},
    };

    constexpr std::array<uint16_t, 3> kTriangleIndices{0, 1, 2};

    constexpr std::array<std::byte, 16> kTexturePixels{
        std::byte{0xff}, std::byte{0x24}, std::byte{0x24}, std::byte{0xff},
        std::byte{0x24}, std::byte{0xd4}, std::byte{0x6b}, std::byte{0xff},
        std::byte{0x34}, std::byte{0x7d}, std::byte{0xff}, std::byte{0xff},
        std::byte{0xff}, std::byte{0xd3}, std::byte{0x24}, std::byte{0xff},
    };

    struct WindowState {
        HWND handle{nullptr};
        uint32_t width{1280};
        uint32_t height{720};
        bool resize_pending{false};
        bool close_requested{false};
    };

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
        WindowState *state = reinterpret_cast<WindowState *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        switch (message) {
            case WM_NCCREATE: {
                const auto *create_struct = reinterpret_cast<const CREATESTRUCTW *>(l_param);
                state = static_cast<WindowState *>(create_struct->lpCreateParams);
                SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
                state->handle = window;
                return TRUE;
            }
            case WM_SIZE:
                if (state != nullptr) {
                    state->width = LOWORD(l_param);
                    state->height = HIWORD(l_param);
                    state->resize_pending = true;
                }
                return 0;
            case WM_CLOSE:
                if (state != nullptr) {
                    state->close_requested = true;
                }
                DestroyWindow(window);
                return 0;
            case WM_DESTROY:
                if (state != nullptr) {
                    state->close_requested = true;
                }
                PostQuitMessage(0);
                return 0;
            default:
                return DefWindowProcW(window, message, w_param, l_param);
        }
    }

    HWND create_window(HINSTANCE instance, WindowState &state) {
        constexpr wchar_t kWindowClassName[] = L"AegEngineTriangleWindow";

        WNDCLASSW window_class{};
        window_class.style = CS_HREDRAW | CS_VREDRAW;
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        window_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        window_class.lpszClassName = kWindowClassName;
        if (RegisterClassW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return nullptr;
        }

        RECT window_rect{0, 0, static_cast<LONG>(state.width), static_cast<LONG>(state.height)};
        if (!AdjustWindowRectEx(&window_rect, WS_OVERLAPPEDWINDOW, FALSE, 0)) {
            return nullptr;
        }

        return CreateWindowExW(0, kWindowClassName, L"AEG Engine - Vulkan Triangle", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                               CW_USEDEFAULT, window_rect.right - window_rect.left,
                               window_rect.bottom - window_rect.top,
                               nullptr, nullptr, instance, &state);
    }

    /**
     * @brief Owns the sample's RHI objects and records its two-pass frame graph.
     *
     * The demo borrows the native window state and owns all engine resources it
     * creates until cleanup() runs from the destructor.
     */

    class TriangleDemo final {
    public:
        TriangleDemo(HWND window, WindowState &window_state) : window_(window), window_state_(window_state) {
        }

        ~TriangleDemo() { cleanup(); }

        [[nodiscard]] bool initialize() {
            DeviceCreateDesc device_desc{};
            device_desc.application_name = "AEG Triangle";
            auto device_result = aeg::rhi::create_device(device_desc);
            if (!check_result(device_result, "create_device")) {
                return false;
            }
            device_ = std::move(device_result.value);

            auto shader_program_result = aeg::shader::load_shader_program(
                    AEG_TRIANGLE_SHADER_PACKAGE_PATH);
            if (!check_result(shader_program_result, "load_triangle_shader_program")) {
                return false;
            }
            gameplay_shader_program_ = std::move(shader_program_result.value);
            shader_manager_ = std::make_unique<ShaderManager>(*device_);
            auto shader_layout_result = shader_manager_->build_pipeline_layout(
                    gameplay_shader_program_, aeg::rhi::DebugName{"Triangle.GameplayLayout"});
            if (!check_result(shader_layout_result, "build_triangle_shader_layout")) {
                return false;
            }
            gameplay_shader_layout_ = std::move(shader_layout_result.value);

            SurfaceDesc surface_desc{};
            surface_desc.native_display_handle = GetModuleHandleW(nullptr);
            surface_desc.native_window_handle = window_;
            surface_desc.name = aeg::rhi::DebugName{"Triangle.Surface"};
            auto surface_result = device_->create_surface(surface_desc);
            if (!check_result(surface_result, "create_surface")) {
                return false;
            }
            surface_ = surface_result.value;

            if (!create_swapchain()) {
                return false;
            }
            frame_context_ = std::make_unique<FrameContext>(*device_);
            render_graph_ = std::make_unique<RenderGraph>(*device_);
            resource_uploader_ = std::make_unique<ResourceUploader>(*device_);
            texture_uploader_ = std::make_unique<TextureUploader>(*device_);
            if (!create_mesh() || !create_texture() || !create_offscreen_render_target() ||
                !create_texture_materials() || !create_transform_binding() || !create_pipelines()) {
                return false;
            }
            return true;
        }

        [[nodiscard]] int run(uint32_t frame_limit) {
            uint32_t frame_count = 0;
            while (!window_state_.close_requested) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
                    if (message.message == WM_QUIT) {
                        window_state_.close_requested = true;
                        break;
                    }
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                if (window_state_.close_requested) {
                    break;
                }

                if (window_state_.resize_pending && window_state_.width != 0 && window_state_.height != 0) {
                    if (!recreate_swapchain()) {
                        return 1;
                    }
                    window_state_.resize_pending = false;
                }
                if (window_state_.width == 0 || window_state_.height == 0) {
                    WaitMessage();
                    continue;
                }

                if (!check_result(frame_context_->begin_frame(), "begin_frame")) {
                    return 1;
                }

                auto acquire_result = frame_context_->acquire_next_image(swapchain_);
                if (!acquire_result) {
                    const Result abort_result = frame_context_->abort_frame();
                    if (acquire_result.result.code == ResultCode::kOutOfDate) {
                        window_state_.resize_pending = true;
                        if (!check_result(abort_result, "abort_out_of_date_frame")) {
                            return 1;
                        }
                        continue;
                    }
                    check_result(acquire_result, "acquire_next_image");
                    (void) abort_result;
                    return 1;
                }
                const FrameAcquireResult &acquired = acquire_result.value;
                if (acquired.suboptimal) {
                    window_state_.resize_pending = true;
                }

                if (!record_frame(acquired)) {
                    (void) frame_context_->abort_frame();
                    return 1;
                }

                auto execution_result = render_graph_->execute(
                        *frame_context_, aeg::render_graph::RenderGraphExecuteDesc{true});
                if (!execution_result) {
                    if (execution_result.result.code == ResultCode::kOutOfDate ||
                        execution_result.result.code == ResultCode::kSurfaceLost) {
                        window_state_.resize_pending = true;
                        (void) frame_context_->abort_frame();
                        render_graph_->reset();
                        continue;
                    }
                    check_result(execution_result, "execute_render_graph");
                    return 1;
                }
                if (execution_result.value.suboptimal) {
                    window_state_.resize_pending = true;
                }
                if (!check_result(frame_context_->end_frame(), "end_frame")) {
                    return 1;
                }
                render_graph_->reset();
                mesh_vertex_state_ = mesh_vertex_graphics_state_;
                mesh_index_state_ = mesh_index_graphics_state_;
                texture_state_ = texture_sampled_graphics_state_;
                offscreen_state_ = offscreen_sampled_graphics_state_;

                rotation_angle_ += 0.02f;
                ++frame_count;
                if (frame_limit != 0 && frame_count >= frame_limit) {
                    break;
                }
            }
            return 0;
        }

    private:
        [[nodiscard]] bool create_swapchain() {
            SwapchainDesc swapchain_desc{};
            swapchain_desc.surface = surface_;
            swapchain_desc.extent = {window_state_.width, window_state_.height};
            swapchain_desc.format = Format::kUnknown;
            swapchain_desc.present_mode = PresentMode::kVsync;
            swapchain_desc.min_image_count = 2;
            swapchain_desc.frames_in_flight = 2;
            swapchain_desc.image_capabilities = aeg::rhi::TextureCapability::kColorAttachment;
            swapchain_desc.name = aeg::rhi::DebugName{"Triangle.Swapchain"};

            auto swapchain_result = device_->create_swapchain(swapchain_desc);
            if (!check_result(swapchain_result, "create_swapchain")) {
                return false;
            }
            swapchain_ = swapchain_result.value;
            return update_swapchain_info();
        }

        [[nodiscard]] bool update_swapchain_info() {
            auto info_result = device_->get_swapchain_info(swapchain_);
            if (!check_result(info_result, "get_swapchain_info")) {
                return false;
            }
            swapchain_info_ = info_result.value;
            color_format_ = swapchain_info_.format;
            return color_format_ != Format::kUnknown;
        }

        [[nodiscard]] bool recreate_swapchain() {
            if (!check_result(device_->wait_idle(), "wait_idle_before_recreate")) {
                return false;
            }

            const Format previous_format = color_format_;
            SwapchainDesc swapchain_desc{};
            swapchain_desc.surface = surface_;
            swapchain_desc.extent = {window_state_.width, window_state_.height};
            swapchain_desc.present_mode = PresentMode::kVsync;
            swapchain_desc.min_image_count = 2;
            swapchain_desc.frames_in_flight = 2;
            swapchain_desc.image_capabilities = aeg::rhi::TextureCapability::kColorAttachment;
            const Result recreate_result = device_->recreate_swapchain(swapchain_, swapchain_desc);
            if (!check_result(recreate_result, "recreate_swapchain")) {
                return false;
            }
            if (!update_swapchain_info()) {
                return false;
            }

            OffscreenRenderTarget replacement{};
            const OffscreenRenderTargetDesc target_desc{
                    {swapchain_info_.extent.width, swapchain_info_.extent.height},
                    color_format_,
                    aeg::rhi::SampleCount::kX1,
                    true,
                    aeg::rhi::DebugName{"Triangle.Offscreen"}
            };
            auto target_result = aeg::resource::create_offscreen_render_target(*device_, target_desc);
            if (!check_result(target_result, "recreate_offscreen_render_target")) {
                return false;
            }
            replacement = target_result.value;

            const aeg::rhi::TextureViewHandle previous_sampled_view = offscreen_target_.sampled_view;
            if (composite_material_.is_valid()) {
                const Result material_result = aeg::resource::update_texture_material(
                        *device_, composite_material_, replacement.sampled_view);
                if (!check_result(material_result, "update_offscreen_material")) {
                    (void) aeg::resource::destroy_offscreen_render_target(*device_, replacement);
                    return false;
                }
            }
            const Result destroy_result = aeg::resource::destroy_offscreen_render_target(*device_, offscreen_target_);
            if (!destroy_result) {
                if (composite_material_.is_valid() && previous_sampled_view.is_valid()) {
                    (void) aeg::resource::update_texture_material(
                            *device_, composite_material_, previous_sampled_view);
                }
                (void) aeg::resource::destroy_offscreen_render_target(*device_, replacement);
                return check_result(destroy_result, "destroy_previous_offscreen_render_target");
            }
            offscreen_target_ = replacement;
            offscreen_state_ = {};
            render_graph_->reset();
            if (previous_format != color_format_) {
                if (!check_result(device_->destroy_pipeline(scene_pipeline_), "destroy_scene_pipeline_for_recreate")) {
                    return false;
                }
                scene_pipeline_ = {};
                if (!check_result(device_->destroy_pipeline(composite_pipeline_),
                                  "destroy_composite_pipeline_for_recreate")) {
                    return false;
                }
                composite_pipeline_ = {};
                return create_pipelines();
            }
            return true;
        }

        [[nodiscard]] bool create_mesh() {
            const MeshData mesh_data{
                std::as_bytes(std::span{kTriangleVertices}),
                sizeof(Vertex),
                static_cast<uint32_t>(kTriangleVertices.size()),
                std::as_bytes(std::span{kTriangleIndices}),
                aeg::rhi::IndexFormat::kUint16,
                static_cast<uint32_t>(kTriangleIndices.size())
            };
            if (!check_result(frame_context_->begin_frame(), "begin_mesh_upload_frame")) {
                return false;
            }

            auto upload_result = resource_uploader_->upload_mesh(
                    *frame_context_, *render_graph_, mesh_data, "Triangle.Mesh");
            if (!check_result(upload_result, "upload_mesh")) {
                (void) frame_context_->abort_frame();
                return false;
            }

            GpuMesh uploaded_mesh = upload_result.value.mesh;
            auto execution_result = render_graph_->execute(*frame_context_);
            if (!check_result(execution_result, "execute_mesh_upload")) {
                (void) device_->wait_idle();
                (void) resource_uploader_->destroy_mesh(uploaded_mesh);
                return false;
            }
            if (execution_result.value.completion.is_valid() &&
                !check_result(device_->wait(execution_result.value.completion,
                                             std::numeric_limits<uint64_t>::max()),
                              "wait_mesh_upload")) {
                (void) resource_uploader_->destroy_mesh(uploaded_mesh);
                (void) frame_context_->abort_frame();
                return false;
            }
            if (!check_result(frame_context_->end_frame(), "end_mesh_upload_frame")) {
                (void) device_->wait_idle();
                (void) resource_uploader_->destroy_mesh(uploaded_mesh);
                return false;
            }

            mesh_ = uploaded_mesh;
            mesh_upload_state_ = aeg::rhi::ResourceState{
                ResourceUsage::kCopyDestination,
                ShaderStage::kAll,
                QueueClass::kCopy,
                false,
                PipelineStage::kCopy
            };
            mesh_vertex_graphics_state_ = aeg::rhi::ResourceState{
                ResourceUsage::kVertexInput,
                ShaderStage::kVertex,
                QueueClass::kGraphics,
                false,
                PipelineStage::kVertexInput
            };
            mesh_index_graphics_state_ = aeg::rhi::ResourceState{
                ResourceUsage::kIndexInput,
                ShaderStage::kVertex,
                QueueClass::kGraphics,
                false,
                PipelineStage::kVertexInput
            };
            mesh_vertex_state_ = mesh_upload_state_;
            mesh_index_state_ = mesh_upload_state_;
            render_graph_->reset();
            return true;
        }

        [[nodiscard]] bool create_offscreen_render_target() {
            const OffscreenRenderTargetDesc target_desc{
                    {swapchain_info_.extent.width, swapchain_info_.extent.height},
                    color_format_,
                    aeg::rhi::SampleCount::kX1,
                    true,
                    aeg::rhi::DebugName{"Triangle.Offscreen"}
            };
            auto target_result = aeg::resource::create_offscreen_render_target(*device_, target_desc);
            if (!check_result(target_result, "create_offscreen_render_target")) {
                return false;
            }
            offscreen_target_ = target_result.value;
            offscreen_sampled_graphics_state_ = aeg::rhi::ResourceState{
                ResourceUsage::kSampledRead,
                ShaderStage::kAll,
                QueueClass::kGraphics,
                false,
                PipelineStage::kAllCommands
            };
            offscreen_state_ = {};
            return true;
        }

        [[nodiscard]] bool create_texture_materials() {
            if (!texture_.has_sampled_view() || !offscreen_target_.sampled_view.is_valid()) {
                AEG_LOG_ERROR(aeg::logging::Category::kTest,
                              "Texture material creation requires sampled texture views");
                return false;
            }

            TextureMaterialDesc scene_material_desc{};
            scene_material_desc.texture_view = texture_.sampled_view;
            scene_material_desc.sampler.min_filter = Filter::kNearest;
            scene_material_desc.sampler.mag_filter = Filter::kNearest;
            scene_material_desc.sampler.mip_filter = Filter::kNearest;
            scene_material_desc.sampler.address_u = aeg::rhi::AddressMode::kClampToEdge;
            scene_material_desc.sampler.address_v = aeg::rhi::AddressMode::kClampToEdge;
            scene_material_desc.sampler.address_w = aeg::rhi::AddressMode::kClampToEdge;
            scene_material_desc.sampler.max_lod = 0.0f;
            scene_material_desc.sampler.name = aeg::rhi::DebugName{"Triangle.TextureSampler"};
            scene_material_desc.name = aeg::rhi::DebugName{"Triangle.TextureMaterial"};
            auto scene_layout_result = shader_manager_->binding_layout(gameplay_shader_layout_, 0);
            if (!check_result(scene_layout_result, "get_scene_binding_layout")) {
                return false;
            }
            scene_material_desc.binding_layout = scene_layout_result.value;
            auto scene_material_result = aeg::resource::create_texture_material(*device_, scene_material_desc);
            if (!check_result(scene_material_result, "create_texture_material")) {
                return false;
            }
            scene_material_ = scene_material_result.value;

            TextureMaterialDesc composite_material_desc{};
            composite_material_desc.texture_view = offscreen_target_.sampled_view;
            composite_material_desc.sampler.address_u = aeg::rhi::AddressMode::kClampToEdge;
            composite_material_desc.sampler.address_v = aeg::rhi::AddressMode::kClampToEdge;
            composite_material_desc.sampler.address_w = aeg::rhi::AddressMode::kClampToEdge;
            composite_material_desc.sampler.max_lod = 0.0f;
            composite_material_desc.sampler.name = aeg::rhi::DebugName{"Triangle.OffscreenSampler"};
            composite_material_desc.name = aeg::rhi::DebugName{"Triangle.OffscreenMaterial"};
            auto composite_material_result = aeg::resource::create_texture_material(
                    *device_, composite_material_desc);
            if (!check_result(composite_material_result, "create_offscreen_material")) {
                return false;
            }
            composite_material_ = composite_material_result.value;
            return true;
        }

        [[nodiscard]] bool create_transform_binding() {
            auto layout_result = shader_manager_->binding_layout(gameplay_shader_layout_, 1);
            if (!check_result(layout_result, "create_transform_binding_layout")) {
                return false;
            }
            transform_binding_layout_ = layout_result.value;

            try {
                transform_buffers_.resize(frame_context_->frames_in_flight());
                transform_binding_sets_.resize(frame_context_->frames_in_flight());
            } catch (const std::bad_alloc&) {
                AEG_LOG_ERROR(aeg::logging::Category::kTest,
                              "Transform binding allocation failed");
                return false;
            }

            for (uint32_t index = 0; index < frame_context_->frames_in_flight(); ++index) {
                BufferDesc buffer_desc{};
                buffer_desc.size = sizeof(Mat4);
                buffer_desc.capabilities = BufferCapability::kUniform;
                buffer_desc.memory = MemoryClass::kUpload;
                buffer_desc.initial_usage = ResourceUsage::kUniformRead;
                buffer_desc.initial_stages = PipelineStage::kVertexShader;
                buffer_desc.initial_queue = QueueClass::kGraphics;
                buffer_desc.name = aeg::rhi::DebugName{"Triangle.TransformBuffer"};
                auto buffer_result = device_->create_buffer(buffer_desc);
                if (!check_result(buffer_result, "create_transform_buffer")) {
                    return false;
                }
                transform_buffers_[index] = buffer_result.value;

                auto set_result = device_->create_binding_set(
                        BindingSetDesc{transform_binding_layout_, BindingSetLifetime::kFrameLocal,
                                       DebugName{"Triangle.TransformBindingSet"}});
                if (!check_result(set_result, "create_transform_binding_set")) {
                    return false;
                }
                transform_binding_sets_[index] = set_result.value;

                BindingWrite write{};
                write.location = aeg::rhi::BindingLocation{1, 0};
                write.resource = BindingResource{};
                write.resource.type = BindingType::kUniformBuffer;
                write.resource.buffer_range = BufferRange{transform_buffers_[index], 0, sizeof(Mat4)};
                if (!check_result(device_->update_binding_set(transform_binding_sets_[index],
                                                              std::span{&write, 1}),
                                  "update_transform_binding_set")) {
                    return false;
                }
            }

            transform_state_ = aeg::rhi::ResourceState{
                ResourceUsage::kUniformRead,
                ShaderStage::kVertex,
                QueueClass::kGraphics,
                false,
                PipelineStage::kVertexShader
            };
            return true;
        }

        [[nodiscard]] bool create_scene_pipeline() {
            if (!scene_material_.is_valid() || !gameplay_shader_layout_.is_valid()) {
                AEG_LOG_ERROR(aeg::logging::Category::kTest,
                              "Scene pipeline requires a valid material and shader contract layout");
                return false;
            }
            const std::array<VertexBindingDesc, 1> vertex_bindings{
                VertexBindingDesc{0, sizeof(Vertex), VertexInputRate::kVertex},
            };
            const std::array<VertexAttributeDesc, 3> vertex_attributes{
                VertexAttributeDesc{0, 0, Format::kRg32Float, 0},
                VertexAttributeDesc{1, 0, Format::kRgba32Float, sizeof(float) * 2},
                VertexAttributeDesc{2, 0, Format::kRg32Float, sizeof(float) * 6},
            };
            const std::array<Format, 1> color_formats{offscreen_target_.texture_desc.format};

            GraphicsPipelineState graphics_state{};
            graphics_state.vertex_bindings = vertex_bindings;
            graphics_state.vertex_attributes = vertex_attributes;
            graphics_state.topology = PrimitiveTopology::kTriangleList;
            graphics_state.cull_mode = aeg::rhi::CullMode::kNone;
            graphics_state.color_formats = color_formats;

            ShaderPipelineDesc pipeline_desc{};
            pipeline_desc.type = PipelineType::kGraphics;
            pipeline_desc.graphics = graphics_state;
            pipeline_desc.name = aeg::rhi::DebugName{"Triangle.GraphicsPipeline"};
            auto pipeline_result = shader_manager_->create_pipeline(
                    gameplay_shader_program_, gameplay_shader_layout_, pipeline_desc);
            if (!check_result(pipeline_result, "create_scene_pipeline")) {
                return false;
            }
            scene_pipeline_ = pipeline_result.value;
            return true;
        }

        [[nodiscard]] bool create_composite_pipeline() {
            if (!composite_material_.is_valid()) {
                AEG_LOG_ERROR(aeg::logging::Category::kTest,
                              "Composite pipeline requires a valid texture material");
                return false;
            }
            const std::array<ShaderStageDesc, 2> shader_stages{
                ShaderStageDesc{
                    ShaderStage::kVertex,
                    aeg::shaders::kPresentationFullscreenTextureBlitVertex.as_span(),
                    "main",
                    aeg::rhi::DebugName{"Triangle.FullscreenTextureBlitVertexShader"}
                },
                ShaderStageDesc{
                    ShaderStage::kFragment,
                    aeg::shaders::kPresentationFullscreenTextureBlitFragment.as_span(),
                    "main",
                    aeg::rhi::DebugName{"Triangle.FullscreenTextureBlitFragmentShader"}
                },
            };
            const std::array<Format, 1> color_formats{color_format_};
            const std::array<BindingLayoutHandle, 1> binding_layouts{composite_material_.binding_layout};

            GraphicsPipelineState graphics_state{};
            graphics_state.topology = PrimitiveTopology::kTriangleList;
            graphics_state.cull_mode = aeg::rhi::CullMode::kNone;
            graphics_state.color_formats = color_formats;

            PipelineDesc pipeline_desc{};
            pipeline_desc.type = PipelineType::kGraphics;
            pipeline_desc.layout.binding_layouts = binding_layouts;
            pipeline_desc.shader_stages = shader_stages;
            pipeline_desc.graphics = graphics_state;
            pipeline_desc.name = aeg::rhi::DebugName{"Triangle.FullscreenTextureBlitPipeline"};
            auto pipeline_result = device_->create_pipeline(pipeline_desc);
            if (!check_result(pipeline_result, "create_composite_pipeline")) {
                return false;
            }
            composite_pipeline_ = pipeline_result.value;
            return true;
        }

        [[nodiscard]] bool create_pipelines() {
            return create_scene_pipeline() && create_composite_pipeline();
        }

        [[nodiscard]] bool create_texture() {
            texture_desc_ = {};
            texture_desc_.extent = {2, 2, 1};
            texture_desc_.format = Format::kRgba8Unorm;
            texture_desc_.mip_levels = 1;
            texture_desc_.array_layers = 1;
            texture_desc_.capabilities = TextureCapability::kTransferDestination | TextureCapability::kSampled;
            texture_desc_.memory = MemoryClass::kDeviceLocal;
            texture_desc_.initial_usage = ResourceUsage::kUndefined;
            texture_desc_.initial_stages = PipelineStage::kAllCommands;
            texture_desc_.initial_queue = QueueClass::kAuto;
            texture_desc_.name = aeg::rhi::DebugName{"Triangle.Texture"};

            const TextureSubresourceData subresource{std::as_bytes(std::span{kTexturePixels}), 0, 0};
            const TextureData texture_data{
                texture_desc_, std::span<const TextureSubresourceData>{&subresource, 1}
            };

            if (!check_result(frame_context_->begin_frame(), "begin_texture_upload_frame")) {
                return false;
            }

            auto upload_result = texture_uploader_->upload_texture(
                    *frame_context_, *render_graph_, texture_data, "Triangle.Texture");
            if (!check_result(upload_result, "upload_texture")) {
                (void) frame_context_->abort_frame();
                return false;
            }

            GpuTexture uploaded_texture = upload_result.value.texture;
            auto execution_result = render_graph_->execute(*frame_context_);
            if (!check_result(execution_result, "execute_texture_upload")) {
                (void) frame_context_->abort_frame();
                (void) device_->wait_idle();
                (void) texture_uploader_->destroy_texture(uploaded_texture);
                return false;
            }
            if (execution_result.value.completion.is_valid() &&
                !check_result(device_->wait(execution_result.value.completion,
                                             std::numeric_limits<uint64_t>::max()),
                              "wait_texture_upload")) {
                (void) frame_context_->abort_frame();
                (void) device_->wait_idle();
                (void) texture_uploader_->destroy_texture(uploaded_texture);
                return false;
            }
            if (!check_result(frame_context_->end_frame(), "end_texture_upload_frame")) {
                (void) device_->wait_idle();
                (void) texture_uploader_->destroy_texture(uploaded_texture);
                return false;
            }

            texture_ = uploaded_texture;
            texture_upload_state_ = aeg::rhi::ResourceState{
                ResourceUsage::kCopyDestination,
                ShaderStage::kAll,
                QueueClass::kCopy,
                false,
                PipelineStage::kCopy
            };
            texture_sampled_graphics_state_ = aeg::rhi::ResourceState{
                ResourceUsage::kSampledRead,
                ShaderStage::kAll,
                QueueClass::kGraphics,
                false,
                PipelineStage::kAllCommands
            };
            texture_state_ = texture_upload_state_;
            render_graph_->reset();
            return true;
        }

        [[nodiscard]] bool record_frame(const FrameAcquireResult &acquired) {
            const float aspect_ratio = static_cast<float>(swapchain_info_.extent.width) /
                                       static_cast<float>(swapchain_info_.extent.height);
            const Transform object_transform{
                Vec3{0.0f, 0.0f, 0.0f},
                aeg::math::quat_from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, rotation_angle_),
                Vec3{1.0f, 1.0f, 1.0f}
            };
            const Mat4 model = object_transform.to_matrix();
            const Mat4 view = aeg::math::look_at(Vec3{0.0f, 0.0f, 3.0f}, Vec3{0.0f, 0.0f, 0.0f},
                                                 Vec3{0.0f, 1.0f, 0.0f});
            const Mat4 projection = aeg::math::perspective(
                60.0f * aeg::math::kPi / 180.0f, aspect_ratio, 0.1f, 100.0f, ClipSpaceConvention{true, false});
            model_view_projection_ = projection * view * model;

            const uint32_t transform_slot = frame_context_->frame_slot_index();
            if (transform_slot >= transform_buffers_.size() ||
                transform_slot >= transform_binding_sets_.size()) {
                AEG_LOG_ERROR(aeg::logging::Category::kTest,
                              "Frame slot exceeds transform binding storage");
                return false;
            }
            const BufferHandle transform_buffer = transform_buffers_[transform_slot];
            const aeg::rhi::BindingSetHandle transform_binding_set =
                transform_binding_sets_[transform_slot];
            if (!check_result(device_->write_buffer(
                                      transform_buffer, 0,
                                      std::as_bytes(std::span{&model_view_projection_, 1})),
                              "write_transform_constants")) {
                return false;
            }

            render_graph_->reset();

            BufferDesc vertex_desc{};
            vertex_desc.size = mesh_.vertex_buffer_size;
            vertex_desc.capabilities = BufferCapability::kTransferDestination | BufferCapability::kVertex;
            vertex_desc.memory = MemoryClass::kDeviceLocal;
            vertex_desc.name = aeg::rhi::DebugName{"Triangle.VertexBuffer"};
            auto vertex_resource_result = render_graph_->import_buffer(
                    mesh_.vertex_buffer, vertex_desc, mesh_vertex_state_);
            if (!check_result(vertex_resource_result, "import_vertex_buffer")) {
                return false;
            }

            BufferDesc index_desc{};
            index_desc.size = mesh_.index_buffer_size;
            index_desc.capabilities = BufferCapability::kTransferDestination | BufferCapability::kIndex;
            index_desc.memory = MemoryClass::kDeviceLocal;
            index_desc.name = aeg::rhi::DebugName{"Triangle.IndexBuffer"};
            auto index_resource_result = render_graph_->import_buffer(
                    mesh_.index_buffer, index_desc, mesh_index_state_);
            if (!check_result(index_resource_result, "import_index_buffer")) {
                return false;
            }

            auto texture_resource_result = render_graph_->import_texture(
                    texture_.texture, texture_.sampled_view, texture_desc_, texture_state_);
            if (!check_result(texture_resource_result, "import_texture")) {
                return false;
            }

            BufferDesc transform_desc{};
            transform_desc.size = sizeof(Mat4);
            transform_desc.capabilities = BufferCapability::kUniform;
            transform_desc.memory = MemoryClass::kUpload;
            transform_desc.initial_usage = ResourceUsage::kUniformRead;
            transform_desc.initial_stages = PipelineStage::kVertexShader;
            transform_desc.initial_queue = QueueClass::kGraphics;
            transform_desc.name = aeg::rhi::DebugName{"Triangle.TransformBuffer"};
            auto transform_resource_result = render_graph_->import_buffer(
                transform_buffer, transform_desc, transform_state_);
            if (!check_result(transform_resource_result, "import_transform_buffer")) {
                return false;
            }

            TextureDesc offscreen_desc = offscreen_target_.texture_desc;
            offscreen_desc.name = aeg::rhi::DebugName{"Triangle.Offscreen"};
            auto offscreen_resource_result = render_graph_->import_texture(
                    offscreen_target_.texture, offscreen_target_.color_view, offscreen_desc, offscreen_state_);
            if (!check_result(offscreen_resource_result, "import_offscreen_texture")) {
                return false;
            }

            TextureDesc swapchain_desc{};
            swapchain_desc.extent = {swapchain_info_.extent.width, swapchain_info_.extent.height, 1};
            swapchain_desc.format = color_format_;
            swapchain_desc.capabilities = TextureCapability::kColorAttachment;
            swapchain_desc.memory = MemoryClass::kDeviceLocal;
            swapchain_desc.initial_usage = ResourceUsage::kPresent;
            swapchain_desc.initial_stages = PipelineStage::kAllCommands;
            swapchain_desc.initial_queue = QueueClass::kGraphics;
            swapchain_desc.name = aeg::rhi::DebugName{"Triangle.SwapchainImage"};
            const aeg::rhi::ResourceState present_state{
                ResourceUsage::kPresent,
                ShaderStage::kAll,
                QueueClass::kGraphics,
                false,
                PipelineStage::kAllCommands
            };
            auto swapchain_resource_result = render_graph_->import_swapchain_image(
                    acquired, swapchain_desc, present_state);
            if (!check_result(swapchain_resource_result, "import_swapchain_image")) {
                return false;
            }

            const RenderGraphBuffer vertex_resource = vertex_resource_result.value;
            const RenderGraphBuffer index_resource = index_resource_result.value;
            const RenderGraphBuffer transform_resource = transform_resource_result.value;
            const RenderGraphTexture texture_resource = texture_resource_result.value;
            const RenderGraphTexture offscreen_resource = offscreen_resource_result.value;
            const RenderGraphTexture swapchain_resource = swapchain_resource_result.value;

            auto scene_pass_result = render_graph_->add_pass(
                    "Triangle.OffscreenScene", QueueClass::kGraphics,
                    [vertex_resource, index_resource, transform_resource, texture_resource,
                     offscreen_resource](PassBuilder &builder) {
                        if (const Result result = builder.read(vertex_resource, ResourceUsage::kVertexInput,
                                                               PipelineStage::kVertexInput, ShaderStage::kVertex, {});
                            !result) {
                            return result;
                        }
                        if (const Result result = builder.read(index_resource, ResourceUsage::kIndexInput,
                                                               PipelineStage::kVertexInput, ShaderStage::kVertex, {});
                            !result) {
                            return result;
                        }
                        if (const Result result = builder.read(transform_resource, ResourceUsage::kUniformRead,
                                                               PipelineStage::kVertexShader, ShaderStage::kVertex, {});
                            !result) {
                            return result;
                        }
                        if (const Result result = builder.read(texture_resource, ResourceUsage::kSampledRead,
                                                               PipelineStage::kAllCommands, ShaderStage::kAll, {});
                            !result) {
                            return result;
                        }
                        return builder.use_color_attachment(
                                offscreen_resource, LoadOp::kClear, aeg::rhi::StoreOp::kStore,
                                aeg::rhi::ClearColorValue{0.025f, 0.035f, 0.06f, 1.0f},
                                ResourceUsage::kSampledRead);
                    },
                    [this, vertex_resource, index_resource, transform_binding_set](PassExecutionContext &context) {
                        auto vertex_buffer_result = context.buffer(vertex_resource);
                        if (!vertex_buffer_result) {
                            return vertex_buffer_result.result;
                        }
                        auto index_buffer_result = context.buffer(index_resource);
                        if (!index_buffer_result) {
                            return index_buffer_result.result;
                        }
                        if (const Result result = context.device().bind_pipeline(
                                    context.command_list(), scene_pipeline_);
                            !result) {
                            return result;
                        }
                        if (const Result result = context.device().bind_binding_set(
                                    context.command_list(), 0, scene_material_.binding_set);
                            !result) {
                            return result;
                        }
                        if (const Result result = context.device().bind_binding_set(
                                    context.command_list(), 1, transform_binding_set);
                            !result) {
                            return result;
                        }
                        if (const Result result = context.device().bind_vertex_buffer(
                                    context.command_list(), 0, vertex_buffer_result.value);
                            !result) {
                            return result;
                        }
                        if (const Result result = context.device().bind_index_buffer(
                                    context.command_list(), index_buffer_result.value, 0, mesh_.index_format);
                            !result) {
                            return result;
                        }
                        if (const Result result = context.begin_rendering(); !result) {
                            return result;
                        }
                        const Viewport viewport{
                            0.0f,
                            0.0f,
                            static_cast<float>(offscreen_target_.texture_desc.extent.width),
                            static_cast<float>(offscreen_target_.texture_desc.extent.height),
                            0.0f,
                            1.0f
                        };
                        const aeg::rhi::ScissorRect scissor{
                            0,
                            0,
                            offscreen_target_.texture_desc.extent.width,
                            offscreen_target_.texture_desc.extent.height
                        };
                        if (const Result result = context.device().set_viewport(context.command_list(), viewport);
                            !result) {
                            return result;
                        }
                        if (const Result result = context.device().set_scissor(context.command_list(), scissor);
                            !result) {
                            return result;
                        }
                        if (const Result result = context.device().draw_indexed(
                                    context.command_list(), DrawIndexedDesc{mesh_.index_count, 1, 0, 0, 0});
                            !result) {
                            return result;
                        }
                        return context.end_rendering();
                    });
            if (!check_result(scene_pass_result, "add_offscreen_scene_pass")) {
                return false;
            }

            auto composite_pass_result = render_graph_->add_pass(
                    "Triangle.OffscreenComposite", QueueClass::kGraphics,
                    [offscreen_resource, swapchain_resource](PassBuilder &builder) {
                        if (const Result result = builder.read(offscreen_resource, ResourceUsage::kSampledRead,
                                                               PipelineStage::kAllCommands, ShaderStage::kAll, {});
                            !result) {
                            return result;
                        }
                        if (const Result result = builder.use_color_attachment(
                                    swapchain_resource, LoadOp::kClear, aeg::rhi::StoreOp::kStore,
                                    aeg::rhi::ClearColorValue{0.0f, 0.0f, 0.0f, 1.0f},
                                    ResourceUsage::kPresent);
                            !result) {
                            return result;
                        }
                        builder.set_side_effects();
                        return Result::success();
                    },
                    [this](PassExecutionContext &context) {
                        if (const Result result = context.device().bind_pipeline(
                                    context.command_list(), composite_pipeline_);
                            !result) {
                            return result;
                        }
                        if (const Result result = context.device().bind_binding_set(
                                    context.command_list(), 0, composite_material_.binding_set);
                            !result) {
                            return result;
                        }
                        if (const Result result = context.begin_rendering(); !result) {
                            return result;
                        }
                        const Viewport viewport{
                            0.0f,
                            0.0f,
                            static_cast<float>(swapchain_info_.extent.width),
                            static_cast<float>(swapchain_info_.extent.height),
                            0.0f,
                            1.0f
                        };
                        const aeg::rhi::ScissorRect scissor{
                            0,
                            0,
                            swapchain_info_.extent.width,
                            swapchain_info_.extent.height
                        };
                        if (const Result result = context.device().set_viewport(context.command_list(), viewport);
                            !result) {
                            return result;
                        }
                        if (const Result result = context.device().set_scissor(context.command_list(), scissor);
                            !result) {
                            return result;
                        }
                        if (const Result result = context.device().draw(
                                    context.command_list(), DrawDesc{3, 1, 0, 0});
                            !result) {
                            return result;
                        }
                        return context.end_rendering();
                    });
            if (!check_result(composite_pass_result, "add_offscreen_composite_pass")) {
                return false;
            }
            return true;
        }

        void cleanup() {
            if (device_ == nullptr) {
                return;
            }
            (void) device_->wait_idle();
            render_graph_.reset();
            frame_context_.reset();

            if (composite_pipeline_.is_valid()) {
                (void) device_->destroy_pipeline(composite_pipeline_);
                composite_pipeline_ = {};
            }
            if (scene_pipeline_.is_valid()) {
                (void) device_->destroy_pipeline(scene_pipeline_);
                scene_pipeline_ = {};
            }
            for (BindingSetHandle& binding_set : transform_binding_sets_) {
                if (binding_set.is_valid()) {
                    (void) device_->destroy_binding_set(binding_set);
                    binding_set = {};
                }
            }
            for (BufferHandle& buffer : transform_buffers_) {
                if (buffer.is_valid()) {
                    (void) device_->destroy_buffer(buffer);
                    buffer = {};
                }
            }
            transform_binding_sets_.clear();
            transform_buffers_.clear();
            (void) aeg::resource::destroy_texture_material(*device_, composite_material_);
            (void) aeg::resource::destroy_texture_material(*device_, scene_material_);
            (void) aeg::resource::destroy_offscreen_render_target(*device_, offscreen_target_);
            if (texture_uploader_ != nullptr && texture_.is_valid()) {
                (void) texture_uploader_->destroy_texture(texture_);
            }
            if (resource_uploader_ != nullptr && mesh_.is_valid()) {
                (void) resource_uploader_->destroy_mesh(mesh_);
            }
            if (shader_manager_ != nullptr) {
                const Result layout_result = shader_manager_->clear_layout_cache();
                if (!check_result(layout_result, "clear_gameplay_shader_layout_cache")) {
                    AEG_LOG_ERROR(aeg::logging::Category::kTest,
                                  "Gameplay shader layout cache still contains live layouts");
                }
                gameplay_shader_layout_ = {};
                shader_manager_.reset();
            }
            texture_uploader_.reset();
            resource_uploader_.reset();
            if (swapchain_.is_valid()) {
                (void) device_->destroy_swapchain(swapchain_);
                swapchain_ = {};
            }
            if (surface_.is_valid()) {
                (void) device_->destroy_surface(surface_);
                surface_ = {};
            }
            device_.reset();
        }

        HWND window_{nullptr};
        WindowState &window_state_;
        std::unique_ptr<Device> device_{};
        std::unique_ptr<ShaderManager> shader_manager_{};
        ShaderProgram gameplay_shader_program_{};
        ShaderPipelineLayout gameplay_shader_layout_{};
        std::unique_ptr<FrameContext> frame_context_{};
        std::unique_ptr<RenderGraph> render_graph_{};
        std::unique_ptr<ResourceUploader> resource_uploader_{};
        std::unique_ptr<TextureUploader> texture_uploader_{};
        SurfaceHandle surface_{};
        SwapchainHandle swapchain_{};
        SwapchainInfo swapchain_info_{};
        GpuMesh mesh_{};
        GpuTexture texture_{};
        OffscreenRenderTarget offscreen_target_{};
        TextureMaterial scene_material_{};
        TextureMaterial composite_material_{};
        std::vector<BufferHandle> transform_buffers_{};
        std::vector<BindingSetHandle> transform_binding_sets_{};
        BindingLayoutHandle transform_binding_layout_{};
        PipelineHandle scene_pipeline_{};
        PipelineHandle composite_pipeline_{};
        Format color_format_{Format::kUnknown};
        aeg::rhi::ResourceState mesh_upload_state_{};
        aeg::rhi::ResourceState mesh_vertex_graphics_state_{};
        aeg::rhi::ResourceState mesh_index_graphics_state_{};
        aeg::rhi::ResourceState mesh_vertex_state_{};
        aeg::rhi::ResourceState mesh_index_state_{};
        TextureDesc texture_desc_{};
        aeg::rhi::ResourceState texture_upload_state_{};
        aeg::rhi::ResourceState texture_sampled_graphics_state_{};
        aeg::rhi::ResourceState texture_state_{};
        aeg::rhi::ResourceState transform_state_{};
        aeg::rhi::ResourceState offscreen_sampled_graphics_state_{};
        aeg::rhi::ResourceState offscreen_state_{};
        Mat4 model_view_projection_{};
        float rotation_angle_{0.0f};
    };

    uint32_t parse_frame_limit(int argc, char **argv) {
        for (int index = 1; index + 1 < argc; ++index) {
            if (std::string_view{argv[index]} == "--frames") {
                const unsigned long value = std::strtoul(argv[index + 1], nullptr, 10);
                return value > std::numeric_limits<uint32_t>::max() ? 0 : static_cast<uint32_t>(value);
            }
        }
        return 0;
    }
} // namespace

int main(int argc, char **argv) {
    if (!aeg::logging::initialize()) {
        return 2;
    }
    const LoggingGuard logging_guard{};

    WindowState window_state{};
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const HWND window = create_window(instance, window_state);
    if (window == nullptr) {
        AEG_LOG_ERROR(aeg::logging::Category::kTest, "Could not create the triangle test window");
        return 1;
    }
    ShowWindow(window, SW_SHOWDEFAULT);
    UpdateWindow(window);

    TriangleDemo demo(window, window_state);
    if (!demo.initialize()) {
        return 1;
    }
    return demo.run(parse_frame_limit(argc, argv));
}
