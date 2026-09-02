#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "frame_context.hpp"
#include "mesh.hpp"
#include "render_graph.hpp"

namespace aeg::resource {
    struct BufferUploadDesc {
        std::span<const std::byte> data{};
        rhi::BufferDesc destination{};
        uint64_t destination_offset{0};
        uint64_t alignment{0};
    };

    // The graph handles are valid until the graph is reset. The RHI destination
    // remains valid until it is destroyed by the caller.
    struct BufferUpload {
        render_graph::FrameAllocation staging{};
        rhi::BufferHandle destination{};
        render_graph::RenderGraphBuffer source{};
        render_graph::RenderGraphBuffer target{};
        uint64_t destination_offset{0};
        uint64_t size{0};
        uint64_t transfer_size{0};

        [[nodiscard]] bool is_valid() const noexcept {
            return staging.is_valid() && destination.is_valid() && source.is_valid() && target.is_valid() &&
                   size != 0 && transfer_size >= size;
        }
    };

    // Mesh graph handles are scoped to the graph used for the upload. The
    // buffers in mesh remain valid independently and are owned by the caller.
    struct MeshUpload {
        GpuMesh mesh{};
        render_graph::RenderGraphBuffer vertex_resource{};
        render_graph::RenderGraphBuffer index_resource{};

        [[nodiscard]] bool is_valid() const noexcept {
            return mesh.is_valid() && vertex_resource.is_valid() && index_resource.is_valid();
        }
    };

    class ResourceUploader final {
    public:
        explicit ResourceUploader(rhi::Device& device) noexcept : device_(device) {
        }

        [[nodiscard]] foundation::ResultValue<BufferUpload> upload_buffer(
                render_graph::FrameContext& frame_context, render_graph::RenderGraph& graph,
                const BufferUploadDesc& desc);

        [[nodiscard]] foundation::ResultValue<MeshUpload> upload_mesh(
                render_graph::FrameContext& frame_context, render_graph::RenderGraph& graph,
                const MeshData& mesh, std::string_view name = {});

        [[nodiscard]] foundation::Result defer_destroy_mesh(
                render_graph::FrameContext& frame_context, GpuMesh& mesh) const;

        [[nodiscard]] foundation::Result destroy_mesh(GpuMesh& mesh) const;

    private:
        rhi::Device& device_;
    };
} // namespace aeg::resource
