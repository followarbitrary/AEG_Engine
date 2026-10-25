#pragma once

#include <string>

#include "../public/render_graph.hpp"

namespace aeg::render_graph::detail {

/** @brief Owning diagnostic value read from one actual native graph Pass record. */
struct RenderGraphPassInspection final {
    RenderGraphPass pass{};
    std::string name{};
    rhi::QueueClass queue{rhi::QueueClass::kGraphics};
};

/** @brief RenderGraph-owned access to execution metadata from the last compile. */
struct RenderGraphExecution final {
    [[nodiscard]] static rhi::Device& device(const RenderGraph& graph) noexcept;
    [[nodiscard]] static const RenderGraphCompileInfo& compile_info(const RenderGraph& graph) noexcept;
    [[nodiscard]] static const RenderGraphPlan& plan(const RenderGraph& graph) noexcept;
    /** @brief Rejects foreign or stale handles and copies the actual record metadata. */
    [[nodiscard]] static foundation::ResultValue<RenderGraphPassInspection> inspect_pass(const RenderGraph& graph,
                                                                                         RenderGraphPass pass);
};

}  // namespace aeg::render_graph::detail
