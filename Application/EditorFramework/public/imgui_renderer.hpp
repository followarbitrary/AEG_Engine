/**
 * @file imgui_renderer.hpp
 * @brief Backend-neutral Dear ImGui RenderFeature integration.
 */
#pragma once

#include <cstdint>
#include <memory>

#include "imgui_draw_packet.hpp"
#include "imgui_texture_registry.hpp"
#include "pipeline.hpp"
#include "rhi_struct.hpp"

namespace aeg::render_graph {
class FrameContext;
class RenderGraph;
}  // namespace aeg::render_graph

namespace aeg::editor {
/** @brief Describes the RenderGraph resource that receives the ImGui overlay. */
struct ImGuiRenderTargetDesc final {
    pipeline::RenderResourceId resource_id{};
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
    [[nodiscard]] foundation::Result build_render_graph(pipeline::RenderGraphBuilder& builder) const;

    friend class ImGuiRenderFeature;
};

/** @brief Thread-confined feature that inserts the ImGui overlay at RenderPipeline.BeforePresent. */
class ImGuiRenderFeature final : public pipeline::RenderFeature {
public:
    explicit ImGuiRenderFeature(ImGuiRenderer& renderer) noexcept;

    [[nodiscard]] const pipeline::FeatureDescriptor& descriptor() const noexcept override { return descriptor_; }
    [[nodiscard]] foundation::Result build_render_graph(pipeline::RenderGraphBuilder& builder) const override;

protected:
    [[nodiscard]] foundation::Result setup(pipeline::RenderGraphBuilder& builder) const override;

private:
    ImGuiRenderer* renderer_{nullptr};
    pipeline::FeatureDescriptor descriptor_{};
};

/** @brief Registers and binds the standard ImGui RenderFeature. */
[[nodiscard]] foundation::Result register_imgui_render_feature(pipeline::PipelineRegistry& registry,
                                                               ImGuiRenderer& renderer);
/** @brief Unbinds and removes the standard ImGui RenderFeature from a registry. */
[[nodiscard]] foundation::Result unregister_imgui_render_feature(pipeline::PipelineRegistry& registry);
}  // namespace aeg::editor
