/**
 * @file runtime_bootstrap.hpp
 * @brief Package-only game startup boundary for static game executables.
 */
#pragma once

#include <functional>

#include "game/game_runtime_bootstrap.hpp"
#include "window.hpp"

namespace aeg::engine {
class EngineFrameOrchestrator;
struct EngineFrameDesc;
struct EngineFramePreparationContext;
}  // namespace aeg::engine
namespace aeg::render_graph {
class FrameContext;
class RenderGraph;
}  // namespace aeg::render_graph
namespace aeg::renderer {
class Renderer;
class RenderResourceResolver;
}  // namespace aeg::renderer
namespace aeg::world {
class WorldScope;
}

namespace aeg::application::runtime {

struct RuntimeAssetUploadHost;

/** @brief Optional Application-owned frame resources used for orchestrator execution. */
struct RuntimeFrameHostResources final {
    render_graph::FrameContext* frame_context{nullptr};
    render_graph::RenderGraph* render_graph{nullptr};
    renderer::Renderer* renderer{nullptr};
    /** @brief Caller-owned upload dependencies borrowed by RuntimeAssetResolver. */
    RuntimeAssetUploadHost* upload_context{nullptr};
    /** @brief Require the orchestrator to resolve renderer resources for this host. */
    bool require_resolved_render_resources{false};
    /** Engine host preparation, borrowed by frame descriptions for the duration of each tick. */
    std::function<foundation::Result(const engine::EngineFramePreparationContext&)> render_feature_preparation{};

    /** @brief Applies this host's resolver contract to an orchestrator frame description. */
    void apply_frame_desc(engine::EngineFrameDesc&, const renderer::RenderResourceResolver*) const noexcept;
};

}  // namespace aeg::application::runtime
