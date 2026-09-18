#include "project_operation_pipeline.hpp"

#include <array>
#include <filesystem>
#include <memory>

#include "pipeline.hpp"
#include "project_cooker.hpp"
#include "project_process.hpp"

namespace aeg::tooling::detail {
namespace {

using namespace aeg::pipeline;

constexpr ModuleId kToolingModule{"Engine.Tooling"};
constexpr PipelineId kProjectOperationPipeline{"Engine.Tooling.ProjectOperation"};
constexpr PipelineInstanceId kProjectOperationInstance{"Engine.Tooling.ProjectOperation.Instance"};

constexpr FeatureId kValidationFeature{"Engine.Tooling.ProjectValidationFeature"};
constexpr FeatureId kConfigureFeature{"Engine.Tooling.ProjectConfigureFeature"};
constexpr FeatureId kCompileFeature{"Engine.Tooling.ProjectCompileFeature"};
constexpr FeatureId kCookFeature{"Engine.Tooling.ProjectCookFeature"};
constexpr FeatureId kPublishFeature{"Engine.Tooling.ProjectPublishFeature"};

constexpr FeaturePointId kValidationPoint{"Engine.Tooling.ProjectValidationPoint"};
constexpr FeaturePointId kConfigurePoint{"Engine.Tooling.ProjectConfigurePoint"};
constexpr FeaturePointId kCompilePoint{"Engine.Tooling.ProjectCompilePoint"};
constexpr FeaturePointId kCookPoint{"Engine.Tooling.ProjectCookPoint"};
constexpr FeaturePointId kPublishPoint{"Engine.Tooling.ProjectPublishPoint"};

constexpr StageId kValidationAnchor{"Engine.Tooling.ProjectValidationAnchor"};
constexpr StageId kConfigureAnchor{"Engine.Tooling.ProjectConfigureAnchor"};
constexpr StageId kCompileAnchor{"Engine.Tooling.ProjectCompileAnchor"};
constexpr StageId kCookAnchor{"Engine.Tooling.ProjectCookAnchor"};
constexpr StageId kPublishAnchor{"Engine.Tooling.ProjectPublishAnchor"};

ProjectOperationStatus cancelled_status() {
    ProjectOperationStatus status{};
    status.state = ProjectOperationState::kCancelled;
    status.result = foundation::Result::failure(foundation::ResultCode::kCancelled, "Project operation cancelled");
    return status;
}

foundation::Result cancelled(ProjectOperationExecution& execution, ProjectOperationStatus& status) {
    status = cancelled_status();
    static_cast<void>(execution);
    return status.result;
}

foundation::Result fail(ProjectOperationStatus& status, foundation::Result result, std::string key) {
    status.state = ProjectOperationState::kFailed;
    status.result = result;
    status.diagnostics.push_back({std::move(key), result.message, {}, {}, 0, 0});
    return result;
}

foundation::Result fail_with_cook_diagnostics(ProjectOperationStatus& status, foundation::Result result,
                                              const ProjectCookResult& cooked) {
    status.state = ProjectOperationState::kFailed;
    status.result = std::move(result);
    status.diagnostics.insert(status.diagnostics.end(), cooked.diagnostics.begin(), cooked.diagnostics.end());
    if (status.diagnostics.empty()) {
        status.diagnostics.push_back({"project.cook", status.result.message, {}, {}, 0, 0});
    }
    return status.result;
}

void record_stage(ProjectOperationStatus& status, std::string_view name) {
    status.pipeline_stages.emplace_back(name);
}

bool wants_cook(const ProjectOperationExecution& execution) noexcept {
    return execution.request.operation == ProjectOperation::kCook ||
           execution.request.operation == ProjectOperation::kBuildAndCook;
}

bool wants_build(const ProjectOperationExecution& execution) noexcept {
    return execution.request.operation != ProjectOperation::kCook;
}

foundation::Result validation_task(PipelineTaskContext& context) {
    auto* execution = static_cast<ProjectOperationExecution*>(context.user_data());
    auto* status = static_cast<ProjectOperationStatus*>(context.scope_data());
    if (execution == nullptr || status == nullptr)
        return foundation::invalid_state("project operation context is null");
    if (execution->is_cancelled && execution->is_cancelled())
        return cancelled(*execution, *status);
    if (execution->request.project_file.empty() || !std::filesystem::exists(execution->request.project_file)) {
        return fail(*status, foundation::not_found("Project file was not found"), "project.file");
    }
    if (execution->request.configuration.empty()) {
        return fail(*status, foundation::invalid_argument("Project operation configuration is empty"),
                    "project.configuration");
    }

    application::project::ProjectFileCodec codec;
    const auto parsed = codec.load(execution->request.project_file);
    if (!parsed)
        return fail(*status, parsed.result, "project.config");
    const auto paths =
        application::project::ProjectPaths::resolve(execution->request.project_file, parsed.value.config.paths);
    if (!paths)
        return fail(*status, paths.result, "project.paths");

    execution->project = parsed.value;
    execution->project_loaded = true;
    execution->project_root = execution->request.project_file.parent_path();
    execution->cmake_file = execution->project_root / "CMakeLists.txt";
    execution->build_root = paths.value.directory(application::project::ProjectDirectory::kBuild);
    record_stage(*status, "ProjectValidationFeature");
    return foundation::Result::success();
}

std::filesystem::path cmake_executable() {
#if defined(_WIN32)
    return std::filesystem::path{L"cmake.exe"};
#else
    return std::filesystem::path{"cmake"};
#endif
}

foundation::Result configure_task(PipelineTaskContext& context) {
    auto* execution = static_cast<ProjectOperationExecution*>(context.user_data());
    auto* status = static_cast<ProjectOperationStatus*>(context.scope_data());
    if (execution == nullptr || status == nullptr)
        return foundation::invalid_state("project operation context is null");
    if (execution->is_cancelled && execution->is_cancelled())
        return cancelled(*execution, *status);
    if (execution->request.engine_directory.empty() || !std::filesystem::exists(execution->cmake_file)) {
        record_stage(*status, "ProjectConfigureFeature");
        return foundation::Result::success();
    }

    ProjectProcessRequest process{};
    process.executable = cmake_executable();
    process.working_directory = execution->project_root;
    process.cancellation_requested = execution->is_cancelled;
    process.arguments = {L"-S", execution->project_root.wstring(), L"-B", execution->build_root.wstring(),
                         L"-DENGINE_DIR=" + execution->request.engine_directory.wstring()};
    const auto configured = run_project_process(process);
    if (!configured)
        return fail(*status, configured.result, "project.configure");
    if (configured.value.cancelled)
        return cancelled(*execution, *status);
    if (configured.value.exit_code != 0) {
        return fail(*status,
                    foundation::Result::failure(foundation::ResultCode::kBackendError, "CMake configure failed"),
                    "project.configure");
    }
    record_stage(*status, "ProjectConfigureFeature");
    return foundation::Result::success();
}

foundation::Result compile_task(PipelineTaskContext& context) {
    auto* execution = static_cast<ProjectOperationExecution*>(context.user_data());
    auto* status = static_cast<ProjectOperationStatus*>(context.scope_data());
    if (execution == nullptr || status == nullptr)
        return foundation::invalid_state("project operation context is null");
    if (execution->is_cancelled && execution->is_cancelled())
        return cancelled(*execution, *status);
    if (!wants_build(*execution) || execution->request.engine_directory.empty() ||
        !std::filesystem::exists(execution->cmake_file)) {
        record_stage(*status, "ProjectCompileFeature");
        return foundation::Result::success();
    }

    ProjectProcessRequest process{};
    process.executable = cmake_executable();
    process.working_directory = execution->project_root;
    process.cancellation_requested = execution->is_cancelled;
    process.arguments = {
        L"--build", execution->build_root.wstring(), L"--config",
        std::wstring(execution->request.configuration.begin(), execution->request.configuration.end())};
    if (!execution->request.target.empty()) {
        process.arguments.push_back(L"--target");
        process.arguments.emplace_back(execution->request.target.begin(), execution->request.target.end());
    }
    const auto built = run_project_process(process);
    if (!built)
        return fail(*status, built.result, "project.build");
    if (built.value.cancelled)
        return cancelled(*execution, *status);
    if (built.value.exit_code != 0) {
        return fail(*status, foundation::Result::failure(foundation::ResultCode::kBackendError, "CMake build failed"),
                    "project.build");
    }
    record_stage(*status, "ProjectCompileFeature");
    return foundation::Result::success();
}

foundation::Result cook_task(PipelineTaskContext& context) {
    auto* execution = static_cast<ProjectOperationExecution*>(context.user_data());
    auto* status = static_cast<ProjectOperationStatus*>(context.scope_data());
    if (execution == nullptr || status == nullptr)
        return foundation::invalid_state("project operation context is null");
    if (execution->is_cancelled && execution->is_cancelled())
        return cancelled(*execution, *status);
    // Empty module manifests preserve the legacy operation-service mode.  A
    // real cook always supplies the verified bundle produced by the module
    // metadata tool.
    if (!wants_cook(*execution) || execution->request.module_manifest.empty()) {
        record_stage(*status, "ProjectCookFeature");
        return foundation::Result::success();
    }

    const auto output_directory =
        execution->request.output_directory.empty()
            ? execution->project_root / execution->project.config.paths.binaries / execution->request.configuration
            : execution->request.output_directory;
    ProjectCookRequest request{};
    request.project_file = execution->request.project_file;
    request.module_manifest = execution->request.module_manifest;
    request.output_directory = output_directory;
    request.configuration = execution->request.configuration;
    const auto cooked = cook_project(request);
    if (!cooked)
        return fail_with_cook_diagnostics(*status, cooked.result, cooked.value);
    status->game_manifest = cooked.value.game_manifest;
    status->module_manifest = execution->request.module_manifest;
    status->diagnostics.insert(status->diagnostics.end(), cooked.value.diagnostics.begin(),
                               cooked.value.diagnostics.end());
    record_stage(*status, "ProjectCookFeature");
    return foundation::Result::success();
}

foundation::Result publish_task(PipelineTaskContext& context) {
    auto* execution = static_cast<ProjectOperationExecution*>(context.user_data());
    auto* status = static_cast<ProjectOperationStatus*>(context.scope_data());
    if (execution == nullptr || status == nullptr)
        return foundation::invalid_state("project operation context is null");
    if (execution->is_cancelled && execution->is_cancelled())
        return cancelled(*execution, *status);
    if (!wants_cook(*execution) || execution->request.module_manifest.empty()) {
        record_stage(*status, "ProjectPublishFeature");
        return foundation::Result::success();
    }
    if (status->game_manifest.empty() || !std::filesystem::exists(status->game_manifest)) {
        return fail(*status, foundation::not_found("Cook did not publish a game manifest"), "project.publish");
    }
    record_stage(*status, "ProjectPublishFeature");
    return foundation::Result::success();
}

template <typename FeatureType>
FeatureDescriptor make_feature_descriptor(FeatureId id, std::string_view name) {
    return FeatureDescriptor{id,
                             name,
                             FeatureDomain::kLogic,
                             to_mask(FeatureDomain::kLogic),
                             to_mask(PipelineScope::kEngine),
                             {1, 0, 0},
                             kToolingModule};
}

class ProjectValidationFeature final : public LogicFeature {
public:
    explicit ProjectValidationFeature(ProjectOperationExecution* execution) : execution_(execution) {
        descriptor_ = make_feature_descriptor<ProjectValidationFeature>(kValidationFeature,
                                                                        "Engine.Tooling.ProjectValidationFeature");
    }
    [[nodiscard]] const FeatureDescriptor& descriptor() const noexcept override { return descriptor_; }

protected:
    [[nodiscard]] foundation::Result setup(BuilderType& builder) const override {
        const auto stage = builder.add_stage({StageId("Engine.Tooling.ProjectValidationStage"), EnginePhase::kStartup,
                                              ExecutionDomain::kCpuWorker, TimePolicy::kStartup, false});
        const auto task = builder.add_task({TaskId("Engine.Tooling.ProjectValidationTask"),
                                            TaskFlags::kSideEffect | TaskFlags::kMustRun | TaskFlags::kMayBlock,
                                            &validation_task, execution_});
        if (!stage || !task)
            return foundation::invalid_state("project validation feature declaration failed");
        return builder.bind_task(task, stage);
    }

private:
    FeatureDescriptor descriptor_{};
    ProjectOperationExecution* execution_{nullptr};
};

class ProjectConfigureFeature final : public LogicFeature {
public:
    explicit ProjectConfigureFeature(ProjectOperationExecution* execution) : execution_(execution) {
        descriptor_ = make_feature_descriptor<ProjectConfigureFeature>(kConfigureFeature,
                                                                       "Engine.Tooling.ProjectConfigureFeature");
    }
    [[nodiscard]] const FeatureDescriptor& descriptor() const noexcept override { return descriptor_; }

protected:
    [[nodiscard]] foundation::Result setup(BuilderType& builder) const override {
        const auto stage = builder.add_stage({StageId("Engine.Tooling.ProjectConfigureStage"), EnginePhase::kStartup,
                                              ExecutionDomain::kCpuWorker, TimePolicy::kStartup, false});
        const auto task = builder.add_task({TaskId("Engine.Tooling.ProjectConfigureTask"),
                                            TaskFlags::kSideEffect | TaskFlags::kMustRun | TaskFlags::kMayBlock,
                                            &configure_task, execution_});
        if (!stage || !task)
            return foundation::invalid_state("project configure feature declaration failed");
        return builder.bind_task(task, stage);
    }

private:
    FeatureDescriptor descriptor_{};
    ProjectOperationExecution* execution_{nullptr};
};

class ProjectCompileFeature final : public LogicFeature {
public:
    explicit ProjectCompileFeature(ProjectOperationExecution* execution) : execution_(execution) {
        descriptor_ =
            make_feature_descriptor<ProjectCompileFeature>(kCompileFeature, "Engine.Tooling.ProjectCompileFeature");
    }
    [[nodiscard]] const FeatureDescriptor& descriptor() const noexcept override { return descriptor_; }

protected:
    [[nodiscard]] foundation::Result setup(BuilderType& builder) const override {
        const auto stage = builder.add_stage({StageId("Engine.Tooling.ProjectCompileStage"), EnginePhase::kStartup,
                                              ExecutionDomain::kCpuWorker, TimePolicy::kStartup, false});
        const auto task = builder.add_task({TaskId("Engine.Tooling.ProjectCompileTask"),
                                            TaskFlags::kSideEffect | TaskFlags::kMustRun | TaskFlags::kMayBlock,
                                            &compile_task, execution_});
        if (!stage || !task)
            return foundation::invalid_state("project compile feature declaration failed");
        return builder.bind_task(task, stage);
    }

private:
    FeatureDescriptor descriptor_{};
    ProjectOperationExecution* execution_{nullptr};
};

class ProjectCookFeature final : public LogicFeature {
public:
    explicit ProjectCookFeature(ProjectOperationExecution* execution) : execution_(execution) {
        descriptor_ = make_feature_descriptor<ProjectCookFeature>(kCookFeature, "Engine.Tooling.ProjectCookFeature");
    }
    [[nodiscard]] const FeatureDescriptor& descriptor() const noexcept override { return descriptor_; }

protected:
    [[nodiscard]] foundation::Result setup(BuilderType& builder) const override {
        const auto stage = builder.add_stage({StageId("Engine.Tooling.ProjectCookStage"), EnginePhase::kStartup,
                                              ExecutionDomain::kCpuWorker, TimePolicy::kStartup, false});
        const auto task = builder.add_task({TaskId("Engine.Tooling.ProjectCookTask"),
                                            TaskFlags::kSideEffect | TaskFlags::kMustRun | TaskFlags::kMayBlock,
                                            &cook_task, execution_});
        if (!stage || !task)
            return foundation::invalid_state("project cook feature declaration failed");
        return builder.bind_task(task, stage);
    }

private:
    FeatureDescriptor descriptor_{};
    ProjectOperationExecution* execution_{nullptr};
};

class ProjectPublishFeature final : public LogicFeature {
public:
    explicit ProjectPublishFeature(ProjectOperationExecution* execution) : execution_(execution) {
        descriptor_ =
            make_feature_descriptor<ProjectPublishFeature>(kPublishFeature, "Engine.Tooling.ProjectPublishFeature");
    }
    [[nodiscard]] const FeatureDescriptor& descriptor() const noexcept override { return descriptor_; }

protected:
    [[nodiscard]] foundation::Result setup(BuilderType& builder) const override {
        const auto stage = builder.add_stage({StageId("Engine.Tooling.ProjectPublishStage"), EnginePhase::kStartup,
                                              ExecutionDomain::kCpuWorker, TimePolicy::kStartup, false});
        const auto task = builder.add_task({TaskId("Engine.Tooling.ProjectPublishTask"),
                                            TaskFlags::kSideEffect | TaskFlags::kMustRun, &publish_task, execution_});
        if (!stage || !task)
            return foundation::invalid_state("project publish feature declaration failed");
        return builder.bind_task(task, stage);
    }

private:
    FeatureDescriptor descriptor_{};
    ProjectOperationExecution* execution_{nullptr};
};

class ProjectOperationPipeline final : public Pipeline {
public:
    explicit ProjectOperationPipeline(ProjectOperationExecution* execution) : execution_(execution) {
        descriptor_.id = kProjectOperationPipeline;
        descriptor_.name = "Engine.Tooling.ProjectOperation";
        descriptor_.version = {1, 0, 0};
        descriptor_.category = PipelineCategory::kTooling;
        descriptor_.supported_scopes = to_mask(PipelineScope::kEngine);
        descriptor_.domains = to_mask(FeatureDomain::kLogic);
        descriptor_.default_execution_domain = ExecutionDomain::kCpuWorker;
        descriptor_.default_time_policy = TimePolicy::kStartup;
        descriptor_.owner_module = kToolingModule;
    }

    [[nodiscard]] const PipelineDescriptor& descriptor() const noexcept override { return descriptor_; }

    [[nodiscard]] foundation::Result build(PipelineBuilder& builder) const override {
        const auto validation = builder.add_stage(
            {kValidationAnchor, EnginePhase::kStartup, ExecutionDomain::kCpuWorker, TimePolicy::kStartup, false});
        const auto configure = builder.add_stage(
            {kConfigureAnchor, EnginePhase::kStartup, ExecutionDomain::kCpuWorker, TimePolicy::kStartup, false});
        const auto compile = builder.add_stage(
            {kCompileAnchor, EnginePhase::kStartup, ExecutionDomain::kCpuWorker, TimePolicy::kStartup, false});
        const auto cook = builder.add_stage(
            {kCookAnchor, EnginePhase::kStartup, ExecutionDomain::kCpuWorker, TimePolicy::kStartup, false});
        const auto publish = builder.add_stage(
            {kPublishAnchor, EnginePhase::kStartup, ExecutionDomain::kCpuWorker, TimePolicy::kStartup, false});
        if (!validation || !configure || !compile || !cook || !publish) {
            return foundation::invalid_state("project operation pipeline stage declaration failed");
        }
        if (const auto result = builder.runs_before(validation, kConfigureAnchor); !result)
            return result;
        if (const auto result = builder.runs_before(configure, kCompileAnchor); !result)
            return result;
        if (const auto result = builder.runs_before(compile, kCookAnchor); !result)
            return result;
        if (const auto result = builder.runs_before(cook, kPublishAnchor); !result)
            return result;

        const auto add_point = [&builder](FeaturePointId id, std::string_view name, StageId anchor) {
            return builder.add_feature_point({id, name, kProjectOperationPipeline, anchor, FeaturePointPosition::kAfter,
                                              to_mask(FeatureDomain::kLogic), to_mask(PipelineScope::kEngine),
                                              FeaturePointFlags::kNone});
        };
        if (!add_point(kValidationPoint, "Validation", kValidationAnchor) ||
            !add_point(kConfigurePoint, "Configure", kConfigureAnchor) ||
            !add_point(kCompilePoint, "Compile", kCompileAnchor) || !add_point(kCookPoint, "Cook", kCookAnchor) ||
            !add_point(kPublishPoint, "Publish", kPublishAnchor)) {
            return foundation::invalid_state("project operation feature point declaration failed");
        }

        const auto bind = [&builder](FeatureId feature, FeaturePointId point) {
            return builder.bind_feature(
                {feature, kProjectOperationPipeline, PipelineScope::kEngine, point, 0, true,
                 make_feature_instance_id(feature, kProjectOperationPipeline, PipelineScope::kEngine, point)});
        };
        if (const auto result = bind(kValidationFeature, kValidationPoint); !result)
            return result;
        if (const auto result = bind(kConfigureFeature, kConfigurePoint); !result)
            return result;
        if (const auto result = bind(kCompileFeature, kCompilePoint); !result)
            return result;
        if (execution_ != nullptr && wants_cook(*execution_) && !execution_->request.module_manifest.empty()) {
            if (const auto result = bind(kCookFeature, kCookPoint); !result)
                return result;
            if (const auto result = bind(kPublishFeature, kPublishPoint); !result)
                return result;
        }
        return foundation::Result::success();
    }

private:
    PipelineDescriptor descriptor_{};
    ProjectOperationExecution* execution_{nullptr};
};

template <typename Feature>
foundation::Result register_feature(PipelineRegistry& registry, ProjectOperationExecution& execution) {
    return registry.register_feature([&execution] { return std::make_unique<Feature>(&execution); });
}

}  // namespace

foundation::Result execute_project_operation_pipeline(ProjectOperationExecution& execution,
                                                      ProjectOperationStatus& status) {
    status = {};
    status.state = ProjectOperationState::kRunning;
    status.result = foundation::Result::success();
    if (execution.is_cancelled && execution.is_cancelled()) {
        status = cancelled_status();
        return status.result;
    }

    PipelineRegistry registry;
    if (const auto result = register_feature<ProjectValidationFeature>(registry, execution); !result)
        return fail(status, result, "pipeline.feature.validation");
    if (const auto result = register_feature<ProjectConfigureFeature>(registry, execution); !result)
        return fail(status, result, "pipeline.feature.configure");
    if (const auto result = register_feature<ProjectCompileFeature>(registry, execution); !result)
        return fail(status, result, "pipeline.feature.compile");
    if (const auto result = register_feature<ProjectCookFeature>(registry, execution); !result)
        return fail(status, result, "pipeline.feature.cook");
    if (const auto result = register_feature<ProjectPublishFeature>(registry, execution); !result)
        return fail(status, result, "pipeline.feature.publish");
    if (const auto result =
            registry.register_pipeline([&execution] { return std::make_unique<ProjectOperationPipeline>(&execution); });
        !result) {
        return fail(status, result, "pipeline.definition");
    }

    const auto compiled = PipelineCompiler{}.compile({kProjectOperationPipeline, PipelineScope::kEngine, 0, &registry,
                                                      kProjectOperationInstance, &registry.phase_registry()});
    if (!compiled)
        return fail(status, compiled.result, "pipeline.compile");

    PipelineExecutionContext context(&execution, PipelineScope::kEngine, nullptr, nullptr, &status);
    PipelineScheduler scheduler(PipelineSchedulerDesc{1, 1, 1, 1});
    const auto executed = scheduler.execute(compiled.value, context);
    if (!executed) {
        if (executed.code == foundation::ResultCode::kCancelled ||
            (execution.is_cancelled && execution.is_cancelled())) {
            status = cancelled_status();
            return status.result;
        }
        if (status.state != ProjectOperationState::kFailed) {
            return fail(status, executed, "pipeline.execute");
        }
        return status.result;
    }
    if (execution.is_cancelled && execution.is_cancelled()) {
        status = cancelled_status();
        return status.result;
    }
    status.state = ProjectOperationState::kSucceeded;
    status.result = foundation::Result::success();
    return status.result;
}

}  // namespace aeg::tooling::detail
