#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "result.hpp"

namespace aeg::tooling::detail {
struct ProjectProcessRequest final {
    std::filesystem::path executable{};
    std::vector<std::wstring> arguments{};
    std::filesystem::path working_directory{};
    std::function<bool()> cancellation_requested{};
};

struct ProjectProcessResult final {
    int exit_code{-1};
    bool cancelled{false};
};

[[nodiscard]] foundation::ResultValue<ProjectProcessResult> run_project_process(const ProjectProcessRequest& request);
}  // namespace aeg::tooling::detail
