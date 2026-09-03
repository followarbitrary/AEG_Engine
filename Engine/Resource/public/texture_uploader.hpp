/**
 * @file texture_uploader.hpp
 * @brief Frame-local staging helper for texture uploads.
 */
#pragma once

#include <cstdint>
#include <string_view>

#include "frame_context.hpp"
#include "render_graph.hpp"
#include "texture.hpp"

namespace aeg::resource {
    /**
     * @brief Result of uploading texture subresources through a render graph.
     *
     * Graph handles remain valid until their graph is reset. The GPU texture is
     * owned by the caller and must be destroyed after all uses complete.
     */
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

    /**
     * @brief Records texture uploads using frame-local staging memory.
     *
     * The uploader borrows the device and expects an active frame while upload
     * methods are called.
     */
    class TextureUploader final {
    public:
        /** @brief Binds the uploader to a device without taking ownership. */
        explicit TextureUploader(rhi::Device& device) noexcept : device_(device) {
        }

        /** @brief Creates a texture and records copies for all supplied subresources. */
        [[nodiscard]] foundation::ResultValue<TextureUpload> upload_texture(
                render_graph::FrameContext& frame_context, render_graph::RenderGraph& graph,
                const TextureData& texture, std::string_view name = {});

        /** @brief Defers destruction of a GPU texture until the frame is reusable. */
        [[nodiscard]] foundation::Result defer_destroy_texture(
                render_graph::FrameContext& frame_context, GpuTexture& texture) const;

        /** @brief Immediately destroys texture-owned RHI objects and clears the value. */
        [[nodiscard]] foundation::Result destroy_texture(GpuTexture& texture) const;

    private:
        rhi::Device& device_;
    };
} // namespace aeg::resource
