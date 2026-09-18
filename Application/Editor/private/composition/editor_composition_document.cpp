#include "editor_composition_document.hpp"

#include <cstddef>
#include <exception>
#include <new>

namespace aeg::application {

EditorCompositionDocument::EditorCompositionDocument(pipeline::PipelineDocument document, pipeline::TypeCatalog catalog)
    : catalog_(std::move(catalog)), current_(std::make_shared<const pipeline::PipelineDocument>(std::move(document))) {
    saved_ = current_;
    refresh_diagnostics();
}

void EditorCompositionDocument::refresh_diagnostics() {
    diagnostics_ = pipeline::validate_pipeline_document(*current_, catalog_);
}

bool EditorCompositionDocument::dirty() const noexcept {
    // Layout and opaque authoring edits must still be persisted even though
    // they intentionally do not participate in the execution semantic digest.
    return saved_ == nullptr || current_ != saved_;
}

foundation::Result EditorCompositionDocument::replace_catalog(pipeline::TypeCatalog catalog) {
    try {
        auto diagnostics = pipeline::validate_pipeline_document(*current_, catalog);
        catalog_ = std::move(catalog);
        diagnostics_ = std::move(diagnostics);
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("editor composition catalog replacement allocation failed");
    } catch (const std::exception& exception) {
        return foundation::unknown_exception(exception);
    }
}

foundation::Result EditorCompositionDocument::execute(
    std::string label, std::function<foundation::Result(pipeline::PipelineDocument&)> mutation) {
    if (!mutation)
        return foundation::invalid_argument("editor composition mutation is empty");
    try {
        auto next = std::make_shared<pipeline::PipelineDocument>(*current_);
        if (const foundation::Result result = mutation(*next); !result)
            return result;
        auto next_const = std::shared_ptr<const pipeline::PipelineDocument>(std::move(next));
        HistoryEntry entry{std::move(label), current_, next_const};
        std::vector<HistoryEntry> candidate = history_;
        candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(cursor_), candidate.end());
        candidate.push_back(entry);
        history_.swap(candidate);
        ++cursor_;
        current_ = std::move(next_const);
        refresh_diagnostics();
        ++revision_;
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("editor composition mutation allocation failed");
    } catch (const std::exception& exception) {
        return foundation::unknown_exception(exception);
    }
}

foundation::Result EditorCompositionDocument::move_history(bool forward) {
    if (forward) {
        if (!can_redo())
            return foundation::invalid_state("editor composition has no redo state");
        current_ = history_[cursor_].after;
        ++cursor_;
    } else {
        if (!can_undo())
            return foundation::invalid_state("editor composition has no undo state");
        --cursor_;
        current_ = history_[cursor_].before;
    }
    refresh_diagnostics();
    ++revision_;
    return foundation::Result::success();
}

foundation::Result EditorCompositionDocument::undo() {
    return move_history(false);
}
foundation::Result EditorCompositionDocument::redo() {
    return move_history(true);
}

}  // namespace aeg::application
