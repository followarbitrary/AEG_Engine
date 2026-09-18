#pragma once

#include <functional>

#include "project_context.hpp"
#include "project_operation.hpp"

namespace aeg::tooling::detail {

/** @brief Mutable operation-scoped state shared by Tooling Pipeline Features. */
struct ProjectOperationExecution final {
    ProjectOperationRequest request{};
    std::function<bool()> is_cancelled{};
    application::project::ProjectFile project{};
    std::filesystem::path project_root{};
    std::filesystem::path cmake_file{};
    std::filesystem::path build_root{};
    bool project_loaded{false};
};

/** @brief Runs one Build/Cook operation through the engine Pipeline scheduler. */
[[nodiscard]] foundation::Result execute_project_operation_pipeline(ProjectOperationExecution& execution,
                                                                    ProjectOperationStatus& status);

}  // namespace aeg::tooling::detail
