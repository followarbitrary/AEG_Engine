#pragma once

#include <filesystem>
#include <vector>

#include "editor_custom_node_workflow.hpp"
#include "editor_node_definition.hpp"
#include "editor_feature_graph_document.hpp"

namespace aeg::application {

enum class NodeParameterMigrationAction : uint8_t { kConvertValue, kResetValue };

struct NodeParameterMigration final {
    feature::ParameterId id{};
    NodeParameterMigrationAction action{NodeParameterMigrationAction::kConvertValue};
};

struct NodeDefinitionSaveRequest final {
    NodeDefinitionDraft draft{};
    bool generate_stub{true};
    bool disconnect_affected_edges{false};
    /** Carries the selected project file's resolver when multiple projects share one source root. */
    feature::AssetKindResolver asset_kind_resolver{};
    /** Selects configured source output and assets for defaults/usage scans; root-only saves use Source. */
    std::filesystem::path selected_project_file{};
    /** Explicitly migrates effective old values, including defaults materialized by the parameter codec. */
    std::vector<NodeParameterMigration> parameter_migrations{};
};

struct NodeDefinitionSaveResult final {
    NodeContractDiff diff{};
    CustomNodeScaffoldResult generated{};
    bool document_changed{false};
    bool draft_saved{false};
    bool generated_stub{false};
};

/** Coordinates contract validation, impact review, code generation and one history transaction. */
class EditorNodeDefinitionService final {
public:
    /** Documents loaded in the current project, including the active document. */
    void set_project_documents(std::vector<EditorFeatureGraphDocument*> documents);

    [[nodiscard]] foundation::ResultValue<NodeContractDiff> preview(
        const EditorFeatureGraphDocument&, const NodeDefinitionDraft&) const;

    [[nodiscard]] foundation::ResultValue<NodeDefinitionSaveResult> save(
        EditorFeatureGraphDocument&, const NodeDefinitionSaveRequest&, const std::filesystem::path& project_root);

private:
    [[nodiscard]] std::vector<NodeContractUsage> usages_for(
        const EditorFeatureGraphDocument&, const feature::FeatureDefinitionId&) const;

    std::vector<EditorFeatureGraphDocument*> project_documents_{};
};

}  // namespace aeg::application
