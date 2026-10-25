#pragma once
#include "editor_custom_node_workflow.hpp"
#include "editor_node_definition_service.hpp"
#include <functional>
#include <optional>
#include <span>

namespace aeg::application {
using EditorIdeLauncher = std::function<foundation::Result(const EditorIdeLaunchRequest&)>;
class EditorFeatureDefinitionPanel final {
public:
    /** Opens the same Definition workspace used by the picker on its next draw. */
    void request_open(feature::FeatureDefinitionId id) { pending_open_ = id; }
    /** Last refreshed source labels for this document lifetime and selected project. */
    [[nodiscard]] std::span<const NodeDefinitionDraft> presentation_overlay(
        const EditorFeatureGraphDocument& document, const std::filesystem::path& project) const noexcept {
        return project_identity_ == project && presentation_document_identity_ == document.identity_owner()
            ? std::span<const NodeDefinitionDraft>{saved_drafts_} : std::span<const NodeDefinitionDraft>{};
    }
    void draw(EditorFeatureGraphDocument&, const std::filesystem::path& project_root,
              std::span<const feature::FeatureDefinitionId> compiled_features,
              const feature::AssetKindResolver& resolver = {},
              const std::filesystem::path& selected_project_file = {},
              const EditorIdeLauncher& launcher = {},
              std::span<const NodeDefinitionDraft> presentations = {}, bool* workspace_open = nullptr);
private:
    NodeDefinitionDraft draft_{};
    EditorNodeDefinitionService service_{};
    bool open_{false};
    bool disconnect_{false};
    feature::FeatureDefinitionId selected_{};
    std::optional<feature::FeatureDefinitionId> pending_open_{};
    std::filesystem::path project_identity_{};
    std::optional<CustomNodeDefinitionTarget> implementation_{};
    EditorIdeKind ide_kind_{EditorIdeKind::kAuto};
    std::string implementation_status_{};
    std::string status_{};
    std::vector<NodeDefinitionDraft> saved_drafts_{};
    std::shared_ptr<const void> presentation_document_identity_{};
    std::vector<std::string> diagnostics_{};
    std::vector<NodeParameterMigration> parameter_migrations_{};
};
}  // namespace aeg::application
