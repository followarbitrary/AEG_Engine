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
    fixture.document.present_connections.push_back("pass");
    // Stable renderer port/binding IDs use all 64 bits, including values above INT64_MAX.
    fixture.document.edges.push_back({.edge_id = "high-bit-resource-binding",
                                      .kind = pipeline::DocumentEdgeKind::kRenderResource,
                                      .source = {"scene_color", 0xffffffffffffffffull, 17},
                                      .destination = {"pass", 0x8000000000000001ull, 19},
                                      .binding_id = 0x8000000000000000ull});
    fixture.document.input_bindings.push_back({.node_id = "feature",
                                               .port_id = 1001,
                                               .slot_id = 17,
                                               .source = pipeline::InputSourceKind::kAsset,
                                               .asset = pipeline::AssetReference{"00000000-0000-0000-0000-000000000123"},
                                               .product_id = "albedo",
                                               .target_variant = "desktop",
                                               .content_revision = 8,
                                               .status = pipeline::InputBindingStatus::kBound});
    fixture.document.variadic_slots.push_back(
        {.node_id = "feature", .group_id = 700, .slot_id = 17, .order = 2, .parameters = {{42, uint64_t{9}}}});

    const auto root = std::filesystem::temp_directory_path() / "aeg_pipeline_document_io_smoke";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    const auto file = root / "CustomForward.aegpipe";
    if (!application::project::save_pipeline_document(file, fixture.document))
        return 1;
    if (!application::project::save_pipeline_document(file, fixture.document) ||
        !std::filesystem::exists(file.string() + ".bak"))
        return 1;
    const auto loaded = application::project::load_pipeline_document(file);
    if (!loaded ||
        pipeline::document_semantic_digest(loaded.value) != pipeline::document_semantic_digest(fixture.document))
        return 2;
    const auto high_bit_edge = std::find_if(loaded.value.edges.begin(), loaded.value.edges.end(),
                                           [](const auto& edge) { return edge.edge_id == "high-bit-resource-binding"; });
    if (loaded.value.resources.size() != 1 || loaded.value.extensions.size() != 1 ||
        loaded.value.present_connections != std::vector<std::string>{"pass"} ||
        loaded.value.input_bindings.size() != 1 || loaded.value.variadic_slots.size() != 1 ||
        high_bit_edge == loaded.value.edges.end() || high_bit_edge->source.slot_id != 17 ||
        high_bit_edge->destination.slot_id != 19 ||
        loaded.value.input_bindings.front().asset.uuid != "00000000-0000-0000-0000-000000000123" ||
        loaded.value.input_bindings.front().content_revision != 8 ||
        loaded.value.variadic_slots.front().parameters.at(42) != pipeline::ParameterValue{uint64_t{9}})
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

    auto layout_only = loaded.value;
    layout_only.layouts.push_back({"feature", 100.0, 200.0, false});
    if (pipeline::document_semantic_digest(layout_only) != pipeline::document_semantic_digest(loaded.value))
        return 7;
    auto semantic_change = loaded.value;
    semantic_change.input_bindings.front().content_revision++;
    if (pipeline::document_semantic_digest(semantic_change) == pipeline::document_semantic_digest(loaded.value))
        return 8;

    std::ofstream(file, std::ios::trunc) << "schema_version = 0\nasset_uuid = \"x\"\nname = \"bad\"\n";
    if (application::project::load_pipeline_document(file))
        return 6;
    std::filesystem::remove_all(root, error);
    std::cout << "Pipeline document IO smoke passed\n";
    return 0;
}
