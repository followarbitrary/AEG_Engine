#include "project_operation_feature.hpp"

#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "feature_registry.hpp"
#include "../../../Engine/Feature/private/feature_clock_admission.hpp"
#include "feature_graph.hpp"
#include "feature_runtime.hpp"
#include "feature_module.hpp"
#include "module.hpp"
#include "project_cooker.hpp"
#include "project_process.hpp"

namespace aeg::tooling::detail {
namespace {

using namespace aeg::feature;

constexpr FeatureDefinitionId kValidationFeature{"Engine.Tooling.ProjectValidationFeature"};
constexpr FeatureDefinitionId kConfigureFeature{"Engine.Tooling.ProjectConfigureFeature"};
constexpr FeatureDefinitionId kCompileFeature{"Engine.Tooling.ProjectCompileFeature"};
constexpr FeatureDefinitionId kCookFeature{"Engine.Tooling.ProjectCookFeature"};
constexpr FeatureDefinitionId kPublishFeature{"Engine.Tooling.ProjectPublishFeature"};
constexpr ScopeId kToolingScope{"Engine"};
constexpr ClockId kToolingClock{"Engine.Tooling.ProjectOperationClock"};
constexpr module::ModuleId kToolingModule{"Engine.Tooling.ProjectOperations"};

using FeatureTask = foundation::Result (*)(ProjectOperationExecution&, ProjectOperationStatus&);

void mark_cancelled(ProjectOperationStatus& status) {
    status.state = ProjectOperationState::kCancelled;
    status.result = foundation::Result::failure(foundation::ResultCode::kCancelled, "Project operation cancelled");
}

foundation::Result cancelled(ProjectOperationExecution& execution, ProjectOperationStatus& status) {
    mark_cancelled(status);
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

bool wants_cook(const ProjectOperationExecution& execution) noexcept {
    return execution.request.operation == ProjectOperation::kCook ||
           execution.request.operation == ProjectOperation::kBuildAndCook;
}

bool wants_build(const ProjectOperationExecution& execution) noexcept {
    return execution.request.operation != ProjectOperation::kCook;
}

foundation::Result validation_feature(ProjectOperationExecution& execution, ProjectOperationStatus& status) {
    if (execution.is_cancelled && execution.is_cancelled())
        return cancelled(execution, status);
    if (execution.request.project_file.empty() || !std::filesystem::exists(execution.request.project_file))
        return fail(status, foundation::not_found("Project file was not found"), "project.file");
    if (execution.request.configuration.empty()) {
        return fail(status, foundation::invalid_argument("Project operation configuration is empty"),
                    "project.configuration");
    }

    application::project::ProjectFileCodec codec;
    const auto parsed = codec.load(execution.request.project_file);
    if (!parsed)
        return fail(status, parsed.result, "project.config");
    const auto paths =
        application::project::ProjectPaths::resolve(execution.request.project_file, parsed.value.config.paths);
    if (!paths)
        return fail(status, paths.result, "project.paths");

    execution.project = parsed.value;
    execution.project_loaded = true;
    execution.project_root = execution.request.project_file.parent_path();
    execution.cmake_file = execution.project_root / "CMakeLists.txt";
    execution.build_root = paths.value.directory(application::project::ProjectDirectory::kBuild);
    return foundation::Result::success();
}

std::filesystem::path cmake_executable(const ProjectOperationExecution& execution) {
    if (!execution.request.cmake_executable.empty()) {
        return execution.request.cmake_executable.is_absolute()
                   ? execution.request.cmake_executable
                   : execution.project_root / execution.request.cmake_executable;
    }
#if defined(_WIN32)
    return std::filesystem::path{L"cmake.exe"};
#else
    return std::filesystem::path{"cmake"};
#endif
}

foundation::Result configure_feature(ProjectOperationExecution& execution, ProjectOperationStatus& status) {
    if (execution.is_cancelled && execution.is_cancelled())
        return cancelled(execution, status);
    if (execution.request.engine_directory.empty())
        return fail(status, foundation::invalid_argument("Build requires an explicit engine directory"), "project.engine");
    const auto engine_directory = execution.request.engine_directory.is_absolute()
                                      ? execution.request.engine_directory
                                      : execution.project_root / execution.request.engine_directory;
    if (!std::filesystem::is_directory(engine_directory))
        return fail(status, foundation::not_found("Build engine directory was not found"), "project.engine");
    if (!std::filesystem::is_regular_file(execution.cmake_file))
        return fail(status, foundation::not_found("Project CMakeLists.txt was not found"), "project.configure");

    ProjectProcessRequest process{};
    process.executable = cmake_executable(execution);
    process.working_directory = execution.project_root;
    process.cancellation_requested = execution.is_cancelled;
    process.arguments = {L"-S", execution.project_root.wstring(), L"-B", execution.build_root.wstring(),
                         L"-DENGINE_DIR=" + engine_directory.wstring()};
    const auto configured = run_project_process(process);
    if (!configured)
        return fail(status, configured.result, "project.configure");
    if (configured.value.cancelled)
        return cancelled(execution, status);
    if (configured.value.exit_code != 0) {
        return fail(status,
                    foundation::Result::failure(foundation::ResultCode::kBackendError, "CMake configure failed"),
                    "project.configure");
    }
    return foundation::Result::success();
}

foundation::Result compile_feature(ProjectOperationExecution& execution, ProjectOperationStatus& status) {
    if (execution.is_cancelled && execution.is_cancelled())
        return cancelled(execution, status);
    ProjectProcessRequest process{};
    process.executable = cmake_executable(execution);
    process.working_directory = execution.project_root;
    process.cancellation_requested = execution.is_cancelled;
    process.arguments = {L"--build", execution.build_root.wstring(), L"--config",
                         std::wstring(execution.request.configuration.begin(), execution.request.configuration.end())};
    if (!execution.request.target.empty()) {
        process.arguments.push_back(L"--target");
        process.arguments.emplace_back(execution.request.target.begin(), execution.request.target.end());
    }
    const auto built = run_project_process(process);
    if (!built)
        return fail(status, built.result, "project.build");
    if (built.value.cancelled)
        return cancelled(execution, status);
    if (built.value.exit_code != 0) {
        return fail(status, foundation::Result::failure(foundation::ResultCode::kBackendError, "CMake build failed"),
                    "project.build");
    }
    return foundation::Result::success();
}

foundation::Result cook_feature(ProjectOperationExecution& execution, ProjectOperationStatus& status) {
    if (execution.is_cancelled && execution.is_cancelled())
        return cancelled(execution, status);
    if (execution.request.module_manifest.empty())
        return fail(status, foundation::invalid_argument("Cook requires an explicit module manifest"), "cook.module");

    const auto output_directory =
        execution.request.output_directory.empty()
            ? execution.project_root / execution.project.config.paths.binaries / execution.request.configuration
            : execution.request.output_directory;
    ProjectCookRequest request{};
    request.project_file = execution.request.project_file;
    request.module_manifest = execution.request.module_manifest;
    request.output_directory = output_directory;
    request.configuration = execution.request.configuration;
    const auto cooked = cook_project(request);
    if (!cooked)
        return fail_with_cook_diagnostics(status, cooked.result, cooked.value);
    status.game_manifest = cooked.value.game_manifest;
    status.module_manifest = execution.request.module_manifest;
    status.diagnostics.insert(status.diagnostics.end(), cooked.value.diagnostics.begin(),
                              cooked.value.diagnostics.end());
    return foundation::Result::success();
}

foundation::Result publish_feature(ProjectOperationExecution& execution, ProjectOperationStatus& status) {
    if (execution.is_cancelled && execution.is_cancelled())
        return cancelled(execution, status);
    if (status.game_manifest.empty() || !std::filesystem::exists(status.game_manifest))
        return fail(status, foundation::not_found("Cook did not publish a game manifest"), "project.publish");
    return foundation::Result::success();
}

FeatureDefinition make_process_feature(FeatureDefinitionId id, SideEffectId effect, FeatureTask task,
                                       ProjectOperationExecution& execution, ProjectOperationStatus& status) {
    FeatureDefinition definition{};
    definition.id = id;
    definition.version = {1, 0, 0};
    definition.contract.definition_id = id;
    definition.contract.version = definition.version;
    definition.contract.structure = FeatureStructure::kAtomic;
    definition.contract.domains = to_mask(FeatureDomain::kLogic);
    definition.contract.execution = ExecutionKind::kProcess;
    definition.contract.state = StatePolicy::kProcessState;
    definition.contract.scope = ScopeContract{kToolingScope};
    definition.contract.clock = make_periodic_clock_contract(kToolingClock, 60, 8, LateDataPolicy::kQueue,
        DeadlinePolicy::kObserve, 300'000'000'000ull, 0);
    definition.contract.clock.timing->relative_deadline_ns = 1'800'000'000'000ull;
    definition.contract.permissions.version = 1;
    definition.contract.has_continuation = true;
    definition.contract.side_effects = {{effect, 1, SideEffectKind::kExternalIo, CommitPolicy::kExternalOwner}};
    definition.contract.ports = {
        {PortId("In"), PortDirection::kInput, ValueSchemaId("ProjectOperation.Stage"), 1},
        {PortId("Out"), PortDirection::kOutput, ValueSchemaId("ProjectOperation.Stage"), 1},
    };
    definition.execute = [&execution, &status, task](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>)
        -> foundation::ResultValue<StateDelta> {
        if (const auto result = task(execution, status); !result)
            return {result, {}};
        return {foundation::Result::success(), StateDelta{token.state_version, status.feature_stages.size() + 1, "Done", true}};
    };
    return definition;
}

FeatureGraphDocument make_operation_graph(const FeatureRegistry& registry, const ProjectOperationExecution& execution) {
    FeatureGraphDocument graph{};
    graph.asset_uuid = "tooling://project-operation";
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.contract_digest = registry.contract_digest();
    const std::array<std::pair<FeatureNodeId, FeatureDefinitionId>, 5> all_nodes = {{
        {FeatureNodeId("ProjectValidationNode"), kValidationFeature},
        {FeatureNodeId("ProjectConfigureNode"), kConfigureFeature},
        {FeatureNodeId("ProjectCompileNode"), kCompileFeature},
        {FeatureNodeId("ProjectCookNode"), kCookFeature},
        {FeatureNodeId("ProjectPublishNode"), kPublishFeature},
    }};
    std::vector<std::pair<FeatureNodeId, FeatureDefinitionId>> nodes;
    nodes.reserve(all_nodes.size());
    for (const auto node : all_nodes) {
        if ((node.second == kCookFeature || node.second == kPublishFeature) && !wants_cook(execution))
            continue;
        if ((node.second == kConfigureFeature || node.second == kCompileFeature) && !wants_build(execution))
            continue;
        nodes.push_back(node);
        graph.nodes.push_back({node.first, node.second, {1, 0, 0}, OwnerPath{{node.first}}, {}});
        graph.process_subgraphs.push_back({node.first, {"Ready", "Done"}, {"Ready->Done"}, 1});
    }
    for (std::size_t index = 1; index < nodes.size(); ++index) {
        graph.edges.push_back({nodes[index - 1].first, PortId("Out"), nodes[index].first, PortId("In"),
                               EdgeKind::kCommand, kToolingClock, kToolingClock, 1, 0, 0});
    }
    return graph;
}

std::string_view stage_name(FeatureDefinitionId id) {
    if (id == kValidationFeature) return "ProjectValidationFeature";
    if (id == kConfigureFeature) return "ProjectConfigureFeature";
    if (id == kCompileFeature) return "ProjectCompileFeature";
    if (id == kCookFeature) return "ProjectCookFeature";
    return "ProjectPublishFeature";
}

foundation::Result capture_execution(const FeatureRegistry& registry, FeatureRuntime& runtime,
                                      const CompiledFeatureGraph& graph, ProjectOperationStatus& status) {
    std::vector<ProjectOperationFeatureExecution> observations;
    observations.reserve(graph.provenance.size());
    for (const auto& source : graph.provenance) {
        const auto* owner = runtime.find_instance(source.instance_id);
        if (!owner) return foundation::invalid_state("Project operation owner has retired");
        const auto* definition = registry.find_definition(owner->definition_id);
        const auto process_id = runtime.process(owner->id);
        if (!definition || !process_id) return foundation::invalid_state("Project operation Process has retired");
        const auto* process = runtime.find_process(process_id.value);
        const auto invocations = runtime.inspect_invocations(owner->id);
        if (!process || !invocations) return foundation::invalid_state("Project operation invocation cannot be observed");
        ProjectOperationFeatureExecution observed{};
        observed.source = source;
        observed.source.owner_module = owner->owner_module;
        observed.source.process_id = process->id;
        observed.source.invocation_id = invocations.value.last_issued
                                            ? invocations.value.last_issued->token.invocation_id : InvocationId{};
        observed.generation = runtime.generation();
        observed.definition_digest = owner->definition_digest;
        observed.side_effects = definition->contract.side_effects;
        observed.process = *process;
        observed.invocation = invocations.value;
        observations.push_back(std::move(observed));
    }
    status.feature_executions = std::move(observations);
    return foundation::Result::success();
}

void publish_status(const ProjectOperationExecution& execution, const ProjectOperationStatus& status) {
    if (execution.publish_status) execution.publish_status(status);
}

foundation::Result cancel_remaining(FeatureRuntime& runtime, const CompiledFeatureGraph& graph) {
    for (const auto& source : graph.provenance) {
        const auto id = runtime.process(source.instance_id);
        if (!id) return id.result;
        const auto* process = runtime.find_process(id.value);
        if (process && (process->status == ProcessStatus::kReady || process->status == ProcessStatus::kSuspended)) {
            if (const auto result = runtime.cancel(id.value); !result) return result;
        }
    }
    return foundation::Result::success();
}

}  // namespace

foundation::Result execute_project_operation_feature(ProjectOperationExecution& execution,
                                                     ProjectOperationStatus& status) {
    status = {};
    status.state = ProjectOperationState::kRunning;
    status.result = foundation::Result::success();
    if (execution.is_cancelled && execution.is_cancelled()) {
        mark_cancelled(status);
        return status.result;
    }

    // Immutable operation-host policy outlives Module registration/reload.
    static const auto clock_host = [] {
        feature::detail::ClockHostLimits limits{};
        limits.allow_tooling_background = true;
        return feature::detail::make_clock_host_context(limits);
    }();
    if (!clock_host) return fail(status, clock_host.result, "feature.clock.host");
    FeatureRegistry registry(clock_host.value);
    module::ModuleRegistry modules(registry);
    module::FeatureModuleDefinition module_definition{};
    module_definition.descriptor = {kToolingModule, "Tooling Project Operations", {1, 0, 0}, {}};
    const std::array<FeatureDefinition, 5> definitions = {
        make_process_feature(kValidationFeature, SideEffectId("Engine.Tooling.Project.ReadInputs"), &validation_feature, execution, status),
        make_process_feature(kConfigureFeature, SideEffectId("Engine.Tooling.Project.Configure"), &configure_feature, execution, status),
        make_process_feature(kCompileFeature, SideEffectId("Engine.Tooling.Project.Compile"), &compile_feature, execution, status),
        make_process_feature(kCookFeature, SideEffectId("Engine.Tooling.Project.Cook"), &cook_feature, execution, status),
        make_process_feature(kPublishFeature, SideEffectId("Engine.Tooling.Project.VerifyPublication"), &publish_feature, execution, status),
    };
    for (const auto& definition : definitions) {
        if (const auto result = module::add_feature_definition(module_definition, definition); !result)
            return fail(status, result, "feature.definition");
    }
    if (const auto result = module::register_static_module(modules, std::move(module_definition)); !result)
        return fail(status, result, "feature.module");
    const auto registered_module = modules.find_module(kToolingModule);
    if (!registered_module) return fail(status, registered_module.result, "feature.module");
    status.feature_module = registered_module.value;

    const auto graph = make_operation_graph(registry, execution);
    auto compiled = FeatureGraphCompiler{}.compile(graph, registry);
    if (!compiled)
        return fail(status, compiled.result, "feature.graph");
    FeatureRuntime runtime(registry);
    if (const auto result = runtime.publish_graph(graph, compiled.value); !result)
        return fail(status, result, "feature.graph.publish");
    if (const auto result = capture_execution(registry, runtime, compiled.value, status); !result)
        return fail(status, result, "feature.inspect");
    publish_status(execution, status);
    const auto finish_cancelled = [&]() {
        const auto cancellation = cancel_remaining(runtime, compiled.value);
        mark_cancelled(status);
        if (!cancellation) status.diagnostics.push_back({"feature.cancel", cancellation.message, {}, {}, 0, 0});
        if (const auto result = capture_execution(registry, runtime, compiled.value, status); !result)
            status.diagnostics.push_back({"feature.inspect", result.message, {}, {}, 0, 0});
        publish_status(execution, status);
        return status.result;
    };
    for (const auto& plan : compiled.value.plans) {
        for (const auto instance : plan.instances) {
            if (execution.is_cancelled && execution.is_cancelled()) return finish_cancelled();
            const auto result = runtime.dispatch_process(instance);
            if (result) {
                const auto invoked = runtime.inspect_invocations(instance);
                const auto* owner = runtime.find_instance(instance);
                if (!invoked || !invoked.value.last_issued || !invoked.value.last_committed || !owner ||
                    invoked.value.last_issued->token != invoked.value.last_committed->attempt.token)
                    return fail(status, foundation::invalid_state("Project operation has no completed owner commit"), "feature.commit");
                status.feature_stages.emplace_back(stage_name(owner->definition_id));
            }
            if (const auto captured = capture_execution(registry, runtime, compiled.value, status); !captured)
                return fail(status, captured, "feature.inspect");
            if (!result) {
                if (result.code == foundation::ResultCode::kCancelled ||
                    (execution.is_cancelled && execution.is_cancelled())) return finish_cancelled();
                if (status.state != ProjectOperationState::kFailed)
                    static_cast<void>(fail(status, result, "feature.execute"));
                publish_status(execution, status);
                return status.result;
            }
            publish_status(execution, status);
        }
    }
    if (execution.is_cancelled && execution.is_cancelled()) return finish_cancelled();
    status.state = ProjectOperationState::kSucceeded;
    status.result = foundation::Result::success();
    return status.result;
}

}  // namespace aeg::tooling::detail
