/**
 * @file engine_frame_pass_registry.hpp
 * @brief Engine-private adapter for authored RenderPass expansion.
 *
 * The frame orchestrator stores this opaque host instead of exposing the
 * Pipeline RenderPassRegistry type through its public Runtime header.
 */
#pragma once

#include "render_pass.hpp"

namespace aeg::engine {

class EngineFramePassRegistry final {
public:
    explicit EngineFramePassRegistry(const pipeline::RenderPassRegistry& registry) noexcept : registry_(&registry) {}

    [[nodiscard]] const pipeline::RenderPassRegistry& registry() const noexcept { return *registry_; }

private:
    const pipeline::RenderPassRegistry* registry_{nullptr};
};

}  // namespace aeg::engine
