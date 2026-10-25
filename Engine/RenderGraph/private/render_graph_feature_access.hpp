/**
 * @file render_graph_feature_access.hpp
 * @brief RenderGraph-private access bridge for Feature projection assembly.
 */
#pragma once

#include "../public/render_graph.hpp"

#include <memory>

namespace aeg::render_graph::detail {

/**
 * @brief Keeps Feature build transaction helpers out of the public Graph type.
 *
 * The bridge exposes only the neutral graph value contracts needed by the
 * RenderGraph-private Feature runtime. It is intentionally defined in the
 * private build target and is the sole friend of RenderGraph for this path.
 */
struct RenderGraphFeatureGraphAccess final {
    [[nodiscard]] static foundation::Result begin_feature_build_transaction(RenderGraph& graph);
    [[nodiscard]] static foundation::Result retain_execution_owner(
        RenderGraph& graph, std::shared_ptr<const void> owner);
    [[nodiscard]] static foundation::Result retain_submission_owner(
        RenderGraph& graph, std::shared_ptr<const void> owner);
    [[nodiscard]] static foundation::Result set_feature_work(
        RenderGraph& graph, RenderGraphPass pass, const RenderWorkItem& work);
    [[nodiscard]] static uint32_t feature_pass_checkpoint(const RenderGraph& graph) noexcept;
    [[nodiscard]] static foundation::ResultValue<RenderGraphPass> attribute_feature_passes_since(
        RenderGraph& graph, uint32_t first, const RenderWorkItem& work, RenderGraphPass previous);
    [[nodiscard]] static foundation::ResultValue<rhi::TextureDesc> texture_description(
        const RenderGraph& graph, RenderGraphTexture texture);
    [[nodiscard]] static bool textures_alias(
        const RenderGraph& graph, RenderGraphTexture source, RenderGraphTexture destination) noexcept;
    static void commit_feature_build_transaction(RenderGraph& graph) noexcept;
    static void rollback_feature_build_transaction(RenderGraph& graph) noexcept;
};

}  // namespace aeg::render_graph::detail
