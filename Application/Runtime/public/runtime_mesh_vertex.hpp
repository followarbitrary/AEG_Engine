/**
 * @file runtime_mesh_vertex.hpp
 * @brief Runtime conversion from neutral mesh payloads to the renderer vertex contract.
 */
#pragma once

#include <vector>

#include "mesh_payload.hpp"
#include "result.hpp"

namespace aeg::application::runtime {

/** @brief Renderer-facing vertex record with the engine's fixed attribute contract. */
struct RuntimeMeshVertex final {
    float position[4]{};
    float uv[2]{};
    float normal[3]{};
};

/**
 * @brief Converts the Asset-owned neutral payload into the runtime renderer layout.
 *
 * Asset decoding intentionally stops at MeshPayload. This adapter is the only
 * runtime-owned conversion point that introduces the renderer vertex contract.
 */
[[nodiscard]] foundation::ResultValue<std::vector<RuntimeMeshVertex>> make_renderer_vertices(
    const aeg::asset::MeshPayload& mesh);

}  // namespace aeg::application::runtime
