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

    [[nodiscard]] aeg::foundation::ResultValue<aeg::platform::DisplayWorkArea> query_display_work_area(
        aeg::platform::WindowId) const override {
        return {aeg::foundation::Result::success(), aeg::platform::DisplayWorkArea{{0, 0}, {2560, 1392}}};
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
                                                     "Fake viewport creation failed"),
                    {}};
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
        ImGui::SetNextWindowPos(ImVec2(1600.0f, 100.0f), ImGuiCond_Once);
        ImGui::SetNextWindowSize(ImVec2(320.0f, 200.0f), ImGuiCond_Once);
        ImGuiWindowClass window_class;
        window_class.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
        ImGui::SetNextWindowClass(&window_class);
        ImGui::Begin("Floating Viewport", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove);
        position = ImGui::GetWindowPos();
        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 36.0f, 3.0f));
        if (ImGui::Button("x", ImVec2(30.0f, 24.0f)))
            ++close_clicks;
        close_hovered = ImGui::IsItemHovered();
        ImGui::SetCursorPos(ImVec2(12.0f, 40.0f));
        ImGui::TextUnformatted("Viewport bridge");
        if (show_combo) {
            ImGui::SetCursorPos(ImVec2(60.0f, 160.0f));
            combo_position = ImGui::GetCursorScreenPos();
            ImGui::SetNextItemWidth(330.0f);
            combo_visible = ImGui::BeginCombo("##Graph", "Pipeline");
            if (combo_visible) {
                popup_position = ImGui::GetWindowPos();
                ImGui::Selectable("Pipeline");
                ImGui::Selectable("Feature / DeferredFeature");
                if (close_combo)
                    ImGui::CloseCurrentPopup();
                ImGui::EndCombo();
            }
        }
        ImGui::End();
    }
    ImVec2 position{};
    ImVec2 combo_position{};
    ImVec2 popup_position{};
    bool show_combo{false};
    bool close_combo{false};
    bool combo_visible{false};
    bool close_hovered{false};
    int close_clicks{0};
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
    FloatingPanel* floating_panel = panel.get();
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
    assert(std::any_of(packets.begin(), packets.end(),
                       [main_window](const auto& packet) { return packet.window == main_window; }));
    assert(window_system.create_count != 0);
    assert(window_system.windows.size() >= 1);
    // Engine-owned ImGui viewports must be borderless: the editor owns the
    // chrome inside the client area and the platform frame would otherwise
    // offset draw data and hide the editor's resize affordances.
    assert(!window_system.windows.front()->desc().client_decorated);
    assert(window_system.windows.front()->desc().resizable);
    // A newly shown native viewport must be active so its first mouse and
    // keyboard events reach Dear ImGui controls.
    assert(window_system.windows.front()->state().focused);

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
    assert(std::any_of(packets.begin(), packets.end(),
                       [floating_window](const auto& packet) { return packet.window == floating_window; }));

    // Moving via the OS caption changes the platform position without an
    // ImGui drag. Input, UI geometry and draw packets must use the same origin.
    const auto run_frame = [&] {
        assert(host.begin_frame(frame_desc));
        assert(host.build_panels());
        assert(host.end_frame());
    };
    int expected_clicks = 0;
    for (const aeg::platform::WindowPosition moved :
         {aeg::platform::WindowPosition{600, 260}, aeg::platform::WindowPosition{100, 80},
          aeg::platform::WindowPosition{760, 400}}) {
        assert(window_system.windows.front()->set_position(moved));
        event.mouse_x = 299.0f;
        event.mouse_y = 15.0f;
        assert(host.submit_event(event));
        run_frame();
        assert(floating_panel->position.x == moved.x && floating_panel->position.y == moved.y);
        run_frame();  // Hovered-window discovery uses last frame's bounds.
        assert(floating_panel->close_hovered);
        packets.clear();
        assert(host.capture_draw_packets(packets));
        const auto packet = std::find_if(packets.begin(), packets.end(), [floating_window](const auto& value) {
            return value.window == floating_window;
        });
        assert(packet != packets.end());
        assert(packet->display_pos_x == moved.x && packet->display_pos_y == moved.y);
        aeg::platform::WindowEvent button{};
        button.window = floating_window;
        button.type = aeg::platform::WindowEventType::kMouseButton;
        button.mouse_button = aeg::platform::MouseButton::kLeft;
        button.mouse_button_down = true;
        assert(host.submit_event(button));
        run_frame();
        button.mouse_button_down = false;
        assert(host.submit_event(button));
        run_frame();
        assert(floating_panel->close_clicks == ++expected_clicks);
        event.mouse_y = 55.0f;
        assert(host.submit_event(event));
        run_frame();
        assert(!floating_panel->close_hovered);
        button.mouse_button_down = true;
        assert(host.submit_event(button));
        run_frame();
        button.mouse_button_down = false;
        assert(host.submit_event(button));
        run_frame();
        assert(floating_panel->close_clicks == expected_clicks);
    }

    // A detached Graph combo must stay anchored when maximizing/restoring
    // while the main editor remains offset from the monitor's screen origin.
    aeg::platform::WindowDesc main_desc{};
    main_desc.position = {240, 120};
    main_desc.extent = {1280, 720};
    window_system.windows.push_back(std::make_shared<FakeWindow>(main_window, main_desc));
    floating_panel->show_combo = true;
    for (const bool maximized : {false, true, false}) {
        assert(window_system.windows.front()->set_position(maximized ? aeg::platform::WindowPosition{0, 0}
                                                                    : aeg::platform::WindowPosition{760, 400}));
        assert(window_system.windows.front()->set_extent(maximized ? aeg::platform::WindowExtent{2560, 1392}
                                                                  : aeg::platform::WindowExtent{480, 320}));
        aeg::platform::WindowEvent resized{};
        resized.window = floating_window;
        resized.type = aeg::platform::WindowEventType::kWindowResized;
        resized.extent = window_system.windows.front()->state().extent;
        assert(host.submit_event(resized));
        event.mouse_x = 370.0f;
        event.mouse_y = 170.0f;
        assert(host.submit_event(event));
        run_frame();
        run_frame();
        aeg::platform::WindowEvent click{};
        click.window = floating_window;
        click.type = aeg::platform::WindowEventType::kMouseButton;
        click.mouse_button = aeg::platform::MouseButton::kLeft;
        click.mouse_button_down = true;
        assert(host.submit_event(click));
        run_frame();
        click.mouse_button_down = false;
        assert(host.submit_event(click));
        run_frame();
        run_frame(); // Allow the popup to measure its contents before checking placement.
        assert(floating_panel->combo_visible);
        assert(floating_panel->popup_position.x == floating_panel->combo_position.x);
        assert(floating_panel->popup_position.y > floating_panel->combo_position.y);
        floating_panel->close_combo = true;
        run_frame();
        floating_panel->close_combo = false;
        run_frame();
    }
    floating_panel->show_combo = false;

    // Native client resizes must be forwarded to Dear ImGui so the platform
    // viewport refreshes its draw-data extent before the next frame.
    aeg::platform::WindowEvent resize_event{};
    resize_event.type = aeg::platform::WindowEventType::kWindowResized;
    resize_event.window = floating_window;
    resize_event.extent = {960, 540};
    assert(host.submit_event(resize_event));
    assert(host.begin_frame(frame_desc));
    const ImGuiPlatformIO& resized_platform_io = ImGui::GetPlatformIO();
    const bool resize_was_forwarded = [&] {
        for (int index = 0; index < resized_platform_io.Viewports.Size; ++index) {
            const ImGuiViewport* viewport = resized_platform_io.Viewports[index];
            if (viewport != nullptr && viewport != ImGui::GetMainViewport() && viewport->PlatformRequestResize) {
                return true;
            }
        }
        return false;
    }();
    assert(resize_was_forwarded);
    assert(host.build_panels());
    assert(host.end_frame());

    // A close request from a native child must be handed back to Dear ImGui;
    // dropping the platform event leaves a visible, unresponsive window.
    aeg::platform::WindowEvent close_event{};
    close_event.type = aeg::platform::WindowEventType::kWindowCloseRequested;
    close_event.window = floating_window;
    assert(host.submit_event(close_event));
    assert(host.begin_frame(frame_desc));
    const ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    const bool close_was_forwarded = [&] {
        for (int index = 0; index < platform_io.Viewports.Size; ++index) {
            const ImGuiViewport* viewport = platform_io.Viewports[index];
            if (viewport != nullptr && viewport != ImGui::GetMainViewport() && viewport->PlatformRequestClose) {
                return true;
            }
        }
        return false;
    }();
    assert(close_was_forwarded);
    assert(host.build_panels());
    assert(host.end_frame());

    // An interrupted viewport frame still finishes platform bookkeeping, so
    // the next NewFrame does not assert that UpdatePlatformWindows was skipped.
    assert(host.begin_frame(frame_desc));
    ImGui::Begin("Interrupted viewport frame");
    ImGui::BeginChild("Interrupted viewport child");
    assert(host.abort_frame());
    assert(host.draw_data() == nullptr);
    assert(host.begin_frame(frame_desc));
    assert(host.build_panels());
    assert(host.end_frame());
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
