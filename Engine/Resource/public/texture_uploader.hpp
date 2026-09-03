#pragma once

#include <cstdint>
#include <string_view>

#include "frame_context.hpp"
#include "render_graph.hpp"
#include "texture.hpp"

namespace aeg::resource {
    struct TextureUpload {
        GpuTexture texture{};
        render_graph::FrameAllocation staging{};
        render_graph::RenderGraphBuffer source{};
        render_graph::RenderGraphTexture target{};
        uint64_t transfer_size{0};
        uint32_t subresource_count{0};

        [[nodiscard]] bool is_valid() const noexcept {
            return texture.is_valid() && staging.is_valid() && source.is_valid() && target.is_valid() &&
                   transfer_size != 0 && subresource_count != 0;
        }
    };

    class TextureUploader final {
    public:
        explicit TextureUploader(rhi::Device& device) noexcept : device_(device) {
        }

        [[nodiscard]] foundation::ResultValue<TextureUpload> upload_texture(
                render_graph::FrameContext& frame_context, render_graph::RenderGraph& graph,
                const TextureData& texture, std::string_view name = {});

        [[nodiscard]] foundation::Result defer_destroy_texture(
                render_graph::FrameContext& frame_context, GpuTexture& texture) const;

        [[nodiscard]] foundation::Result destroy_texture(GpuTexture& texture) const;

    private:
        rhi::Device& device_;
    };
} // namespace aeg::resource
