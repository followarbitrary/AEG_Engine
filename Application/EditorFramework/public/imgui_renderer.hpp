/**
 * @file imgui_renderer.hpp
 * @brief Backend-neutral Dear ImGui renderer ownership and frame preparation.
 */
#pragma once

#include <cstdint>
#include <memory>

#include "imgui_draw_packet.hpp"
#include "imgui_texture_registry.hpp"
#include "feature_types.hpp"
#include "rhi_struct.hpp"

namespace aeg::rhi {
class Device;
}

namespace aeg::render_graph {
class FrameContext;
class RenderGraph;
}  // namespace aeg::render_graph

namespace aeg::editor {
namespace detail {
struct ImGuiRenderExecution;
}
/** @brief Describes the RenderGraph resource that receives the ImGui overlay. */
struct ImGuiRenderTargetDesc final {
    feature::RenderResourceId resource_id{};
    rhi::Format format{rhi::Format::kUnknown};
    rhi::Extent2D extent{};
    rhi::ResourceUsage final_usage{rhi::ResourceUsage::kPresent};
};

/** @brief Thread-confined owner of persistent ImGui RHI objects and one prepared frame. */
class ImGuiRenderer final {
public:
    ImGuiRenderer(rhi::Device& device, ImGuiTextureRegistry& texture_registry);
    ~ImGuiRenderer();

    ImGuiRenderer(const ImGuiRenderer&) = delete;
    ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;
    ImGuiRenderer(ImGuiRenderer&&) = delete;
    ImGuiRenderer& operator=(ImGuiRenderer&&) = delete;

    /** @brief Loads and validates the ImGui shader contract and its derived layout. */
    [[nodiscard]] foundation::Result initialize();
    /** @brief Uploads one draw packet and prepares its RenderGraph resources. */
    [[nodiscard]] foundation::Result prepare_frame(render_graph::FrameContext& frame_context,
                                                   render_graph::RenderGraph& graph, const ImGuiDrawPacket& packet,
                                                   const ImGuiRenderTargetDesc& target);
    /** @brief Releases the current frame's transient RHI references after GPU work has stopped. */
    [[nodiscard]] foundation::Result release_prepared_frame();
    /** @brief Releases persistent ImGui RHI objects after GPU work has stopped. */
    [[nodiscard]] foundation::Result shutdown();

    [[nodiscard]] bool is_initialized() const noexcept;
    [[nodiscard]] bool has_prepared_frame() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_{};

    [[nodiscard]] foundation::ResultValue<rhi::PipelineHandle> get_or_create_pipeline(rhi::Format format);
    friend struct detail::ImGuiRenderExecution;
};
}  // namespace aeg::editor
