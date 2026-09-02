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
#include <rhi.hpp>
#include <span>
#include <string_view>

#include "triangle_shaders.hpp"

namespace {
    using aeg::rhi::BufferCapability;
    using aeg::rhi::BufferDesc;
    using aeg::rhi::BufferHandle;
    using aeg::rhi::ColorAttachmentDesc;
    using aeg::rhi::CommandListDesc;
    using aeg::rhi::CommandListHandle;
    using aeg::rhi::Device;
    using aeg::rhi::DeviceCreateDesc;
    using aeg::rhi::DrawDesc;
    using aeg::rhi::Format;
    using aeg::rhi::FrameAcquireResult;
    using aeg::rhi::FrameSubmitDesc;
    using aeg::rhi::GraphicsPipelineState;
    using aeg::rhi::InlineConstantRange;
    using aeg::rhi::LoadOp;
    using aeg::rhi::MemoryClass;
    using aeg::rhi::PipelineDesc;
    using aeg::rhi::PipelineHandle;
    using aeg::rhi::PipelineStage;
    using aeg::rhi::PipelineType;
    using aeg::rhi::PresentDesc;
    using aeg::rhi::PresentMode;
    using aeg::rhi::PrimitiveTopology;
    using aeg::rhi::QueueClass;
    using aeg::rhi::RenderingDesc;
    using aeg::rhi::ResourceUsage;
    using aeg::foundation::Result;
    using aeg::foundation::ResultCode;
    using aeg::rhi::ScissorRect;
    using aeg::rhi::ShaderStage;
    using aeg::rhi::ShaderStageDesc;
    using aeg::rhi::StoreOp;
    using aeg::rhi::SurfaceDesc;
    using aeg::rhi::SurfaceHandle;
    using aeg::rhi::SwapchainDesc;
    using aeg::rhi::SwapchainHandle;
    using aeg::rhi::SwapchainInfo;
    using aeg::rhi::TextureViewHandle;
    using aeg::rhi::VertexAttributeDesc;
    using aeg::rhi::VertexBindingDesc;
    using aeg::rhi::VertexInputRate;
    using aeg::rhi::Viewport;
    using aeg::math::ClipSpaceConvention;
    using aeg::math::Mat4;
    using aeg::math::Quat;
    using aeg::math::Transform;
    using aeg::math::Vec3;

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
    };

    constexpr std::array<Vertex, 3> kTriangleVertices{
        Vertex{{0.0f, -0.75f}, {1.0f, 0.2f, 0.2f, 1.0f}},
        Vertex{{0.75f, 0.65f}, {0.2f, 1.0f, 0.3f, 1.0f}},
        Vertex{{-0.75f, 0.65f}, {0.2f, 0.4f, 1.0f, 1.0f}},
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

            if (!create_swapchain() || !create_vertex_buffer() || !create_command_list() || !create_pipeline()) {
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

                if (command_list_recorded_) {
                    if (!check_result(device_->reset_command_list(command_list_), "reset_command_list")) {
                        return 1;
                    }
                    command_list_recorded_ = false;
                }

                auto acquire_result = device_->acquire_next_image(swapchain_);
                if (!acquire_result) {
                    if (acquire_result.result.code == ResultCode::kOutOfDate) {
                        window_state_.resize_pending = true;
                        continue;
                    }
                    check_result(acquire_result, "acquire_next_image");
                    return 1;
                }
                const FrameAcquireResult &acquired = acquire_result.value;
                if (acquired.suboptimal) {
                    window_state_.resize_pending = true;
                }

                if (!record_frame(acquired)) {
                    (void) device_->cancel_frame(acquired.frame);
                    return 1;
                }

                const std::array<CommandListHandle, 1> command_lists{command_list_};
                FrameSubmitDesc submit_desc{};
                submit_desc.frame = acquired.frame;
                submit_desc.queue = QueueClass::kGraphics;
                submit_desc.command_lists = command_lists;
                auto submit_result = device_->submit_frame(submit_desc);
                if (!check_result(submit_result, "submit_frame")) {
                    (void) device_->cancel_frame(acquired.frame);
                    return 1;
                }
                command_list_recorded_ = true;

                PresentDesc present_desc{};
                present_desc.frame = acquired.frame;
                auto present_result = device_->present(present_desc);
                if (!present_result && present_result.result.code != ResultCode::kOutOfDate) {
                    check_result(present_result, "present");
                    return 1;
                }
                if (present_result.result.code == ResultCode::kOutOfDate ||
                    (present_result && present_result.value.suboptimal)) {
                    window_state_.resize_pending = true;
                }

                if (!check_result(device_->wait(submit_result.value.completion, std::numeric_limits<uint64_t>::max()),
                                  "wait_frame")) {
                    return 1;
                }

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
            if (command_list_recorded_) {
                if (!check_result(device_->reset_command_list(command_list_), "reset_command_list_before_recreate")) {
                    return false;
                }
                command_list_recorded_ = false;
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
            if (previous_format != color_format_) {
                if (!check_result(device_->destroy_pipeline(pipeline_), "destroy_pipeline_for_recreate")) {
                    return false;
                }
                pipeline_ = {};
                return create_pipeline();
            }
            return true;
        }

        [[nodiscard]] bool create_vertex_buffer() {
            BufferDesc buffer_desc{};
            buffer_desc.size = sizeof(kTriangleVertices);
            buffer_desc.capabilities = BufferCapability::kVertex;
            buffer_desc.memory = MemoryClass::kUpload;
            buffer_desc.initial_usage = ResourceUsage::kVertexInput;
            buffer_desc.initial_stages = PipelineStage::kVertexInput;
            buffer_desc.initial_queue = QueueClass::kGraphics;
            buffer_desc.name = aeg::rhi::DebugName{"Triangle.VertexBuffer"};
            auto buffer_result = device_->create_buffer(buffer_desc);
            if (!check_result(buffer_result, "create_vertex_buffer")) {
                return false;
            }
            vertex_buffer_ = buffer_result.value;

            const std::span<const std::byte> vertex_bytes = std::as_bytes(std::span{kTriangleVertices});
            return check_result(device_->write_buffer(vertex_buffer_, 0, vertex_bytes), "write_vertex_buffer");
        }

        [[nodiscard]] bool create_command_list() {
            CommandListDesc command_desc{};
            command_desc.queue = QueueClass::kGraphics;
            command_desc.one_time_submit = true;
            command_desc.name = aeg::rhi::DebugName{"Triangle.CommandList"};
            auto command_result = device_->create_command_list(command_desc);
            if (!check_result(command_result, "create_command_list")) {
                return false;
            }
            command_list_ = command_result.value;
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
            const std::array<VertexAttributeDesc, 2> vertex_attributes{
                VertexAttributeDesc{0, 0, Format::kRg32Float, 0},
                VertexAttributeDesc{1, 0, Format::kRgba32Float, sizeof(float) * 2},
            };
            const std::array<Format, 1> color_formats{color_format_};

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

            if (!check_result(device_->begin_command_list(command_list_), "begin_command_list")) {
                return false;
            }

            ColorAttachmentDesc color_attachment{};
            color_attachment.view = acquired.image.view;
            color_attachment.load_op = LoadOp::kClear;
            color_attachment.store_op = StoreOp::kStore;
            color_attachment.clear_color = {0.025f, 0.035f, 0.06f, 1.0f};
            color_attachment.final_usage = ResourceUsage::kPresent;
            const std::array<ColorAttachmentDesc, 1> color_attachments{color_attachment};

            RenderingDesc rendering_desc{};
            rendering_desc.color_attachments = color_attachments;
            rendering_desc.name = aeg::rhi::DebugName{"Triangle.Rendering"};
            if (!check_result(device_->begin_rendering(command_list_, rendering_desc), "begin_rendering")) {
                return false;
            }

            const Viewport viewport{
                0.0f,
                0.0f,
                static_cast<float>(swapchain_info_.extent.width),
                static_cast<float>(swapchain_info_.extent.height),
                0.0f,
                1.0f
            };
            const ScissorRect scissor{0, 0, swapchain_info_.extent.width, swapchain_info_.extent.height};
            if (!check_result(device_->set_viewport(command_list_, viewport), "set_viewport") ||
                !check_result(device_->set_scissor(command_list_, scissor), "set_scissor") ||
                !check_result(device_->bind_pipeline(command_list_, pipeline_), "bind_pipeline") ||
                !check_result(device_->set_inline_constants(command_list_, 0, ShaderStage::kVertex,
                                                            std::as_bytes(std::span{&model_view_projection_, 1})),
                              "set_inline_constants") ||
                !check_result(device_->bind_vertex_buffer(command_list_, 0, vertex_buffer_),
                              "bind_vertex_buffer") ||
                !check_result(device_->draw(command_list_, DrawDesc{3, 1, 0, 0}), "draw")) {
                return false;
            }
            if (!check_result(device_->end_rendering(command_list_), "end_rendering")) {
                return false;
            }
            return check_result(device_->end_command_list(command_list_), "end_command_list");
        }

        void cleanup() {
            if (device_ == nullptr) {
                return;
            }
            (void) device_->wait_idle();
            if (command_list_.is_valid()) {
                (void) device_->destroy_command_list(command_list_);
                command_list_ = {};
            }
            if (pipeline_.is_valid()) {
                (void) device_->destroy_pipeline(pipeline_);
                pipeline_ = {};
            }
            if (vertex_buffer_.is_valid()) {
                (void) device_->destroy_buffer(vertex_buffer_);
                vertex_buffer_ = {};
            }
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
        SurfaceHandle surface_{};
        SwapchainHandle swapchain_{};
        SwapchainInfo swapchain_info_{};
        BufferHandle vertex_buffer_{};
        PipelineHandle pipeline_{};
        CommandListHandle command_list_{};
        Format color_format_{Format::kUnknown};
        Mat4 model_view_projection_{};
        float rotation_angle_{0.0f};
        bool command_list_recorded_{false};
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
