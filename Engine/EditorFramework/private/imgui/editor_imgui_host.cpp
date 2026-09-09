/**
 * @file editor_imgui_host.cpp
 * @brief Implements the engine-owned Dear ImGui context and panel host.
 */
#include <imgui.h>

#include <cmath>
#include <cstddef>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

#include "editor_imgui.hpp"
#include "imgui_platform_bridge.hpp"

namespace aeg::editor {
struct EditorImGuiHost::Impl final {
    struct PanelSlot final {
        uint32_t generation{1};
        std::unique_ptr<EditorPanel> panel{};
    };

    ImGuiContext* context{nullptr};
    EditorImGuiDesc desc{};
    std::mutex event_mutex{};
    std::vector<platform::WindowEvent> pending_events{};
    std::vector<platform::WindowEvent> frame_events{};
    std::vector<PanelSlot> panels{};
    ImGuiFontAtlasData font_atlas{};
    ImGuiTextureHandle font_texture{};
    uint64_t frame_number{0};
    bool initialized{false};
    bool frame_active{false};
};

EditorImGuiHost::EditorImGuiHost() : impl_(std::make_unique<Impl>()) {}

EditorImGuiHost::~EditorImGuiHost() {
    if (impl_ != nullptr && impl_->initialized) {
        static_cast<void>(shutdown());
    }
}

foundation::Result EditorImGuiHost::initialize(const EditorImGuiDesc& desc) {
    if (impl_->initialized) {
        return foundation::invalid_state("EditorImGuiHost is already initialized");
    }
    if (!std::isfinite(desc.font_global_scale) || desc.font_global_scale <= 0.0f) {
        return foundation::invalid_argument("EditorImGui font_global_scale must be finite and positive");
    }

    impl_->desc = desc;
    impl_->context = ImGui::CreateContext();
    if (impl_->context == nullptr) {
        return foundation::out_of_memory("Dear ImGui context creation failed");
    }

    ImGui::SetCurrentContext(impl_->context);
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags = ImGuiConfigFlags_NavEnableKeyboard;
    if (desc.enable_gamepad_navigation) {
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    }
    io.FontGlobalScale = desc.font_global_scale;
    io.IniFilename = nullptr;
    io.BackendPlatformName = "AEG.Editor.ImGuiPlatformBridge";
    io.BackendRendererName = nullptr;
    io.BackendFlags = ImGuiBackendFlags_None;
    unsigned char* font_pixels = nullptr;
    int font_width = 0;
    int font_height = 0;
    io.Fonts->GetTexDataAsRGBA32(&font_pixels, &font_width, &font_height);
    if (font_pixels == nullptr || font_width <= 0 || font_height <= 0) {
        ImGui::DestroyContext(impl_->context);
        impl_->context = nullptr;
        return foundation::invalid_state("Dear ImGui font atlas construction failed");
    }
    if (static_cast<uint64_t>(font_width) > std::numeric_limits<uint32_t>::max() ||
        static_cast<uint64_t>(font_height) > std::numeric_limits<uint32_t>::max()) {
        ImGui::DestroyContext(impl_->context);
        impl_->context = nullptr;
        return foundation::invalid_state("Dear ImGui font atlas dimensions exceed engine limits");
    }
    const uint64_t row_pitch = static_cast<uint64_t>(font_width) * 4u;
    const uint64_t pixel_size = row_pitch * static_cast<uint64_t>(font_height);
    if (row_pitch > std::numeric_limits<uint32_t>::max() || pixel_size > std::numeric_limits<std::size_t>::max()) {
        ImGui::DestroyContext(impl_->context);
        impl_->context = nullptr;
        return foundation::invalid_state("Dear ImGui font atlas size exceeds engine limits");
    }
    try {
        impl_->font_atlas.width = static_cast<uint32_t>(font_width);
        impl_->font_atlas.height = static_cast<uint32_t>(font_height);
        impl_->font_atlas.row_pitch = static_cast<uint32_t>(row_pitch);
        const auto* begin = reinterpret_cast<const std::byte*>(font_pixels);
        impl_->font_atlas.rgba8_pixels.assign(begin, begin + static_cast<std::size_t>(pixel_size));
    } catch (const std::bad_alloc&) {
        ImGui::DestroyContext(impl_->context);
        impl_->context = nullptr;
        return foundation::out_of_memory("Dear ImGui font atlas snapshot allocation failed");
    }
    impl_->initialized = true;
    return foundation::Result::success();
}

foundation::Result EditorImGuiHost::shutdown() {
    if (!impl_->initialized) {
        return foundation::Result::success();
    }
    if (impl_->frame_active) {
        return foundation::invalid_state("EditorImGuiHost cannot shut down during an active frame");
    }

    ImGui::SetCurrentContext(impl_->context);
    ImGui::DestroyContext(impl_->context);
    impl_->context = nullptr;
    impl_->pending_events.clear();
    impl_->frame_events.clear();
    impl_->panels.clear();
    impl_->font_atlas.clear();
    impl_->font_texture = {};
    impl_->initialized = false;
    return foundation::Result::success();
}

foundation::Result EditorImGuiHost::submit_event(const platform::WindowEvent& event) {
    if (!impl_->initialized) {
        return foundation::invalid_state("EditorImGuiHost is not initialized");
    }
    if (!event.window.is_valid()) {
        return foundation::invalid_argument("Editor ImGui event has an invalid window identity");
    }

    std::scoped_lock lock(impl_->event_mutex);
    impl_->pending_events.push_back(event);
    return foundation::Result::success();
}

foundation::Result EditorImGuiHost::set_font_texture(ImGuiTextureHandle texture) {
    if (!impl_->initialized) {
        return foundation::invalid_state("EditorImGuiHost is not initialized");
    }
    if (!texture.is_valid()) {
        return foundation::invalid_argument("ImGui font texture identity is invalid");
    }
    if (impl_->frame_active) {
        return foundation::invalid_state("ImGui font texture cannot change during an active frame");
    }

    ImGui::SetCurrentContext(impl_->context);
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->SetTexID(ImTextureRef(static_cast<ImTextureID>(make_imgui_texture_id(texture))));
    impl_->font_texture = texture;
    return foundation::Result::success();
}

foundation::Result EditorImGuiHost::capture_font_atlas(ImGuiFontAtlasData& atlas) const {
    if (!impl_->initialized) {
        return foundation::invalid_state("EditorImGuiHost is not initialized");
    }
    try {
        ImGuiFontAtlasData captured = impl_->font_atlas;
        atlas = std::move(captured);
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Dear ImGui font atlas copy allocation failed");
    }
}

foundation::Result EditorImGuiHost::begin_frame(const EditorImGuiFrameDesc& desc) {
    if (!impl_->initialized) {
        return foundation::invalid_state("EditorImGuiHost is not initialized");
    }
    if (impl_->frame_active) {
        return foundation::invalid_state("EditorImGuiHost frame is already active");
    }
    if (!desc.display_extent.is_valid() || !std::isfinite(desc.delta_seconds) || desc.delta_seconds <= 0.0f ||
        !std::isfinite(desc.framebuffer_scale_x) || !std::isfinite(desc.framebuffer_scale_y) ||
        desc.framebuffer_scale_x <= 0.0f || desc.framebuffer_scale_y <= 0.0f) {
        return foundation::invalid_argument("Editor ImGui frame description is invalid");
    }

    ImGui::SetCurrentContext(impl_->context);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize =
        ImVec2(static_cast<float>(desc.display_extent.width), static_cast<float>(desc.display_extent.height));
    io.DisplayFramebufferScale = ImVec2(desc.framebuffer_scale_x, desc.framebuffer_scale_y);
    io.DeltaTime = desc.delta_seconds;

    {
        std::scoped_lock lock(impl_->event_mutex);
        impl_->frame_events.clear();
        impl_->frame_events.swap(impl_->pending_events);
    }
    for (const platform::WindowEvent& event : impl_->frame_events) {
        if (event.window == desc.window) {
            detail::ImGuiPlatformBridge::submit_event(io, event);
        }
    }
    ImGui::NewFrame();
    ++impl_->frame_number;
    impl_->frame_active = true;
    return foundation::Result::success();
}

foundation::Result EditorImGuiHost::build_panels() {
    if (!impl_->initialized || !impl_->frame_active) {
        return foundation::invalid_state("Editor ImGui panels require an active frame");
    }

    ImGui::SetCurrentContext(impl_->context);
    for (const Impl::PanelSlot& slot : impl_->panels) {
        if (slot.panel != nullptr) {
            slot.panel->build_ui();
        }
    }
    return foundation::Result::success();
}

foundation::Result EditorImGuiHost::end_frame() {
    if (!impl_->initialized || !impl_->frame_active) {
        return foundation::invalid_state("Editor ImGui frame is not active");
    }

    ImGui::SetCurrentContext(impl_->context);
    ImGui::Render();
    impl_->frame_active = false;
    return foundation::Result::success();
}

foundation::Result EditorImGuiHost::capture_draw_packet(ImGuiDrawPacket& packet) const {
    if (!impl_->initialized || impl_->frame_active) {
        return foundation::invalid_state("ImGui draw data requires a finalized frame");
    }

    try {
        ImGui::SetCurrentContext(impl_->context);
        const ImDrawData* draw_data = ImGui::GetDrawData();
        if (draw_data == nullptr || !draw_data->Valid) {
            return foundation::invalid_state("Dear ImGui has no valid draw data");
        }

        ImGuiDrawPacket captured;
        captured.display_pos_x = draw_data->DisplayPos.x;
        captured.display_pos_y = draw_data->DisplayPos.y;
        captured.display_width = draw_data->DisplaySize.x;
        captured.display_height = draw_data->DisplaySize.y;
        captured.framebuffer_scale_x = draw_data->FramebufferScale.x;
        captured.framebuffer_scale_y = draw_data->FramebufferScale.y;
        captured.frame_number = impl_->frame_number;
        if (draw_data->TotalVtxCount > 0) {
            captured.vertices.reserve(static_cast<std::size_t>(draw_data->TotalVtxCount));
        }
        if (draw_data->TotalIdxCount > 0) {
            captured.indices.reserve(static_cast<std::size_t>(draw_data->TotalIdxCount));
        }

        for (int list_index = 0; list_index < draw_data->CmdLists.Size; ++list_index) {
            const ImDrawList* list = draw_data->CmdLists[list_index];
            if (list == nullptr) {
                continue;
            }
            const uint32_t vertex_base = static_cast<uint32_t>(captured.vertices.size());
            const uint32_t index_base = static_cast<uint32_t>(captured.indices.size());
            for (const ImDrawVert& vertex : list->VtxBuffer) {
                captured.vertices.push_back(ImGuiDrawVertex{
                    vertex.pos.x,
                    vertex.pos.y,
                    vertex.uv.x,
                    vertex.uv.y,
                    vertex.col,
                });
            }
            for (const ImDrawIdx index : list->IdxBuffer) {
                captured.indices.push_back(static_cast<uint32_t>(index));
            }
            for (const ImDrawCmd& command : list->CmdBuffer) {
                if (command.UserCallback != nullptr) {
                    return foundation::unsupported("ImGui user draw callbacks are not supported by ImGuiDrawPacket");
                }
                const uint64_t texture_id = static_cast<uint64_t>(command.GetTexID());
                const ImGuiTextureHandle texture = decode_imgui_texture_id(texture_id);
                if (!texture.is_valid()) {
                    return foundation::invalid_argument("ImGui draw command contains an invalid texture ID");
                }
                captured.commands.push_back(ImGuiDrawCommand{
                    texture,
                    command.ClipRect.x,
                    command.ClipRect.y,
                    command.ClipRect.z,
                    command.ClipRect.w,
                    command.ElemCount,
                    index_base + command.IdxOffset,
                    static_cast<int32_t>(vertex_base) + static_cast<int32_t>(command.VtxOffset),
                });
            }
        }

        packet = std::move(captured);
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("ImGui draw packet allocation failed");
    } catch (const std::exception& exception) {
        return foundation::unknown_exception(exception);
    } catch (...) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                           "ImGui draw packet capture threw an unknown exception");
    }
}

foundation::ResultValue<EditorPanelId> EditorImGuiHost::register_panel(std::unique_ptr<EditorPanel> panel) {
    if (!impl_->initialized) {
        return {foundation::invalid_state("EditorImGuiHost is not initialized"), {}};
    }
    if (panel == nullptr) {
        return {foundation::invalid_argument("Cannot register a null editor panel"), {}};
    }

    for (uint32_t index = 0; index < impl_->panels.size(); ++index) {
        Impl::PanelSlot& slot = impl_->panels[index];
        if (slot.panel == nullptr) {
            slot.panel = std::move(panel);
            return {foundation::Result::success(), EditorPanelId{index, slot.generation}};
        }
    }

    if (impl_->panels.size() == UINT32_MAX) {
        return {foundation::out_of_memory("Editor panel registry exhausted"), {}};
    }
    impl_->panels.push_back(Impl::PanelSlot{1, std::move(panel)});
    return {foundation::Result::success(),
            EditorPanelId{static_cast<uint32_t>(impl_->panels.size() - 1), impl_->panels.back().generation}};
}

foundation::Result EditorImGuiHost::unregister_panel(EditorPanelId panel) {
    if (!impl_->initialized) {
        return foundation::invalid_state("EditorImGuiHost is not initialized");
    }
    if (panel.index >= impl_->panels.size()) {
        return foundation::not_found("Editor panel identity is not registered");
    }

    Impl::PanelSlot& slot = impl_->panels[panel.index];
    if (slot.panel == nullptr || slot.generation != panel.generation) {
        return foundation::not_found("Editor panel identity is stale");
    }
    slot.panel.reset();
    ++slot.generation;
    if (slot.generation == 0) {
        slot.generation = 1;
    }
    return foundation::Result::success();
}

bool EditorImGuiHost::is_initialized() const noexcept {
    return impl_ != nullptr && impl_->initialized;
}

bool EditorImGuiHost::is_frame_active() const noexcept {
    return impl_ != nullptr && impl_->frame_active;
}

std::size_t EditorImGuiHost::panel_count() const noexcept {
    if (impl_ == nullptr) {
        return 0;
    }
    std::size_t count = 0;
    for (const Impl::PanelSlot& slot : impl_->panels) {
        count += slot.panel != nullptr ? 1u : 0u;
    }
    return count;
}

const void* EditorImGuiHost::draw_data() const noexcept {
    if (impl_ == nullptr || !impl_->initialized || impl_->frame_active) {
        return nullptr;
    }
    ImGui::SetCurrentContext(impl_->context);
    return ImGui::GetDrawData();
}
}  // namespace aeg::editor
