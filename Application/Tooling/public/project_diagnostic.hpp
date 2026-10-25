/**
 * @file project_diagnostic.hpp
 * @brief Diagnostics emitted by project validation, build, and cook Features.
 */
#pragma once

#include <cstdint>
#include <string>

namespace aeg::tooling {

struct ProjectDiagnostic final {
    std::string code{};
    std::string message{};
    std::string node_id{};
    std::string edge_id{};
    uint64_t port_id{};
    uint64_t parameter_id{};
    uint64_t slot_id{};
};

}  // namespace aeg::tooling
