#include "project_operation.hpp"

#include <mutex>
#include <unordered_map>

#include "concurrency.hpp"
#include "project_operation_feature.hpp"
#include "project_context.hpp"
#include "project_module_agreement.hpp"

namespace aeg::tooling {
namespace {
void mark_cancelled(ProjectOperationStatus& status) {
    status.state = ProjectOperationState::kCancelled;
    status.result = foundation::Result::failure(foundation::ResultCode::kCancelled, "Project operation cancelled");
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
    std::shared_ptr<concurrency::WorkerExecutor> executor{
        std::make_shared<concurrency::WorkerExecutor>(concurrency::WorkerExecutorDesc{
            concurrency::ExecutorDomain::kIoWorker, 1, true})};
    concurrency::CancellationSource active_cancellation{};
    uint64_t next_id{1};
    uint64_t active_id{};

    ~Impl() {
        concurrency::CancellationSource cancellation;
        {
            std::scoped_lock lock(mutex);
            if (active_id != 0) {
                const auto found = operations.find(active_id);
                if (found != operations.end()) {
                    found->second.cancel_requested = true;
                    mark_cancelled(found->second.status);
                }
            }
            active_id = 0;
            cancellation = std::move(active_cancellation);
        }
        static_cast<void>(cancellation.cancel());
        if (executor != nullptr)
            static_cast<void>(executor->wait_idle());
    }
};

ProjectOperationService::ProjectOperationService() : impl_(std::make_unique<Impl>()) {}
ProjectOperationService::~ProjectOperationService() = default;

foundation::ResultValue<module::ModuleBundle> ProjectOperationService::select_module(
    const std::filesystem::path& project_file, const std::filesystem::path& module_manifest,
    std::string_view configuration) {
    if (project_file.empty() || module_manifest.empty() || configuration.empty())
        return {foundation::invalid_argument("Module selection requires project, manifest and configuration"), {}};
    const auto project = application::project::ProjectFileCodec{}.load(project_file);
    if (!project) return {project.result, {}};
    const auto paths = application::project::ProjectPaths::resolve(project_file, project.value.config.paths);
    if (!paths) return {paths.result, {}};
    auto manifest = module_manifest;
    if (!manifest.is_absolute()) {
        const auto resolved = paths.value.resolve_under(application::project::ProjectDirectory::kRoot, manifest);
        if (!resolved) return {resolved.result, {}};
        manifest = resolved.value;
    }
    auto bundle = module::validate_module_bundle(manifest);
    if (!bundle) return bundle;
    if (const auto agreement = detail::validate_project_module_agreement(project.value.config, bundle.value.metadata,
                                                                        configuration); !agreement)
        return {agreement, {}};
    return bundle;
}

foundation::ResultValue<uint64_t> ProjectOperationService::start(const ProjectOperationRequest& request) {
    if (!impl_)
        return {foundation::invalid_state("Project operation service is unavailable"), {}};
    if (request.project_file.empty()) {
        return {foundation::invalid_argument("Project operation requires a project file"), {}};
    }

    uint64_t operation_id = 0;
    concurrency::CancellationToken cancellation_token;
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
        impl_->active_cancellation = concurrency::CancellationSource{};
        cancellation_token = impl_->active_cancellation.token();
    }

    concurrency::TaskDesc task{};
    task.domain = concurrency::ExecutorDomain::kIoWorker;
    task.priority = concurrency::TaskPriority::kNormal;
    // Deliberately leave task.cancellation empty.  WorkerExecutor may skip a
    // queued task whose TaskDesc token is cancelled before execution; this
    // callback must run to publish the terminal status and release active_id.
    // The operation token is still checked cooperatively by the callback and
    // every child process feature.
    task.execute = [this, operation_id, cancellation_token](const concurrency::TaskContext& context) {
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
                return foundation::Result::failure(foundation::ResultCode::kNotFound,
                                                   "Project operation was retired");
            operation_request = found->second.request;
        }

        detail::ProjectOperationExecution execution{};
        execution.request = operation_request;
        execution.is_cancelled = [is_cancelled, cancellation_token, &context] {
            return context.is_stop_requested() || cancellation_token.is_cancellation_requested() || is_cancelled();
        };
        execution.publish_status = [this, operation_id](const ProjectOperationStatus& progress) {
            // Copy on the Runtime owner thread; consumers only receive these owning facts.
            auto published = progress;
            published.execution_complete = false;
            std::scoped_lock lock(impl_->mutex);
            const auto found = impl_->operations.find(operation_id);
            if (found == impl_->operations.end()) return;
            if (found->second.cancel_requested) mark_cancelled(published);
            found->second.status = std::move(published);
        };
        ProjectOperationStatus result{};
        foundation::Result feature_result;
        try {
            feature_result = detail::execute_project_operation_feature(execution, result);
        } catch (const std::exception& exception) {
            feature_result = foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what());
            result.state = ProjectOperationState::kFailed;
            result.result = feature_result;
        } catch (...) {
            feature_result = foundation::Result::failure(foundation::ResultCode::kUnknown,
                                                        "Project operation threw an unknown exception");
            result.state = ProjectOperationState::kFailed;
            result.result = feature_result;
        }

        std::scoped_lock lock(impl_->mutex);
        const auto found = impl_->operations.find(operation_id);
        if (found != impl_->operations.end()) {
            if (found->second.cancel_requested || cancellation_token.is_cancellation_requested() ||
                result.state == ProjectOperationState::kCancelled)
                mark_cancelled(result);
            if (found->second.status.state == ProjectOperationState::kCancelled)
                mark_cancelled(result);
            result.execution_complete = true;
            found->second.status = std::move(result);
        }
        if (impl_->active_id == operation_id)
            impl_->active_id = 0;

        if (cancellation_token.is_cancellation_requested())
            return foundation::Result::failure(foundation::ResultCode::kCancelled,
                                               "Project operation cancelled");
        return feature_result;
    };

    const auto submitted = impl_->executor->submit(std::move(task));
    if (!submitted) {
        std::scoped_lock lock(impl_->mutex);
        const auto found = impl_->operations.find(operation_id);
        if (found != impl_->operations.end()) {
            if (found->second.cancel_requested || cancellation_token.is_cancellation_requested()) {
                mark_cancelled(found->second.status);
            } else {
                found->second.status.state = ProjectOperationState::kFailed;
                found->second.status.result = submitted.result;
            }
            found->second.status.execution_complete = true;
        }
        if (impl_->active_id == operation_id)
            impl_->active_id = 0;
        return {submitted.result, {}};
    }

    // A cancellation may race with submission (for example, a caller may
    // already know the next operation ID).  An executor may complete a
    // cancelled task before invoking its callback, so a completed submission
    // must still release the single-operation slot.
    if (submitted.value.is_complete()) {
        const foundation::Result completion_result = submitted.value.result();
        std::scoped_lock lock(impl_->mutex);
        const auto found = impl_->operations.find(operation_id);
        if (found != impl_->operations.end() && impl_->active_id == operation_id) {
            if (found->second.cancel_requested || cancellation_token.is_cancellation_requested()) {
                mark_cancelled(found->second.status);
            } else if (!completion_result) {
                found->second.status.state = ProjectOperationState::kFailed;
                found->second.status.result = completion_result;
            }
            found->second.status.execution_complete = true;
            impl_->active_id = 0;
        }
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

foundation::ResultValue<ProjectOperationStatus> ProjectOperationService::wait(uint64_t operation_id) {
    if (!impl_ || operation_id == 0)
        return {foundation::invalid_argument("Project operation ID is invalid"), {}};
    {
        std::scoped_lock lock(impl_->mutex);
        if (impl_->operations.find(operation_id) == impl_->operations.end())
            return {foundation::not_found("Project operation ID is unknown"), {}};
    }
    // The worker needs the service mutex for its final owning snapshot and slot release.
    if (const auto result = impl_->executor->wait_idle(); !result)
        return {result, {}};
    return poll(operation_id);
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
    mark_cancelled(found->second.status);
    static_cast<void>(impl_->active_cancellation.cancel());
    return foundation::Result::success();
}

}  // namespace aeg::tooling
