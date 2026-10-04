#include <iostream>

#include "editor_composition_document.hpp"
#include "editor_composition_test_support.hpp"

int main() {
    aeg::tests::EditorCompositionFixture fixture;
    aeg::application::EditorCompositionDocument editor(fixture.document, fixture.catalog);
    const auto original = editor.snapshot();
    const bool enabled = original->feature_nodes.front().enabled;
    const auto result = editor.execute("Toggle feature", [&](auto& next) {
        next.feature_nodes.front().enabled = !enabled;
        return aeg::foundation::Result::success();
    });
    if (!result || !editor.dirty() || original->feature_nodes.front().enabled != enabled || !editor.can_undo())
        return 1;
    if (!editor.undo() || editor.dirty() || !editor.can_redo())
        return 2;
    if (!editor.redo() || !editor.dirty() || editor.snapshot()->feature_nodes.front().enabled != !enabled)
        return 3;
    editor.mark_saved();
    if (editor.dirty())
        return 4;
    const auto before_revision = editor.revision();
    const auto before = editor.snapshot();
    if (editor.execute("Rejected", [](auto&) { return aeg::foundation::invalid_argument("rejected"); }) ||
        editor.revision() != before_revision || editor.snapshot() != before)
        return 5;
    if (!editor.undo())
        return 6;
    if (!editor.execute("Branch", [](auto& next) {
            next.feature_nodes.front().enabled = true;
            return aeg::foundation::Result::success();
        }))
        return 7;
    if (editor.can_redo())
        return 8;
    aeg::pipeline::TypeCatalog empty_catalog;
    if (!editor.replace_catalog(std::move(empty_catalog)) || editor.diagnostics().empty())
        return 9;
    auto persisted = fixture.document;
    persisted.pipeline_nodes.push_back({"custom-pipeline", aeg::pipeline::PipelineId("Game.Custom.Pipeline"),
                                        {1, 0, 0}, aeg::pipeline::PipelineScope::kFrame, "Renderer", {}});
    persisted.extensions.push_back({"@flowgraph:empty-type", aeg::pipeline::OpaqueAuthoringEncoding::kToml,
                                    "AEG_FLOWGRAPH_EMPTY 1 0 \"Game.Custom.Pipeline\" 0"});
    aeg::application::EditorCompositionDocument restored(persisted, fixture.catalog);
    if (restored.catalog().find({aeg::pipeline::AuthoringTypeKind::kPipeline,
                                 aeg::pipeline::PipelineId("Game.Custom.Pipeline").value()}) == nullptr)
        return 10;
    std::cout << "Editor composition history smoke passed\n";
    return 0;
}
