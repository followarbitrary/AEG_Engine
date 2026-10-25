#pragma once

#include "editor_custom_node_workflow.hpp"

namespace aeg::application {
/** Resolves a generated Definition by canonical contract identity, never its display name.
 * The target is an existing project-local execute definition. Missing, ambiguous
 * or unsupported source returns a failure rather than a guessed file/line.
 */
[[nodiscard]] foundation::ResultValue<CustomNodeDefinitionTarget> locate_feature_implementation(
    const std::filesystem::path& project_root_or_file, feature::FeatureDefinitionId definition);
}  // namespace aeg::application
