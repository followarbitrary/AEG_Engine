#include <algorithm>
#include <iostream>

#include "editor_composition_document.hpp"
#include "editor_node_definition.hpp"
#include "editor_composition_test_support.hpp"

namespace {
int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}

aeg::pipeline::TypeCatalogEntry editable_feature() {
    using namespace aeg::pipeline;
    TypeCatalogEntry entry{};
    entry.key = {AuthoringTypeKind::kFeature, FeatureId("Editor.DefinitionFeature").value()};
    entry.version = {1, 0, 0};
    entry.owner_module = ModuleId("Game.Editor");
    entry.name = "Editor.DefinitionFeature";
    entry.display_name = "Definition Feature";
    entry.domains = to_mask(FeatureDomain::kRender);
    entry.scopes = to_mask(PipelineScope::kFrame);
    auto input = PortDescriptor{.id = 11,
                                .name = "SceneColor",
                                .direction = PortDirection::kInput,
                                .value_kind = PortValueKind::kTexture,
                                .schema_id = 1,
                                .required = false,
                                .min_connections = 0,
                                .max_connections = 1,
                                .binding_id = 11,
                                .resource_constraints = {.resource_class = ResourceClass::kTexture}};
    input.input = InputPortContract{.allowed_sources = input_source_mask(InputSourceKind::kEdge) |
                                                       input_source_mask(InputSourceKind::kAsset),
                                    .presence = InputPresenceRequirement::kOptional,
                                    .min_edges = 0,
                                    .max_edges = 1};
    entry.ports.push_back(input);
    entry.ports.push_back({.id = 12,
                           .name = "Result",
                           .direction = PortDirection::kOutput,
                           .value_kind = PortValueKind::kTexture,
                           .schema_id = 1,
                           .max_connections = 2,
                           .binding_id = 12,
                           .resource_constraints = {.resource_class = ResourceClass::kTexture}});
    entry.parameters.push_back({.id = 21,
                                .name = "Intensity",
                                .display_name = "Intensity",
                                .kind = ParameterKind::kFloat64,
                                .default_value = 1.0,
                                .minimum = 0.0,
                                .maximum = 2.0,
                                .affects_topology = false});
    entry.declaration = FeatureAuthoringDeclaration{.parts = {{0, FeatureDomain::kRender, "render"}},
                                                     .allowed_pass_types = {RenderPassTypeId("Game.Pass")},
                                                     .allow_add_passes = true};
    return entry;
}

}  // namespace

int main() {
    using namespace aeg;
    auto entry = editable_feature();
    application::NodeDefinitionDraft draft{};
    draft.entry = entry;
    draft.source = application::NodeContractSource::kEditorDraft;
    draft.generated = true;

    check(static_cast<bool>(application::validate_node_definition(draft)), "valid node definition accepted");

    auto renamed = draft;
    renamed.entry.ports[0].name = "InputColor";
    const auto rename_diff = application::diff_node_definition(
        entry, renamed, std::vector<application::NodeContractUsage>{{"Graph.aegpipe", "node", {}, {}}});
    check(!rename_diff.requires_confirmation, "port rename does not require disconnect confirmation");
    check(rename_diff.renamed_ports.size() == 1 && rename_diff.renamed_ports.front() == 11,
          "port rename preserves stable PortId");

    auto reordered = draft;
    std::swap(reordered.entry.ports[0], reordered.entry.ports[1]);
    const auto reorder_diff = application::diff_node_definition(
        entry, reordered, std::vector<application::NodeContractUsage>{{"Graph.aegpipe", "node", {}, {}}});
    check(reorder_diff.changed_ports.empty(), "port reorder is not a contract change");

    auto removed = draft;
    removed.entry.ports.erase(removed.entry.ports.begin());
    const auto removal_diff = application::diff_node_definition(
        entry, removed,
        std::vector<application::NodeContractUsage>{{"Graph.aegpipe", "node", {"edge-scene-color"}, {11}}});
    check(removal_diff.requires_confirmation, "connected port removal requires confirmation");
    check(removal_diff.removed_ports.size() == 1 && removal_diff.affected_edge_ids.size() == 1 &&
              removal_diff.affected_binding_ids.size() == 1,
          "port removal reports exact affected identities");

    auto invalid = draft;
    invalid.entry.ports[0].id = invalid.entry.ports[1].id;
    check(!application::validate_node_definition(invalid), "duplicate port id rejected");
    invalid = draft;
    invalid.entry.parameters[0].default_value = 3.0;
    check(!application::validate_node_definition(invalid), "parameter default outside bounds rejected");

    pipeline::TypeCatalog catalog;
    check(static_cast<bool>(catalog.add(entry)), "catalog fixture added");
    auto document = tests::EditorCompositionFixture{}.document;
    application::EditorCompositionDocument editor{std::move(document), std::move(catalog)};
    auto replacement = draft.entry;
    replacement.display_name = "Updated Definition Feature";
    pipeline::TypeCatalog next_catalog;
    check(static_cast<bool>(next_catalog.add(replacement)), "replacement catalog prepared");
    check(static_cast<bool>(editor.apply_contract_transaction(
              "Update node definition", std::move(next_catalog), [](pipeline::PipelineDocument& next) {
                  next.edges.clear();
                  return foundation::Result::success();
              })),
          "contract transaction committed");
    check(editor.catalog().find(replacement.key)->display_name == "Updated Definition Feature" &&
              editor.snapshot()->edges.empty(),
          "contract and document changed together");
    check(static_cast<bool>(editor.undo()), "contract transaction undo succeeds");
    check(editor.catalog().find(replacement.key)->display_name == entry.display_name &&
              !editor.snapshot()->edges.empty(),
          "undo restores contract and edges together");

    if (failures)
        return 1;
    std::cout << "Editor node definition model smoke passed\n";
    return 0;
}
