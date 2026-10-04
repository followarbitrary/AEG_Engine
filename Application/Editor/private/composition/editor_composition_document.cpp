#include "editor_composition_document.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iomanip>
#include <new>
#include <sstream>

#include "editor_composition_catalog.hpp"

namespace aeg::application {

namespace {
struct EmptyDraft final {
    pipeline::TypeCatalogEntry entry{};
    pipeline::AuthoringTypeKey owner_feature{};
    std::string node_id{};
};

std::vector<EmptyDraft> read_empty_drafts(const pipeline::PipelineDocument& document) {
    std::vector<EmptyDraft> drafts;
    for (const auto& extension : document.extensions) {
        if (extension.owner_node != "@flowgraph:empty-type" ||
            extension.payload.rfind("AEG_FLOWGRAPH_EMPTY 1 ", 0) != 0)
            continue;
        std::istringstream input(extension.payload);
        std::string signature;
        uint32_t version = 0;
        uint32_t kind_value = 0;
        std::string name;
        uint64_t owner_id = 0;
        if (!(input >> signature >> version >> kind_value >> std::quoted(name) >> owner_id) ||
            signature != "AEG_FLOWGRAPH_EMPTY" || version != 1 || kind_value > 2 || name.empty())
            continue;
        const auto kind = static_cast<pipeline::AuthoringTypeKind>(kind_value);
        std::string node_id;
        static_cast<void>(input >> std::quoted(node_id));
        EmptyDraft draft{make_empty_custom_type(kind, name), {pipeline::AuthoringTypeKind::kFeature, owner_id},
                         std::move(node_id)};
        drafts.push_back(std::move(draft));
    }
    return drafts;
}

pipeline::TypeCatalog restore_empty_drafts(const pipeline::PipelineDocument& document,
                                           pipeline::TypeCatalog catalog) {
    const auto drafts = read_empty_drafts(document);
    std::vector<pipeline::TypeCatalogEntry> entries;
    for (const auto& entry : catalog.entries())
        entries.push_back(entry);
    for (const auto& draft : drafts) {
        const bool node_exists = !draft.node_id.empty()
                                     ? std::any_of(document.pipeline_nodes.begin(), document.pipeline_nodes.end(),
                                                   [&](const auto& node) { return node.node_id == draft.node_id; }) ||
                                           std::any_of(document.feature_nodes.begin(), document.feature_nodes.end(),
                                                       [&](const auto& node) { return node.node_id == draft.node_id; }) ||
                                           std::any_of(document.pass_nodes.begin(), document.pass_nodes.end(),
                                                       [&](const auto& node) { return node.node_id == draft.node_id; })
                                     : (draft.entry.key.kind == pipeline::AuthoringTypeKind::kPipeline
                                            ? std::any_of(document.pipeline_nodes.begin(), document.pipeline_nodes.end(),
                                                          [&](const auto& node) {
                                                              return node.type.value() == draft.entry.key.id;
                                                          })
                                            : draft.entry.key.kind == pipeline::AuthoringTypeKind::kFeature
                                                  ? std::any_of(document.feature_nodes.begin(), document.feature_nodes.end(),
                                                                [&](const auto& node) {
                                                                    return node.type.value() == draft.entry.key.id;
                                                                })
                                                  : std::any_of(document.pass_nodes.begin(), document.pass_nodes.end(),
                                                                [&](const auto& node) {
                                                                    return node.type.value() == draft.entry.key.id;
                                                                }));
        if (!node_exists)
            continue;
        if (catalog.find(draft.entry.key) == nullptr) {
            if (catalog.add(draft.entry))
                entries.push_back(draft.entry);
        }
    }
    for (const auto& draft : drafts) {
        if (draft.owner_feature.id == 0)
            continue;
        const auto* owner = catalog.find(draft.owner_feature);
        if (owner == nullptr)
            continue;
        auto owner_copy = *owner;
        auto* declaration = std::get_if<pipeline::FeatureAuthoringDeclaration>(&owner_copy.declaration);
        if (declaration == nullptr)
            continue;
        declaration->allow_add_passes = true;
        const auto pass_id = pipeline::RenderPassTypeId(draft.entry.key.id);
        if (std::find(declaration->allowed_pass_types.begin(), declaration->allowed_pass_types.end(), pass_id) ==
            declaration->allowed_pass_types.end())
            declaration->allowed_pass_types.push_back(pass_id);
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [&](const auto& entry) { return entry.key == owner_copy.key; }),
                      entries.end());
        entries.push_back(std::move(owner_copy));
        catalog = pipeline::TypeCatalog{};
        for (const auto& entry : entries)
            static_cast<void>(catalog.add(entry));
    }
    return catalog;
}

}  // namespace

EditorCompositionDocument::EditorCompositionDocument(pipeline::PipelineDocument document, pipeline::TypeCatalog catalog)
    : catalog_(restore_empty_drafts(document, std::move(catalog))),
      current_(std::make_shared<const pipeline::PipelineDocument>(std::move(document))) {
    saved_ = current_;
    refresh_diagnostics();
}

void EditorCompositionDocument::refresh_diagnostics() {
    diagnostics_ = pipeline::validate_pipeline_document(*current_, catalog_);
}

bool EditorCompositionDocument::dirty() const noexcept {
    // Layout and opaque authoring edits must still be persisted even though
    // they intentionally do not participate in the execution semantic digest.
    return saved_ == nullptr || current_ != saved_ || catalog_revision_ != saved_catalog_revision_;
}

foundation::Result EditorCompositionDocument::replace_catalog(pipeline::TypeCatalog catalog) {
    try {
        auto diagnostics = pipeline::validate_pipeline_document(*current_, catalog);
        catalog_ = std::move(catalog);
        diagnostics_ = std::move(diagnostics);
        ++catalog_revision_;
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("editor composition catalog replacement allocation failed");
    } catch (const std::exception& exception) {
        return foundation::unknown_exception(exception);
    }
}

foundation::Result EditorCompositionDocument::apply_contract_transaction(
    std::string label, pipeline::TypeCatalog next_catalog,
    std::function<foundation::Result(pipeline::PipelineDocument&)> mutation) {
    if (label.empty() || !mutation)
        return foundation::invalid_argument("editor contract transaction is incomplete");
    try {
        auto next = std::make_shared<pipeline::PipelineDocument>(*current_);
        if (const foundation::Result result = mutation(*next); !result)
            return result;
        // Catalog entries are validated when they are added/replaced. Running
        // document validation here keeps diagnostics synchronized with the
        // prospective contract before publishing the transaction.
        auto next_diagnostics = pipeline::validate_pipeline_document(*next, next_catalog);
        auto next_const = std::shared_ptr<const pipeline::PipelineDocument>(std::move(next));
        const uint64_t next_catalog_revision = catalog_revision_ + 1;
        HistoryEntry entry{std::move(label), current_, next_const, catalog_, next_catalog,
                           catalog_revision_, next_catalog_revision};
        std::vector<HistoryEntry> candidate = history_;
        candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(cursor_), candidate.end());
        candidate.push_back(std::move(entry));
        history_.swap(candidate);
        ++cursor_;
        current_ = std::move(next_const);
        catalog_ = std::move(next_catalog);
        catalog_revision_ = next_catalog_revision;
        diagnostics_ = std::move(next_diagnostics);
        ++revision_;
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("editor contract transaction allocation failed");
    } catch (const std::exception& exception) {
        return foundation::unknown_exception(exception);
    }
}


foundation::Result EditorCompositionDocument::set_layout(std::string_view node_id, double x, double y,
                                                         bool expanded) {
    if (node_id.empty() || !std::isfinite(x) || !std::isfinite(y))
        return foundation::invalid_argument("editor layout identity or coordinates are invalid");
    try {
        auto next = std::make_shared<pipeline::PipelineDocument>(*current_);
        auto found = std::find_if(next->layouts.begin(), next->layouts.end(),
                                  [&](const auto& layout) { return layout.node_id == node_id; });
        if (found == next->layouts.end())
            next->layouts.push_back({std::string{node_id}, x, y, expanded});
        else {
            found->x = x;
            found->y = y;
            found->expanded = expanded;
        }
        ++next->layout_revision;
        current_ = std::shared_ptr<const pipeline::PipelineDocument>(std::move(next));
        ++revision_;
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("editor layout update allocation failed");
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
        HistoryEntry entry{std::move(label), current_, next_const, catalog_, catalog_, catalog_revision_,
                           catalog_revision_};
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
        catalog_ = history_[cursor_].after_catalog;
        catalog_revision_ = history_[cursor_].after_catalog_revision;
        ++cursor_;
    } else {
        if (!can_undo())
            return foundation::invalid_state("editor composition has no undo state");
        --cursor_;
        current_ = history_[cursor_].before;
        catalog_ = history_[cursor_].before_catalog;
        catalog_revision_ = history_[cursor_].before_catalog_revision;
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
