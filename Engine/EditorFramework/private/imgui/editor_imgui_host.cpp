/**
 * @file editor_imgui_host.cpp
 * @brief Implements the engine-owned Dear ImGui context and panel host.
 */
#include <imgui.h>

#include <cmath>
#include <mutex>
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
