#include <iostream>
#include <engine_frame_orchestrator.hpp>
#include <engine_frame_pipeline_host.hpp>
#include <engine_frame_pipeline_registration.hpp>
#include <rhi.hpp>

int main() {
    auto device = aeg::rhi::create_device({});
    if (!device) return 1;
    aeg::pipeline::PipelineRegistry registry;
    aeg::pipeline::PipelineScheduler scheduler;
    aeg::engine::EngineFramePipelineHost host(registry, scheduler);
    aeg::render_graph::FrameContext frames(*device.value);
    aeg::render_graph::RenderGraph graph(*device.value);
    aeg::pipeline::PipelineScopeHost raw_world(aeg::pipeline::PipelineScope::kWorld,
        aeg::pipeline::PipelineInstanceId("UnownedWorldPlan"));
    aeg::pipeline::PipelineScopeHost frame_observer(aeg::pipeline::PipelineScope::kFrame,
        aeg::pipeline::PipelineInstanceId("UnownedWorldPlan"));
    aeg::engine::EngineFrameOrchestrator orchestrator(host, frames, graph);
    const auto result = orchestrator.register_pipeline(aeg::engine::EngineFramePipelineRegistration(raw_world));
    if (result || result.code != aeg::foundation::ResultCode::kUnsupported) {
        std::cerr << "Private technical registration can still accept a raw World business Pipeline\n";
        return 42;
    }
    if (!orchestrator.register_pipeline(aeg::engine::EngineFramePipelineRegistration(frame_observer))) {
        std::cerr << "Rejected World registration polluted the supported Frame observer registration\n";
        return 1;
    }
    return 0;
}
