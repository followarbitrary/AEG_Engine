#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "pipeline_authoring.hpp"
#include "result.hpp"

namespace aeg::tooling {

enum class ProjectOperation : uint8_t { kBuild, kCook, kBuildAndCook };
enum class ProjectOperationState : uint8_t { kRunning, kSucceeded, kFailed, kCancelled };

struct ProjectOperationRequest final {
    std::filesystem::path project_file{};
    std::filesystem::path engine_directory{};
    std::string configuration{"Debug"};
    std::string target{};
    ProjectOperation operation{ProjectOperation::kBuildAndCook};
    // Cook inputs are explicit so the operation service can publish the same
    // verified module/package pair used by runtime boot.  Empty values retain
    // the legacy build-only orchestration behavior for older callers.
    std::filesystem::path module_manifest{};
    std::filesystem::path output_directory{};
};

struct ProjectOperationStatus final {
    ProjectOperationState state{ProjectOperationState::kRunning};
    foundation::Result result{};
    std::vector<pipeline::DocumentDiagnostic> diagnostics{};
    // Ordered names of LogicFeatures that completed in the operation Pipeline.
    // This is progress evidence for the Editor and never carries execution
    // handles or implementation pointers.
    std::vector<std::string> pipeline_stages{};
    std::filesystem::path module_manifest{};
    std::filesystem::path game_manifest{};
};

class ProjectOperationService final {
public:
    ProjectOperationService();
    ~ProjectOperationService();
    ProjectOperationService(const ProjectOperationService&) = delete;
    ProjectOperationService& operator=(const ProjectOperationService&) = delete;

    [[nodiscard]] foundation::ResultValue<uint64_t> start(const ProjectOperationRequest& request);
    [[nodiscard]] foundation::ResultValue<ProjectOperationStatus> poll(uint64_t operation_id);
    [[nodiscard]] foundation::Result cancel(uint64_t operation_id);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aeg::tooling
