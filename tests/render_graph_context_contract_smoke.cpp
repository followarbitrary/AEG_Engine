/**
 * @file render_graph_context_contract_smoke.cpp
 * @brief Verifies that pass execution cannot access the RHI device directly.
 */
#include "render_graph.hpp"

namespace {
template <typename ContextType>
concept exposes_device_accessor = requires(const ContextType& context) {
    context.device();
};

template <typename ExecuteDescType>
concept exposes_manual_present_signal = requires(ExecuteDescType& desc) {
    desc.signal_present;
};
}  // namespace

static_assert(!exposes_device_accessor<aeg::render_graph::RenderPassCommandContext>);
static_assert(!exposes_manual_present_signal<aeg::render_graph::RenderGraphExecuteDesc>);

int main() {
    return 0;
}
