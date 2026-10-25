#pragma once

#include "imgui_renderer.hpp"
#include "../../../Engine/RenderGraph/public/render_graph_types.hpp"

#include <span>

namespace aeg::editor {
namespace detail {
/** @brief Fixed Editor overlay and resource retirement, without a business Feature identity. */
struct ImGuiRenderExecution final {
    [[nodiscard]] static foundation::Result build_render_graph(const ImGuiRenderer& renderer,
        render_graph::RenderGraph& graph, render_graph::RenderGraphTexture target);
    /** Releases every possible texture consumer before replacing a shared borrowed image. */
    [[nodiscard]] static foundation::Result release_prepared_frames(std::span<ImGuiRenderer* const> renderers);
};
}  // namespace detail
}  // namespace aeg::editor
