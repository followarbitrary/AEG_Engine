/**
 * @file module_descriptor.hpp
 * @brief Stable module identity shared by authoring and runtime boundaries.
 *
 * This header intentionally contains only identity/dependency data.  It does
 * not expose ModuleRegistry, PipelineRegistry, or RenderPass registration.
 */
#pragma once

#include <string>
#include <vector>

#include "module_identity.hpp"

namespace aeg::module {

/** @brief Minimum version required from one dependent module. */
struct ModuleDependency final {
    ModuleId module{};
    Version minimum_version{1, 0, 0};
};

/** @brief Immutable identity and dependency metadata for one engine module. */
struct ModuleDescriptor final {
    ModuleId id{};
    std::string name{};
    Version version{1, 0, 0};
    std::vector<ModuleDependency> dependencies{};
};

}  // namespace aeg::module
