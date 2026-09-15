/**
 * @file cgltf_mesh_importer.cpp
 * @brief cgltf mesh importer implementation.
 */
#include "cgltf_mesh_importer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace aeg::tooling {
namespace {
constexpr uint32_t kMeshPayloadSchemaVersion = 1;
constexpr uint32_t kMeshVertexStride = 32;

[[nodiscard]] std::string lowercase_extension(const std::filesystem::path& source) {
    std::string extension = source.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return extension;
}

void write_u32(std::vector<std::byte>& bytes, size_t offset, uint32_t value) {
    for (size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

void write_float(std::vector<std::byte>& bytes, size_t offset, float value) {
    std::array<std::byte, sizeof(value)> raw{};
    std::memcpy(raw.data(), &value, sizeof(value));
    std::copy(raw.begin(), raw.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

[[nodiscard]] const cgltf_accessor* find_accessor(const cgltf_primitive& primitive, cgltf_attribute_type type) {
    for (cgltf_size index = 0; index < primitive.attributes_count; ++index) {
        if (primitive.attributes[index].type == type) {
            return primitive.attributes[index].data;
        }
    }
    return nullptr;
}

[[nodiscard]] bool buffers_are_bounded(const cgltf_data& data) noexcept {
    for (cgltf_size index = 0; index < data.buffer_views_count; ++index) {
        const auto& view = data.buffer_views[index];
        if (view.buffer == nullptr || view.offset > view.buffer->size || view.size > view.buffer->size - view.offset ||
            view.has_meshopt_compression) {
            return false;
        }
    }
    for (cgltf_size index = 0; index < data.accessors_count; ++index) {
        const auto& accessor = data.accessors[index];
        const cgltf_size element = cgltf_calc_size(accessor.type, accessor.component_type);
        // Sparse data is not supported by the accessor readers used below.
        if (accessor.is_sparse || accessor.count == 0 || element == 0 || accessor.stride < element) {
            return false;
        }
        if (const auto* view = accessor.buffer_view; view != nullptr) {
            if (accessor.offset > view->size || element > view->size - accessor.offset ||
                accessor.count - 1 > (view->size - accessor.offset - element) / accessor.stride) {
                return false;
            }
        }
    }
    return true;
}
}  // namespace

bool CgltfMeshImporter::supports(const std::filesystem::path& source) const noexcept try {
    const std::string extension = lowercase_extension(source);
    return extension == ".gltf" || extension == ".glb";
} catch (...) {
    return false;
}

foundation::ResultValue<std::vector<CookedAsset>> CgltfMeshImporter::cook(const std::filesystem::path& source,
                                                                          const AssetMeta& meta) const try {
    cgltf_options options{};
    cgltf_data* raw_data = nullptr;
    if (cgltf_parse_file(&options, source.string().c_str(), &raw_data) != cgltf_result_success || raw_data == nullptr) {
        return {foundation::invalid_argument("cgltf could not parse the mesh source"), {}};
    }
    const std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(raw_data, cgltf_free);
    if (cgltf_load_buffers(&options, raw_data, source.string().c_str()) != cgltf_result_success) {
        return {foundation::invalid_argument("cgltf could not load mesh buffers"), {}};
    }
    if (!buffers_are_bounded(*raw_data) || cgltf_validate(raw_data) != cgltf_result_success) {
        return {foundation::invalid_argument("glTF buffer ranges or primitive attributes are invalid or unsupported"), {}};
    }

    std::vector<const cgltf_primitive*> primitives;
    for (cgltf_size mesh_index = 0; mesh_index < raw_data->meshes_count; ++mesh_index) {
        const cgltf_mesh& mesh = raw_data->meshes[mesh_index];
        for (cgltf_size primitive_index = 0; primitive_index < mesh.primitives_count; ++primitive_index) {
            if (mesh.primitives[primitive_index].type == cgltf_primitive_type_triangles) {
                primitives.push_back(&mesh.primitives[primitive_index]);
            }
        }
    }
    std::vector<const CookedProduct*> products;
    for (const CookedProduct& product : meta.products) {
        if (product.kind == AssetKind::kMesh) {
            products.push_back(&product);
        }
    }
    if (primitives.empty() || products.size() != primitives.size() || products.size() != meta.products.size()) {
        return {
            foundation::invalid_argument("mesh metadata must declare exactly one product per glTF triangle primitive"),
            {}};
    }

    std::vector<CookedAsset> result;
    result.reserve(primitives.size());
    for (size_t primitive_index = 0; primitive_index < primitives.size(); ++primitive_index) {
        const cgltf_primitive& primitive = *primitives[primitive_index];
        const cgltf_accessor* positions = find_accessor(primitive, cgltf_attribute_type_position);
        const cgltf_accessor* normals = find_accessor(primitive, cgltf_attribute_type_normal);
        const cgltf_accessor* texcoords = find_accessor(primitive, cgltf_attribute_type_texcoord);
        if (positions == nullptr || positions->buffer_view == nullptr || positions->type != cgltf_type_vec3 ||
            positions->count == 0 || positions->count > UINT32_MAX ||
            (normals != nullptr && (normals->type != cgltf_type_vec3 || normals->count != positions->count)) ||
            (texcoords != nullptr && (texcoords->type != cgltf_type_vec2 || texcoords->count != positions->count))) {
            return {foundation::invalid_argument("glTF triangle primitive has invalid POSITION data"), {}};
        }
        const uint32_t vertex_count = static_cast<uint32_t>(positions->count);
        const uint32_t index_count =
            primitive.indices == nullptr ? vertex_count : static_cast<uint32_t>(primitive.indices->count);
        if (primitive.indices != nullptr && primitive.indices->count > UINT32_MAX) {
            return {foundation::invalid_argument("glTF triangle primitive has too many indices"), {}};
        }
        const uint64_t payload_size = 16ull + static_cast<uint64_t>(vertex_count) * kMeshVertexStride +
                                     static_cast<uint64_t>(index_count) * sizeof(uint32_t);
        if (index_count == 0 || index_count % 3 != 0 || payload_size > std::numeric_limits<std::size_t>::max()) {
            return {foundation::invalid_argument("glTF triangle count or decoded mesh size is invalid"), {}};
        }
        std::vector<std::byte> payload(static_cast<std::size_t>(payload_size));
        write_u32(payload, 0, vertex_count);
        write_u32(payload, 4, index_count);
        write_u32(payload, 8, kMeshVertexStride);
        write_u32(payload, 12, 0);
        for (uint32_t vertex = 0; vertex < vertex_count; ++vertex) {
            std::array<float, 3> position{};
            std::array<float, 3> normal{};
            std::array<float, 2> texcoord{};
            if (cgltf_accessor_read_float(positions, vertex, position.data(), position.size()) == 0) {
                return {foundation::invalid_argument("cgltf could not read mesh position"), {}};
            }
            if (normals != nullptr && !cgltf_accessor_read_float(normals, vertex, normal.data(), normal.size())) {
                return {foundation::invalid_argument("cgltf could not read mesh normal"), {}};
            }
            if (texcoords != nullptr && !cgltf_accessor_read_float(texcoords, vertex, texcoord.data(), texcoord.size())) {
                return {foundation::invalid_argument("cgltf could not read mesh texcoord"), {}};
            }
            const size_t offset = 16U + static_cast<size_t>(vertex) * kMeshVertexStride;
            for (size_t component = 0; component < position.size(); ++component) {
                write_float(payload, offset + component * sizeof(float), position[component]);
                write_float(payload, offset + 12U + component * sizeof(float), normal[component]);
            }
            for (size_t component = 0; component < texcoord.size(); ++component) {
                write_float(payload, offset + 24U + component * sizeof(float), texcoord[component]);
            }
        }
        for (uint32_t index = 0; index < index_count; ++index) {
            const uint32_t value = primitive.indices == nullptr
                                       ? index
                                       : static_cast<uint32_t>(cgltf_accessor_read_index(primitive.indices, index));
            if (value >= vertex_count) {
                return {foundation::invalid_argument("glTF index references a vertex outside its primitive"), {}};
            }
            write_u32(payload,
                      16U + static_cast<size_t>(vertex_count) * kMeshVertexStride +
                          static_cast<size_t>(index) * sizeof(uint32_t),
                      value);
        }
        result.push_back(
            {products[primitive_index]->id, AssetKind::kMesh, kMeshPayloadSchemaVersion, {}, std::move(payload)});
    }
    return {{}, std::move(result)};
} catch (const std::bad_alloc&) {
    return {foundation::out_of_memory("Mesh import allocation failed"), {}};
} catch (const std::exception& exception) {
    return {foundation::unknown_exception(exception), {}};
}
}  // namespace aeg::tooling
