#pragma once

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "pipeline_authoring.hpp"

namespace aeg::application {

class EditorCompositionDocument final {
public:
    EditorCompositionDocument(pipeline::PipelineDocument document, pipeline::TypeCatalog catalog);

    [[nodiscard]] std::shared_ptr<const pipeline::PipelineDocument> snapshot() const noexcept { return current_; }
    [[nodiscard]] const pipeline::TypeCatalog& catalog() const noexcept { return catalog_; }
    [[nodiscard]] foundation::Result replace_catalog(pipeline::TypeCatalog catalog);
    [[nodiscard]] foundation::Result execute(std::string label,
                                             std::function<foundation::Result(pipeline::PipelineDocument&)> mutation);
    [[nodiscard]] foundation::Result undo();
    [[nodiscard]] foundation::Result redo();
    void mark_saved() noexcept { saved_ = current_; }
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
    };

    void refresh_diagnostics();
    [[nodiscard]] foundation::Result move_history(bool forward);

    pipeline::TypeCatalog catalog_{};
    std::shared_ptr<const pipeline::PipelineDocument> current_{};
    std::shared_ptr<const pipeline::PipelineDocument> saved_{};
    std::vector<HistoryEntry> history_{};
    std::size_t cursor_{};
    uint64_t revision_{};
    std::vector<pipeline::DocumentDiagnostic> diagnostics_{};
};

}  // namespace aeg::application
