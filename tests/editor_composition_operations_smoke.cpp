#include <iostream>

#include "editor_composition_operations.hpp"

int main() {
    using namespace aeg;
    uint32_t starts = 0;
    tooling::ProjectOperationRequest captured;
    application::EditorProjectOperationController controller(
        [&](const tooling::ProjectOperationRequest& request) -> foundation::ResultValue<uint64_t> {
            ++starts;
            captured = request;
            return {foundation::Result::success(), 41};
        },
        [](uint64_t id) -> foundation::ResultValue<tooling::ProjectOperationStatus> {
            if (id != 41)
                return {foundation::invalid_argument("Unexpected operation ID"), {}};
            tooling::ProjectOperationStatus status;
            status.state = tooling::ProjectOperationState::kSucceeded;
            status.result = foundation::Result::success();
            return {foundation::Result::success(), status};
        },
        [](uint64_t) { return foundation::Result::success(); });
    tooling::ProjectOperationRequest request;
    request.project_file = L"C:/Editor Tests/游戏/Project.aegproj";
    request.operation = tooling::ProjectOperation::kCook;
    if (!controller.start(request) || starts != 1 || captured.project_file != request.project_file ||
        captured.operation != tooling::ProjectOperation::kCook || !controller.busy())
        return 1;
    if (!controller.tick() || controller.busy() || !controller.status() ||
        controller.status()->state != tooling::ProjectOperationState::kSucceeded)
        return 2;
    if (controller.cancel())
        return 3;
    std::cout << "Editor composition operations smoke passed\n";
    return 0;
}
