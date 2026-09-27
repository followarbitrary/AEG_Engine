#include <iostream>
#include <vector>

#include "runtime_boot_fixture.hpp"
using namespace aeg::application::runtime;
struct State {
    std::vector<int> calls;
    bool quit{false};
    int updates{};
};
static double now_value = 0.0;
static double clock_now(void*) noexcept {
    return now_value;
}
static GameRuntimeStatus fail_start(void*, void*) {
    return {99, "intentional start failure"};
}
static GameRuntimeStatus start(void* context, void* data) {
    if (!current_frame(static_cast<GameRuntimeContext*>(context)))
        return {1, "no context"};
    static_cast<State*>(data)->calls.push_back(1);
    return {};
}
static GameRuntimeStatus fixed(void* context, const FixedUpdateContext*, void* data) {
    if (!current_frame(static_cast<GameRuntimeContext*>(context)))
        return {2, "no frame"};
    static_cast<State*>(data)->calls.push_back(2);
    return {};
}
static GameRuntimeStatus update(void* context, const UpdateContext*, void* data) {
    auto& s = *static_cast<State*>(data);
    s.calls.push_back(3);
    ++s.updates;
    if (s.updates == 2) {
        request_quit(static_cast<GameRuntimeContext*>(context));
        s.quit = true;
    }
    return {};
}
static GameRuntimeStatus stop(void*, void* data) {
    static_cast<State*>(data)->calls.push_back(4);
    return {};
}
int main() {
    runtime_test::Fixture fixture{};
    if (!runtime_test::prepare(fixture))
        return 1;
    State state{};
    GameRuntimeCallbacks cb{start, fixed, update, stop, &state};
    RuntimeBootstrap boot;
    RuntimeBootRequest request{};
    request.manifest_path = fixture.root / "game.manifest";
    request.package_path = fixture.root / "game.aegbin";
    request.fixed_delta_seconds = 0.02f;
    request.max_frame_delta_seconds = 0.05f;
    request.max_fixed_steps_per_frame = 2;
    request.clock = {clock_now, nullptr};
    if (!boot.initialize(request, std::move(fixture.definition), fixture.identity, cb))
        return 2;
    now_value = 0.0;
    if (!boot.tick())
        return 3;
    now_value = 1.0;
    if (!boot.tick())
        return 3;
    const auto first = boot.last_frame_context();
    if (first.real_delta_seconds < 0.99f || first.delta_seconds != 0.05f || first.fixed_steps != 2 ||
        first.fixed_tick != 3 || first.interpolation_alpha < 0.4f || !first.quit_requested)
        return 4;
    if (!boot.tick() || boot.frame_count() != 2 || state.updates != 2)
        return 5;
    if (!boot.shutdown())
        return 6;
    const std::vector<int> expected{1, 2, 3, 2, 2, 3, 4};
    if (state.calls != expected || !state.quit)
        return 7;
    runtime_test::cleanup(fixture);

    // A failed start must leave the bootstrap reusable and must not later call
    // stop for a runtime that never reached the started state.
    runtime_test::Fixture failed_fixture{};
    if (!runtime_test::prepare(failed_fixture))
        return 8;
    State failed_state{};
    GameRuntimeCallbacks failing{fail_start, nullptr, nullptr, stop, &failed_state};
    RuntimeBootstrap reusable;
    RuntimeBootRequest failed_request{};
    failed_request.manifest_path = failed_fixture.root / "game.manifest";
    failed_request.package_path = failed_fixture.root / "game.aegbin";
    failed_request.clock = {clock_now, nullptr};
    if (reusable.initialize(failed_request, std::move(failed_fixture.definition), failed_fixture.identity, failing))
        return 9;
    if (reusable.mode() != RuntimeMode::kStopped || reusable.frame_count() != 0 || !failed_state.calls.empty())
        return 10;
    runtime_test::cleanup(failed_fixture);

    runtime_test::Fixture retry_fixture{};
    if (!runtime_test::prepare(retry_fixture))
        return 11;
    State retry_state{};
    GameRuntimeCallbacks retry_callbacks{start, nullptr, nullptr, stop, &retry_state};
    failed_request.manifest_path = retry_fixture.root / "game.manifest";
    failed_request.package_path = retry_fixture.root / "game.aegbin";
    if (!reusable.initialize(failed_request, std::move(retry_fixture.definition), retry_fixture.identity,
                             retry_callbacks))
        return 12;
    if (reusable.frame_count() != 0 || reusable.mode() != RuntimeMode::kPlaying)
        return 13;
    if (!reusable.shutdown() || retry_state.calls != std::vector<int>{1, 4})
        return 14;
    runtime_test::cleanup(retry_fixture);
    std::cout << "bootstrap callbacks, clock clamp, fixed cap, quit, rollback and reinitialize passed\n";
    return 0;
}
