/**
 * @file engine_frame_pipeline_host.hpp
 * @brief Engine-private registry/scheduler host for EngineFrameOrchestrator.
 */
#pragma once

#include "pipeline.hpp"

namespace aeg::engine {
class EngineFramePassRegistry;

/**
 * @brief Keeps Pipeline execution services behind the Runtime constructor seam.
 *
 * The host borrows the registry and scheduler; both must outlive the
 * EngineFrameOrchestrator that receives this adapter.
 */
class EngineFramePipelineHost final {
public:
    EngineFramePipelineHost(pipeline::PipelineRegistry& registry, pipeline::PipelineScheduler& scheduler,
                            const EngineFramePassRegistry* render_passes = nullptr) noexcept
        : registry_(&registry), scheduler_(&scheduler), render_passes_(render_passes) {}

    [[nodiscard]] pipeline::PipelineRegistry& registry() const noexcept { return *registry_; }
    [[nodiscard]] pipeline::PipelineScheduler& scheduler() const noexcept { return *scheduler_; }
    [[nodiscard]] const EngineFramePassRegistry* render_pass_registry() const noexcept { return render_passes_; }

private:
    pipeline::PipelineRegistry* registry_{nullptr};
    pipeline::PipelineScheduler* scheduler_{nullptr};
    const EngineFramePassRegistry* render_passes_{nullptr};
};
}  // namespace aeg::engine
