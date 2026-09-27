#include <iostream>

#include "runtime_boot_fixture.hpp"
using namespace aeg::application::runtime;
static double now_value = 0.0;
static double clock_now(void*) noexcept {
    return now_value;
}
int main() {
    runtime_test::Fixture fixture{};
    if (!runtime_test::prepare(fixture))
        return 1;
    RuntimeBootstrap boot;
    RuntimeBootRequest request{};
    request.manifest_path = fixture.root / "game.manifest";
    request.package_path = fixture.root / "game.aegbin";
    request.fixed_delta_seconds = 0.02f;
    request.max_frame_delta_seconds = 0.05f;
    request.max_fixed_steps_per_frame = 2;
    request.clock = {clock_now, nullptr};
    if (!boot.initialize(request, std::move(fixture.definition), fixture.identity))
        return 2;
    if (!boot.tick())
        return 3;
    now_value = 1.0;
    if (!boot.tick())
        return 4;
    const auto& frame = boot.last_frame_context();
    if (frame.real_delta_seconds < 0.99f || frame.delta_seconds != 0.05f || frame.fixed_steps != 2 ||
        frame.fixed_tick != 3 || frame.interpolation_alpha < 0.49f || frame.interpolation_alpha > 0.51f)
        return 5;
    if (!boot.shutdown())
        return 6;
    runtime_test::cleanup(fixture);
    std::cout << "bootstrap clock clamp, fixed-step cap, tick and interpolation passed\n";
    return 0;
}
