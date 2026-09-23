/** @file deferred_lighting_resources.hpp @brief Host-side deferred shader and descriptor ownership. */
#pragma once

#include <filesystem>
#include <memory>

#include "renderer.hpp"

namespace aeg::rhi {
class Device;
}
namespace aeg::render_graph {
class FrameContext;
}

namespace aeg::renderer {
/**
 * @brief Creates deferred lighting resources outside render-pass callbacks.
 * The device and resource owner must outlive all GPU frames using the program.
 * Descriptors are allocated per execution and retired through FrameContext.
 */
class DeferredLightingResources final {
public:
    DeferredLightingResources();
    ~DeferredLightingResources();
    DeferredLightingResources(const DeferredLightingResources&) = delete;
    DeferredLightingResources& operator=(const DeferredLightingResources&) = delete;

    [[nodiscard]] foundation::Result initialize(rhi::Device& device, const std::filesystem::path& shader_package,
                                                rhi::Format color_format,
                                                const std::filesystem::path& shadow_package = {});
    /** @brief The frame is borrowed through graph execution, as with RenderResourceResolver. */
    [[nodiscard]] foundation::ResultValue<RenderDeferredLightingResource> for_frame(
        render_graph::FrameContext& frame) const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_{};
};
}  // namespace aeg::renderer
