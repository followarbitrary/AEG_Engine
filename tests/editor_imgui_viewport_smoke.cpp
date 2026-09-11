/**
 * @file editor_imgui_viewport_smoke.cpp
 * @brief Verifies engine-owned ImGui platform viewport integration.
 */
#include <imgui.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "editor_imgui.hpp"
#include "imgui_texture_registry.hpp"
#include "window.hpp"

namespace {
class FakePlatformServices final : public aeg::platform::IPlatformServices {
public:
    [[nodiscard]] aeg::foundation::ResultValue<std::string> get_clipboard_text() const override {
        return {aeg::foundation::Result::success(), clipboard_text};
    }

    [[nodiscard]] aeg::foundation::Result set_clipboard_text(std::string_view text) override {
        clipboard_text = text;
        return aeg::foundation::Result::success();
    }

    [[nodiscard]] aeg::foundation::Result set_cursor(aeg::platform::WindowId,
                                                     aeg::platform::CursorShape) noexcept override {
        return aeg::foundation::Result::success();
    }

    [[nodiscard]] aeg::foundation::Result set_ime_state(aeg::platform::WindowId,
                                                        const aeg::platform::ImeState&) noexcept override {
        return aeg::foundation::Result::success();
    }

    [[nodiscard]] aeg::foundation::ResultValue<aeg::platform::WindowMetrics> query_window_metrics(
        aeg::platform::WindowId) const override {
        return {aeg::foundation::Result::success(), aeg::platform::WindowMetrics{{1280, 720}, 1.0f, 1.0f}};
    }

    std::string clipboard_text{};
};

class FakeWindow final : public aeg::platform::IWindow {
public:
    FakeWindow(aeg::platform::WindowId id, aeg::platform::WindowDesc desc) noexcept
        : id_(id), desc_(std::move(desc)), state_{desc_.position, desc_.extent, desc_.visible, false, false, false} {}

    [[nodiscard]] aeg::platform::WindowId id() const noexcept override { return id_; }
    [[nodiscard]] const aeg::platform::WindowDesc& desc() const noexcept override { return desc_; }
    [[nodiscard]] aeg::platform::WindowState state() const noexcept override { return state_; }
    [[nodiscard]] aeg::platform::WindowSurfaceBinding surface_binding() const noexcept override { return {}; }

    [[nodiscard]] aeg::foundation::Result set_position(aeg::platform::WindowPosition position) noexcept override {
        state_.position = position;
        return aeg::foundation::Result::success();
    }

    [[nodiscard]] aeg::foundation::Result set_extent(aeg::platform::WindowExtent extent) noexcept override {
        if (!extent.is_valid()) {
            return aeg::foundation::invalid_argument("Fake window extent must be non-zero");
        }
        state_.extent = extent;
        return aeg::foundation::Result::success();
    }

    [[nodiscard]] aeg::foundation::Result set_visible(bool visible) noexcept override {
        state_.visible = visible;
        return aeg::foundation::Result::success();
    }

    [[nodiscard]] aeg::foundation::Result focus() noexcept override {
        state_.focused = true;
        return aeg::foundation::Result::success();
    }

    [[nodiscard]] aeg::foundation::Result request_close() noexcept override {
        state_.close_requested = true;
        return aeg::foundation::Result::success();
    }

private:
    aeg::platform::WindowId id_{};
    aeg::platform::WindowDesc desc_{};
    aeg::platform::WindowState state_{};
};

class FakeWindowSystem final : public aeg::platform::IWindowSystem {
public:
    [[nodiscard]] aeg::foundation::ResultValue<std::shared_ptr<aeg::platform::IWindow>> create_window(
        const aeg::platform::WindowDesc& desc) override {
        if (fail_create) {
            return {aeg::foundation::Result::failure(aeg::foundation::ResultCode::kBackendError,
                                                     "Fake viewport creation failed"), {}};
        }
        const aeg::platform::WindowId id{next_index++, 1};
        auto window = std::make_shared<FakeWindow>(id, desc);
        windows.push_back(window);
        ++create_count;
        return {aeg::foundation::Result::success(), std::static_pointer_cast<aeg::platform::IWindow>(window)};
    }

    [[nodiscard]] aeg::foundation::Result destroy_window(aeg::platform::WindowId window) override {
        const auto iterator = std::find_if(windows.begin(), windows.end(), [window](const auto& candidate) {
            return candidate != nullptr && candidate->id() == window;
        });
        if (iterator == windows.end()) {
            return aeg::foundation::not_found("Fake viewport window was not found");
        }
        windows.erase(iterator);
        ++destroy_count;
        return aeg::foundation::Result::success();
    }

    [[nodiscard]] aeg::foundation::Result poll_events(const aeg::platform::WindowEventSink& sink) override {
        if (!sink) {
            return aeg::foundation::invalid_argument("Fake event sink must not be empty");
        }
        return aeg::foundation::Result::success();
    }

    [[nodiscard]] aeg::foundation::ResultValue<std::shared_ptr<aeg::platform::IWindow>> find_window(
        aeg::platform::WindowId window) const override {
        const auto iterator = std::find_if(windows.begin(), windows.end(), [window](const auto& candidate) {
            return candidate != nullptr && candidate->id() == window;
        });
        if (iterator == windows.end()) {
            return {aeg::foundation::not_found("Fake viewport window was not found"), {}};
        }
        return {aeg::foundation::Result::success(), *iterator};
    }

    [[nodiscard]] aeg::platform::IPlatformServices& platform_services() noexcept override { return services; }

    FakePlatformServices services{};
    std::vector<std::shared_ptr<aeg::platform::IWindow>> windows{};
    uint32_t next_index{1};
    uint32_t create_count{0};
    uint32_t destroy_count{0};
    bool fail_create{false};
};

class FloatingPanel final : public aeg::editor::EditorPanel {
public:
    void build_ui() override {
        ImGui::SetNextWindowPos(ImVec2(1600.0f, 100.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(320.0f, 200.0f), ImGuiCond_Always);
        ImGui::Begin("Floating Viewport");
        ImGui::TextUnformatted("Viewport bridge");
        ImGui::End();
    }
};
}  // namespace

int main() {
    FakeWindowSystem window_system;
    aeg::editor::ImGuiTextureRegistry textures;
    const auto texture_result = textures.register_texture(
        aeg::editor::ImGuiTextureBinding{aeg::rhi::TextureViewHandle{11}, aeg::rhi::SamplerHandle{17}});
    assert(texture_result);
    aeg::editor::EditorImGuiHost host;
    aeg::editor::EditorImGuiDesc desc{};
    desc.platform_services = &window_system.platform_services();
    desc.window_system = &window_system;
    desc.enable_viewports = true;
    assert(host.initialize(desc));
    assert(host.set_font_texture(texture_result.value));

    auto panel = std::make_unique<FloatingPanel>();
    assert(host.register_panel(std::move(panel)));

    const aeg::platform::WindowId main_window{0, 1};
    aeg::editor::EditorImGuiFrameDesc frame_desc{};
    frame_desc.window = main_window;
    frame_desc.display_extent = {1280, 720};
    assert(host.begin_frame(frame_desc));
    assert(host.build_panels());
    assert(host.end_frame());

    std::vector<aeg::editor::ImGuiDrawPacket> packets;
    assert(host.capture_draw_packets(packets));
    assert(packets.size() >= 2);
    assert(std::any_of(packets.begin(), packets.end(), [main_window](const auto& packet) {
        return packet.window == main_window;
    }));
    assert(window_system.create_count != 0);
    assert(window_system.windows.size() >= 1);

    const aeg::platform::WindowId floating_window = window_system.windows.front()->id();
    aeg::platform::WindowEvent event{};
    event.type = aeg::platform::WindowEventType::kMouseMoved;
    event.window = floating_window;
    event.mouse_x = 20.0f;
    event.mouse_y = 30.0f;
    assert(host.submit_event(event));

    assert(host.begin_frame(frame_desc));
    assert(host.build_panels());
    assert(host.end_frame());
    packets.clear();
    assert(host.capture_draw_packets(packets));
    assert(std::any_of(packets.begin(), packets.end(), [floating_window](const auto& packet) {
        return packet.window == floating_window;
    }));

    assert(host.shutdown());
    assert(window_system.destroy_count != 0);

    FakeWindowSystem failing_window_system;
    failing_window_system.fail_create = true;
    aeg::editor::EditorImGuiHost failing_host;
    desc.platform_services = &failing_window_system.platform_services();
    desc.window_system = &failing_window_system;
    assert(failing_host.initialize(desc));
    assert(failing_host.set_font_texture(texture_result.value));
    assert(failing_host.register_panel(std::make_unique<FloatingPanel>()));
    assert(failing_host.begin_frame(frame_desc));
    assert(failing_host.build_panels());
    assert(failing_host.end_frame());
    packets.clear();
    assert(failing_host.capture_draw_packets(packets));
    assert(packets.size() == 1);
    assert(failing_host.shutdown());
    return 0;
}
