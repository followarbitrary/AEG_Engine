#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "pipeline_document_io.hpp"
#include "project_context.hpp"
#include "project_scaffold.hpp"
#include "scene_asset.hpp"

int main() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "aeg scaffold 示例 project";
    std::error_code error;
    fs::remove_all(root, error);
    const auto result = aeg::application::project::create_project({root, "Outline 示例", "GameModule"});
    if (!result || result.value.filename() != "Project.aegproj")
        return 1;
    const auto loaded = aeg::application::project::ProjectFileCodec{}.load(result.value);
    if (!loaded || loaded.value.config.name != "Outline 示例" || loaded.value.config.module.target != "GameModule" ||
        loaded.value.config.startup_pipelines.size() != 1U || loaded.value.config.project_uuid.size() != 36U)
        return 2;
    for (const auto& path :
         {fs::path("Assets/Pipelines/Default.aegpipe"), fs::path("Source/GameModule.cpp"),
          fs::path("Source/GameModule.metadata.json"), fs::path("Source/Render/Features/CustomOutlineFeature.cpp"),
          fs::path("Source/Render/Passes/VolumetricFogPass.cpp")}) {
        if (!fs::exists(root / path))
            return 3;
    }
    for (const auto& path :
         {fs::path("Assets/Pipelines/Default.aegpipe.meta"), fs::path("Assets/Scenes/Main.aegscene.meta")}) {
        if (!fs::exists(root / path))
            return 10;
    }
    const auto pipeline = aeg::application::project::load_pipeline_document(root / "Assets/Pipelines/Default.aegpipe");
    if (!pipeline || pipeline.value.pipeline_nodes.size() != 1 || pipeline.value.feature_nodes.size() != 1 ||
        pipeline.value.pass_nodes.size() != 1 || pipeline.value.edges.size() != 1)
        return 8;
    if (!fs::is_directory(root / "Build") || !fs::is_directory(root / "Cache") ||
        !fs::exists(root / "Assets" / "Scenes"))
        return 4;
    if (!fs::exists(root / ".gitignore"))
        return 9;
    std::ifstream scene_stream(root / "Assets" / "Scenes" / "Main.aegscene", std::ios::binary);
    const std::vector<uint8_t> scene_bytes((std::istreambuf_iterator<char>(scene_stream)), {});
    aeg::serialization::BinaryReader scene_reader(scene_bytes);
    const auto scene = aeg::scene::SceneSerializer::deserialize(scene_reader);
    if (!scene || scene.value.entities().size() != 0U)
        return 7;
    std::ofstream(root / "sentinel.txt") << "keep";
    if (aeg::application::project::create_project({root, "Should Fail", "GameModule"}))
        return 5;
    std::ifstream sentinel(root / "sentinel.txt");
    if (!sentinel || sentinel.peek() == std::ifstream::traits_type::eof())
        return 6;
    fs::remove_all(root, error);
    std::cout << "Project scaffold smoke passed\n";
    return 0;
}
