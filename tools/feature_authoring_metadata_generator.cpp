/** Regenerates checked-in authoring metadata from its actual source definitions. */
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

#include "module_bundle.hpp"
#include "feature_graph_io.hpp"
#include "../DemoProject/Source/GameModule.hpp"
#include "../Application/Project/templates/game/Source/GameModule.cpp"

namespace {
bool write(const std::filesystem::path& path, std::string_view text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.flush();
    return static_cast<bool>(output);
}
}

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        std::cerr << "usage: aeg_feature_authoring_metadata_generator template|demo metadata_path [demo_graph_path]\n";
        return 2;
    }
    const std::string_view mode{argv[1]};
    if ((mode != "template" && mode != "demo") || (argc == 4 && mode != "demo")) return 2;
    const auto definition = mode == "demo" ? demo::make_module() : aeg_define_GameModule();
    if (!definition.status || definition.feature_definitions.empty()) return 3;
    aeg::module::ModuleMetadata metadata{};
    metadata.module = definition.descriptor;
    // Re-export each real definition through the canonical validator and digest.
    for (const auto& feature : definition.feature_definitions) {
        const auto exported = metadata.feature_catalog.export_definition(feature);
        if (!exported) { std::cerr << exported.message << '\n'; return 4; }
    }
    if (mode == "template") {
        metadata.build_id = "dev";
        metadata.target = "windows-x64-vulkan";
        metadata.configuration = "Debug";
    }
    // Source declarations leave the bundle digest for the existing publication tool.
    const auto encoded = aeg::module::encode_module_metadata(metadata);
    if (!encoded) { std::cerr << encoded.result.message << '\n'; return 5; }
    if (argc == 4) {
        auto graph = aeg::application::authoring::load_feature_graph_document(argv[3]);
        if (!graph) { std::cerr << graph.result.message << '\n'; return 6; }
        graph.value.contract_digest = metadata.feature_catalog.contract_digest();
        const auto saved = aeg::application::authoring::save_feature_graph_document(argv[3], graph.value);
        if (!saved) { std::cerr << saved.message << '\n'; return 7; }
    }
    if (!write(argv[2], encoded.value)) return 8;
    std::cout << "Generated " << mode << " authoring metadata with schema " << metadata.schema_version
              << " and SDK " << metadata.sdk_contract_major << '\n';
    return 0;
}
