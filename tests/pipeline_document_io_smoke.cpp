#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "editor_composition_test_support.hpp"
#include "pipeline_authoring.hpp"
#include "pipeline_document_io.hpp"

int main() {
    using namespace aeg;
    tests::EditorCompositionFixture fixture;
    fixture.document.resources.push_back(
        {.node_id = "scene_color",
         .resource_id = pipeline::RenderResourceId("SceneColor"),
         .type = pipeline::ArtifactTypeId("Texture2D"),
         .version = 1,
         .parameters = {{9001, pipeline::ParameterValue{uint64_t{0xffffffffffffffffull}}}},
         .owner_scope = pipeline::PipelineScope::kFrame,
         .visible_to = pipeline::to_mask(pipeline::PipelineScope::kFrame),
         .storage = pipeline::ArtifactStorage::kGpuLogicalResource,
         .lifetime = pipeline::ArtifactLifetime::kFrame,
         .ownership = pipeline::OwnershipPolicy::kGraphOwned,
         .resource_constraints = {.resource_class = pipeline::ResourceClass::kTexture,
                                  .formats = {"rgba16f"},
                                  .size_rule = "swapchain",
                                  .sample_count = 1}});
    fixture.document.extensions.push_back({"feature", pipeline::OpaqueAuthoringEncoding::kJson, "{\"custom\":true}"});

    const auto root = std::filesystem::temp_directory_path() / "aeg_pipeline_document_io_smoke";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    const auto file = root / "CustomForward.aegpipe";
    if (!application::project::save_pipeline_document(file, fixture.document))
        return 1;
    const auto loaded = application::project::load_pipeline_document(file);
    if (!loaded ||
        pipeline::document_semantic_digest(loaded.value) != pipeline::document_semantic_digest(fixture.document))
        return 2;
    if (loaded.value.resources.size() != 1 || loaded.value.extensions.size() != 1)
        return 3;
    if (!std::holds_alternative<uint64_t>(loaded.value.resources.front().parameters.at(9001)) ||
        std::get<uint64_t>(loaded.value.resources.front().parameters.at(9001)) != 0xffffffffffffffffull)
        return 4;

    {
        std::ifstream source(file);
        std::string text((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
        const std::string anchor = "name = \"Editor Composition\"\n";
        const auto position = text.find(anchor);
        if (position == std::string::npos)
            return 5;
        text.insert(position + anchor.size(), "unknown_extension = { answer = 42 }\n");
        std::ofstream(file, std::ios::trunc) << text;
    }
    const auto unknown = application::project::load_pipeline_document(file);
    if (!unknown ||
        std::none_of(unknown.value.extensions.begin(), unknown.value.extensions.end(),
                     [](const auto& extension) { return extension.owner_node == "@root:unknown_extension"; }))
        return 5;

    std::ofstream(file, std::ios::trunc) << "schema_version = 0\nasset_uuid = \"x\"\nname = \"bad\"\n";
    if (application::project::load_pipeline_document(file))
        return 6;
    std::filesystem::remove_all(root, error);
    std::cout << "Pipeline document IO smoke passed\n";
    return 0;
}
