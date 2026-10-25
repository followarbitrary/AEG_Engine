/** @file runtime_mesh_vertex.cpp */
#include "runtime_mesh_vertex.hpp"

#include <cstddef>
#include <utility>

namespace aeg::application::runtime {

foundation::ResultValue<std::vector<RuntimeMeshVertex>> make_renderer_vertices(const aeg::asset::MeshPayload& mesh) {
    if (mesh.positions.empty() || mesh.positions.size() != mesh.normals.size() ||
        mesh.positions.size() != mesh.uvs.size()) {
        return {foundation::invalid_argument("Mesh attribute arrays have inconsistent sizes"), {}};
    }
    std::vector<RuntimeMeshVertex> result(mesh.positions.size());
    for (std::size_t i = 0; i < result.size(); ++i) {
        result[i].position[0] = mesh.positions[i][0];
        result[i].position[1] = mesh.positions[i][1];
        result[i].position[2] = mesh.positions[i][2];
        result[i].position[3] = 1.0f;
        result[i].uv[0] = mesh.uvs[i][0];
        result[i].uv[1] = mesh.uvs[i][1];
        result[i].normal[0] = mesh.normals[i][0];
        result[i].normal[1] = mesh.normals[i][1];
        result[i].normal[2] = mesh.normals[i][2];
    }
    return {foundation::Result::success(), std::move(result)};
}

}  // namespace aeg::application::runtime
