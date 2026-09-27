/**
 * @file runtime_window_host_smoke.cpp
 * @brief Verifies RuntimeBootstrap polls a borrowed WindowSystem before input begins.
 */
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include "runtime_boot_fixture.hpp"

namespace {
using aeg::foundation::Result;
using aeg::foundation::ResultValue;

class PlatformServices final : public aeg::platform::IPlatformServices {
public:
    ResultValue<std::string> get_clipboard_text() const override {
        return {aeg::foundation::unsupported("test platform service"), {}};
    }
    Result set_clipboard_text(std::string_view) override {
        return aeg::foundation::unsupported("test platform service");
    }
    Result set_cursor(aeg::platform::WindowId, aeg::platform::CursorShape) noexcept override {
        return aeg::foundation::unsupported("test platform service");
    }
    Result set_ime_state(aeg::platform::WindowId, const aeg::platform::ImeState&) noexcept override {
        return aeg::foundation::unsupported("test platform service");
    }
    ResultValue<aeg::platform::WindowMetrics> query_window_metrics(aeg::platform::WindowId) const override {
        return {aeg::foundation::unsupported("test platform service"), {}};
    }
};

class WindowSystem final : public aeg::platform::IWindowSystem {
public:
    ResultValue<std::shared_ptr<aeg::platform::IWindow>> create_window(const aeg::platform::WindowDesc&) override {
        return {aeg::foundation::unsupported("test window system"), {}};
    }
    Result destroy_window(aeg::platform::WindowId) override {
        return aeg::foundation::unsupported("test window system");
    }
    Result poll_events(const aeg::platform::WindowEventSink& sink) override {
        ++poll_count;
        if (poll_count != 1) {
            return Result::success();
        }
        aeg::platform::WindowEvent key{};
        key.type = aeg::platform::WindowEventType::kKey;
        key.window = aeg::platform::WindowId{1, 1};
        key.key = aeg::platform::KeyCode::kW;
        key.key_action = aeg::platform::KeyAction::kPressed;
        sink(key);

        aeg::platform::WindowEvent resize{};
        resize.type = aeg::platform::WindowEventType::kWindowResized;
        resize.window = key.window;
        resize.extent = {1600, 900};
        sink(resize);

        aeg::platform::WindowEvent close{};
        close.type = aeg::platform::WindowEventType::kWindowCloseRequested;
        close.window = key.window;
        sink(close);
        return Result::success();
    }
    ResultValue<std::shared_ptr<aeg::platform::IWindow>> find_window(aeg::platform::WindowId) const override {
        return {aeg::foundation::unsupported("test window system"), {}};
    }
    aeg::platform::IPlatformServices& platform_services() noexcept override { return services_; }

    uint32_t poll_count{0};

private:
    PlatformServices services_{};
};

struct State final {
    uint32_t updates{0};
    std::size_t event_count{0};
    bool close_seen{false};
    bool resize_seen{false};
    aeg::platform::WindowExtent resize_extent{};
    bool key_down{false};
    bool quit_seen{false};
};

aeg::application::runtime::GameRuntimeStatus update(void* context,
                                                    const aeg::application::runtime::UpdateContext* update_context,
                                                    void* user_data) {
    auto& state = *static_cast<State*>(user_data);
    ++state.updates;
    state.event_count = update_context->frame->events.size();
    state.key_down = update_context->frame->input != nullptr &&
                     update_context->frame->input->is_key_down(aeg::platform::KeyCode::kW);
    for (const auto& event : update_context->frame->events) {
        state.close_seen = state.close_seen || event.type == aeg::platform::WindowEventType::kWindowCloseRequested;
        if (event.type == aeg::platform::WindowEventType::kWindowResized) {
            state.resize_seen = true;
            state.resize_extent = event.extent;
        }
    }
    state.quit_seen = update_context->frame->quit_requested;
    static_cast<void>(context);
    return {};
}
}  // namespace

int main() {
    runtime_test::Fixture fixture{};
    if (!runtime_test::prepare(fixture)) {
        return 1;
    }

    WindowSystem window_system{};
    State state{};
    aeg::application::runtime::RuntimeBootRequest request{};
    request.manifest_path = fixture.root / "game.manifest";
    request.package_path = fixture.root / "game.aegbin";
    request.window_system = &window_system;
    aeg::application::runtime::GameRuntimeCallbacks callbacks{};
    callbacks.update = &update;
    callbacks.user_data = &state;

    aeg::application::runtime::RuntimeBootstrap bootstrap;
    if (!bootstrap.initialize(request, std::move(fixture.definition), fixture.identity, callbacks)) {
        runtime_test::cleanup(fixture);
        return 2;
    }
    if (!bootstrap.tick() || !bootstrap.last_frame_context().quit_requested || window_system.poll_count != 1) {
        runtime_test::cleanup(fixture);
        return 3;
    }
    if (state.updates != 1 || state.event_count != 3 || !state.close_seen || !state.resize_seen ||
        state.resize_extent != aeg::platform::WindowExtent{1600, 900} || !state.key_down || !state.quit_seen) {
        runtime_test::cleanup(fixture);
        return 4;
    }
    if (!bootstrap.tick() || window_system.poll_count != 1 || bootstrap.frame_count() != 1) {
        runtime_test::cleanup(fixture);
        return 5;
    }
    if (!bootstrap.shutdown()) {
        runtime_test::cleanup(fixture);
        return 6;
    }
    runtime_test::cleanup(fixture);
    std::cout << "Runtime WindowSystem polling smoke passed\n";
    return 0;
}
