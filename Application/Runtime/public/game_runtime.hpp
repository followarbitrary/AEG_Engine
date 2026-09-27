#pragma once

#include <cstdint>

#include "runtime_frame_context.hpp"

namespace aeg::application::runtime {
struct GameRuntimeContext final {
    void* opaque{nullptr};
    void (*request_quit)(void*) noexcept {nullptr};
    const RuntimeFrameContext* (*frame)(void*) noexcept {nullptr};
};

inline void request_quit(GameRuntimeContext* context) noexcept {
    if (context && context->request_quit)
        context->request_quit(context->opaque);
}

inline const RuntimeFrameContext* current_frame(const GameRuntimeContext* context) noexcept {
    return context && context->frame ? context->frame(context->opaque) : nullptr;
}

struct GameRuntimeStatus final {
    int32_t code{0};
    const char* message{nullptr};
};
using GameRuntimeStart = GameRuntimeStatus (*)(void* context, void* user_data);
using GameRuntimeFixedUpdate = GameRuntimeStatus (*)(void* context, const FixedUpdateContext* update, void* user_data);
using GameRuntimeUpdate = GameRuntimeStatus (*)(void* context, const UpdateContext* update, void* user_data);
using GameRuntimeStop = GameRuntimeStatus (*)(void* context, void* user_data);

/** C ABI-shaped callback table. Null entries are treated as no-ops. */
struct GameRuntimeCallbacks final {
    GameRuntimeStart start{nullptr};
    GameRuntimeFixedUpdate fixed_update{nullptr};
    GameRuntimeUpdate update{nullptr};
    GameRuntimeStop stop{nullptr};
    void* user_data{nullptr};
};
}  // namespace aeg::application::runtime
