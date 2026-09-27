#include "runtime_bootstrap.hpp"

aeg::foundation::Result aeg_configure_game_application(aeg::application::runtime::RuntimeBootRequest& request) {
    request.window_title = "MyGameProject";
    request.width = 1280;
    request.height = 720;
    request.fixed_delta_seconds = 1.0f / 60.0f;
    request.max_frame_delta_seconds = 0.25f;
    request.max_fixed_steps_per_frame = 8;
    request.owned_host = !request.headless && !request.offscreen;
    return aeg::foundation::Result::success();
}
