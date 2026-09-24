/**
 * @file editor_asset_database_smoke.cpp
 * @brief Verifies Asset Browser scanning, stable IDs, import failures, and manifest persistence.
 */
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

#include "editor_asset_database.hpp"

namespace {
bool require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}
}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "aeg_editor_asset_database_smoke";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "Assets" / "Materials", error);
    if (!require(!error, "asset database smoke could not create temporary project")) {
        return 1;
    }
    {
        std::ofstream(root / "Assets" / "hero.obj") << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
        std::ofstream(root / "Assets" / "broken.obj") << "v 0 0 0\n";
        std::ofstream(root / "Assets" / "hero.png") << "not-a-real-png-but-a-stable-import-record\n";
        std::ofstream(root / "Assets" / "Materials" / "hero.aegmat") << "texture=Assets/hero.png\n";
        std::ofstream(root / "Assets" / "notes.txt") << "not an imported asset\n";
    }

    aeg::application::EditorAssetDatabase database;
    if (!require(static_cast<bool>(database.initialize(root)), "asset database smoke initialization failed")) {
        return 2;
    }
    const auto find_path = [&database](const char* path) {
        for (const auto& asset : database.assets()) {
            if (asset.relative_path == path) {
                return &asset;
            }
        }
        return static_cast<const aeg::application::EditorAssetRecord*>(nullptr);
    };
    const auto* hero = find_path("Assets/hero.obj");
    const auto* broken = find_path("Assets/broken.obj");
    const auto* notes = find_path("Assets/notes.txt");
    if (!require(hero != nullptr && hero->kind == aeg::application::EditorAssetKind::kMesh &&
                     hero->status == aeg::application::EditorAssetStatus::kImported,
                 "asset database smoke did not import valid OBJ") ||
        !require(broken != nullptr && broken->status == aeg::application::EditorAssetStatus::kFailed,
                 "asset database smoke did not preserve failed import") ||
        !require(notes != nullptr && notes->status == aeg::application::EditorAssetStatus::kUnsupported,
                 "asset database smoke did not preserve unsupported import")) {
        return 3;
    }
    const auto* material = find_path("Assets/Materials/hero.aegmat");
    const auto* texture = find_path("Assets/hero.png");
    if (!require(material != nullptr && texture != nullptr && material->dependencies.size() == 1 &&
                     material->dependencies.front() == texture->id,
                 "asset database smoke did not index material dependencies")) {
        return 3;
    }
    const uint64_t hero_id = hero->id;
    const uint64_t texture_id = texture->id;
    const auto dependent_rename = database.rename(texture_id, "renamed_texture");
    if (!require(static_cast<bool>(dependent_rename), "asset database smoke dependent rename failed")) {
        std::cerr << dependent_rename.message << '\n';
        return 4;
    }
    std::ifstream material_source(root / "Assets" / "Materials" / "hero.aegmat");
    const std::string material_text((std::istreambuf_iterator<char>(material_source)),
                                    std::istreambuf_iterator<char>());
    if (!require(material_text.find("Assets/renamed_texture.png") != std::string::npos,
                 "asset database smoke did not rewrite dependent path")) {
        return 4;
    }
    std::filesystem::rename(root / "Assets" / "hero.obj", root / "Assets" / "renamed.obj", error);
    if (!require(!error && database.refresh(), "asset database smoke rename refresh failed")) {
        return 5;
    }
    const auto* renamed = find_path("Assets/renamed.obj");
    if (!require(renamed != nullptr && renamed->id == hero_id,
                 "asset database smoke did not preserve ID across rename")) {
        return 6;
    }
    if (!require(static_cast<bool>(database.remove(hero_id)), "asset database smoke delete failed")) {
        return 7;
    }
    const auto* missing = database.find(hero_id);
    if (!require(missing != nullptr && missing->status == aeg::application::EditorAssetStatus::kMissing,
                 "asset database smoke did not retain missing record")) {
        return 8;
    }

    aeg::application::EditorAssetDatabase reopened;
    if (!require(static_cast<bool>(reopened.initialize(root)), "asset database smoke manifest reload failed") ||
        !require(reopened.find(hero_id) != nullptr, "asset database smoke lost stable ID after reload")) {
        return 9;
    }

    const auto external_root = std::filesystem::temp_directory_path() / "aeg_editor_external_import_smoke";
    std::filesystem::remove_all(external_root, error);
    std::filesystem::create_directories(external_root / "Assets", error);
    if (!require(!error, "asset database smoke could not create external import project")) {
        std::filesystem::remove_all(root, error);
        return 10;
    }
    const auto external_source = external_root / "incoming.obj";
    std::ofstream(external_source) << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    const auto audio_source = external_root / "incoming.wav";
    std::ofstream(audio_source, std::ios::binary) << "RIFF";
    aeg::application::EditorAssetDatabase importing;
    if (!require(static_cast<bool>(importing.initialize(external_root)),
                 "asset database smoke external import initialization failed")) {
        std::filesystem::remove_all(root, error);
        std::filesystem::remove_all(external_root, error);
        return 11;
    }
    aeg::application::EditorAssetImportRequest import_request{};
    import_request.source = external_source;
    import_request.destination_directory = "Assets/Imported";
    if (!require(static_cast<bool>(importing.import_external(import_request)),
                 "asset database smoke external import failed") ||
        !require(std::filesystem::exists(external_root / "Assets" / "Imported" / "incoming.obj"),
                 "asset database smoke external import did not copy source")) {
        std::filesystem::remove_all(root, error);
        std::filesystem::remove_all(external_root, error);
        return 12;
    }
    import_request.source = audio_source;
    import_request.destination_directory = "Assets/ImportedAudio";
    if (!require(static_cast<bool>(importing.import_external(import_request)),
                 "asset database smoke audio import entry point failed") ||
        !require(std::filesystem::exists(external_root / "Assets" / "ImportedAudio" / "incoming.wav"),
                 "asset database smoke audio import did not copy source")) {
        std::filesystem::remove_all(root, error);
        std::filesystem::remove_all(external_root, error);
        return 13;
    }
    import_request.source = external_source;
    import_request.destination_directory = "Assets/Imported";
    if (!require(!importing.import_external(import_request),
                 "asset database smoke external import overwrote an existing destination")) {
        std::filesystem::remove_all(root, error);
        std::filesystem::remove_all(external_root, error);
        return 14;
    }
    import_request.destination_directory = "Assets/../Outside";
    if (!require(!importing.import_external(import_request),
                 "asset database smoke external import escaped Assets directory")) {
        std::filesystem::remove_all(root, error);
        std::filesystem::remove_all(external_root, error);
        return 15;
    }
    import_request.destination_directory = "Assets/ImportedAgain";
    import_request.options.mesh_scale = 0.0f;
    if (!require(!importing.import_external(import_request),
                 "asset database smoke accepted an invalid import scale")) {
        std::filesystem::remove_all(root, error);
        std::filesystem::remove_all(external_root, error);
        return 16;
    }
    std::filesystem::remove_all(root, error);
    std::filesystem::remove_all(external_root, error);
    std::cout << "Editor asset database smoke passed\n";
    return 0;
}
