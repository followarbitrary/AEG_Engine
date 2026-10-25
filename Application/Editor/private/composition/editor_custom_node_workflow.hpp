#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "editor_node_definition.hpp"

namespace aeg::application {

enum class CustomNodeGenerationMode : uint8_t { kCreate, kUpdateGeneratedRegions };

/**
 * @brief User choices needed to create one project-owned code node.
 *
 * `project_root` is the only filesystem authority. `source_subdirectory` is
 * intentionally relative to it so a malformed project setting cannot make
 * the wizard write outside the checkout.
 */
struct CustomNodeScaffoldRequest final {
    std::filesystem::path project_root{};
    std::filesystem::path source_subdirectory{"Source/FlowGraph"};
    std::string namespace_name{"game"};
    std::string type_name{};
    feature::ModuleIdentity owner_module{};
    CustomNodeGenerationMode mode{CustomNodeGenerationMode::kCreate};
    std::string contract_digest{};
    bool overwrite{false};
};

struct CustomNodeDefinitionTarget final {
    std::filesystem::path path{};
    uint32_t line{1};
    std::string symbol{};
};

struct CustomNodeScaffoldResult final {
    feature::FeatureDefinitionId definition_id{};
    std::vector<std::filesystem::path> files{};
    CustomNodeDefinitionTarget contract_target{};
    CustomNodeDefinitionTarget implementation_target{};
    CustomNodeDefinitionTarget registration_target{};
    bool updated_generated_regions{false};
    bool implementation_preserved{false};
    bool contract_updated{false};
};

enum class EditorIdeKind : uint8_t { kAuto, kVisualStudioCode, kClion, kVisualStudio };

struct EditorIdeLaunchRequest final {
    std::filesystem::path project_root{};
    std::filesystem::path source_file{};
    uint32_t line{1};
    uint32_t column{1};
    EditorIdeKind ide{EditorIdeKind::kAuto};
};

/** Opens a generated node source at its definition location without blocking the editor. */
[[nodiscard]] foundation::Result launch_editor_ide(const EditorIdeLaunchRequest&);

/** @brief Checks the wizard request and the catalog contract without touching disk. */

[[nodiscard]] foundation::Result validate_custom_node_scaffold_request(
    const CustomNodeScaffoldRequest&, const NodeDefinitionDraft&);

/**
 * @brief Generates source, contract and registration guidance for a custom node.
 *
 * The operation is deterministic and does not launch an IDE. The returned
 * definition targets are the exact file/symbol locations an IDE adapter should
 * open. Existing files are preserved unless `overwrite` is explicitly set.
 */

[[nodiscard]] foundation::ResultValue<CustomNodeScaffoldResult> scaffold_custom_node(
    const CustomNodeScaffoldRequest&, const NodeDefinitionDraft&);

}  // namespace aeg::application
