/** @brief Fixed engine-native pass sequencing, without a Feature author identity. */
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "render_pass.hpp"

namespace aeg::renderer {

struct NativePassStep final {
    pipeline::RenderPassTypeId type{};
    pipeline::RenderPassInstanceId instance{};
    rhi::QueueClass queue{rhi::QueueClass::kGraphics};
    bool enabled{true};
    foundation::ParameterBlock parameters{};
    std::unordered_map<uint64_t, uint64_t> resource_bindings{};
    std::string source{};
    std::vector<pipeline::RenderPassInstanceId> ordered_after{};
};

struct NativePassSequence final {
    std::vector<NativePassStep> passes{};
};

} // namespace aeg::renderer
