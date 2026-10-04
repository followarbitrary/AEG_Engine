#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "editor_composition_test_support.hpp"
#include "editor_custom_node_workflow.hpp"
#include "editor_node_contract_storage.hpp"

namespace {
std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void check(bool condition, const char* message, int& failures) {
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}
}

int main() {
    using namespace aeg;
    namespace fs = std::filesystem;
    int failures = 0;
    const fs::path root = fs::temp_directory_path() / "aeg_editor_node_definition_generation_smoke";
    std::error_code error;
    fs::remove_all(root, error);

    tests::EditorCompositionFixture fixture;
    application::NodeDefinitionDraft draft{};
    draft.entry = *fixture.catalog.find({pipeline::AuthoringTypeKind::kFeature,
                                         pipeline::FeatureId("Game.EditorFeature").value()});
    draft.generated = true;
    application::CustomNodeScaffoldRequest request{};
    request.project_root = root;
    request.source_subdirectory = "Source/FlowGraph";
    request.namespace_name = "game::graph";
    request.type_name = "IncrementalFeature";
    const auto first = application::scaffold_custom_node(request, draft);
    check(static_cast<bool>(first) && first.value.files.size() == 4, "initial generation failed", failures);
    if (!first)
        return 1;
    const auto contract = application::read_editor_node_contract(first.value.files[3]);
    check(static_cast<bool>(contract) && contract.value.draft.entry.ports.size() == draft.entry.ports.size(),
          "generated contract does not match the draft", failures);

    const auto implementation_path = first.value.files[1];
    auto handwritten = read(implementation_path);
    handwritten += "\n// HANDWRITTEN_SENTINEL\n";
    { std::ofstream output(implementation_path, std::ios::binary | std::ios::trunc); output << handwritten; }
    auto updated = draft;
    updated.entry.ports.front().name = "UpdatedSceneColor";
    request.mode = application::CustomNodeGenerationMode::kUpdateGeneratedRegions;
    const auto second = application::scaffold_custom_node(request, updated);
    check(static_cast<bool>(second) && second.value.updated_generated_regions && second.value.implementation_preserved,
          "incremental generation failed", failures);
    check(read(implementation_path).find("HANDWRITTEN_SENTINEL") != std::string::npos,
          "incremental generation overwrote handwritten implementation", failures);
    const auto updated_contract = application::read_editor_node_contract(first.value.files[3]);
    check(static_cast<bool>(updated_contract) && updated_contract.value.draft.entry.ports.front().name == "UpdatedSceneColor",
          "incremental contract did not update", failures);

    const auto pristine_registration = read(first.value.files[2]);
    auto modified_registration = pristine_registration;
    const auto edited_offset = modified_registration.find("UpdatedSceneColor");
    modified_registration.replace(edited_offset, std::string_view{"UpdatedSceneColor"}.size(), "ManuallyEditedColor");
    { std::ofstream output(first.value.files[2], std::ios::binary | std::ios::trunc); output << modified_registration; }
    check(!application::scaffold_custom_node(request, updated),
          "manual edits inside a generated region were overwritten", failures);
    { std::ofstream output(first.value.files[2], std::ios::binary | std::ios::trunc); output << pristine_registration; }

    const std::vector<std::string> before_conflict{read(first.value.files[0]), read(first.value.files[1]),
                                                   read(first.value.files[2]), read(first.value.files[3])};
    {
        auto header = read(first.value.files[0]);
        const auto marker = header.find("// AEG_FLOWGRAPH_GENERATED_BEGIN");
        header.erase(marker, marker == std::string::npos ? 0 : std::string_view{"// AEG_FLOWGRAPH_GENERATED_BEGIN"}.size());
        std::ofstream output(first.value.files[0], std::ios::binary | std::ios::trunc);
        output << header;
    }
    auto conflicted = updated;
    conflicted.entry.display_name = "Conflict Update";
    const auto conflict = application::scaffold_custom_node(request, conflicted);
    check(!conflict, "marker conflict was silently accepted", failures);
    check(read(first.value.files[1]) == before_conflict[1] && read(first.value.files[2]) == before_conflict[2] &&
              read(first.value.files[3]) == before_conflict[3],
          "failed generation changed files outside the conflict", failures);

    fs::remove_all(root, error);
    if (failures)
        return 1;
    std::cout << "Editor node definition generation smoke passed\n";
    return 0;
}

\n