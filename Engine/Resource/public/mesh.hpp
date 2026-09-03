/**
 * @file mesh.hpp
 * @brief CPU mesh input and GPU mesh ownership types.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "result.hpp"
#include "rhi_struct.hpp"

namespace aeg::resource {
    /**
     * @brief Non-owning CPU-side vertex and index data for an indexed mesh.
     *
     * The byte spans must remain alive while an upload operation consumes them.
     * Counts are element counts, while vertex_stride is measured in bytes.
     */
    struct MeshData {
        std::span<const std::byte> vertex_data{};
        uint32_t vertex_stride{0};
        uint32_t vertex_count{0};

        std::span<const std::byte> index_data{};
        rhi::IndexFormat index_format{rhi::IndexFormat::kUint16};
        uint32_t index_count{0};
    };

    /**
     * @brief GPU-resident buffers and draw metadata for an indexed mesh.
     *
     * The buffers are owned by the caller and must be destroyed through the RHI
     * or ResourceUploader before this value is discarded.
     */
    struct GpuMesh {
        rhi::BufferHandle vertex_buffer{};
        rhi::BufferHandle index_buffer{};
        uint32_t vertex_stride{0};
        uint32_t vertex_count{0};
        uint32_t index_count{0};
        rhi::IndexFormat index_format{rhi::IndexFormat::kUint16};

        [[nodiscard]] bool is_valid() const noexcept {
            return vertex_buffer.is_valid() && index_buffer.is_valid() && vertex_stride != 0 &&
                   vertex_count != 0 && index_count != 0;
        }

        void reset() noexcept {
            *this = {};
        }
    };

    /** @brief Validates byte ranges, strides, counts, and index format consistency. */
    [[nodiscard]] foundation::Result validate_mesh_data(const MeshData& mesh) noexcept;
} // namespace aeg::resource
