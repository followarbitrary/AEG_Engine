#include "editor_composition_operations.hpp"

namespace aeg::application {

EditorProjectOperationController::EditorProjectOperationController(Start start, Poll poll, Cancel cancel)
    : start_(std::move(start)), poll_(std::move(poll)), cancel_(std::move(cancel)) {}

EditorProjectOperationController::EditorProjectOperationController(tooling::ProjectOperationService& service)
    : start_([&service](const tooling::ProjectOperationRequest& request) { return service.start(request); }),
      poll_([&service](uint64_t id) { return service.poll(id); }),
      cancel_([&service](uint64_t id) { return service.cancel(id); }) {}

foundation::Result EditorProjectOperationController::start(const tooling::ProjectOperationRequest& request) {
    if (busy())
        return foundation::invalid_state("A project operation is already running");
    if (!start_ || !poll_ || !cancel_)
        return foundation::invalid_state("Project operation service is unavailable");
    if (request.project_file.empty())
        return foundation::invalid_argument("Project operation requires a project file");
    const auto started = start_(request);
    if (!started)
        return started.result;
    if (started.value == 0)
        return foundation::invalid_state("Project operation returned an invalid ID");
    operation_id_ = started.value;
    status_.reset();
    return foundation::Result::success();
}

foundation::Result EditorProjectOperationController::tick() {
    if (!operation_id_)
        return foundation::invalid_state("No project operation is running");
    const auto polled = poll_(*operation_id_);
    if (!polled)
        return polled.result;
    status_ = polled.value;
    switch (status_->state) {
        case tooling::ProjectOperationState::kRunning:
            break;
        case tooling::ProjectOperationState::kSucceeded:
        case tooling::ProjectOperationState::kFailed:
        case tooling::ProjectOperationState::kCancelled:
            operation_id_.reset();
            break;
    }
    return status_->result;
}

foundation::Result EditorProjectOperationController::cancel() {
    if (!operation_id_)
        return foundation::invalid_state("No project operation is running");
    const auto result = cancel_(*operation_id_);
    if (!result)
        return result;
    return foundation::Result::success();
}

}  // namespace aeg::application
