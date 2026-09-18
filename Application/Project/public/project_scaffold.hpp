/**
 * @file project_scaffold.hpp
 * @brief Creates a self-contained game-project source tree from the engine template.
 */
#pragma once

#include <filesystem>
#include <string>

#include "result.hpp"

namespace aeg::application::project {

struct ProjectScaffoldRequest final {
    std::filesystem::path root{};
    std::string name{};
    std::string module_target{"GameModule"};
};

/** @brief Copies the tracked game template and publishes Project.aegproj last. */
[[nodiscard]] foundation::ResultValue<std::filesystem::path> create_project(const ProjectScaffoldRequest& request);

}  // namespace aeg::application::project
