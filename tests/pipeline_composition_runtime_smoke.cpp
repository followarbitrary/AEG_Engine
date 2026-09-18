#include <iostream>

#include "pipeline_composition_runtime.hpp"
int main() {
    using namespace aeg::pipeline;
    using namespace aeg::engine;
    PipelineScopeHost host(PipelineScope::kFrame, PipelineInstanceId("Host"));
    PreparedPipelineDocument prepared;
    auto configuration = std::make_shared<PipelineInstanceConfiguration>();
    configuration->instance = PipelineInstanceId("Host");
    configuration->pipeline = PipelineId("Game.TestPipeline");
    prepared.configurations.push_back(std::move(configuration));
    PipelineCompositionRuntime runtime;
    PipelineHostBinding binding{"main", &host};
    if (!runtime.stage(prepared, std::span<const PipelineHostBinding>(&binding, 1)))
        return 1;
    if (!runtime.has_pending() || runtime.apply_pending(false))
        return 2;
    if (!runtime.apply_pending() || runtime.active_host_count() != 1 || runtime.active_configuration_count() != 1 ||
        !host.configuration())
        return 3;
    if (!runtime.stop_and_retire() || runtime.active_host_count() != 0 || runtime.active_configuration_count() != 0)
        return 4;
    std::cout << "pipeline composition runtime smoke passed\n";
    return 0;
}
