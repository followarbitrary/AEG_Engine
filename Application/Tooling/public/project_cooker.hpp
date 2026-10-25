/**
 * @file project_cooker.hpp
 * @brief Dependency-complete project package preparation boundary.
 */
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "project_diagnostic.hpp"
#include "result.hpp"

namespace aeg::tooling {
struct ProjectCookRequest final {
    std::filesystem::path project_file{};
    std::filesystem::path module_manifest{};
    std::filesystem::path output_directory{};
    std::string configuration{"Debug"};
};

struct ProjectCookResult final {
    std::filesystem::path package{};
    std::filesystem::path game_manifest{};
    std::vector<ProjectDiagnostic> diagnostics{};
};

[[nodiscard]] foundation::ResultValue<ProjectCookResult> cook_project(const ProjectCookRequest& request);
}  // namespace aeg::tooling
