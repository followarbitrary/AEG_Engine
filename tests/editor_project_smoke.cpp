/**
 * @file editor_project_smoke.cpp
 * @brief Verifies explicit game project creation and import without engine-root discovery.
 */
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <system_error>

#include "editor_project.hpp"
#include "project_context.hpp"

namespace {
bool require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool removed_startup_fields_keep_project_and_source(aeg::application::EditorProjectManager& manager,
    const std::filesystem::path& root, const std::string& modern_source) {
    const auto active = manager.descriptor();
    for (const std::string field : {"startup_pipelines", "startup_frame_projections"}) {
        const auto fixture = root / field;
        std::filesystem::create_directories(fixture / "Assets/Scenes");
        const auto path = fixture / "Project.aegproj";
        auto payload = modern_source;
        const auto build = payload.find("[build]\n");
        if (!require(build != std::string::npos, "Removed-field fixture has no actual build table")) return false;
        payload.insert(build + 8, field + " = []\n");
        { std::ofstream output(path, std::ios::binary); output << payload; }
        const auto before = read_text(path);
        const auto result = manager.import_project(path);
        const auto label = "Removed startup field was accepted or lost its identity: " + field;
        if (!require(!result && result.code == aeg::foundation::ResultCode::kUnsupported &&
                result.message.find(field) != std::string::npos, label.c_str()) ||
            !require(manager.loaded() && manager.descriptor().root == active.root && manager.descriptor().name == active.name &&
                manager.descriptor().default_scene == active.default_scene, "Removed startup field replaced the active project") ||
            !require(read_text(path) == before, "Removed startup field import rewrote its source")) return false;
    }
    return true;
}

bool retired_project_formats_keep_current_source(aeg::application::EditorProjectManager& manager,
    const std::filesystem::path& root, const std::filesystem::path& recent_file, const std::string& modern_source) {
    struct Case { std::string name, filename; int header; bool explicit_file, canonical_present; };
    std::vector<Case> cases;
    for (const int header : {1, 2}) {
        const auto prefix = "header-" + std::to_string(header);
        cases.push_back({prefix + "-canonical-file", "Project.aegproj", header, true, false});
        cases.push_back({prefix + "-canonical-directory", "Project.aegproj", header, false, false});
        cases.push_back({prefix + "-legacy-file", "project.aegproject", header, true, false});
        cases.push_back({prefix + "-legacy-directory", "project.aegproject", header, false, false});
        cases.push_back({prefix + "-legacy-explicit-coexist", "project.aegproject", header, true, true});
        cases.push_back({prefix + "-vendor-disguise", "Vendor.aegproj", header, true, false});
    }
    cases.push_back({"modern-bytes-legacy-extension", "project.aegproject", 0, true, false});
    cases.push_back({"modern-bytes-legacy-explicit-coexist", "project.aegproject", 0, true, true});
    cases.push_back({"modern-bytes-casefold-explicit-coexist", "Vendor.AEGPROJECT", 0, true, true});
    const auto active = manager.descriptor();
    const auto active_file = manager.project_file();
    const auto active_source = read_text(active_file);
    const auto active_recent_scene = read_text(manager.recent_state_file());
    bool all_preserved = true;
    for (const auto& test : cases) {
        const auto fixture = root / test.name;
        std::filesystem::create_directories(fixture / "Assets/Scenes");
        std::filesystem::create_directories(fixture / "Scenes");
        const auto source = fixture / test.filename;
        const auto canonical = fixture / "Project.aegproj";
        const auto payload = test.header == 0 ? modern_source : "AEG_PROJECT\t" + std::to_string(test.header) +
            "\nRetired project\nassets=Assets\nscenes=Scenes\ndefault_scene=Scenes/Main.aegscene\n";
        { std::ofstream output(source, std::ios::binary); output << payload; }
        if (test.canonical_present) { std::ofstream output(canonical, std::ios::binary); output << modern_source; }
        const auto original_source = read_text(source);
        const bool canonical_existed = std::filesystem::exists(canonical);
        const auto original_canonical = canonical_existed ? read_text(canonical) : std::string{};
        const auto original_recents = read_text(recent_file);
        auto candidate = manager;
        const auto result = candidate.import_project(test.explicit_file ? source : fixture);
        const bool rejected = !result && result.code == aeg::foundation::ResultCode::kUnsupported;
        const bool preserved = candidate.loaded() && candidate.descriptor().root == active.root &&
            candidate.descriptor().name == active.name && candidate.descriptor().default_scene == active.default_scene &&
            candidate.project_file() == active_file && read_text(active_file) == active_source &&
            read_text(candidate.recent_state_file()) == active_recent_scene && read_text(recent_file) == original_recents &&
            read_text(source) == original_source && std::filesystem::exists(canonical) == canonical_existed &&
            (!canonical_existed || read_text(canonical) == original_canonical);
        const auto label = "Retired project accepted or changed active/source/recent/canonical: " + test.name;
        if (!require(rejected && preserved, label.c_str())) all_preserved = false;
    }
    return all_preserved;
}

bool modern_selected_file_keeps_its_identity(aeg::application::EditorProjectManager& manager,
    const std::filesystem::path& root, const std::string& modern_source) {
    std::filesystem::create_directories(root / "Assets/Scenes");
    const auto canonical = root / "Project.aegproj";
    const auto selected = root / "Vendor.aegproj";
    const auto canonical_source = read_text(manager.project_file());
    { std::ofstream output(canonical, std::ios::binary); output << canonical_source; }
    { std::ofstream output(selected, std::ios::binary); output << modern_source; }
    auto opened = manager;
    if (!require(static_cast<bool>(opened.import_project(selected)), "Modern selected project import failed") ||
        !require(opened.project_file() == std::filesystem::weakly_canonical(selected),
                 "Modern explicit project selection was replaced by a coexisting canonical file") ||
        !require(read_text(canonical) == canonical_source && read_text(selected) == modern_source,
                 "Modern import rewrote the selected or coexisting project source")) return false;
    const auto loaded = aeg::application::project::ProjectFileCodec{}.load(selected);
    if (!require(static_cast<bool>(loaded), "Modern selected file cannot be loaded")) return false;
    auto updated = loaded.value.config;
    updated.name = "Updated Vendor Game";
    if (!require(static_cast<bool>(opened.update_project_settings(updated)), "Modern selected settings update failed") ||
        !require(read_text(canonical) == canonical_source,
                 "Modern selected settings overwrote the canonical project instead of their own source")) return false;
    const auto updated_file = aeg::application::project::ProjectFileCodec{}.load(selected);
    if (!require(updated_file && updated_file.value.config.name == updated.name && opened.descriptor().name == updated.name,
                 "Modern selected settings did not persist in the actual source")) return false;
    const auto before_failed_save = read_text(selected);
    auto retired_update = updated;
    retired_update.format_version = 1;
    const auto failed_save = opened.update_project_settings(retired_update);
    return require(!failed_save && failed_save.code == aeg::foundation::ResultCode::kUnsupported &&
        opened.descriptor().name == updated.name && opened.project_file() == std::filesystem::weakly_canonical(selected) &&
        read_text(selected) == before_failed_save && read_text(canonical) == canonical_source,
        "Rejected retired settings replaced a modern selected file or descriptor");
}

bool recent_project_reopens_selected_file(aeg::application::EditorProjectManager& manager,
    const std::filesystem::path& root, const std::string& modern_source) {
    std::filesystem::create_directories(root / "Assets/Scenes");
    const auto canonical = root / "Project.aegproj";
    const auto selected = root / "Vendor.aegproj";
    const auto canonical_source = read_text(manager.project_file());
    { std::ofstream output(canonical, std::ios::binary); output << canonical_source; }
    { std::ofstream output(selected, std::ios::binary); output << modern_source; }
    auto opened = manager;
    if (!require(static_cast<bool>(opened.import_project(root)), "Canonical directory Recent import failed") ||
        !require(static_cast<bool>(opened.import_project(selected)), "Selected-file Recent import failed")) return false;
    const auto expected_name = opened.descriptor().name;
    if (!require(static_cast<bool>(opened.close_project()), "Recent project close failed")) return false;
    const auto recents = opened.recent_projects();
    if (!require(static_cast<bool>(recents), "Recent project list could not be read")) return false;
    const auto recent = std::find_if(recents.value.begin(), recents.value.end(), [&](const auto& entry) {
        return entry.project_file == std::filesystem::weakly_canonical(selected) && entry.name == expected_name;
    });
    if (!require(recent != recents.value.end(), "Selected project disappeared from Recent")) return false;
    const auto canonical_recent = std::find_if(recents.value.begin(), recents.value.end(), [&](const auto& entry) {
        return entry.project_file == std::filesystem::weakly_canonical(canonical);
    });
    if (!require(canonical_recent != recents.value.end(), "Coexisting canonical and Vendor Recent identities collapsed")) return false;
    // Mirrors Main's exact modern source-file reopening action.
    if (!require(static_cast<bool>(opened.import_project(recent->project_file)), "Recent reopen failed")) return false;
    if (!require(opened.project_file() == std::filesystem::weakly_canonical(selected) && opened.descriptor().name == expected_name,
                 "Recent reopened coexisting canonical A instead of explicitly selected Vendor B")) return false;
    return require(read_text(canonical) == canonical_source && read_text(selected) == modern_source,
        "Recent roundtrip rewrote canonical or selected source bytes");
}

bool recent_unicode_paths_roundtrip(aeg::application::EditorProjectManager& manager,
    const std::filesystem::path& root, const std::string& modern_source) {
    try {
        const auto unicode_root = root / std::filesystem::path{u8"Project-\U0001F680"};
        std::filesystem::create_directories(unicode_root / "Assets/Scenes");
        const auto selected = unicode_root / std::filesystem::path{u8"Vendor-\U0001F680.aegproj"};
        { std::ofstream output(selected, std::ios::binary); output << modern_source; }
        const auto scene = unicode_root / std::filesystem::path{u8"Assets/Scenes/\U0001F680.aegscene"};
        std::filesystem::copy_file(manager.default_scene_file(), scene);
        auto opened = manager;
        const auto imported = opened.import_project(selected);
        if (!imported) std::cerr << "Unicode project import diagnostic: " << imported.message << '\n';
        if (!require(static_cast<bool>(imported), "Unicode project import failed") ||
            !require(static_cast<bool>(opened.remember_scene(scene)), "Unicode recent Scene save failed") ||
            !require(opened.recent_scene() == std::filesystem::weakly_canonical(scene),
                     "Unicode recent Scene path lost its native identity")) return false;
        const auto expected_name = opened.descriptor().name;
        if (!require(static_cast<bool>(opened.close_project()), "Unicode project close failed")) return false;
        const auto recents = opened.recent_projects();
        if (!require(static_cast<bool>(recents), "Unicode Recent records cannot be read")) return false;
        const auto recent = std::find_if(recents.value.begin(), recents.value.end(), [&](const auto& entry) {
            return entry.root == std::filesystem::weakly_canonical(unicode_root);
        });
        if (!require(recent != recents.value.end(), "Unicode project root was damaged in Recent")) return false;
        if (!require(recent->project_file == std::filesystem::weakly_canonical(selected),
                     "Unicode selected source was damaged in Recent") ||
            !require(static_cast<bool>(opened.import_project(recent->project_file)), "Unicode Recent reopen failed")) return false;
        return require(opened.project_file() == std::filesystem::weakly_canonical(selected) &&
            opened.descriptor().name == expected_name && opened.recent_scene() == std::filesystem::weakly_canonical(scene) &&
            read_text(selected) == modern_source, "Unicode Recent file/Scene identity did not survive close and reopen");
    } catch (const std::exception& error) {
        std::cerr << "Unicode Recent roundtrip threw: " << error.what() << '\n';
        return false;
    }
}

bool ambiguous_ui_recent_records_are_not_guessed(aeg::application::EditorProjectManager& manager,
    const std::filesystem::path& recent_file) {
    const auto original = read_text(recent_file);
    const auto root_text = manager.descriptor().root.generic_u8string();
    const std::string encoded_root{root_text.begin(), root_text.end()};
    { std::ofstream output(recent_file, std::ios::binary | std::ios::app);
      output << encoded_root << "\tAmbiguous directory record\n"; }
    const auto observed = manager.recent_projects();
    { std::ofstream output(recent_file, std::ios::binary | std::ios::trunc); output << original; }
    return require(observed && std::none_of(observed.value.begin(), observed.value.end(), [](const auto& row) {
        return row.name == "Ambiguous directory record";
    }), "Two-column UI Recent record was guessed into an exact modern source identity");
}

bool invalid_utf8_recent_rows_do_not_break_import(aeg::application::EditorProjectManager& manager,
    const std::filesystem::path& root, const std::filesystem::path& recent_file, const std::string& modern_source) {
    std::filesystem::create_directories(root / "Assets/Scenes");
    const auto valid_project = root / "Project.aegproj";
    { std::ofstream output(valid_project, std::ios::binary); output << modern_source; }
    const auto original = read_text(recent_file);
    const auto expected = manager.recent_projects();
    if (!require(static_cast<bool>(expected), "Valid Recent baseline cannot be read")) return false;
    const auto root_u8 = manager.descriptor().root.generic_u8string();
    const auto source_u8 = manager.project_file().generic_u8string();
    const std::string encoded_root{root_u8.begin(), root_u8.end()}, encoded_source{source_u8.begin(), source_u8.end()};
    const std::string invalid{"\xF0\x28\x8C\x28"};
    bool all_preserved = true;
    for (const bool bad_root : {true, false}) {
        const auto label = bad_root ? "Bad UTF8 root" : "Bad UTF8 source";
        { std::ofstream output(recent_file, std::ios::binary | std::ios::trunc);
          output << original << (bad_root ? invalid : encoded_root) << '\t' << label << '\t'
                 << (bad_root ? encoded_source : invalid) << '\n'; }
        try {
            const auto observed = manager.recent_projects();
            const bool valid_rows_remain = observed && std::all_of(expected.value.begin(), expected.value.end(), [&](const auto& row) {
                return std::any_of(observed.value.begin(), observed.value.end(), [&](const auto& current) {
                    return row.project_file == current.project_file && row.root == current.root && row.name == current.name;
                });
            });
            const bool invalid_row_skipped = observed && std::none_of(observed.value.begin(), observed.value.end(), [&](const auto& row) {
                return row.name == label;
            });
            if (!require(valid_rows_remain && invalid_row_skipped, label)) all_preserved = false;
            auto opened = manager;
            if (!require(static_cast<bool>(opened.import_project(valid_project)) &&
                    opened.project_file() == std::filesystem::weakly_canonical(valid_project) &&
                    read_text(valid_project) == modern_source,
                    "A malformed UI Recent row prevented a valid project import")) all_preserved = false;
        } catch (const std::system_error& error) {
            std::cerr << label << " system_error across the Result boundary: " << error.code().category().name()
                      << '/' << error.code().value() << ' ' << error.what() << '\n';
            all_preserved = false;
        } catch (const std::exception& error) {
            std::cerr << label << " threw across the Result boundary: " << error.what() << '\n';
            all_preserved = false;
        }
        { std::ofstream output(recent_file, std::ios::binary | std::ios::trunc); output << original; }
    }
    return all_preserved;
}

bool invalid_utf8_recent_scene_preserves_source(aeg::application::EditorProjectManager& manager) {
    const auto active = manager.descriptor();
    const auto active_file = manager.project_file();
    const auto active_source = read_text(active_file);
    const auto cache = manager.recent_state_file();
    const auto original_cache = read_text(cache);
    const std::string invalid{"\xF0\x28\x8C\x28\n"};
    { std::ofstream output(cache, std::ios::binary | std::ios::trunc); output << invalid; }
    bool no_scene = false;
    try {
        no_scene = require(manager.recent_scene().empty(),
            "Malformed UTF8 remembered Scene was accepted instead of an empty optional Scene");
    } catch (const std::system_error& error) {
        std::cerr << "Bad UTF8 Scene system_error across the optional path boundary: "
                  << error.code().category().name() << '/' << error.code().value() << ' ' << error.what() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "Bad UTF8 Scene threw across the optional path boundary: " << error.what() << '\n';
    }
    const bool preserved = require(manager.loaded() && manager.descriptor().root == active.root &&
        manager.descriptor().name == active.name && manager.descriptor().assets_directory == active.assets_directory &&
        manager.descriptor().scenes_directory == active.scenes_directory &&
        manager.descriptor().default_scene == active.default_scene && manager.project_file() == active_file &&
        read_text(active_file) == active_source && read_text(cache) == invalid,
        "Malformed UTF8 remembered Scene rewrote its cache, project source, or active identity");
    { std::ofstream output(cache, std::ios::binary | std::ios::trunc); output << original_cache; }
    return no_scene && preserved;
}

bool recent_unicode_scene_roundtrip(aeg::application::EditorProjectManager& manager,
    const std::filesystem::path& root, const std::string& modern_source) {
    try {
        std::filesystem::create_directories(root / "Assets/Scenes");
        const auto project = root / "Project.aegproj";
        { std::ofstream output(project, std::ios::binary); output << modern_source; }
        const auto scene = root / std::filesystem::path{u8"Assets/Scenes/\U0001F680.aegscene"};
        std::filesystem::copy_file(manager.default_scene_file(), scene);
        auto opened = manager;
        if (!require(static_cast<bool>(opened.import_project(project)), "ASCII project import for Unicode Scene failed") ||
            !require(static_cast<bool>(opened.remember_scene(scene)), "Unicode Scene persistence failed")) return false;
        return require(opened.recent_scene() == std::filesystem::weakly_canonical(scene),
            "Native Unicode Scene identity was damaged by the recent-state byte roundtrip");
    } catch (const std::exception& error) {
        std::cerr << "Unicode recent Scene roundtrip threw: " << error.what() << '\n';
        return false;
    }
}
}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "aeg_editor_project_smoke";
    std::error_code error;
    std::filesystem::remove_all(root, error);

    const auto user_settings = root / "UserSettings";
#if defined(_WIN32)
    _putenv_s("APPDATA", user_settings.string().c_str());
#else
    setenv("APPDATA", user_settings.string().c_str(), 1);
#endif
    aeg::application::EditorProjectManager manager;
    if (!require(static_cast<bool>(manager.new_project(root, "Smoke Game")),
                 "editor project smoke project creation failed") ||
        !require(manager.loaded(), "editor project smoke project is not loaded") ||
        !require(manager.descriptor().name == "Smoke Game", "editor project smoke project name is invalid") ||
        !require(std::filesystem::is_directory(root / "Assets"), "editor project smoke Assets directory is missing") ||
        !require(std::filesystem::is_directory(root / "Assets" / "Scenes"),
                 "editor project smoke Scenes directory is missing") ||
        !require(std::filesystem::is_directory(root / "Cache"), "editor project smoke Cache directory is missing") ||
        !require(std::filesystem::is_directory(root / "Saved"), "editor project smoke Saved directory is missing") ||
        !require(std::filesystem::is_directory(root / "Binaries"),
                 "editor project smoke Binaries directory is missing") ||
        !require(std::filesystem::exists(root / "CMakeLists.txt"), "editor project smoke CMakeLists.txt is missing") ||
        !require(std::filesystem::exists(root / "Source" / "GameModule.cpp") &&
                     std::filesystem::exists(root / "Source" / "GameModule.metadata.json"),
                 "editor project smoke game module sources are missing") ||
        !require(std::filesystem::exists(root / "Assets" / "Features" / "Default.aegfeature"),
                 "editor project smoke default FeatureGraph asset is missing") ||
        !require(manager.assets_directory().filename() == "Assets",
                 "editor project smoke Assets path is not project-relative") ||
        !require(manager.scenes_directory().filename() == "Scenes",
                 "editor project smoke Scenes path is not project-relative") ||
        !require(manager.default_scene_file().filename() == "Main.aegscene" &&
                     manager.default_scene_file().parent_path().filename() == "Scenes" &&
                     manager.default_scene_file().parent_path().parent_path().filename() == "Assets",
                 "editor project smoke default scene path is incorrect") ||
        !require(std::filesystem::exists(root / "Project.aegproj"), "editor project smoke project file is missing")) {
        std::filesystem::remove_all(root, error);
        return 1;
    }

    std::ifstream project_contents(root / "Project.aegproj");
    const std::string project_text((std::istreambuf_iterator<char>(project_contents)),
                                   std::istreambuf_iterator<char>());
    if (!require(project_text.starts_with("format_version = 2"), "editor project smoke did not write TOML") ||
        !require(project_text.find("[paths]") != std::string::npos &&
                     project_text.find("cache = \"Cache\"") != std::string::npos,
                 "editor project smoke TOML paths are incomplete")) {
        std::filesystem::remove_all(root, error);
        return 6;
    }
    project_contents.close();

    std::ofstream(root / "Assets" / "example.obj") << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    aeg::application::EditorProjectManager imported;
    if (!require(static_cast<bool>(imported.import_project(root / "Project.aegproj")),
                 "editor project smoke project-file import failed") ||
        !require(imported.descriptor().root == manager.descriptor().root,
                 "editor project smoke imported root differs") ||
        !require(static_cast<bool>(imported.close_project()), "editor project smoke project close failed") ||
        !require(!imported.loaded() && imported.project_file().empty(),
                 "editor project smoke close left a project or source selected")) {
        std::filesystem::remove_all(root, error);
        return 2;
    }

    std::filesystem::remove(root / "Project.aegproj", error);
    if (!require(!imported.import_project(root), "editor project smoke accepted a project without project file")) {
        std::filesystem::remove_all(root, error);
        return 3;
    }

    const auto options_root = std::filesystem::temp_directory_path() / "aeg_editor_project_options_smoke";
    std::filesystem::remove_all(options_root, error);
    aeg::application::ProjectCreateOptions options{};
    options.root = options_root;
    options.name = "Options Game";
    options.engine_sdk = "2.0";
    options.startup_feature_graph = "Assets/Features/Default.aegfeature";
    aeg::application::EditorProjectManager configured;
    if (!require(static_cast<bool>(configured.new_project(options)),
                 "editor project smoke configured project creation failed")) {
        std::filesystem::remove_all(root, error);
        std::filesystem::remove_all(options_root, error);
        return 8;
    }
    const auto configured_file = aeg::application::project::ProjectFileCodec{}.load(
        options_root / "Project.aegproj");
    if (!require(static_cast<bool>(configured_file), "editor project smoke configured file could not be loaded") ||
        !require(configured_file.value.config.engine.sdk_requirement == "2.0",
                 "editor project smoke did not persist configured SDK") ||
        !require(!configured_file.value.config.startup_feature_graphs.empty() &&
                     configured_file.value.config.startup_feature_graphs.front() == options.startup_feature_graph,
                 "editor project smoke did not persist configured FeatureGraph")) {
        std::filesystem::remove_all(root, error);
        std::filesystem::remove_all(options_root, error);
        return 9;
    }
    auto updated_config = configured_file.value.config;
    updated_config.name = "Updated Options Game";
    if (!require(static_cast<bool>(configured.update_project_settings(updated_config)),
                 "editor project smoke settings update failed")) {
        std::filesystem::remove_all(root, error);
        std::filesystem::remove_all(options_root, error);
        return 10;
    }
    const auto updated_file = aeg::application::project::ProjectFileCodec{}.load(
        options_root / "Project.aegproj");
    if (!require(static_cast<bool>(updated_file) && updated_file.value.config.name == "Updated Options Game",
                 "editor project smoke settings update was not persisted")) {
        std::filesystem::remove_all(root, error);
        std::filesystem::remove_all(options_root, error);
        return 11;
    }
    if (!removed_startup_fields_keep_project_and_source(configured, root / "removed-fields", project_text)) return 12;

    if (!require(static_cast<bool>(configured.remember_scene(options_root / "Assets/Scenes/Example.aegscene")),
                 "editor project smoke modern recent scene write failed") ||
        !require(configured.recent_scene().filename() == "Example.aegscene" &&
                     configured.recent_scene().parent_path().filename() == "Scenes",
                 "editor project smoke modern recent scene was not resolved") ||
        !require(!configured.remember_scene(std::filesystem::temp_directory_path() / "outside.aegscene"),
                 "editor project smoke accepted a scene outside the project")) return 4;
    if (!retired_project_formats_keep_current_source(configured, root / "retired-projects",
            user_settings / "AEG/recent_projects.tsv", project_text)) return 13;
    if (!modern_selected_file_keeps_its_identity(configured, root / "modern-selected", project_text)) return 14;
    const bool recent_identity = recent_project_reopens_selected_file(configured, root / "recent-selected", project_text);
    const bool recent_unicode_scene = recent_unicode_scene_roundtrip(configured, root / "recent-scene", project_text);
    const bool recent_unicode = recent_unicode_paths_roundtrip(configured, root / "recent-unicode", project_text);
    if (!recent_identity || !recent_unicode_scene || !recent_unicode) return 15;
    if (!ambiguous_ui_recent_records_are_not_guessed(configured, user_settings / "AEG/recent_projects.tsv")) return 16;
    const bool invalid_recent_rows = invalid_utf8_recent_rows_do_not_break_import(configured, root / "recent-invalid-utf8",
        user_settings / "AEG/recent_projects.tsv", project_text);
    const bool invalid_recent_scene = invalid_utf8_recent_scene_preserves_source(configured);
    if (!invalid_recent_rows || !invalid_recent_scene) return 17;

    std::filesystem::remove_all(root, error);
    std::cout << "Editor project smoke passed\n";
    return 0;
}
