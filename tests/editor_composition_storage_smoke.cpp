#include <filesystem>
#include <fstream>
#include <iostream>

#include "editor_composition_storage.hpp"
#include "editor_composition_test_support.hpp"

int main() {
    using namespace aeg;
    tests::EditorCompositionFixture fixture;
    application::EditorCompositionDocument editor(fixture.document, fixture.catalog);
    const auto root = std::filesystem::temp_directory_path() / "aeg_editor_composition_storage_smoke";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    const auto document_path = root / "custom.aegpipe";
    if (!application::save_editor_composition(document_path, editor))
        return 1;
    if (editor.dirty())
        return 2;
    if (!editor.execute("layout", [](auto& document) {
            document.layouts.push_back({"feature", 10.0, 20.0, false});
            return foundation::Result::success();
        }))
        return 3;
    if (application::save_editor_composition(root, editor) || !editor.dirty())
        return 4;
    const auto reopened = application::project::load_pipeline_document(document_path);
    if (!reopened) {
        std::cerr << "reopen failed: " << reopened.result.message << "\n";
        return 5;
    }
    if (reopened.value.layouts.size() != 0) {
        std::cerr << "unexpected layout count\n";
        return 5;
    }

    const auto legacy_path = root / "legacy.pipeline";
    std::ofstream(legacy_path) << "AEG_PIPELINE_EDITS 3\n"
                               << "EDGE \"Game / Renderer\" 101 201 102 202 1\n"
                               << "LAYOUT \"Game / Renderer\" 101 201 80 120\n"
                               << "BLEND \"Game / Renderer\" 101 201 1\n"
                               << "EDGE \"Game / Renderer\" 999 999 102 202 0\n";
    const std::vector<application::LegacyIdentityMapping> mappings = {{"Game / Renderer", 101, 201, "pass", 11},
                                                                      {"Game / Renderer", 102, 202, "feature", 22}};
    const auto migration =
        application::import_legacy_pipeline_edits(legacy_path, fixture.document, mappings, fixture.catalog);
    if (!migration || migration.value.original_bytes.empty() || migration.value.unresolved.size() != 1)
        return 6;
    if (migration.value.document.layouts.size() != 1 || migration.value.document.extensions.size() != 1)
        return 7;
    const auto report = root / "migration.report";
    if (!application::save_legacy_migration_report(report, migration.value) || !std::filesystem::exists(report))
        return 8;
    std::filesystem::remove_all(root, error);
    std::cout << "Editor composition storage smoke passed\n";
    return 0;
}
