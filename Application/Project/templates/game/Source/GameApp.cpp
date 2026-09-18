#include "runtime_bootstrap.hpp"

aeg::foundation::Result aeg_configure_game_application(aeg::application::runtime::RuntimeBootRequest& request) {
    request.window_title = "MyGameProject";
    request.width = 1280;
    request.height = 720;
    return aeg::foundation::Result::success();
}
