/** @brief Host-owned procedural sky and HDR postprocessing resources. */
#pragma once

#include <filesystem>

#include "renderer.hpp"

namespace aeg::rhi {
class Device;
}
namespace aeg::render_graph {
class FrameContext;
}
namespace aeg::renderer {
inline constexpr rhi::Format kSceneLightingFormat = rhi::Format::kRgba16Float;

[[nodiscard]] foundation::Result validate_post_process_settings(const RenderPostProcessSettings& settings);

class PostProcessResources final {
public:
    PostProcessResources();
    ~PostProcessResources();
    PostProcessResources(const PostProcessResources&) = delete;
    PostProcessResources& operator=(const PostProcessResources&) = delete;
    [[nodiscard]] foundation::Result initialize(rhi::Device& device, const std::filesystem::path& package,
                                                rhi::Format output_format);
    [[nodiscard]] foundation::ResultValue<RenderPostProcessResource> for_frame(
        render_graph::FrameContext& frame, const RenderView& view, const RenderPostProcessSettings& settings) const;
    [[nodiscard]] foundation::ResultValue<RenderPostProcessResource> for_frame(render_graph::FrameContext& frame,
                                                                               const RenderView& view) const {
        return for_frame(frame, view, view.post_process);
    }

private:
    struct Impl;
    std::shared_ptr<Impl> impl_{};
};

/** @brief Samples HDR scene color and depth, then writes the view's final output. */
[[nodiscard]] foundation::Result build_post_process_render_graph(
    pipeline::RenderGraphBuilder& builder, const RenderFeatureContext& context, pipeline::RenderGraphTexture hdr_color,
    pipeline::RenderGraphTexture depth, pipeline::RenderGraphTexture output, rhi::ResourceUsage final_usage);
}  // namespace aeg::renderer
