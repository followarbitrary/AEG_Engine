#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "pipeline.hpp"
#include "world_scope.hpp"

namespace aeg::engine { class EngineFrameOrchestrator; }
namespace aeg::engine::detail {

/** Owning, read-only observations of completed technical Stage visits; no callback entry. */
struct EngineFrameStageObservation final {
    pipeline::PipelineInstanceId instance{};
    pipeline::StageId stage{};
    pipeline::PhaseId phase{};
    pipeline::PipelineScope scope{pipeline::PipelineScope::kEngine};
    uint64_t frame_number{0};
    uint64_t fixed_tick{0};
    uintptr_t user_data_identity{0};
    uintptr_t scope_identity{0};
    std::optional<world::WorldFrameKey> world_frame{};
    std::optional<ecs::WorldSnapshot> world_snapshot{};
    /** Retains the actual host bridge read, independently of the typed World view. */
    std::optional<pipeline::ArtifactView> bridged_snapshot{};
};

struct EngineFrameInspectionTrace final {
    std::vector<EngineFrameStageObservation> stages{};
};

/** Explicitly enabled for diagnostics; ordinary hosts do not accumulate traces. */
struct EngineFrameInspection final {
    [[nodiscard]] static foundation::Result enable(EngineFrameOrchestrator& owner, bool enabled = true);
    [[nodiscard]] static foundation::ResultValue<EngineFrameInspectionTrace> capture(
        const EngineFrameOrchestrator& owner);
};
}
