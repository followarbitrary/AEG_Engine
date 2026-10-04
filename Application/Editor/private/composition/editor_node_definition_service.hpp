#pragma once

#include <filesystem>
#include <vector>

#include "editor_custom_node_workflow.hpp"
#include "editor_node_definition.hpp"
#include "editor_composition_document.hpp"

namespace aeg::application {

struct NodeDefinitionSaveRequest final {
    NodeDefinitionDraft draft{};
    bool generate_stub{true};
    bool disconnect_affected_edges{false};
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
    void set_project_documents(std::vector<EditorCompositionDocument*> documents);

    [[nodiscard]] foundation::ResultValue<NodeContractDiff> preview(
        const EditorCompositionDocument&, const NodeDefinitionDraft&) const;

    [[nodiscard]] foundation::ResultValue<NodeDefinitionSaveResult> save(
        EditorCompositionDocument&, const NodeDefinitionSaveRequest&, const std::filesystem::path& project_root);

private:
    [[nodiscard]] std::vector<NodeContractUsage> usages_for(
        const EditorCompositionDocument&, const pipeline::AuthoringTypeKey&) const;

    std::vector<EditorCompositionDocument*> project_documents_{};
};

}  // namespace aeg::application
