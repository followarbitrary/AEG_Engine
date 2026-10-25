#pragma once

#include "render_pass.hpp"

namespace aeg::pipeline::detail {

/** @brief Reads the Engine-owned authored-pass execution epoch. */
struct RenderPassRegistryExecution final {
    [[nodiscard]] static uint64_t execution_epoch(const RenderPassRegistry& registry) noexcept;
    [[nodiscard]] static foundation::ResultValue<std::shared_ptr<const RenderPass>> find_pass(
        const RenderPassRegistry& registry, RenderPassTypeId type);
};

}  // namespace aeg::pipeline::detail
