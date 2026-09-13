/**
 * @file runtime_session.cpp
 * @brief Implements RuntimeSession lifecycle transitions and frame numbering.
 */
#include "runtime_session.hpp"

namespace aeg::engine {

RuntimeSession::RuntimeSession(EngineFrameOrchestrator& orchestrator) : orchestrator_(orchestrator) {}

foundation::Result RuntimeSession::initialize() {
    if (initialized_) {
        return foundation::Result::success();
    }
    const foundation::Result result = orchestrator_.initialize();
    if (result) {
        initialized_ = true;
    }
    return result;
}

foundation::Result RuntimeSession::play() {
    if (!initialized_) {
        return foundation::invalid_state("RuntimeSession must be initialized before play");
    }
    if (mode_ == RuntimeMode::kPlaying) {
        return foundation::Result::success();
    }
    mode_ = RuntimeMode::kPlaying;
    return foundation::Result::success();
}

foundation::Result RuntimeSession::pause() {
    if (mode_ != RuntimeMode::kPlaying) {
        return foundation::invalid_state("RuntimeSession is not playing");
    }
    mode_ = RuntimeMode::kPaused;
    return foundation::Result::success();
}

foundation::Result RuntimeSession::stop() {
    if (!initialized_) {
        return foundation::invalid_state("RuntimeSession is not initialized");
    }
    mode_ = RuntimeMode::kStopped;
    frame_number_ = 0;
    return foundation::Result::success();
}

foundation::Result RuntimeSession::tick(const EngineFrameDesc& input) {
    if (mode_ != RuntimeMode::kPlaying) {
        return foundation::invalid_state("RuntimeSession must be playing to tick");
    }
    // Preserve all caller-provided frame settings while assigning the runtime tick number.
    EngineFrameDesc desc = input;
    desc.fixed_tick = frame_number_++;
    const foundation::Result result = orchestrator_.run_frame(desc);
    if (!result) {
        --frame_number_;
    }
    return result;
}

foundation::Result RuntimeSession::shutdown() {
    if (!initialized_) {
        return foundation::Result::success();
    }
    mode_ = RuntimeMode::kStopped;
    const foundation::Result result = orchestrator_.shutdown();
    if (result) {
        initialized_ = false;
        frame_number_ = 0;
    }
    return result;
}

RuntimeMode RuntimeSession::mode() const noexcept {
    return mode_;
}

uint64_t RuntimeSession::frame_number() const noexcept {
    return frame_number_;
}

}  // namespace aeg::engine
