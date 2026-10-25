#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "project_diagnostic.hpp"
#include "result.hpp"
#include "module_bundle.hpp"
#include "feature_graph.hpp"

namespace aeg::tooling {

enum class ProjectOperation : uint8_t { kBuild, kCook, kBuildAndCook };
enum class ProjectOperationState : uint8_t { kRunning, kSucceeded, kFailed, kCancelled };

struct ProjectOperationRequest final {
    std::filesystem::path project_file{};
    std::filesystem::path engine_directory{};
    std::string configuration{"Debug"};
    std::string target{};
    ProjectOperation operation{ProjectOperation::kBuildAndCook};
    // Cook inputs select the verified module/package pair used by runtime boot.
    // Cook and BuildAndCook require an explicit module manifest.
    std::filesystem::path module_manifest{};
    std::filesystem::path output_directory{};
    /** Empty selects the system CMake executable; relative paths are project-root-relative. */
    std::filesystem::path cmake_executable{};
};

/** Owning observations copied from the actual operation Runtime on its owner thread. */
struct ProjectOperationFeatureExecution final {
    feature::FeatureNodeProvenance source{};
    feature::RuntimeGeneration generation{};
    uint64_t definition_digest{0};
    std::vector<feature::SideEffectContract> side_effects{};
    feature::ProcessInstance process{};
    feature::InvocationSnapshot invocation{};
};

struct ProjectOperationStatus final {
    ProjectOperationState state{ProjectOperationState::kRunning};
    foundation::Result result{};
    std::vector<ProjectDiagnostic> diagnostics{};
    // Ordered names of Logic Process Features that completed in the operation.
    // This is progress evidence for the Editor and never carries execution
    // handles or implementation pointers.
    std::vector<std::string> feature_stages{};
    std::filesystem::path module_manifest{};
    std::filesystem::path game_manifest{};
    module::ModuleDescriptor feature_module{};
    std::vector<ProjectOperationFeatureExecution> feature_executions{};
    /** Cancellation requests may precede retirement; true only after operation work has finished. */
    bool execution_complete{false};
};

class ProjectOperationService final {
public:
    ProjectOperationService();
    ~ProjectOperationService();
    ProjectOperationService(const ProjectOperationService&) = delete;
    ProjectOperationService& operator=(const ProjectOperationService&) = delete;

    [[nodiscard]] foundation::ResultValue<uint64_t> start(const ProjectOperationRequest& request);
    [[nodiscard]] foundation::ResultValue<ProjectOperationStatus> poll(uint64_t operation_id);
    /** Waits for worker retirement and returns the final owning operation facts. */
    [[nodiscard]] foundation::ResultValue<ProjectOperationStatus> wait(uint64_t operation_id);
    [[nodiscard]] foundation::Result cancel(uint64_t operation_id);
    /** Validates an explicit manifest against this project and configuration.
     * Relative manifests are project-root-relative; absolute custom build paths
     * are allowed. Returns owning verified facts; does not load or activate code.
     */
    [[nodiscard]] static foundation::ResultValue<module::ModuleBundle> select_module(
        const std::filesystem::path& project_file, const std::filesystem::path& module_manifest,
        std::string_view configuration);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aeg::tooling
