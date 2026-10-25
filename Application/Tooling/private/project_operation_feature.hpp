#pragma once

#include <functional>

#include "project_context.hpp"
#include "project_operation.hpp"

namespace aeg::tooling::detail {

/** @brief Mutable operation-scoped state shared by Tooling Logic Process Features. */
struct ProjectOperationExecution final {
    ProjectOperationRequest request{};
    std::function<bool()> is_cancelled{};
    /** Receives owning Runtime snapshots on the operation worker after actual publication/dispatch. */
    std::function<void(const ProjectOperationStatus&)> publish_status{};
    application::project::ProjectFile project{};
    std::filesystem::path project_root{};
    std::filesystem::path cmake_file{};
    std::filesystem::path build_root{};
    bool project_loaded{false};
};

/** @brief Runs one Build/Cook operation through ordered Logic Process Features. */
[[nodiscard]] foundation::Result execute_project_operation_feature(ProjectOperationExecution& execution,
                                                                    ProjectOperationStatus& status);

}  // namespace aeg::tooling::detail
