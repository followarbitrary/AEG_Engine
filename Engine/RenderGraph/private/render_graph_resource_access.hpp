#pragma once

#include "../public/render_graph.hpp"

namespace aeg::render_graph::detail {

/** @brief RenderGraph-private ownership and generation validation access. */
struct RenderGraphResourceAccess final {
    [[nodiscard]] static bool contains_buffer(const RenderGraph& graph, RenderGraphBuffer resource) noexcept;
    [[nodiscard]] static bool contains_texture(const RenderGraph& graph, RenderGraphTexture resource) noexcept;
};

}  // namespace aeg::render_graph::detail
