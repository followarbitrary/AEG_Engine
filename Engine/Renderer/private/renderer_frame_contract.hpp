/**
 * @file renderer_frame_contract.hpp
 * @brief Renderer-owned bridge for publishing scene snapshots into Frame hosts.
 *
 * PipelineScopeHost is an Engine orchestration detail. Keep this bridge out of
 * the public Renderer scene/extraction contract and expose it only to the
 * Engine frame orchestrator through the Renderer private include surface.
 */
#pragma once

#include <memory>

#include "pipeline_types.hpp"
#include "result.hpp"

namespace aeg::pipeline {
class PipelineScopeHost;
}

namespace aeg::renderer {
class Renderer;
struct RenderSceneSnapshot;

inline constexpr pipeline::ArtifactId kRenderSceneSnapshotArtifactId{"RenderSceneSnapshot"};
inline constexpr pipeline::ArtifactTypeId kRenderSceneSnapshotArtifactTypeId{"RenderSceneSnapshot"};
inline constexpr uint32_t kRenderSceneSnapshotArtifactVersion = 1;

[[nodiscard]] foundation::Result install_frame_contract(const Renderer& renderer,
                                                        pipeline::PipelineScopeHost& frame_host) noexcept;
[[nodiscard]] foundation::Result publish_frame_snapshot(const Renderer& renderer,
                                                        pipeline::PipelineScopeHost& frame_host,
                                                        std::shared_ptr<const RenderSceneSnapshot> snapshot) noexcept;
}  // namespace aeg::renderer
