#include <iostream>
#include <algorithm>
#include <variant>

#include "editor_composition_commands.hpp"
#include "editor_composition_test_support.hpp"

int main() {
    aeg::tests::EditorCompositionFixture fixture;
    aeg::application::EditorCompositionDocument editor(fixture.document, fixture.catalog);
    aeg::application::EditorCompositionCommands commands(editor);
    using namespace aeg::application;
    {
        aeg::tests::EditorCompositionFixture root_fixture;
        aeg::application::EditorCompositionDocument root_document(root_fixture.document, root_fixture.catalog);
        aeg::application::EditorCompositionCommands root_commands(root_document);
        if (root_commands.add_type(
                {EditorGraphKind::kPipeline, {}, {}, {}},
                {aeg::pipeline::AuthoringTypeKind::kFeature,
                 aeg::pipeline::FeatureId("Game.EditorFeature").value()},
                "root_feature"))
            return 73;
        if (root_document.snapshot()->feature_nodes.size() != 1U ||
            root_document.snapshot()->pass_nodes.size() != 1U)
            return 74;
    }
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
    if (!commands.move_pass("second_pass", -1) || editor.snapshot()->pass_nodes.front().node_id != "second_pass" ||
        editor.snapshot()->pass_nodes.back().node_id != "pass")
        return 48;
    if (!commands.move_pass("second_pass", 1) || editor.snapshot()->pass_nodes.back().node_id != "second_pass")
        return 49;
    if (!editor.undo() || !editor.undo() || !editor.undo() || editor.snapshot()->pass_nodes.size() != before_passes ||
        !editor.redo() ||
        editor.snapshot()->pass_nodes.back().node_id != "second_pass")
        return 3;
    if (!commands.erase_node("second_pass") || editor.snapshot()->pass_nodes.size() != before_passes)
        return 4;
    // Empty custom-node creation persists its draft marker in the same
    // semantic history entry, so one Undo removes the node and its metadata.
    if (!commands.add_type({EditorGraphKind::kPipeline, {}, {}, {}},
                           {aeg::pipeline::AuthoringTypeKind::kPipeline,
                            aeg::pipeline::PipelineId("Game.EditorPipeline").value()},
                           "empty_pipeline", {}, "AEG_FLOWGRAPH_EMPTY 1 0 \"Game.Empty\" 0"))
        return 41;
    if (editor.snapshot()->pipeline_nodes.size() != 2U || editor.snapshot()->extensions.empty() ||
        editor.snapshot()->extensions.back().owner_node != "@flowgraph:empty-type")
        return 42;
    if (!editor.undo() || editor.snapshot()->pipeline_nodes.size() != 1U || !editor.snapshot()->extensions.empty())
        return 43;
    if (!editor.redo() || editor.snapshot()->pipeline_nodes.size() != 2U || editor.snapshot()->extensions.empty())
        return 44;
    {
        aeg::tests::EditorCompositionFixture marker_fixture;
        aeg::application::EditorCompositionDocument marker_document{marker_fixture.document, marker_fixture.catalog};
        aeg::application::EditorCompositionCommands marker_commands(marker_document);
        if (!marker_commands.add_type({EditorGraphKind::kPipeline, {}, {}, {}},
                                      {aeg::pipeline::AuthoringTypeKind::kPipeline,
                                       aeg::pipeline::PipelineId("Game.EditorPipeline").value()},
                                      "marker_pipeline", {},
                                      "AEG_FLOWGRAPH_EMPTY 1 0 \"Game.Marker\" 0 \"marker_pipeline\""))
            return 440;
        if (!marker_commands.erase_node("marker_pipeline") || !marker_document.snapshot()->extensions.empty())
            return 441;
    }
    {
        aeg::tests::EditorCompositionFixture catalog_fixture;
        auto custom_pipeline = *catalog_fixture.catalog.find(
            {aeg::pipeline::AuthoringTypeKind::kPipeline,
             aeg::pipeline::PipelineId("Game.EditorPipeline").value()});
        custom_pipeline.key = {aeg::pipeline::AuthoringTypeKind::kPipeline,
                               aeg::pipeline::PipelineId("Game.EmptyUndo").value()};
        custom_pipeline.name = "Game.EmptyUndo";
        custom_pipeline.display_name = "Empty Undo Pipeline";
        const auto custom_key = custom_pipeline.key;
        aeg::pipeline::TypeCatalog next_catalog = catalog_fixture.catalog;
        if (!next_catalog.add(custom_pipeline))
            return 442;
        aeg::application::EditorCompositionDocument catalog_document{catalog_fixture.document, catalog_fixture.catalog};
        aeg::application::EditorCompositionCommands catalog_commands(catalog_document);
        if (!catalog_commands.add_type_with_catalog({EditorGraphKind::kPipeline, {}, {}, {}}, custom_key,
                                                    "catalog_pipeline", std::move(next_catalog)))
            return 443;
        if (catalog_document.catalog().find(custom_key) == nullptr || !catalog_document.undo() ||
            catalog_document.catalog().find(custom_key) != nullptr || !catalog_document.redo() ||
            catalog_document.catalog().find(custom_key) == nullptr)
            return 444;
    }
    if (!commands.disconnect("color") || !editor.snapshot()->edges.empty())
        return 5;
    if (!editor.execute("Add fixed ordering", [](auto& next) {
            next.edges.push_back({"fixed-order", aeg::pipeline::DocumentEdgeKind::kOrdering,
                                  {"pass", 0, 0}, {"feature", 0, 0}, 0});
            return aeg::foundation::Result::success();
        }) || commands.disconnect("fixed-order"))
        return 46;
    if (!editor.undo())
        return 47;
    if (!commands.connect({"color", aeg::pipeline::DocumentEdgeKind::kRenderResource, "pass", 11, "feature", 22, 77}) ||
        editor.snapshot()->edges.size() != 1U)
        return 6;
    {
        aeg::tests::EditorCompositionFixture resource_fixture;
        resource_fixture.document.resources.push_back(
            {.node_id = "SceneColor", .resource_id = aeg::pipeline::RenderResourceId(77),
             .type = aeg::pipeline::ArtifactTypeId(1), .resource_constraints =
                                                    {.resource_class = aeg::pipeline::ResourceClass::kTexture}});
        aeg::application::EditorCompositionDocument resource_document(resource_fixture.document,
                                                                       resource_fixture.catalog);
        aeg::application::EditorCompositionCommands resource_commands(resource_document);
        if (!resource_commands.connect({"pass-resource", aeg::pipeline::DocumentEdgeKind::kRenderResource,
                                         "pass", 11, "SceneColor", 77, 77, 0, 0}) ||
            resource_document.snapshot()->edges.size() != 2U) {
            std::cerr << "render-resource ports must connect pass outputs to resource nodes\n";
            return 66;
        }
    }
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
    if (!commands.begin_transaction("incomplete draft") || !commands.update_transaction([](auto& draft) {
            draft.input_bindings.push_back({.node_id = "feature", .port_id = 22,
                                            .source = aeg::pipeline::InputSourceKind::kUnset,
                                            .status = aeg::pipeline::InputBindingStatus::kUnset});
            return aeg::foundation::Result::success();
        }) || !commands.commit_transaction() || editor.snapshot()->input_bindings.empty())
        return 45;

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

    {
        using namespace aeg::pipeline;
        auto draft = fixture.document;
        TypeCatalog explicit_catalog;
        for (auto entry : fixture.catalog.entries()) {
            if (entry.key.kind == AuthoringTypeKind::kFeature) {
                entry.ports.front().input = InputPortContract{
                    .allowed_sources = input_source_mask(InputSourceKind::kEdge) |
                                       input_source_mask(InputSourceKind::kAsset),
                    .presence = InputPresenceRequirement::kRequired, .min_edges = 1, .max_edges = 1};
                auto layers = entry.ports.front();
                layers.id = 23;
                layers.name = "Layers";
                layers.input->variadic_group_id = 100;
                entry.ports.push_back(layers);
                entry.variadic_inputs = {{100, "Layers", 23, 0, 2, true, true}};
            }
            if (!explicit_catalog.add(entry)) return 30;
        }
        draft.edges.clear();
        draft.feature_nodes.push_back(draft.feature_nodes.front());
        draft.feature_nodes.back().node_id = "unfinished";
        EditorCompositionDocument document(draft, explicit_catalog);
        EditorCompositionCommands edits(document);
        // A legal connection must be possible while another node remains unfinished.
        if (!edits.connect({"legal", DocumentEdgeKind::kRenderResource, "pass", 11, "feature", 22, 77})) return 31;
        InputBindingDocument duplicate_edges{.node_id = "feature", .port_id = 22,
            .source = InputSourceKind::kEdge, .edge_ids = {"legal", "legal"},
            .status = InputBindingStatus::kBound};
        if (const auto duplicate_result = edits.set_input_source(duplicate_edges); duplicate_result) {
            std::cerr << "duplicate unexpectedly accepted\n";
            return 310;
        }
        InputBindingDocument asset{.node_id = "feature", .port_id = 22, .source = InputSourceKind::kAsset,
            .asset = {"00000000-0000-0000-0000-000000000011"}, .status = InputBindingStatus::kBound};
        if (!document.execute("Add unrelated input binding", [](auto& next) {
                next.input_bindings.push_back({.node_id = "feature", .port_id = 23,
                                                .source = aeg::pipeline::InputSourceKind::kEdge,
                                                .edge_ids = {"keep"},
                                                .status = aeg::pipeline::InputBindingStatus::kBound});
                return aeg::foundation::Result::success();
            })) return 32;
        const auto with_unrelated = document.snapshot();
        if (!edits.set_input_source(asset) || !document.snapshot()->edges.empty()) return 32;
        if (document.snapshot()->input_bindings.back().edge_ids != std::vector<std::string>{"keep"}) return 32;
        if (!document.undo() || document.snapshot() != with_unrelated || !document.redo()) return 33;
        if (!edits.add_variadic_slot("feature", 100) || !edits.add_variadic_slot("feature", 100) ||
            edits.add_variadic_slot("feature", 100)) return 34;
        const auto first = document.snapshot()->variadic_slots[0].slot_id;
        const auto second = document.snapshot()->variadic_slots[1].slot_id;
        if (edits.connect({"unknown-slot", DocumentEdgeKind::kRenderResource, "pass", 11, "feature", 23, 77, 0,
                           999}))
            return 340;
        if (document.snapshot()->edges.size() != 0U ||
            std::any_of(document.snapshot()->variadic_slots.begin(), document.snapshot()->variadic_slots.end(),
                        [](const auto& slot) { return slot.slot_id == 999; }))
            return 341;
        if (!edits.move_variadic_slot("feature", 100, second, -1) ||
            document.snapshot()->variadic_slots[1].order != 0 ||
            document.snapshot()->variadic_slots[0].slot_id != first) return 35;
        if (!edits.remove_variadic_slot("feature", 100, first) || !document.undo()) return 36;
        InputBindingDocument invalid_source{.node_id = "feature", .port_id = 22,
            .source = InputSourceKind::kContext, .context_key = "missing", .status = InputBindingStatus::kBound};
        if (edits.set_input_source(invalid_source))
            return 360;
        if (!edits.duplicate_node("feature", "feature_copy") ||
            document.snapshot()->feature_nodes.back().node_id != "feature_copy" ||
            std::none_of(document.snapshot()->input_bindings.begin(), document.snapshot()->input_bindings.end(),
                [](const auto& binding) { return binding.node_id == "feature_copy" && binding.source == InputSourceKind::kAsset; })) {
            return 37;
        }
        if (!edits.erase_node("pipeline") || !document.snapshot()->edges.empty() ||
            !document.snapshot()->input_bindings.empty() || !document.snapshot()->variadic_slots.empty()) return 38;
        if (!document.undo() || document.snapshot()->feature_nodes.size() != 3) return 39;
    }
    std::cout << "Editor composition commands smoke passed\n";
    return 0;
}
