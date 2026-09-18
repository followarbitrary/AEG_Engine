#include <iostream>

#include "editor_composition_commands.hpp"
#include "editor_composition_test_support.hpp"

int main() {
    aeg::tests::EditorCompositionFixture fixture;
    aeg::application::EditorCompositionDocument editor(fixture.document, fixture.catalog);
    aeg::application::EditorCompositionCommands commands(editor);
    using namespace aeg::application;
    if (!commands.add_type(
            {EditorGraphKind::kFeature, "pipeline", {}, {}},
            {aeg::pipeline::AuthoringTypeKind::kFeature, aeg::pipeline::FeatureId("Game.EditorFeature").value()},
            "second_feature", "PostProcess"))
        return 10;
    if (editor.snapshot()->feature_nodes.size() != 2U)
        return 11;
    if (!editor.undo() || editor.snapshot()->feature_nodes.size() != 1U || !editor.redo())
        return 12;
    const auto before_passes = editor.snapshot()->pass_nodes.size();
    if (!commands.add_type(
            {EditorGraphKind::kPass, "pipeline", "feature", "render"},
            {aeg::pipeline::AuthoringTypeKind::kRenderPass, aeg::pipeline::RenderPassTypeId("Game.EditorPass").value()},
            "second_pass"))
        return 1;
    if (editor.snapshot()->pass_nodes.size() != before_passes + 1U || !editor.dirty())
        return 2;
    if (!editor.undo() || editor.snapshot()->pass_nodes.size() != before_passes || !editor.redo() ||
        editor.snapshot()->pass_nodes.back().node_id != "second_pass")
        return 3;
    if (!commands.erase_node("second_pass") || editor.snapshot()->pass_nodes.size() != before_passes)
        return 4;
    if (!commands.disconnect("color") || !editor.snapshot()->edges.empty())
        return 5;
    if (!commands.connect({"color", aeg::pipeline::DocumentEdgeKind::kRenderResource, "pass", 11, "feature", 22, 77}) ||
        editor.snapshot()->edges.size() != 1U)
        return 6;
    if (commands.set_parameter("feature", 999, true))
        return 7;
    if (!commands.begin_transaction("layout edit") || !commands.update_transaction([](auto& draft) {
            draft.layouts.push_back({"feature", 32.0, 64.0, true});
            return aeg::foundation::Result::success();
        }) ||
        !commands.commit_transaction() || editor.snapshot()->layouts.size() != 1U)
        return 8;
    if (!commands.begin_transaction("cancel") || !commands.cancel_transaction() || commands.transaction_active())
        return 9;
    std::cout << "Editor composition commands smoke passed\n";
    return 0;
}
