#include <iostream>
#include <variant>

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
    if (editor.snapshot()->feature_nodes.size() != 2U || editor.snapshot()->pass_nodes.size() != 2U ||
        editor.snapshot()->pass_nodes.back().node_id != "second_feature/DefaultPass")
        return 11;
    if (!editor.undo() || editor.snapshot()->feature_nodes.size() != 1U || editor.snapshot()->pass_nodes.size() != 1U ||
        !editor.redo())
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

    {
        aeg::tests::EditorCompositionFixture constrained_fixture;
        auto constrained_entry = *constrained_fixture.catalog.find(
            {aeg::pipeline::AuthoringTypeKind::kFeature, aeg::pipeline::FeatureId("Game.EditorFeature").value()});
        constrained_entry.key = {aeg::pipeline::AuthoringTypeKind::kFeature,
                                 aeg::pipeline::FeatureId("Game.FixedFeature").value()};
        constrained_entry.name = "Game.FixedFeature";
        constrained_entry.display_name = "Fixed Feature";
        auto constrained_declaration =
            std::get<aeg::pipeline::FeatureAuthoringDeclaration>(constrained_entry.declaration);
        constrained_declaration.default_passes.clear();
        constrained_declaration.allow_add_passes = false;
        constrained_entry.declaration = std::move(constrained_declaration);
        if (!constrained_fixture.catalog.add(std::move(constrained_entry)))
            return 13;
        constrained_fixture.document.feature_nodes.clear();
        constrained_fixture.document.pass_nodes.clear();
        constrained_fixture.document.edges.clear();
        aeg::application::EditorCompositionDocument constrained_document(constrained_fixture.document,
                                                                         constrained_fixture.catalog);
        aeg::application::EditorCompositionCommands constrained_commands(constrained_document);
        if (!constrained_commands.add_type(
                {aeg::application::EditorGraphKind::kFeature, "pipeline", {}, {}},
                {aeg::pipeline::AuthoringTypeKind::kFeature, aeg::pipeline::FeatureId("Game.FixedFeature").value()},
                "fixed_feature", "PostProcess"))
            return 14;
        const auto pass_count_before = constrained_document.snapshot()->pass_nodes.size();
        if (constrained_commands.add_type(
                {aeg::application::EditorGraphKind::kPass, "pipeline", "fixed_feature", "render"},
                {aeg::pipeline::AuthoringTypeKind::kRenderPass,
                 aeg::pipeline::RenderPassTypeId("Game.EditorPass").value()},
                "fixed_pass"))
            return 15;
        if (constrained_document.snapshot()->pass_nodes.size() != pass_count_before)
            return 16;

        if (const auto palette =
                build_composition_palette(*constrained_document.snapshot(), constrained_fixture.catalog,
                                          {EditorGraphKind::kPass, "pipeline", "fixed_feature", "render"});
            !palette || !palette.value.empty())
            return 17;
    }

    {
        aeg::tests::EditorCompositionFixture direct_fixture;
        auto direct_entry = *direct_fixture.catalog.find(
            {aeg::pipeline::AuthoringTypeKind::kFeature, aeg::pipeline::FeatureId("Game.EditorFeature").value()});
        direct_entry.key = {aeg::pipeline::AuthoringTypeKind::kFeature,
                            aeg::pipeline::FeatureId("Game.DirectFeature").value()};
        direct_entry.name = "Game.DirectFeature";
        direct_entry.display_name = "Direct Feature";
        auto direct_declaration = std::get<aeg::pipeline::FeatureAuthoringDeclaration>(direct_entry.declaration);
        direct_declaration.parts.clear();
        direct_declaration.default_passes.clear();
        direct_declaration.allow_add_passes = true;
        direct_entry.declaration = std::move(direct_declaration);
        if (!direct_fixture.catalog.add(std::move(direct_entry)))
            return 18;
        direct_fixture.document.feature_nodes.clear();
        direct_fixture.document.pass_nodes.clear();
        direct_fixture.document.edges.clear();
        aeg::application::EditorCompositionDocument direct_document(direct_fixture.document, direct_fixture.catalog);
        aeg::application::EditorCompositionCommands direct_commands(direct_document);
        if (!direct_commands.add_type(
                {aeg::application::EditorGraphKind::kFeature, "pipeline", {}, {}},
                {aeg::pipeline::AuthoringTypeKind::kFeature, aeg::pipeline::FeatureId("Game.DirectFeature").value()},
                "direct_feature", "PostProcess"))
            return 19;
        if (!direct_commands.add_type({aeg::application::EditorGraphKind::kPass, "pipeline", "direct_feature", {}},
                                      {aeg::pipeline::AuthoringTypeKind::kRenderPass,
                                       aeg::pipeline::RenderPassTypeId("Game.EditorPass").value()},
                                      "direct_pass"))
            return 20;
        const auto& direct_pass = direct_document.snapshot()->pass_nodes.back();
        if (direct_pass.feature_node != "direct_feature" || !direct_pass.part_id.empty())
            return 21;

        if (const auto palette = build_composition_palette(*direct_document.snapshot(), direct_fixture.catalog,
                                                           {EditorGraphKind::kPass, "pipeline", "direct_feature", {}});
            !palette || palette.value.size() != 1U)
            return 22;
    }

    std::cout << "Editor composition commands smoke passed\n";
    return 0;
}
