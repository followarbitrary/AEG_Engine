/**
 * @file imgui_renderer.cpp
 * @brief Implements the backend-neutral Dear ImGui RenderFeature.
 */
#include "../../public/imgui_renderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

#include "frame_context.hpp"
#include "math.hpp"
#include "render_graph.hpp"
#include "resource_uploader.hpp"
#include "rhi.hpp"
#include "shader_contract.hpp"

namespace aeg::editor {
namespace {
inline constexpr pipeline::FeatureId kImGuiRenderFeatureId{"Editor.ImGuiRenderFeature"};
inline constexpr pipeline::FeaturePointId kImGuiFeaturePointId{"RenderPipeline.BeforePresent"};
inline constexpr pipeline::StageId kImGuiStageId{"Editor.ImGuiRenderFeature"};
inline constexpr pipeline::RenderResourceId kImGuiVertexResourceId{"Editor.ImGui.VertexBuffer"};
inline constexpr pipeline::RenderResourceId kImGuiIndexResourceId{"Editor.ImGui.IndexBuffer"};
inline constexpr pipeline::RenderResourceId kImGuiProjectionResourceId{"Editor.ImGui.ProjectionBuffer"};

struct ImGuiTextureHandleHash final {
    [[nodiscard]] std::size_t operator()(ImGuiTextureHandle handle) const noexcept {
        const uint64_t value = (static_cast<uint64_t>(handle.generation) << 32u) | handle.index;
        return static_cast<std::size_t>(value ^ (value >> 32u));
    }
};

[[nodiscard]] pipeline::RenderResourceId make_imgui_texture_resource_id(ImGuiTextureHandle texture) noexcept {
    uint64_t value = 0x4541475f494d4755ull;
    value ^= static_cast<uint64_t>(texture.generation) << 32u;
    value ^= static_cast<uint64_t>(texture.index) + 0x9e3779b97f4a7c15ull + (value << 6u) + (value >> 2u);
    return pipeline::RenderResourceId(value == 0 ? 1 : value);
}

[[nodiscard]] pipeline::RenderGraphBuffer to_pipeline_buffer(render_graph::RenderGraphBuffer buffer) noexcept {
    return pipeline::RenderGraphBuffer{buffer.index, buffer.generation, buffer.graph_id};
}

[[nodiscard]] pipeline::RenderGraphTexture to_pipeline_texture(render_graph::RenderGraphTexture texture) noexcept {
    return pipeline::RenderGraphTexture{texture.index, texture.generation, texture.graph_id};
}

[[nodiscard]] bool is_finite(float value) noexcept {
    return std::isfinite(value);
}

[[nodiscard]] bool is_finite(const ImGuiDrawPacket& packet) noexcept {
    return is_finite(packet.display_pos_x) && is_finite(packet.display_pos_y) && is_finite(packet.display_width) &&
           is_finite(packet.display_height) && is_finite(packet.framebuffer_scale_x) &&
           is_finite(packet.framebuffer_scale_y) && packet.framebuffer_scale_x > 0.0f &&
           packet.framebuffer_scale_y > 0.0f;
}

[[nodiscard]] foundation::Result first_failure(const foundation::Result& current, const foundation::Result& next) {
    return current ? next : current;
}

[[nodiscard]] math::Mat4 make_imgui_projection(const ImGuiDrawPacket& packet) noexcept {
    const float left = packet.display_pos_x;
    const float right = left + packet.display_width;
    const float top = packet.display_pos_y;
    const float bottom = top + packet.display_height;

    math::Mat4 projection{};
    projection.at(0, 0) = 2.0f / (right - left);
    projection.at(1, 1) = 2.0f / (bottom - top);
    projection.at(2, 2) = 1.0f;
    projection.at(3, 3) = 1.0f;
    projection.at(0, 3) = (right + left) / (left - right);
    projection.at(1, 3) = (top + bottom) / (top - bottom);
    return projection;
}

[[nodiscard]] foundation::ResultValue<rhi::ScissorRect> make_scissor(const ImGuiDrawPacket& packet,
                                                                     const ImGuiDrawCommand& command,
                                                                     const ImGuiRenderTargetDesc& target) {
    const float min_x = std::floor((command.clip_min_x - packet.display_pos_x) * packet.framebuffer_scale_x);
    const float min_y = std::floor((command.clip_min_y - packet.display_pos_y) * packet.framebuffer_scale_y);
    const float max_x = std::ceil((command.clip_max_x - packet.display_pos_x) * packet.framebuffer_scale_x);
    const float max_y = std::ceil((command.clip_max_y - packet.display_pos_y) * packet.framebuffer_scale_y);
    if (!is_finite(min_x) || !is_finite(min_y) || !is_finite(max_x) || !is_finite(max_y)) {
        return {foundation::invalid_argument("ImGui draw command clip rectangle is not finite"), {}};
    }

    const float framebuffer_width = static_cast<float>(target.extent.width);
    const float framebuffer_height = static_cast<float>(target.extent.height);
    const float clipped_min_x = std::clamp(min_x, 0.0f, framebuffer_width);
    const float clipped_min_y = std::clamp(min_y, 0.0f, framebuffer_height);
    const float clipped_max_x = std::clamp(max_x, 0.0f, framebuffer_width);
    const float clipped_max_y = std::clamp(max_y, 0.0f, framebuffer_height);
    if (clipped_max_x <= clipped_min_x || clipped_max_y <= clipped_min_y) {
        return {foundation::Result::success(), rhi::ScissorRect{0, 0, 0, 0}};
    }

    const auto to_int32 = [](float value) {
        return static_cast<int32_t>(std::clamp(value, 0.0f, static_cast<float>(std::numeric_limits<int32_t>::max())));
    };
    const int32_t x = to_int32(clipped_min_x);
    const int32_t y = to_int32(clipped_min_y);
    const uint32_t width = static_cast<uint32_t>(clipped_max_x - clipped_min_x);
    const uint32_t height = static_cast<uint32_t>(clipped_max_y - clipped_min_y);
    return {foundation::Result::success(), rhi::ScissorRect{x, y, width, height}};
}

[[nodiscard]] foundation::Result validate_texture_binding(const ImGuiTextureBinding& binding) {
    if (!binding.texture.is_valid() || !binding.sampled_view.is_valid() || !binding.sampler.is_valid()) {
        return foundation::invalid_argument(
            "ImGui renderer requires a texture, sampled view, and sampler for every draw texture");
    }
    if (binding.texture_desc.format == rhi::Format::kUnknown || binding.texture_desc.extent.width == 0 ||
        binding.texture_desc.extent.height == 0 || binding.texture_desc.extent.depth == 0 ||
        binding.texture_desc.mip_levels == 0 || binding.texture_desc.array_layers == 0 ||
        binding.texture_desc.capabilities == rhi::TextureCapability::kNone) {
        return foundation::invalid_argument("ImGui texture binding has an incomplete texture description");
    }
    if (!rhi::has_flag(binding.texture_desc.capabilities, rhi::TextureCapability::kSampled)) {
        return foundation::invalid_argument("ImGui texture binding texture lacks Sampled capability");
    }
    return foundation::Result::success();
}

[[nodiscard]] rhi::TextureDesc make_cached_texture_desc(const rhi::TextureDesc& desc) noexcept {
    rhi::TextureDesc cached = desc;
    cached.compatible_view_formats = {};
    return cached;
}

[[nodiscard]] bool texture_desc_matches(const rhi::TextureDesc& lhs, const rhi::TextureDesc& rhs) noexcept {
    if (lhs.dimension != rhs.dimension || lhs.extent.width != rhs.extent.width ||
        lhs.extent.height != rhs.extent.height || lhs.extent.depth != rhs.extent.depth || lhs.format != rhs.format ||
        lhs.mip_levels != rhs.mip_levels || lhs.array_layers != rhs.array_layers || lhs.samples != rhs.samples ||
        lhs.capabilities != rhs.capabilities || lhs.memory != rhs.memory || lhs.flags != rhs.flags ||
        lhs.initial_usage != rhs.initial_usage || lhs.initial_stages != rhs.initial_stages ||
        lhs.initial_queue != rhs.initial_queue) {
        return false;
    }
    return true;
}

[[nodiscard]] rhi::ResourceState make_imgui_sampled_state() noexcept {
    return rhi::ResourceState{
        rhi::ResourceUsage::kSampledRead,
        rhi::ShaderStage::kFragment,
        rhi::QueueClass::kGraphics,
        false,
        rhi::PipelineStage::kFragmentShader,
    };
}
}  // namespace

struct ImGuiRenderer::Impl final {
    struct TextureEntry final {
        ImGuiTextureHandle identity{};
        ImGuiTextureBinding binding{};
        render_graph::RenderGraphTexture resource{};
        rhi::BindingSetHandle binding_set{};
    };

    struct DrawCall final {
        uint32_t texture_index{0};
        rhi::ScissorRect scissor{};
        uint32_t index_count{0};
        uint32_t first_index{0};
        int32_t vertex_offset{0};
    };

    struct ImportedTexture final {
        render_graph::RenderGraphTexture resource{};
        rhi::TextureHandle texture{};
        rhi::TextureViewHandle sampled_view{};
        rhi::TextureDesc desc{};
        rhi::ResourceState state{};
        bool state_initialized{false};
    };

    struct PreparedFrame final {
        explicit PreparedFrame(rhi::Device& device_reference) noexcept : device(&device_reference) {}

        ~PreparedFrame() {
            if (device == nullptr) {
                return;
            }
            try {
                static_cast<void>(release_rhi_resources());
            } catch (...) {
                // Destructors must remain best-effort at the module boundary.
            }
        }

        PreparedFrame(const PreparedFrame&) = delete;
        PreparedFrame& operator=(const PreparedFrame&) = delete;

        [[nodiscard]] foundation::Result release_rhi_resources() {
            if (device == nullptr) {
                return foundation::Result::success();
            }

            foundation::Result first_result = foundation::Result::success();
            for (TextureEntry& texture : textures) {
                if (!texture.binding_set.is_valid()) {
                    continue;
                }
                const foundation::Result result = device->destroy_binding_set(texture.binding_set);
                if (result || result.code == foundation::ResultCode::kNotFound) {
                    texture.binding_set = {};
                } else {
                    first_result = first_failure(first_result, result);
                }
            }

            const auto release_buffer = [this, &first_result](rhi::BufferHandle& buffer) {
                if (!buffer.is_valid()) {
                    return;
                }
                const foundation::Result result = device->destroy_buffer(buffer);
                if (result || result.code == foundation::ResultCode::kNotFound) {
                    buffer = {};
                } else {
                    first_result = first_failure(first_result, result);
                }
            };
            release_buffer(projection_buffer);
            release_buffer(index_buffer);
            release_buffer(vertex_buffer);
            return first_result;
        }

        rhi::Device* device{nullptr};
        uint64_t frame_number{0};
        ImGuiRenderTargetDesc target{};
        render_graph::RenderGraphBuffer vertex_resource{};
        render_graph::RenderGraphBuffer index_resource{};
        render_graph::RenderGraphBuffer projection_resource{};
        uint64_t vertex_size{0};
        uint64_t index_size{0};
        rhi::BufferHandle vertex_buffer{};
        rhi::BufferHandle index_buffer{};
        rhi::BufferHandle projection_buffer{};
        rhi::PipelineHandle pipeline{};
        std::vector<TextureEntry> textures{};
        std::vector<DrawCall> draw_calls{};
    };

    Impl(rhi::Device& device_reference, ImGuiTextureRegistry& registry_reference)
        : device(device_reference), texture_registry(registry_reference), resource_uploader(device_reference) {}

    /** @brief Stores a prepared frame while its FrameContext may still retain GPU references. */
    [[nodiscard]] foundation::Result store_prepared_frame(std::shared_ptr<PreparedFrame> prepared) {
        if (prepared == nullptr) {
            return foundation::invalid_argument("ImGui prepared frame cannot be empty");
        }
        prepared_frame_history.erase(
            std::remove_if(prepared_frame_history.begin(), prepared_frame_history.end(),
                           [](const std::weak_ptr<PreparedFrame>& frame) { return frame.expired(); }),
            prepared_frame_history.end());
        try {
            prepared_frame_history.emplace_back(prepared);
        } catch (const std::bad_alloc&) {
            return foundation::out_of_memory("ImGui prepared frame history allocation failed");
        }
        prepared_frame = std::move(prepared);
        return foundation::Result::success();
    }

    rhi::Device& device;
    ImGuiTextureRegistry& texture_registry;
    resource::ResourceUploader resource_uploader;
    std::unique_ptr<shader::ShaderManager> shader_manager{};
    shader::ShaderProgram shader_program{};
    shader::ShaderPipelineLayout shader_layout{};
    rhi::BindingLayoutHandle binding_layout{};
    std::unordered_map<uint16_t, rhi::PipelineHandle> pipelines{};
    std::unordered_map<ImGuiTextureHandle, ImportedTexture, ImGuiTextureHandleHash> imported_textures{};
    std::shared_ptr<PreparedFrame> prepared_frame{};
    std::vector<std::weak_ptr<PreparedFrame>> prepared_frame_history{};
    bool initialized{false};
};

ImGuiRenderer::ImGuiRenderer(rhi::Device& device, ImGuiTextureRegistry& texture_registry)
    : impl_(std::make_unique<Impl>(device, texture_registry)) {}

ImGuiRenderer::~ImGuiRenderer() {
    if (impl_ == nullptr) {
        return;
    }
    static_cast<void>(release_prepared_frame());
    if (impl_->initialized) {
        static_cast<void>(shutdown());
    }
}

foundation::Result ImGuiRenderer::initialize() {
    if (impl_ == nullptr) {
        return foundation::invalid_state("ImGui renderer implementation is not initialized");
    }
    if (impl_->initialized) {
        return foundation::invalid_state("ImGui renderer is already initialized");
    }
    try {
        const foundation::ResultValue<shader::ShaderProgram> program_result =
            shader::load_shader_program(std::filesystem::path{AEG_EDITOR_IMGUI_SHADER_PACKAGE_PATH});
        if (!program_result) {
            return program_result.result;
        }

        auto shader_manager = std::make_unique<shader::ShaderManager>(impl_->device);
        const foundation::ResultValue<shader::ShaderPipelineLayout> layout_result =
            shader_manager->build_pipeline_layout(program_result.value, rhi::DebugName{"Editor.ImGui.BindingLayout"});
        if (!layout_result) {
            return layout_result.result;
        }
        const foundation::ResultValue<rhi::BindingLayoutHandle> binding_layout_result =
            shader_manager->binding_layout(layout_result.value, 0);
        if (!binding_layout_result) {
            return binding_layout_result.result;
        }

        impl_->shader_manager = std::move(shader_manager);
        impl_->shader_program = program_result.value;
        impl_->shader_layout = layout_result.value;
        impl_->binding_layout = binding_layout_result.value;
        impl_->initialized = true;
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("ImGui shader contract initialization allocation failed");
    } catch (const std::exception& exception) {
        return foundation::unknown_exception(exception);
    }
}

foundation::ResultValue<rhi::PipelineHandle> ImGuiRenderer::get_or_create_pipeline(rhi::Format format) {
    if (impl_ == nullptr || !impl_->initialized) {
        return {foundation::invalid_state("ImGui renderer is not initialized"), {}};
    }
    const uint16_t format_key = static_cast<uint16_t>(format);
    const auto cached = impl_->pipelines.find(format_key);
    if (cached != impl_->pipelines.end()) {
        return {foundation::Result::success(), cached->second};
    }

    const std::array<rhi::VertexBindingDesc, 1> vertex_bindings{
        rhi::VertexBindingDesc{0, sizeof(ImGuiDrawVertex), rhi::VertexInputRate::kVertex},
    };
    const std::array<rhi::VertexAttributeDesc, 3> vertex_attributes{
        rhi::VertexAttributeDesc{0, 0, rhi::Format::kRg32Float, 0},
        rhi::VertexAttributeDesc{1, 0, rhi::Format::kRg32Float, sizeof(float) * 2},
        rhi::VertexAttributeDesc{2, 0, rhi::Format::kR32Uint, sizeof(float) * 4},
    };
    const std::array<rhi::Format, 1> color_formats{format};
    const std::array<rhi::BlendAttachmentDesc, 1> blend_attachments{
        rhi::BlendAttachmentDesc{
            true,
            rhi::BlendFactor::kSourceAlpha,
            rhi::BlendFactor::kOneMinusSourceAlpha,
            rhi::BlendOp::kAdd,
            rhi::BlendFactor::kOne,
            rhi::BlendFactor::kOneMinusSourceAlpha,
            rhi::BlendOp::kAdd,
            rhi::ColorComponent::kAll,
        },
    };
    rhi::GraphicsPipelineState graphics{};
    graphics.vertex_bindings = vertex_bindings;
    graphics.vertex_attributes = vertex_attributes;
    graphics.topology = rhi::PrimitiveTopology::kTriangleList;
    graphics.cull_mode = rhi::CullMode::kNone;
    graphics.depth_test_enable = false;
    graphics.depth_write_enable = false;
    graphics.color_formats = color_formats;
    graphics.blend_attachments = blend_attachments;

    if (impl_->shader_manager == nullptr || !impl_->shader_layout.is_valid()) {
        return {foundation::invalid_state("ImGui shader contract layout is not initialized"), {}};
    }
    const shader::ShaderPipelineDesc pipeline_desc{
        rhi::PipelineType::kGraphics,
        graphics,
        rhi::DebugName{"Editor.ImGui.DrawMesh"},
    };
    const foundation::ResultValue<rhi::PipelineHandle> pipeline_result =
        impl_->shader_manager->create_pipeline(impl_->shader_program, impl_->shader_layout, pipeline_desc);
    if (!pipeline_result) {
        return pipeline_result;
    }
    try {
        impl_->pipelines.emplace(format_key, pipeline_result.value);
    } catch (const std::bad_alloc&) {
        static_cast<void>(impl_->device.destroy_pipeline(pipeline_result.value));
        return {foundation::out_of_memory("ImGui pipeline cache allocation failed"), {}};
    }
    return pipeline_result;
}

foundation::Result ImGuiRenderer::prepare_frame(render_graph::FrameContext& frame_context,
                                                render_graph::RenderGraph& graph, const ImGuiDrawPacket& packet,
                                                const ImGuiRenderTargetDesc& target) {
    if (impl_ == nullptr || !impl_->initialized) {
        return foundation::invalid_state("ImGui renderer is not initialized");
    }
    if (&frame_context.device() != &impl_->device || &graph.device() != &impl_->device) {
        return foundation::invalid_argument("ImGui renderer objects must use the same RHI device");
    }
    if (!frame_context.is_frame_active()) {
        return foundation::invalid_state("ImGui renderer preparation requires an active FrameContext frame");
    }
    if (!target.resource_id.is_valid() || target.format == rhi::Format::kUnknown || target.extent.width == 0 ||
        target.extent.height == 0 || target.extent.width > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
        target.extent.height > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
        return foundation::invalid_argument("ImGui render target description is invalid");
    }
    if (!is_finite(packet)) {
        return foundation::invalid_argument("ImGui draw packet contains invalid display parameters");
    }

    const uint64_t active_frame_number = frame_context.frame_number() - 1;
    if (impl_->prepared_frame != nullptr && impl_->prepared_frame->frame_number == active_frame_number) {
        return foundation::invalid_state("ImGui renderer already prepared the active frame");
    }
    std::shared_ptr<Impl::PreparedFrame> prepared;
    try {
        prepared = std::make_shared<Impl::PreparedFrame>(impl_->device);
        prepared->frame_number = active_frame_number;
        prepared->target = target;
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("ImGui frame state allocation failed");
    }

    const auto cleanup = [&prepared] {
        for (Impl::TextureEntry& texture : prepared->textures) {
            if (texture.binding_set.is_valid()) {
                static_cast<void>(prepared->device->destroy_binding_set(texture.binding_set));
                texture.binding_set = {};
            }
        }
        const auto destroy_buffer = [&prepared](rhi::BufferHandle& buffer) {
            if (buffer.is_valid()) {
                static_cast<void>(prepared->device->destroy_buffer(buffer));
                buffer = {};
            }
        };
        destroy_buffer(prepared->vertex_buffer);
        destroy_buffer(prepared->index_buffer);
        destroy_buffer(prepared->projection_buffer);
    };

    if (packet.empty()) {
        return impl_->store_prepared_frame(std::move(prepared));
    }
    if (packet.display_width <= 0.0f || packet.display_height <= 0.0f || packet.vertices.empty() ||
        packet.indices.empty()) {
        return foundation::invalid_argument("ImGui draw packet has no drawable geometry");
    }
    if (packet.vertices.size() > std::numeric_limits<uint32_t>::max() ||
        packet.indices.size() > std::numeric_limits<uint32_t>::max()) {
        return foundation::invalid_argument("ImGui draw packet exceeds RHI indexable limits");
    }

    std::unordered_map<ImGuiTextureHandle, uint32_t, ImGuiTextureHandleHash> texture_indices;
    try {
        texture_indices.reserve(packet.commands.size());
        prepared->textures.reserve(packet.commands.size());
        prepared->draw_calls.reserve(packet.commands.size());
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("ImGui draw packet indexing allocation failed");
    }

    for (const ImGuiDrawCommand& command : packet.commands) {
        if (command.elem_count == 0) {
            continue;
        }
        const uint64_t index_end = static_cast<uint64_t>(command.index_offset) + command.elem_count;
        if (index_end > packet.indices.size()) {
            return foundation::invalid_argument("ImGui draw command index range is out of bounds");
        }
        for (uint64_t index = command.index_offset; index < index_end; ++index) {
            const int64_t vertex_index =
                static_cast<int64_t>(packet.indices[static_cast<std::size_t>(index)]) + command.vertex_offset;
            if (vertex_index < 0 || static_cast<uint64_t>(vertex_index) >= packet.vertices.size()) {
                return foundation::invalid_argument("ImGui draw command vertex range is out of bounds");
            }
        }

        const foundation::ResultValue<rhi::ScissorRect> scissor_result = make_scissor(packet, command, target);
        if (!scissor_result) {
            return scissor_result.result;
        }
        if (scissor_result.value.width == 0 || scissor_result.value.height == 0) {
            continue;
        }

        uint32_t texture_index = 0;
        const auto existing_texture = texture_indices.find(command.texture);
        if (existing_texture != texture_indices.end()) {
            texture_index = existing_texture->second;
        } else {
            const foundation::ResultValue<ImGuiTextureBinding> binding_result =
                impl_->texture_registry.resolve(command.texture);
            if (!binding_result) {
                return binding_result.result;
            }
            if (const foundation::Result result = validate_texture_binding(binding_result.value); !result) {
                return result;
            }
            texture_index = static_cast<uint32_t>(prepared->textures.size());
            try {
                texture_indices.emplace(command.texture, texture_index);
                prepared->textures.push_back(Impl::TextureEntry{command.texture, binding_result.value});
            } catch (const std::bad_alloc&) {
                return foundation::out_of_memory("ImGui texture indexing allocation failed");
            }
        }
        try {
            prepared->draw_calls.push_back(Impl::DrawCall{
                texture_index,
                scissor_result.value,
                command.elem_count,
                command.index_offset,
                command.vertex_offset,
            });
        } catch (const std::bad_alloc&) {
            return foundation::out_of_memory("ImGui draw command allocation failed");
        }
    }

    if (prepared->draw_calls.empty()) {
        return impl_->store_prepared_frame(std::move(prepared));
    }

    const math::Mat4 projection = make_imgui_projection(packet);
    const auto make_buffer_upload = [this, &frame_context, &graph](
                                        std::span<const std::byte> data, rhi::BufferDesc desc,
                                        rhi::BufferHandle& destination,
                                        render_graph::RenderGraphBuffer& resource) -> foundation::Result {
        const resource::BufferUploadDesc upload_desc{data, desc, 0, 256};
        const foundation::ResultValue<resource::BufferUpload> upload_result =
            impl_->resource_uploader.upload_buffer(frame_context, graph, upload_desc);
        if (!upload_result) {
            return upload_result.result;
        }
        destination = upload_result.value.destination;
        resource = upload_result.value.target;
        return foundation::Result::success();
    };

    rhi::BufferDesc vertex_desc{};
    vertex_desc.size = packet.vertices.size() * sizeof(ImGuiDrawVertex);
    vertex_desc.capabilities = rhi::BufferCapability::kVertex | rhi::BufferCapability::kTransferDestination;
    vertex_desc.memory = rhi::MemoryClass::kDeviceLocal;
    vertex_desc.name = rhi::DebugName{"Editor.ImGui.VertexBuffer"};
    if (const foundation::Result result = make_buffer_upload(
            std::as_bytes(std::span<const ImGuiDrawVertex>{packet.vertices.data(), packet.vertices.size()}),
            vertex_desc, prepared->vertex_buffer, prepared->vertex_resource);
        !result) {
        return result;
    }
    prepared->vertex_size = vertex_desc.size;

    rhi::BufferDesc index_desc{};
    index_desc.size = packet.indices.size() * sizeof(uint32_t);
    index_desc.capabilities = rhi::BufferCapability::kIndex | rhi::BufferCapability::kTransferDestination;
    index_desc.memory = rhi::MemoryClass::kDeviceLocal;
    index_desc.name = rhi::DebugName{"Editor.ImGui.IndexBuffer"};
    if (const foundation::Result result =
            make_buffer_upload(std::as_bytes(std::span<const uint32_t>{packet.indices.data(), packet.indices.size()}),
                               index_desc, prepared->index_buffer, prepared->index_resource);
        !result) {
        cleanup();
        return result;
    }
    prepared->index_size = index_desc.size;

    rhi::BufferDesc projection_desc{};
    projection_desc.size = sizeof(math::Mat4);
    projection_desc.capabilities = rhi::BufferCapability::kUniform | rhi::BufferCapability::kTransferDestination;
    projection_desc.memory = rhi::MemoryClass::kDeviceLocal;
    projection_desc.name = rhi::DebugName{"Editor.ImGui.ProjectionBuffer"};
    if (const foundation::Result result =
            make_buffer_upload(std::as_bytes(std::span<const math::Mat4>{&projection, 1}), projection_desc,
                               prepared->projection_buffer, prepared->projection_resource);
        !result) {
        cleanup();
        return result;
    }

    const foundation::ResultValue<rhi::PipelineHandle> pipeline_result = get_or_create_pipeline(target.format);
    if (!pipeline_result) {
        cleanup();
        return pipeline_result.result;
    }
    prepared->pipeline = pipeline_result.value;

    for (Impl::TextureEntry& texture : prepared->textures) {
        Impl::ImportedTexture* imported = nullptr;
        const auto imported_iterator = impl_->imported_textures.find(texture.identity);
        if (imported_iterator == impl_->imported_textures.end()) {
            const foundation::ResultValue<render_graph::RenderGraphTexture> resource_result =
                graph.import_texture(texture.binding.texture, texture.binding.sampled_view, texture.binding.texture_desc,
                                     texture.binding.initial_state);
            if (!resource_result) {
                cleanup();
                return resource_result.result;
            }
            try {
                const auto inserted = impl_->imported_textures.emplace(
                    texture.identity,
                    Impl::ImportedTexture{resource_result.value, texture.binding.texture,
                                          texture.binding.sampled_view, make_cached_texture_desc(texture.binding.texture_desc),
                                          make_imgui_sampled_state(), true});
                imported = &inserted.first->second;
            } catch (const std::bad_alloc&) {
                cleanup();
                return foundation::out_of_memory("ImGui imported texture cache allocation failed");
            }
        } else {
            imported = &imported_iterator->second;
            const bool graph_resource_valid = graph.contains_texture(imported->resource);
            const bool external_resource_matches = imported->texture == texture.binding.texture &&
                                                    imported->sampled_view == texture.binding.sampled_view &&
                                                    texture_desc_matches(imported->desc, texture.binding.texture_desc);
            if (!graph_resource_valid) {
                const rhi::ResourceState initial_state =
                    external_resource_matches && imported->state_initialized
                        ? imported->state
                        : texture.binding.initial_state;
                const foundation::ResultValue<render_graph::RenderGraphTexture> resource_result =
                    graph.import_texture(texture.binding.texture, texture.binding.sampled_view,
                                         texture.binding.texture_desc, initial_state);
                if (!resource_result) {
                    cleanup();
                    return resource_result.result;
                }
                imported->resource = resource_result.value;
            } else if (!external_resource_matches) {
                const rhi::ResourceState initial_state = texture.binding.initial_state;
                if (const foundation::Result result = graph.update_imported_texture(
                        imported->resource, texture.binding.texture, texture.binding.sampled_view,
                        texture.binding.texture_desc, initial_state);
                    !result) {
                    cleanup();
                    return result;
                }
                imported->state_initialized = false;
            }
            imported->texture = texture.binding.texture;
            imported->sampled_view = texture.binding.sampled_view;
            imported->desc = make_cached_texture_desc(texture.binding.texture_desc);
            imported->state = make_imgui_sampled_state();
            imported->state_initialized = true;
        }
        texture.resource = imported->resource;

        const foundation::ResultValue<rhi::BindingSetHandle> set_result = impl_->device.create_binding_set(
            rhi::BindingSetDesc{impl_->binding_layout, rhi::BindingSetLifetime::kFrameLocal,
                                rhi::DebugName{"Editor.ImGui.BindingSet"}});
        if (!set_result) {
            cleanup();
            return set_result.result;
        }
        texture.binding_set = set_result.value;

        std::array<rhi::BindingWrite, 3> writes{};
        writes[0].location = rhi::BindingLocation{0, 0};
        writes[0].resource.type = rhi::BindingType::kUniformBuffer;
        writes[0].resource.buffer_range = rhi::BufferRange{prepared->projection_buffer, 0, sizeof(math::Mat4)};
        writes[1].location = rhi::BindingLocation{0, 1};
        writes[1].resource.type = rhi::BindingType::kSampledTexture;
        writes[1].resource.texture_view = texture.binding.sampled_view;
        writes[2].location = rhi::BindingLocation{0, 2};
        writes[2].resource.type = rhi::BindingType::kSampler;
        writes[2].resource.sampler = texture.binding.sampler;
        if (const foundation::Result result = impl_->device.update_binding_set(texture.binding_set, writes); !result) {
            cleanup();
            return result;
        }
    }

    const std::shared_ptr<const void> frame_owner = prepared;
    if (const foundation::Result result = frame_context.retain_until_complete(frame_owner); !result) {
        cleanup();
        return result;
    }
    return impl_->store_prepared_frame(std::move(prepared));
}

foundation::Result ImGuiRenderer::build_render_graph(pipeline::RenderGraphBuilder& builder) const {
    if (impl_ == nullptr || !impl_->initialized) {
        return foundation::invalid_state("ImGui renderer is not initialized");
    }
    const std::shared_ptr<Impl::PreparedFrame> prepared = impl_->prepared_frame;
    if (prepared == nullptr) {
        return foundation::Result::success();
    }

    const foundation::ResultValue<pipeline::RenderGraphTexture> target_result =
        builder.find_texture(prepared->target.resource_id);
    if (!target_result) {
        return target_result.result;
    }
    const pipeline::RenderGraphTexture target = target_result.value;

    if (prepared->draw_calls.empty()) {
        const foundation::ResultValue<pipeline::RenderGraphPass> pass_result = builder.add_graphics_pass(
            "Editor.ImGui.Draw",
            [target, final_usage = prepared->target.final_usage](pipeline::RenderPassBuilder& pass_builder) {
                if (const foundation::Result result = pass_builder.use_color_attachment(
                        target, rhi::LoadOp::kLoad, rhi::StoreOp::kStore, {}, final_usage);
                    !result) {
                    return result;
                }
                pass_builder.set_side_effects();
                return foundation::Result::success();
            },
            [](pipeline::RenderPassExecutionContext& context) {
                if (const foundation::Result result = context.begin_rendering(); !result) {
                    return result;
                }
                return context.end_rendering();
            });
        return pass_result ? foundation::Result::success() : pass_result.result;
    }

    const pipeline::RenderGraphBuffer vertex_resource = to_pipeline_buffer(prepared->vertex_resource);
    const pipeline::RenderGraphBuffer index_resource = to_pipeline_buffer(prepared->index_resource);
    const pipeline::RenderGraphBuffer projection_resource = to_pipeline_buffer(prepared->projection_resource);
    if (const foundation::Result result = builder.bind_buffer(kImGuiVertexResourceId, vertex_resource); !result) {
        return result;
    }
    if (const foundation::Result result = builder.bind_buffer(kImGuiIndexResourceId, index_resource); !result) {
        return result;
    }
    if (const foundation::Result result = builder.bind_buffer(kImGuiProjectionResourceId, projection_resource);
        !result) {
        return result;
    }

    std::vector<pipeline::RenderGraphTexture> texture_resources;
    try {
        texture_resources.reserve(prepared->textures.size());
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("ImGui graph texture resource allocation failed");
    }
    for (std::size_t index = 0; index < prepared->textures.size(); ++index) {
        const Impl::TextureEntry& texture = prepared->textures[index];
        const pipeline::RenderResourceId resource_id = make_imgui_texture_resource_id(texture.identity);
        const pipeline::RenderGraphTexture resource = to_pipeline_texture(texture.resource);
        if (const foundation::Result result = builder.bind_texture(resource_id, resource); !result) {
            return result;
        }
        texture_resources.push_back(resource);
    }

    const foundation::ResultValue<pipeline::RenderGraphPass> pass_result = builder.add_graphics_pass(
        "Editor.ImGui.Draw",
        [prepared, target, vertex = vertex_resource, index = index_resource, projection = projection_resource,
         texture_resources](pipeline::RenderPassBuilder& pass_builder) {
            if (const foundation::Result result = pass_builder.read(
                    vertex, rhi::ResourceUsage::kVertexInput, rhi::PipelineStage::kVertexInput,
                    rhi::ShaderStage::kVertex, pipeline::RenderGraphBufferRange{0, prepared->vertex_size});
                !result) {
                return result;
            }
            if (const foundation::Result result = pass_builder.read(
                    index, rhi::ResourceUsage::kIndexInput, rhi::PipelineStage::kVertexInput, rhi::ShaderStage::kVertex,
                    pipeline::RenderGraphBufferRange{0, prepared->index_size});
                !result) {
                return result;
            }
            if (const foundation::Result result = pass_builder.read(
                    projection, rhi::ResourceUsage::kUniformRead, rhi::PipelineStage::kVertexShader,
                    rhi::ShaderStage::kVertex, pipeline::RenderGraphBufferRange{0, sizeof(math::Mat4)});
                !result) {
                return result;
            }
            for (const pipeline::RenderGraphTexture texture : texture_resources) {
                if (const foundation::Result result =
                        pass_builder.read(texture, rhi::ResourceUsage::kSampledRead,
                                          rhi::PipelineStage::kFragmentShader, rhi::ShaderStage::kFragment);
                    !result) {
                    return result;
                }
            }
            if (const foundation::Result result = pass_builder.use_color_attachment(
                    target, rhi::LoadOp::kLoad, rhi::StoreOp::kStore, {}, prepared->target.final_usage);
                !result) {
                return result;
            }
            pass_builder.set_side_effects();
            return foundation::Result::success();
        },
        [prepared, vertex = vertex_resource, index = index_resource](pipeline::RenderPassExecutionContext& context) {
            const foundation::ResultValue<rhi::BufferHandle> vertex_buffer = context.buffer(vertex);
            if (!vertex_buffer) {
                return vertex_buffer.result;
            }
            const foundation::ResultValue<rhi::BufferHandle> index_buffer = context.buffer(index);
            if (!index_buffer) {
                return index_buffer.result;
            }
            if (const foundation::Result result = context.begin_rendering(); !result) {
                return result;
            }
            const auto fail_rendering = [&context](const foundation::Result& result) {
                static_cast<void>(context.end_rendering());
                return result;
            };
            if (const foundation::Result result = context.bind_pipeline(prepared->pipeline); !result) {
                return fail_rendering(result);
            }
            const rhi::Viewport viewport{
                0.0f,
                0.0f,
                static_cast<float>(prepared->target.extent.width),
                static_cast<float>(prepared->target.extent.height),
                0.0f,
                1.0f,
            };
            if (const foundation::Result result = context.set_viewport(viewport); !result) {
                return fail_rendering(result);
            }
            if (const foundation::Result result = context.bind_vertex_buffer(0, vertex_buffer.value); !result) {
                return fail_rendering(result);
            }
            if (const foundation::Result result =
                    context.bind_index_buffer(index_buffer.value, 0, rhi::IndexFormat::kUint32);
                !result) {
                return fail_rendering(result);
            }
            for (const Impl::DrawCall& draw_call : prepared->draw_calls) {
                if (const foundation::Result result = context.set_scissor(draw_call.scissor); !result) {
                    return fail_rendering(result);
                }
                if (draw_call.texture_index >= prepared->textures.size()) {
                    return fail_rendering(foundation::invalid_state("ImGui draw texture index is out of bounds"));
                }
                if (const foundation::Result result =
                        context.bind_binding_set(0, prepared->textures[draw_call.texture_index].binding_set);
                    !result) {
                    return fail_rendering(foundation::Result::failure(
                        result.code, "ImGui texture index " + std::to_string(draw_call.texture_index) + ": " +
                                         result.message));
                }
                if (const foundation::Result result = context.draw_indexed(rhi::DrawIndexedDesc{
                        draw_call.index_count, 1, draw_call.first_index, draw_call.vertex_offset, 0});
                    !result) {
                    return fail_rendering(result);
                }
            }
            return context.end_rendering();
        });
    return pass_result ? foundation::Result::success() : pass_result.result;
}

foundation::Result ImGuiRenderer::shutdown() {
    if (impl_ == nullptr || !impl_->initialized) {
        return foundation::Result::success();
    }
    if (const foundation::Result result = release_prepared_frame(); !result) {
        return result;
    }

    foundation::Result first_result = foundation::Result::success();
    for (auto iterator = impl_->pipelines.begin(); iterator != impl_->pipelines.end();) {
        const foundation::Result result = impl_->device.destroy_pipeline(iterator->second);
        if (result || result.code == foundation::ResultCode::kNotFound) {
            iterator = impl_->pipelines.erase(iterator);
        } else {
            first_result = first_failure(first_result, result);
            ++iterator;
        }
    }
    if (!impl_->pipelines.empty()) {
        return first_result;
    }
    if (impl_->shader_manager != nullptr) {
        if (const foundation::Result result = impl_->shader_manager->clear_layout_cache(); !result) {
            return result;
        }
    }
    impl_->binding_layout = {};
    impl_->shader_layout = {};
    impl_->shader_program = {};
    impl_->shader_manager.reset();
    impl_->imported_textures.clear();
    impl_->initialized = false;
    return foundation::Result::success();
}

foundation::Result ImGuiRenderer::release_prepared_frame() {
    if (impl_ == nullptr || !impl_->initialized) {
        return foundation::Result::success();
    }
    if (const foundation::Result result = impl_->device.wait_idle(); !result) {
        return result;
    }
    if (impl_->prepared_frame == nullptr && impl_->prepared_frame_history.empty()) {
        // The caller may be replacing an externally-owned texture even when
        // this renderer has no current draw packet. Do not retain its logical
        // RenderGraph resource: the RHI handle may have been recycled with a
        // different state on the next frame.
        impl_->imported_textures.clear();
        return foundation::Result::success();
    }
    foundation::Result first_result = foundation::Result::success();
    for (const std::weak_ptr<Impl::PreparedFrame>& frame_weak : impl_->prepared_frame_history) {
        const std::shared_ptr<Impl::PreparedFrame> frame = frame_weak.lock();
        if (frame == nullptr) {
            continue;
        }
        const foundation::Result result = frame->release_rhi_resources();
        if (!result && first_result) {
            first_result = result;
        }
    }
    if (!first_result) {
        return first_result;
    }
    impl_->prepared_frame.reset();
    impl_->prepared_frame_history.clear();
    // A caller releases the prepared frame before replacing an externally
    // owned texture (for example the editor viewport after a resize). The
    // next graph must import that replacement instead of reusing a logical
    // resource that still describes the retired image.
    impl_->imported_textures.clear();
    return foundation::Result::success();
}

bool ImGuiRenderer::is_initialized() const noexcept {
    return impl_ != nullptr && impl_->initialized;
}

bool ImGuiRenderer::has_prepared_frame() const noexcept {
    return impl_ != nullptr && impl_->prepared_frame != nullptr;
}

ImGuiRenderFeature::ImGuiRenderFeature(ImGuiRenderer& renderer) noexcept : renderer_(&renderer) {
    descriptor_.id = kImGuiRenderFeatureId;
    descriptor_.name = "Editor.ImGuiRenderFeature";
    descriptor_.domain = pipeline::FeatureDomain::kRender;
    descriptor_.domain_mask = pipeline::to_mask(pipeline::FeatureDomain::kRender);
    descriptor_.supported_scopes = pipeline::to_mask(pipeline::PipelineScope::kFrame);
    descriptor_.version = {1, 0, 0};
}

foundation::Result ImGuiRenderFeature::setup(pipeline::RenderGraphBuilder& builder) const {
    const pipeline::PipelineStageHandle stage = builder.add_stage({
        kImGuiStageId,
        pipeline::EnginePhase::kPresent,
        pipeline::ExecutionDomain::kRenderThread,
        pipeline::TimePolicy::kFrame,
        false,
    });
    if (!stage) {
        return foundation::invalid_state("ImGui RenderFeature stage declaration failed");
    }
    return foundation::Result::success();
}

foundation::Result ImGuiRenderFeature::build_render_graph(pipeline::RenderGraphBuilder& builder) const {
    if (renderer_ == nullptr) {
        return foundation::invalid_state("ImGui RenderFeature has no renderer");
    }
    return renderer_->build_render_graph(builder);
}

foundation::Result register_imgui_render_feature(pipeline::PipelineRegistry& registry, ImGuiRenderer& renderer) {
    if (!renderer.is_initialized()) {
        return foundation::invalid_state("ImGui renderer must be initialized before feature registration");
    }
    if (const foundation::Result result =
            registry.register_feature([&renderer] { return std::make_unique<ImGuiRenderFeature>(renderer); });
        !result) {
        return result;
    }
    const foundation::Result binding_result = registry.bind_feature({
        kImGuiRenderFeatureId,
        pipeline::PipelineId("RenderPipeline"),
        pipeline::PipelineScope::kFrame,
        kImGuiFeaturePointId,
        0,
        true,
        {},
    });
    if (!binding_result) {
        static_cast<void>(registry.unregister_feature(kImGuiRenderFeatureId));
        return binding_result;
    }
    return foundation::Result::success();
}

foundation::Result unregister_imgui_render_feature(pipeline::PipelineRegistry& registry) {
    const pipeline::FeatureInstanceId instance = pipeline::make_feature_instance_id(
        kImGuiRenderFeatureId, pipeline::PipelineId("RenderPipeline"), pipeline::PipelineScope::kFrame,
        kImGuiFeaturePointId);
    foundation::Result first_result = registry.unbind_feature(instance);
    if (!first_result && first_result.code == foundation::ResultCode::kNotFound) {
        first_result = foundation::Result::success();
    }

    const foundation::Result unregister_pipeline_result =
        registry.unregister_pipeline(pipeline::PipelineId("RenderPipeline"));
    if (!unregister_pipeline_result && unregister_pipeline_result.code != foundation::ResultCode::kNotFound &&
        first_result) {
        first_result = unregister_pipeline_result;
    }

    const foundation::Result unregister_feature_result = registry.unregister_feature(kImGuiRenderFeatureId);
    if (!unregister_feature_result && unregister_feature_result.code != foundation::ResultCode::kNotFound &&
        first_result) {
        first_result = unregister_feature_result;
    }
    return first_result;
}
}  // namespace aeg::editor
