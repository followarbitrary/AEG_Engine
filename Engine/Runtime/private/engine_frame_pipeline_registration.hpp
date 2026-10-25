/**
 * @file engine_frame_pipeline_registration.hpp
 * @brief Runtime-private adapter for registering a PipelineScopeHost.
 */
#pragma once

#include "pipeline.hpp"

namespace aeg::engine {

/** @brief Keeps PipelineScopeHost out of the EngineFrameOrchestrator public registration seam. */
class EngineFramePipelineRegistration final {
public:
    explicit EngineFramePipelineRegistration(pipeline::PipelineScopeHost& host) noexcept : host_(&host) {}

    [[nodiscard]] pipeline::PipelineScopeHost& host() const noexcept { return *host_; }

private:
    pipeline::PipelineScopeHost* host_{nullptr};
};

}  // namespace aeg::engine
