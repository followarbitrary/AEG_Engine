/**
 * @file editor_feature_graph_panel.hpp
 * @brief FeatureGraph authoring panel used by the main editor FlowGraph tab.
 */
#pragma once

#include <array>
#include <functional>
#include <optional>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <imgui.h>

#include "editor_feature_graph_document.hpp"
#include "editor_feature_definition_panel.hpp"
#include "editor_project_feature_authoring.hpp"
#include "editor_process_graph_canvas.hpp"
#include "editor_feature_graph_canvas.hpp"
#include "editor_window_chrome.hpp"
#include "feature_catalog.hpp"
#include "feature_graph_runtime_host.hpp"
#include "editor_feature_graph_preview.hpp"

namespace aeg::application {

struct EditorProjectFeatureGraphDocument;
struct EditorRenderGraphSnapshot;

class EditorFeatureGraphPanel final {
public:
    struct Actions final {
        std::function<foundation::Result()> save_draft{};
        /** Draws navigation inside this window; apply queued root changes after draw returns. */
        std::function<void()> draw_graph_selector{};
        std::function<void(bool)> report_focus{};
        const feature::FeatureCatalog* feature_catalog{nullptr};
        std::span<const NodeDefinitionDraft> feature_presentations{};
        bool native_window{false};
        aeg::editor::EditorWindowChromeActions window_actions{};
        std::filesystem::path project_root{};
        std::filesystem::path project_file{};
        feature::AssetKindResolver asset_kind_resolver{};
        std::span<const feature::FeatureDefinitionId> compiled_features{};
        /** Metadata catalog for the currently active executable module. */
        const feature::FeatureCatalog* active_module_catalog{nullptr};
        /** Reads a candidate manifest's validated metadata without loading its DLL. */
        std::function<foundation::ResultValue<feature::FeatureCatalog>(const std::filesystem::path&)>
            inspect_module_catalog{};
        std::function<foundation::Result(const std::filesystem::path&, std::string_view)> select_module{};
        std::function<foundation::Result(const feature::FeatureGraphDocument&)> apply_graph{};
        std::function<foundation::ResultValue<runtime::FeatureRuntimeNodeSnapshot>(
            std::string_view, feature::FeatureNodeId)> inspect_node{};
        std::function<foundation::ResultValue<runtime::FeatureRuntimeExecutionSnapshot>(std::string_view)> inspect_execution{};
        std::function<foundation::ResultValue<std::shared_ptr<const EditorRenderGraphSnapshot>>()> inspect_render_graph{};
        std::function<foundation::Result(const runtime::RuntimeProcessCommand&)> control_process{};
        /** Current selected Hierarchy target and Editor-private World route actions. */
        std::function<std::string()> selected_world_target_label{};
        std::function<std::string(std::string_view, feature::FeatureNodeId)> world_binding_status{};
        std::function<foundation::Result(std::string_view, feature::FeatureNodeId)> bind_world_command{};
        std::function<foundation::Result(std::string_view, feature::FeatureNodeId)> clear_world_command{};
        bool world_binding_editable{false};
        std::span<const EditorProjectFeatureGraphDocument> authored_feature_documents{};
        bool publication_pending{false};
        /** Explicitly reloads the saved project's complete startup graph batch. */
        std::function<foundation::Result(const std::filesystem::path&, std::string_view)> select_saved_module{};
    };

    void draw(EditorFeatureGraphDocument&, const std::filesystem::path& source_path,
              const Actions& actions);

private:
    EditorFeatureDefinitionPanel definition_panel_{};
    bool show_definitions_{false};
    EditorProcessGraphCanvas process_canvas_{};
    EditorFeatureGraphCanvas feature_canvas_{};
    void draw_parameters(EditorFeatureGraphDocument&, const feature::FeatureCatalog&,
                         const feature::AssetKindResolver&);
    void draw_orchestration(EditorFeatureGraphDocument&, const feature::FeatureGraphDocument&, const feature::FeatureCatalog&,
                            const std::filesystem::path& source_path,
                            std::span<const NodeDefinitionDraft> presentations);
    struct ParameterDraft final {
        feature::ParameterId id{};
        std::optional<foundation::ParameterValue> value{};
    };
    feature::FeatureNodeId parameter_node_{};
    std::shared_ptr<const feature::FeatureGraphDocument> parameter_snapshot_{};
    uint64_t parameter_revision_{UINT64_MAX};
    uint64_t parameter_catalog_digest_{0};
    std::vector<ParameterDraft> parameter_draft_{};
    std::vector<feature::FeatureParameterRecord> opaque_parameters_{};
    bool show_expert_parameters_{false};
    void draw_runtime_inspect(const feature::FeatureGraphDocument&, const Actions&);
    void draw_world_binding_editor(const feature::FeatureGraphDocument&, const Actions&);
    void draw_execution_inspect(const feature::FeatureGraphDocument&, const Actions&);
    bool show_execution_plan_{false};
    void draw_render_graph_inspect(const feature::FeatureGraphDocument&, const Actions&);
    bool show_render_graph_{false};
    std::string module_manifest_{};
    std::string module_configuration_{"Debug"};
    std::string selected_node_{};
    std::string selected_edge_{};
    std::string last_action_{};
    std::string reviewed_manifest_{};
    std::string reviewed_configuration_{};
    std::string contract_review_error_{};
    uint64_t reviewed_before_digest_{0};
    uint64_t reviewed_after_digest_{0};
    FeatureCatalogDiff contract_review_{};
    bool has_contract_review_{false};
    std::array<char, 128> catalog_search_{};
    int catalog_domain_filter_{0};
    int catalog_execution_filter_{0};
    std::optional<feature::ClockId> catalog_clock_filter_{};
    int catalog_structure_filter_{0};
    std::optional<feature::ScopeId> catalog_scope_filter_{};
    std::string edge_source_node_{};
    std::string edge_target_node_{};
    std::array<char, 32> edge_source_port_{};
    std::array<char, 32> edge_target_port_{};
    int edge_kind_{0};
    int edge_capacity_{1};
    int edge_deadline_{1};
    std::string process_owner_node_{};
    std::array<char, 64> process_state_{};
    std::array<char, 64> process_from_state_{};
    std::array<char, 64> process_to_state_{};
    std::optional<EditorFeatureGraphPreview> last_good_preview_{};
    std::optional<EditorFeatureGraphPreview> current_projection_{};
    std::shared_ptr<const feature::FeatureGraphDocument> projection_snapshot_{};
    std::shared_ptr<const void> preview_document_identity_{};
    foundation::Result projection_result_{};
    uint64_t projection_catalog_digest_{0};
    uint64_t projection_semantic_digest_{0};
    uint64_t last_good_preview_semantic_digest_{0};
    std::filesystem::path last_good_preview_path_{};
    std::string preview_asset_uuid_{};
    uint64_t last_good_preview_revision_{0};
    uint64_t last_good_preview_catalog_digest_{0};
    std::vector<EditorFeatureGraphPreviewDiagnostic> preview_diagnostics_{};
    std::shared_ptr<const feature::FeatureGraphDocument> last_good_preview_snapshot_{};
    std::string preview_action_status_{};
    int authoring_jump_{0};
};

}  // namespace aeg::application
