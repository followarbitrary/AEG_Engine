#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "result.hpp"
#include "rhi_struct.hpp"

namespace aeg::resource {
    struct MeshData {
        std::span<const std::byte> vertex_data{};
        uint32_t vertex_stride{0};
        uint32_t vertex_count{0};

        std::span<const std::byte> index_data{};
        rhi::IndexFormat index_format{rhi::IndexFormat::kUint16};
        uint32_t index_count{0};
    };

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

    [[nodiscard]] foundation::Result validate_mesh_data(const MeshData& mesh) noexcept;
} // namespace aeg::resource
