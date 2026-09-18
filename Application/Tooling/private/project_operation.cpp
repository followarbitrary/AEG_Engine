#include "project_operation.hpp"

#include <mutex>
#include <thread>
#include <unordered_map>

#include "project_operation_pipeline.hpp"

namespace aeg::tooling {
namespace {
ProjectOperationStatus cancelled_status() {
    ProjectOperationStatus status{};
    status.state = ProjectOperationState::kCancelled;
    status.result = foundation::Result::failure(foundation::ResultCode::kCancelled, "Project operation cancelled");
    return status;
}
}  // namespace

struct ProjectOperationService::Impl final {
    struct Operation final {
        ProjectOperationRequest request{};
        ProjectOperationStatus status{};
        bool cancel_requested{};
    };

    std::mutex mutex{};
    std::unordered_map<uint64_t, Operation> operations{};
    std::thread worker{};
    uint64_t next_id{1};
    uint64_t active_id{};

    ~Impl() {
        {
            std::scoped_lock lock(mutex);
            if (active_id != 0) {
                const auto found = operations.find(active_id);
                if (found != operations.end())
                    found->second.cancel_requested = true;
            }
        }
        if (worker.joinable())
            worker.join();
    }
};

ProjectOperationService::ProjectOperationService() : impl_(std::make_unique<Impl>()) {}
ProjectOperationService::~ProjectOperationService() = default;

foundation::ResultValue<uint64_t> ProjectOperationService::start(const ProjectOperationRequest& request) {
    if (!impl_)
        return {foundation::invalid_state("Project operation service is unavailable"), {}};
    if (request.project_file.empty()) {
        return {foundation::invalid_argument("Project operation requires a project file"), {}};
    }

    std::thread previous;
    uint64_t operation_id = 0;
    {
        std::scoped_lock lock(impl_->mutex);
        if (impl_->active_id != 0)
            return {foundation::invalid_state("A project operation is already running"), {}};
        if (impl_->worker.joinable())
            previous = std::move(impl_->worker);
    }
    if (previous.joinable())
        previous.join();
    {
        std::scoped_lock lock(impl_->mutex);
        if (impl_->active_id != 0)
            return {foundation::invalid_state("A project operation is already running"), {}};
        operation_id = impl_->next_id++;
        if (operation_id == 0)
            operation_id = impl_->next_id++;
        impl_->operations[operation_id] = {request, {}, false};
        impl_->operations[operation_id].status.state = ProjectOperationState::kRunning;
        impl_->active_id = operation_id;
        impl_->worker = std::thread([this, operation_id] {
            ProjectOperationRequest operation_request{};
            auto is_cancelled = [this, operation_id] {
                std::scoped_lock lock(impl_->mutex);
                const auto found = impl_->operations.find(operation_id);
                return found == impl_->operations.end() || found->second.cancel_requested;
            };
            {
                std::scoped_lock lock(impl_->mutex);
                const auto found = impl_->operations.find(operation_id);
                if (found == impl_->operations.end())
                    return;
                operation_request = found->second.request;
            }

            detail::ProjectOperationExecution execution{};
            execution.request = operation_request;
            execution.is_cancelled = is_cancelled;
            ProjectOperationStatus result{};
            const auto pipeline_result = detail::execute_project_operation_pipeline(execution, result);
            static_cast<void>(pipeline_result);

            std::scoped_lock lock(impl_->mutex);
            const auto found = impl_->operations.find(operation_id);
            if (found != impl_->operations.end()) {
                if (found->second.cancel_requested && result.state == ProjectOperationState::kSucceeded)
                    result = cancelled_status();
                found->second.status = std::move(result);
            }
            if (impl_->active_id == operation_id)
                impl_->active_id = 0;
        });
    }
    return {foundation::Result::success(), operation_id};
}

foundation::ResultValue<ProjectOperationStatus> ProjectOperationService::poll(uint64_t operation_id) {
    if (!impl_ || operation_id == 0)
        return {foundation::invalid_argument("Project operation ID is invalid"), {}};
    std::scoped_lock lock(impl_->mutex);
    const auto found = impl_->operations.find(operation_id);
    if (found == impl_->operations.end())
        return {foundation::not_found("Project operation ID is unknown"), {}};
    return {foundation::Result::success(), found->second.status};
}

foundation::Result ProjectOperationService::cancel(uint64_t operation_id) {
    if (!impl_ || operation_id == 0)
        return foundation::invalid_argument("Project operation ID is invalid");
    std::scoped_lock lock(impl_->mutex);
    const auto found = impl_->operations.find(operation_id);
    if (found == impl_->operations.end())
        return foundation::not_found("Project operation ID is unknown");
    if (found->second.status.state != ProjectOperationState::kRunning)
        return foundation::Result::success();
    found->second.cancel_requested = true;
    found->second.status = cancelled_status();
    return foundation::Result::success();
}

}  // namespace aeg::tooling
