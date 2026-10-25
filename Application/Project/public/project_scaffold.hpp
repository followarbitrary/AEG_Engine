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
    std::string template_id{"empty"};
    std::string engine_sdk{"1.0"};
    std::string startup_feature_graph{"Assets/Features/Default.aegfeature"};
};

/** @brief Copies the tracked game template and publishes Project.aegproj last. */
[[nodiscard]] foundation::ResultValue<std::filesystem::path> create_project(const ProjectScaffoldRequest& request);

}  // namespace aeg::application::project
