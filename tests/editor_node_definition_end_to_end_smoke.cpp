#include <filesystem>
#include <iostream>

#include "editor_composition_test_support.hpp"
#include "editor_composition_view.hpp"
#include "editor_node_definition_service.hpp"
#include "editor_project_pipeline_authoring.hpp"
#include "pipeline_document_io.hpp"

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}
}

int main() {
    using namespace aeg;
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "aeg_editor_node_definition_end_to_end_smoke";
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root / "Assets", error);

    tests::EditorCompositionFixture fixture;
    application::EditorCompositionDocument editor{fixture.document, fixture.catalog};
    const auto* source = editor.catalog().find({pipeline::AuthoringTypeKind::kFeature,
                                                pipeline::FeatureId("Game.EditorFeature").value()});
    check(source != nullptr, "end-to-end fixture feature is missing");
    if (!source)
        return 1;
    application::NodeDefinitionDraft draft{};
    draft.entry = *source;
    draft.entry.ports.push_back({.id = 31,
                                 .name = "OutputColor",
                                 .direction = pipeline::PortDirection::kOutput,
                                 .value_kind = pipeline::PortValueKind::kTexture,
                                 .schema_id = 1,
                                 .schema_version = 1,
                                 .max_connections = 1,
                                 .binding_id = 31,
                                 .resource_constraints = {.resource_class = pipeline::ResourceClass::kTexture}});
    draft.entry.parameters.push_back({.id = 41,
                                      .name = "Exposure",
                                      .display_name = "Exposure",
                                      .kind = pipeline::ParameterKind::kFloat64,
                                      .default_value = 1.0,
                                      .minimum = 0.0,
                                      .maximum = 4.0});
    draft.source = application::NodeContractSource::kEditorDraft;
    application::EditorNodeDefinitionService service;
    const auto saved = service.save(editor, {draft, true, false}, root);
    check(static_cast<bool>(saved) && saved.value.generated_stub, "end-to-end generated save failed");
    check(fs::exists(root / "Source" / "FlowGraph" / "EditorFeature.flowgraph.contract.json"),
          "generated contract was not persisted");
    check(fs::exists(root / "Source" / "FlowGraph" / "EditorFeature.flowgraph.registration.cpp"),
          "generated registration was not persisted");

    check(static_cast<bool>(application::project::save_pipeline_document(root / "Assets" / "Graph.aegpipe",
                                                            *editor.snapshot())),
          "authoring graph was not persisted");
    const auto loaded = application::load_editor_project_pipeline_authoring(root, root / "Source", root / "Assets");
    check(static_cast<bool>(loaded) && loaded.value.contracts.size() == 1 && loaded.value.documents.size() == 1,
          "project restart did not discover the generated contract and graph");
    if (loaded) {
        const auto key = draft.entry.key;
        check(loaded.value.catalog.find(key) != nullptr, "custom contract disappeared after restart");
        const auto& document = loaded.value.documents.front().document;
        for (const auto address : {application::EditorGraphAddress{application::EditorGraphKind::kPipeline, {}, {}, {}},
                                   application::EditorGraphAddress{application::EditorGraphKind::kFeature, "pipeline", "feature", {}},
                                   application::EditorGraphAddress{application::EditorGraphKind::kPass, "pipeline", "feature", "render"}}) {
            const auto projected = application::project_composition_graph(document, loaded.value.catalog, address);
            check(static_cast<bool>(projected), "graph switch projection failed after restart");
        }
    }
    fs::remove_all(root, error);
    if (failures)
        return 1;
    std::cout << "Editor node definition end-to-end smoke passed\n";
    return 0;
}
