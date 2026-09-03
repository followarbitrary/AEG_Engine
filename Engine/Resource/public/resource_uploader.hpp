/**
 * @file resource_uploader.hpp
 * @brief Frame-local staging helpers for buffer and mesh uploads.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "frame_context.hpp"
#include "mesh.hpp"
#include "render_graph.hpp"

namespace aeg::resource {
    /** @brief Describes a byte upload into a newly created destination buffer. */
    struct BufferUploadDesc {
        std::span<const std::byte> data{};
        rhi::BufferDesc destination{};
        uint64_t destination_offset{0};
        uint64_t alignment{0};
    };

    /**
     * @brief Result of a buffer upload recorded into a render graph.
     *
     * The graph handles are valid until the graph is reset. The RHI destination
     * remains valid until it is destroyed by the caller.
     */
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

    /**
     * @brief Result of uploading an indexed mesh through a render graph.
     *
     * Mesh graph handles are scoped to the graph used for the upload. The
     * buffers in mesh remain valid independently and are owned by the caller.
     */
    struct MeshUpload {
        GpuMesh mesh{};
        render_graph::RenderGraphBuffer vertex_resource{};
        render_graph::RenderGraphBuffer index_resource{};

        [[nodiscard]] bool is_valid() const noexcept {
            return mesh.is_valid() && vertex_resource.is_valid() && index_resource.is_valid();
        }
    };

    /**
     * @brief Records frame-local uploads and creates their destination resources.
     *
     * The uploader borrows the device and does not outlive it. Upload methods
     * require an active FrameContext frame and a graph that is compiled after
     * the returned upload declarations are added.
     */
    class ResourceUploader final {
    public:
        /** @brief Binds the uploader to a device without taking ownership. */
        explicit ResourceUploader(rhi::Device& device) noexcept : device_(device) {
        }

        /** @brief Creates a destination buffer and records a copy from staging data. */
        [[nodiscard]] foundation::ResultValue<BufferUpload> upload_buffer(
                render_graph::FrameContext& frame_context, render_graph::RenderGraph& graph,
                const BufferUploadDesc& desc);

        /** @brief Creates vertex/index buffers and records an indexed mesh upload. */
        [[nodiscard]] foundation::ResultValue<MeshUpload> upload_mesh(
                render_graph::FrameContext& frame_context, render_graph::RenderGraph& graph,
                const MeshData& mesh, std::string_view name = {});

        /** @brief Defers destruction of mesh buffers until the frame is reusable. */
        [[nodiscard]] foundation::Result defer_destroy_mesh(
                render_graph::FrameContext& frame_context, GpuMesh& mesh) const;

        /** @brief Immediately destroys mesh buffers and clears the mesh value. */
        [[nodiscard]] foundation::Result destroy_mesh(GpuMesh& mesh) const;

    private:
        rhi::Device& device_;
    };
} // namespace aeg::resource
