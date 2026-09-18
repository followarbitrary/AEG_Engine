#include <iostream>

#include "pipeline_asset.hpp"
#include "pipeline_asset_encoder.hpp"

int main() {
    using namespace aeg;
    pipeline::PipelineDocument document;
    document.schema_version = 1;
    document.asset_uuid = "00000000-0000-0000-0000-000000000001";
    document.name = "Codec";
    document.module_requirements = {pipeline::ModuleId("Game.Module")};
    document.pipeline_nodes = {{.node_id = "pipeline",
                                .type = pipeline::PipelineId("Game.Pipeline"),
                                .version = {1, 0, 0},
                                .scope = pipeline::PipelineScope::kFrame,
                                .parameters = {{1, pipeline::ParameterValue{uint64_t{0xffffffffffffffffull}}}}}};
    document.layouts = {{"pipeline", 88.0, 42.0, true}};
    document.edges.push_back({"order", pipeline::DocumentEdgeKind::kOrdering, {"pipeline", 0}, {"pipeline", 0}, 0});
    const auto encoded = tooling::encode_pipeline_asset(document);
    if (!encoded)
        return 1;
    const auto decoded = asset::decode_pipeline_asset(encoded.value);
    if (!decoded || decoded.value.layouts.size() != 0 || decoded.value.pipeline_nodes.size() != 1)
        return 2;
    const auto reencoded = tooling::encode_pipeline_asset(decoded.value);
    if (!reencoded || encoded.value != reencoded.value)
        return 3;
    auto corrupt = encoded.value;
    corrupt[0] = static_cast<std::byte>('X');
    if (asset::decode_pipeline_asset(corrupt))
        return 4;
    corrupt = encoded.value;
    corrupt.resize(corrupt.size() - 1);
    if (asset::decode_pipeline_asset(corrupt))
        return 5;
    document.pipeline_nodes.push_back(document.pipeline_nodes.front());
    if (tooling::encode_pipeline_asset(document))
        return 6;
    std::cout << "Pipeline asset codec smoke passed\n";
    return 0;
}
