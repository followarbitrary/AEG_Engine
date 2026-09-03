/**
 * @file mesh.cpp
 * @brief Implements indexed mesh data validation.
 */
#include "../public/mesh.hpp"

#include <cstdint>
#include <limits>

namespace aeg::resource {
    namespace {
        [[nodiscard]] uint32_t index_size(rhi::IndexFormat format) noexcept {
            switch (format) {
                case rhi::IndexFormat::kUint16:
                    return sizeof(uint16_t);
                case rhi::IndexFormat::kUint32:
                    return sizeof(uint32_t);
            }
            return 0;
        }
    } // namespace

    foundation::Result validate_mesh_data(const MeshData& mesh) noexcept {
        if (mesh.vertex_data.empty() || mesh.vertex_stride == 0 || mesh.vertex_count == 0) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                "Mesh vertex data must not be empty");
        }
        if (mesh.vertex_count > std::numeric_limits<uint64_t>::max() / mesh.vertex_stride) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                "Mesh vertex data size overflows");
        }
        const uint64_t expected_vertex_size = static_cast<uint64_t>(mesh.vertex_count) * mesh.vertex_stride;
        if (mesh.vertex_data.size() != expected_vertex_size) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                "Mesh vertex data size does not match stride and count");
        }

        const uint32_t element_size = index_size(mesh.index_format);
        if (element_size == 0 || mesh.index_data.empty() || mesh.index_count == 0) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                "Mesh index data is invalid");
        }
        if (mesh.index_count > std::numeric_limits<uint64_t>::max() / element_size) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                "Mesh index data size overflows");
        }
        const uint64_t expected_index_size = static_cast<uint64_t>(mesh.index_count) * element_size;
        if (mesh.index_data.size() != expected_index_size) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                "Mesh index data size does not match format and count");
        }
        return foundation::Result::success();
    }
} // namespace aeg::resource
