#pragma once

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pipeline_authoring.hpp"

namespace aeg::application {

class EditorCompositionDocument final {
public:
    EditorCompositionDocument(pipeline::PipelineDocument document, pipeline::TypeCatalog catalog);

    [[nodiscard]] std::shared_ptr<const pipeline::PipelineDocument> snapshot() const noexcept { return current_; }
    [[nodiscard]] const pipeline::TypeCatalog& catalog() const noexcept { return catalog_; }
    [[nodiscard]] foundation::Result replace_catalog(pipeline::TypeCatalog catalog);
    /** Apply a node-contract catalog edit and its dependent document changes as one undoable step. */
    [[nodiscard]] foundation::Result apply_contract_transaction(
        std::string label, pipeline::TypeCatalog next_catalog,
        std::function<foundation::Result(pipeline::PipelineDocument&)> mutation);
    /** Update editor-only placement without adding a semantic history entry. */
    [[nodiscard]] foundation::Result set_layout(std::string_view node_id, double x, double y,
                                                bool expanded = true);
    [[nodiscard]] foundation::Result execute(std::string label,
                                             std::function<foundation::Result(pipeline::PipelineDocument&)> mutation);
    [[nodiscard]] foundation::Result undo();
    [[nodiscard]] foundation::Result redo();
    void mark_saved() noexcept {
        saved_ = current_;
        saved_catalog_revision_ = catalog_revision_;
    }
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] bool can_undo() const noexcept { return cursor_ != 0; }
    [[nodiscard]] bool can_redo() const noexcept { return cursor_ < history_.size(); }
    [[nodiscard]] uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] std::span<const pipeline::DocumentDiagnostic> diagnostics() const noexcept { return diagnostics_; }

private:
    struct HistoryEntry final {
        std::string label{};
        std::shared_ptr<const pipeline::PipelineDocument> before{};
        std::shared_ptr<const pipeline::PipelineDocument> after{};
        pipeline::TypeCatalog before_catalog{};
        pipeline::TypeCatalog after_catalog{};
        uint64_t before_catalog_revision{};
        uint64_t after_catalog_revision{};
    };

    void refresh_diagnostics();
    [[nodiscard]] foundation::Result move_history(bool forward);

    pipeline::TypeCatalog catalog_{};
    std::shared_ptr<const pipeline::PipelineDocument> current_{};
    std::shared_ptr<const pipeline::PipelineDocument> saved_{};
    uint64_t catalog_revision_{};
    uint64_t saved_catalog_revision_{};
    std::vector<HistoryEntry> history_{};
    std::size_t cursor_{};
    uint64_t revision_{};
    std::vector<pipeline::DocumentDiagnostic> diagnostics_{};
};

}  // namespace aeg::application
