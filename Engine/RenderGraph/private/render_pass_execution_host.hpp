#pragma once

#include <cstdint>

#include "../../Pipeline/private/render_pass_registry_execution.hpp"
#include "render_pass.hpp"

namespace aeg::render_graph {

/** @brief Private borrowed capability used only while authored RenderPasses are expanded. */
struct RenderPassExecutionHost final {
    const pipeline::RenderPassRegistry* registry{nullptr};
    uint64_t epoch{1};

    [[nodiscard]] bool is_valid() const noexcept {
        return registry != nullptr && epoch != 0 &&
               pipeline::detail::RenderPassRegistryExecution::execution_epoch(*registry) == epoch;
    }
};

}  // namespace aeg::render_graph
