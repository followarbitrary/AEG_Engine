/**
 * @file render_graph_context_contract_smoke.cpp
 * @brief Verifies that pass execution cannot access the RHI device directly.
 */
#include "render_graph.hpp"
#include <concepts>

#if __has_include("render_graph_execution.hpp") || __has_include("render_graph_resource_access.hpp")
#error "RenderGraph private execution/resource bridges must not propagate to public consumers"
#endif

namespace {
template <typename ContextType>
concept exposes_device_accessor = requires(const ContextType& context) {
    context.device();
};

template <typename ExecuteDescType>
concept exposes_manual_present_signal = requires(ExecuteDescType& desc) {
    desc.signal_present;
};

template <typename Graph>
concept exposes_compile_info = requires(const Graph& graph) { graph.compile_info(); };

template <typename Graph>
concept exposes_plan = requires(const Graph& graph) { graph.plan(); };

template <typename Graph>
concept exposes_owning_inspection = requires(const Graph& graph) {
    { graph.inspect() } -> std::same_as<aeg::foundation::ResultValue<aeg::render_graph::RenderGraphInspection>>;
};

template <typename Graph>
concept exposes_buffer_membership = requires(const Graph& graph, aeg::render_graph::RenderGraphBuffer buffer) {
    graph.contains_buffer(buffer);
};

template <typename Graph>
concept exposes_texture_membership = requires(const Graph& graph, aeg::render_graph::RenderGraphTexture texture) {
    graph.contains_texture(texture);
};
}  // namespace

static_assert(!exposes_device_accessor<aeg::render_graph::RenderPassCommandContext>);
static_assert(!exposes_device_accessor<aeg::render_graph::RenderGraph>);
static_assert(!exposes_compile_info<aeg::render_graph::RenderGraph>);
static_assert(!exposes_plan<aeg::render_graph::RenderGraph>);
static_assert(exposes_owning_inspection<aeg::render_graph::RenderGraph>);
static_assert(!exposes_buffer_membership<aeg::render_graph::RenderGraph>);
static_assert(!exposes_texture_membership<aeg::render_graph::RenderGraph>);
static_assert(!exposes_manual_present_signal<aeg::render_graph::RenderGraphExecuteDesc>);

int main() {
    return 0;
}
