#include <iostream>

#include "pipeline_configuration.hpp"

int main() {
    using namespace aeg::pipeline;
    PipelineScopeHost host(PipelineScope::kFrame, PipelineInstanceId("View.Left"));
    auto config = std::make_shared<PipelineInstanceConfiguration>();
    config->instance = PipelineInstanceId("View.Left");
    config->pipeline = PipelineId("Game.Forward");
    FeatureInstanceConfiguration feature;
    feature.instance = FeatureInstanceId("Outline.Left");
    feature.feature = FeatureId("Game.Outline");
    feature.parameters.emplace(1, 2.0);
    config->features.push_back(feature);
    if (!host.set_configuration(config) || !host.configuration() ||
        std::get<double>(host.configuration()->features.front().parameters.at(1)) != 2.0)
        return 1;
    auto bad = std::make_shared<PipelineInstanceConfiguration>(*config);
    bad->instance = PipelineInstanceId("View.Right");
    if (host.set_configuration(bad))
        return 2;
    std::cout << "pipeline instance configuration smoke passed\n";
    return 0;
}
