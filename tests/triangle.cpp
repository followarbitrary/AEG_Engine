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
#include <memory>
#include <math.hpp>
#include <render_graph.hpp>
#include <resource_uploader.hpp>
#include <rhi.hpp>
#include <span>
#include <string_view>
#include <texture_uploader.hpp>

#include "triangle_shaders.hpp"

namespace {
    using aeg::rhi::BufferCapability;
    using aeg::rhi::BufferDesc;
    using aeg::rhi::BufferHandle;
    using aeg::rhi::BindingLayoutDesc;
    using aeg::rhi::BindingLayoutEntry;
    using aeg::rhi::BindingLayoutHandle;
    using aeg::rhi::BindingSetHandle;
    using aeg::rhi::BindingType;
    using aeg::rhi::BindingWrite;
    using aeg::rhi::Device;
    using aeg::rhi::DeviceCreateDesc;
    using aeg::rhi::DrawIndexedDesc;
    using aeg::rhi::Filter;
    using aeg::rhi::Format;
    using aeg::rhi::FrameAcquireResult;
    using aeg::rhi::GraphicsPipelineState;
    using aeg::rhi::InlineConstantRange;
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
    using aeg::rhi::SamplerDesc;
    using aeg::rhi::SamplerHandle;
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
    using aeg::resource::ResourceUploader;
    using aeg::resource::TextureData;
    using aeg::resource::TextureSubresourceData;
    using aeg::resource::TextureUploader;

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
            if (!create_mesh() || !create_texture() || !create_texture_bindings() || !create_pipeline()) {
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
            render_graph_->reset();
            if (previous_format != color_format_) {
                if (!check_result(device_->destroy_pipeline(pipeline_), "destroy_pipeline_for_recreate")) {
                    return false;
                }
                pipeline_ = {};
                return create_pipeline();
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

        [[nodiscard]] bool create_pipeline() {
            const std::array<ShaderStageDesc, 2> shader_stages{
                ShaderStageDesc{
                    ShaderStage::kVertex, std::as_bytes(std::span{aeg::triangle_shaders::kVertex}), "main",
                    aeg::rhi::DebugName{"Triangle.VertexShader"}
                },
                ShaderStageDesc{
                    ShaderStage::kFragment, std::as_bytes(std::span{aeg::triangle_shaders::kFragment}), "main",
                    aeg::rhi::DebugName{"Triangle.FragmentShader"}
                },
            };
            const std::array<VertexBindingDesc, 1> vertex_bindings{
                VertexBindingDesc{0, sizeof(Vertex), VertexInputRate::kVertex},
            };
            const std::array<VertexAttributeDesc, 3> vertex_attributes{
                VertexAttributeDesc{0, 0, Format::kRg32Float, 0},
                VertexAttributeDesc{1, 0, Format::kRgba32Float, sizeof(float) * 2},
                VertexAttributeDesc{2, 0, Format::kRg32Float, sizeof(float) * 6},
            };
            const std::array<Format, 1> color_formats{color_format_};
            const std::array<aeg::rhi::BindingLayoutHandle, 1> binding_layouts{texture_binding_layout_};

            GraphicsPipelineState graphics_state{};
            graphics_state.vertex_bindings = vertex_bindings;
            graphics_state.vertex_attributes = vertex_attributes;
            graphics_state.topology = PrimitiveTopology::kTriangleList;
            graphics_state.cull_mode = aeg::rhi::CullMode::kNone;
            graphics_state.color_formats = color_formats;

            const std::array<InlineConstantRange, 1> inline_constant_ranges{
                InlineConstantRange{ShaderStage::kVertex, 0, sizeof(Mat4)}
            };

            PipelineDesc pipeline_desc{};
            pipeline_desc.type = PipelineType::kGraphics;
            pipeline_desc.layout.binding_layouts = binding_layouts;
            pipeline_desc.layout.inline_constant_ranges = inline_constant_ranges;
            pipeline_desc.shader_stages = shader_stages;
            pipeline_desc.graphics = graphics_state;
            pipeline_desc.name = aeg::rhi::DebugName{"Triangle.GraphicsPipeline"};
            auto pipeline_result = device_->create_pipeline(pipeline_desc);
            if (!check_result(pipeline_result, "create_graphics_pipeline")) {
                return false;
            }
            pipeline_ = pipeline_result.value;
            return true;
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

        [[nodiscard]] bool create_texture_bindings() {
            if (!texture_.has_sampled_view()) {
                AEG_LOG_ERROR(aeg::logging::Category::kTest,
                              "Texture upload did not create a sampled texture view");
                return false;
            }

            SamplerDesc sampler_desc{};
            sampler_desc.min_filter = Filter::kNearest;
            sampler_desc.mag_filter = Filter::kNearest;
            sampler_desc.mip_filter = Filter::kNearest;
            sampler_desc.address_u = aeg::rhi::AddressMode::kClampToEdge;
            sampler_desc.address_v = aeg::rhi::AddressMode::kClampToEdge;
            sampler_desc.address_w = aeg::rhi::AddressMode::kClampToEdge;
            sampler_desc.max_lod = 0.0f;
            sampler_desc.name = aeg::rhi::DebugName{"Triangle.TextureSampler"};
            auto sampler_result = device_->create_sampler(sampler_desc);
            if (!check_result(sampler_result, "create_texture_sampler")) {
                return false;
            }
            sampler_ = sampler_result.value;

            std::array<BindingLayoutEntry, 2> layout_entries{};
            layout_entries[0].location = {0, 0};
            layout_entries[0].type = BindingType::kSampledTexture;
            layout_entries[0].shader_stages = ShaderStage::kFragment;
            layout_entries[1].location = {0, 1};
            layout_entries[1].type = BindingType::kSampler;
            layout_entries[1].shader_stages = ShaderStage::kFragment;

            BindingLayoutDesc layout_desc{};
            layout_desc.entries = layout_entries;
            layout_desc.name = aeg::rhi::DebugName{"Triangle.TextureBindingLayout"};
            auto layout_result = device_->create_binding_layout(layout_desc);
            if (!check_result(layout_result, "create_texture_binding_layout")) {
                return false;
            }
            texture_binding_layout_ = layout_result.value;

            auto set_result = device_->create_binding_set(texture_binding_layout_);
            if (!check_result(set_result, "create_texture_binding_set")) {
                return false;
            }
            texture_binding_set_ = set_result.value;

            std::array<BindingWrite, 2> writes{};
            writes[0].location = {0, 0};
            writes[0].resource.type = BindingType::kSampledTexture;
            writes[0].resource.texture_view = texture_.sampled_view;
            writes[1].location = {0, 1};
            writes[1].resource.type = BindingType::kSampler;
            writes[1].resource.sampler = sampler_;
            return check_result(device_->update_binding_set(texture_binding_set_, writes),
                                "update_texture_binding_set");
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

            render_graph_->reset();

            BufferDesc vertex_desc{};
            vertex_desc.size = sizeof(kTriangleVertices);
            vertex_desc.capabilities = BufferCapability::kTransferDestination | BufferCapability::kVertex;
            vertex_desc.memory = MemoryClass::kDeviceLocal;
            vertex_desc.name = aeg::rhi::DebugName{"Triangle.VertexBuffer"};
            auto vertex_resource_result = render_graph_->import_buffer(
                    mesh_.vertex_buffer, vertex_desc, mesh_vertex_state_);
            if (!check_result(vertex_resource_result, "import_vertex_buffer")) {
                return false;
            }

            BufferDesc index_desc{};
            index_desc.size = static_cast<uint64_t>(mesh_.index_count) *
                              (mesh_.index_format == aeg::rhi::IndexFormat::kUint16 ? sizeof(uint16_t)
                                                                                     : sizeof(uint32_t));
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

            aeg::rhi::TextureDesc color_desc{};
            color_desc.extent = {swapchain_info_.extent.width, swapchain_info_.extent.height, 1};
            color_desc.format = color_format_;
            color_desc.capabilities = aeg::rhi::TextureCapability::kColorAttachment;
            color_desc.memory = MemoryClass::kDeviceLocal;
            color_desc.initial_usage = ResourceUsage::kPresent;
            color_desc.initial_stages = PipelineStage::kAllCommands;
            color_desc.initial_queue = QueueClass::kGraphics;
            color_desc.name = aeg::rhi::DebugName{"Triangle.SwapchainImage"};
            const aeg::rhi::ResourceState present_state{
                ResourceUsage::kPresent,
                ShaderStage::kAll,
                QueueClass::kGraphics,
                false,
                PipelineStage::kAllCommands
            };
            auto color_resource_result = render_graph_->import_swapchain_image(acquired, color_desc, present_state);
            if (!check_result(color_resource_result, "import_swapchain_image")) {
                return false;
            }

            const RenderGraphBuffer vertex_resource = vertex_resource_result.value;
            const RenderGraphBuffer index_resource = index_resource_result.value;
            const RenderGraphTexture texture_resource = texture_resource_result.value;
            const auto color_resource = color_resource_result.value;
            auto pass_result = render_graph_->add_pass(
                    "Triangle.Graphics", QueueClass::kGraphics,
                    [vertex_resource, index_resource, texture_resource, color_resource](PassBuilder &builder) {
                        if (const Result result = builder.read(vertex_resource, ResourceUsage::kVertexInput,
                                                               PipelineStage::kVertexInput, ShaderStage::kVertex,
                                                               {});
                            !result) {
                            return result;
                        }
                        if (const Result result = builder.read(index_resource, ResourceUsage::kIndexInput,
                                                               PipelineStage::kVertexInput, ShaderStage::kVertex,
                                                               {});
                            !result) {
                            return result;
                        }
                        if (const Result result = builder.read(texture_resource, ResourceUsage::kSampledRead,
                                                               PipelineStage::kAllCommands, ShaderStage::kAll, {});
                            !result) {
                            return result;
                        }
                        if (const Result result = builder.use_color_attachment(
                                    color_resource, LoadOp::kClear, aeg::rhi::StoreOp::kStore,
                                    aeg::rhi::ClearColorValue{0.025f, 0.035f, 0.06f, 1.0f},
                                    ResourceUsage::kPresent);
                            !result) {
                            return result;
                        }
                        builder.set_side_effects();
                        return Result::success();
                    },
                    [this, vertex_resource, index_resource](PassExecutionContext &context) {
                        auto vertex_buffer_result = context.buffer(vertex_resource);
                        if (!vertex_buffer_result) {
                            return vertex_buffer_result.result;
                        }
                        auto index_buffer_result = context.buffer(index_resource);
                        if (!index_buffer_result) {
                            return index_buffer_result.result;
                        }
                        if (const Result result = context.device().bind_pipeline(context.command_list(), pipeline_);
                            !result) {
                            return result;
                        }
                        if (const Result result = context.device().bind_binding_set(
                                    context.command_list(), 0, texture_binding_set_);
                            !result) {
                            return result;
                        }
                        if (const Result result = context.device().set_inline_constants(
                                    context.command_list(), 0, ShaderStage::kVertex,
                                    std::as_bytes(std::span{&model_view_projection_, 1}));
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
                            static_cast<float>(swapchain_info_.extent.width),
                            static_cast<float>(swapchain_info_.extent.height),
                            0.0f,
                            1.0f
                        };
                        const aeg::rhi::ScissorRect scissor{
                            0, 0, swapchain_info_.extent.width, swapchain_info_.extent.height
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
            if (!check_result(pass_result, "add_triangle_pass")) {
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

            if (texture_binding_set_.is_valid()) {
                (void) device_->destroy_binding_set(texture_binding_set_);
                texture_binding_set_ = {};
            }
            if (pipeline_.is_valid()) {
                (void) device_->destroy_pipeline(pipeline_);
                pipeline_ = {};
            }
            if (texture_binding_layout_.is_valid()) {
                (void) device_->destroy_binding_layout(texture_binding_layout_);
                texture_binding_layout_ = {};
            }
            if (sampler_.is_valid()) {
                (void) device_->destroy_sampler(sampler_);
                sampler_ = {};
            }
            if (texture_uploader_ != nullptr && texture_.is_valid()) {
                (void) texture_uploader_->destroy_texture(texture_);
            }
            if (resource_uploader_ != nullptr && mesh_.is_valid()) {
                (void) resource_uploader_->destroy_mesh(mesh_);
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
        std::unique_ptr<FrameContext> frame_context_{};
        std::unique_ptr<RenderGraph> render_graph_{};
        std::unique_ptr<ResourceUploader> resource_uploader_{};
        std::unique_ptr<TextureUploader> texture_uploader_{};
        SurfaceHandle surface_{};
        SwapchainHandle swapchain_{};
        SwapchainInfo swapchain_info_{};
        GpuMesh mesh_{};
        GpuTexture texture_{};
        PipelineHandle pipeline_{};
        BindingLayoutHandle texture_binding_layout_{};
        BindingSetHandle texture_binding_set_{};
        SamplerHandle sampler_{};
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
