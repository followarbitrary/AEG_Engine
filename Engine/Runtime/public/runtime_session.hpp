/**
 * @file runtime_session.hpp
 * @brief Public lifecycle state machine around an EngineFrameOrchestrator.
 */
#pragma once

#include <cstdint>

#include "engine_frame_orchestrator.hpp"

namespace aeg::engine {

/** @brief Runtime lifecycle state used by the game loop and editor Play controls. */
enum class RuntimeMode : uint8_t { kStopped, kPlaying, kPaused };

/** @brief Owns the high-level Play/Pause/Stop state around an engine frame orchestrator. */
class RuntimeSession final {
public:
    /** @brief Creates a non-owning runtime session around an existing orchestrator. */
    explicit RuntimeSession(EngineFrameOrchestrator& orchestrator);

    /** @brief Initializes registered pipelines and enters the stopped state. */
    [[nodiscard]] foundation::Result initialize();
    /** @brief Starts simulation and permits frame ticks. */
    [[nodiscard]] foundation::Result play();
    /** @brief Suspends simulation without destroying runtime state. */
    [[nodiscard]] foundation::Result pause();
    /** @brief Stops simulation and resets the runtime frame counter. */
    [[nodiscard]] foundation::Result stop();
    /** @brief Runs one orchestrated frame while the session is playing. */
    [[nodiscard]] foundation::Result tick(const EngineFrameDesc& desc = {});
    /** @brief Shuts down the orchestrator and releases runtime activation. */
    [[nodiscard]] foundation::Result shutdown();

    /** @brief Returns the current lifecycle state. */
    [[nodiscard]] RuntimeMode mode() const noexcept;
    /** @brief Returns the next fixed-tick number that will be submitted. */
    [[nodiscard]] uint64_t frame_number() const noexcept;

private:
    /** @brief Non-owning frame orchestration service supplied by the application. */
    EngineFrameOrchestrator& orchestrator_;
    /** @brief Current Play/Pause/Stop state. */
    RuntimeMode mode_{RuntimeMode::kStopped};
    /** @brief Monotonic frame counter for the current run. */
    uint64_t frame_number_{0};
    /** @brief Whether orchestrator initialization succeeded. */
    bool initialized_{false};
};
}  // namespace aeg::engine
