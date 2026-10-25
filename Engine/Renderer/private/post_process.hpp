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

class PostProcessResources final {
public:
    PostProcessResources();
    ~PostProcessResources();
    PostProcessResources(const PostProcessResources&) = delete;
    PostProcessResources& operator=(const PostProcessResources&) = delete;
    [[nodiscard]] foundation::Result initialize(rhi::Device& device, const std::filesystem::path& package,
                                                rhi::Format output_format);
    [[nodiscard]] foundation::ResultValue<RenderPostProcessResource> for_frame(
        render_graph::FrameContext& frame, const RenderView& view, const feature::RenderPostProcessSettings& settings) const;
    [[nodiscard]] foundation::ResultValue<RenderPostProcessResource> for_frame(render_graph::FrameContext& frame,
                                                                               const RenderView& view) const {
        return for_frame(frame, view, view.post_process);
    }

private:
    struct Impl;
    std::shared_ptr<Impl> impl_{};
};

}  // namespace aeg::renderer
