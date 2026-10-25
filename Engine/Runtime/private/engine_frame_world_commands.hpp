/** @brief Engine-only connection of the fixed World command consumer to frame execution. */
#pragma once

#include "result.hpp"

namespace aeg::engine { class EngineFrameOrchestrator; }
namespace aeg::engine::detail {
class FeatureWorldCommandConsumer;

struct EngineFrameWorldCommands final {
    /** The consumer is borrowed until the orchestrator stops using it; attach only between frames. */
    [[nodiscard]] static foundation::Result attach(EngineFrameOrchestrator& owner,
                                                  FeatureWorldCommandConsumer& consumer);
};
}  // namespace aeg::engine::detail
