/**
 * @file component_ids.hpp
 * @brief Stable logical identities for the built-in Runtime component set.
 */
#pragma once

#include <cstdint>
#include <string_view>

namespace aeg::engine::components {
/** @brief Stable ECS and Reflection identity for the transform component. */
extern const std::string_view kTransformName;
/** @brief Stable ECS and Reflection identity for the camera component. */
extern const std::string_view kCameraName;
/** @brief Stable ECS and Reflection identity for the mesh renderer component. */
extern const std::string_view kMeshRendererName;
/** @brief Stable ECS and Reflection identity for world-space runtime text. */
extern const std::string_view kTextRendererName;
/** @brief Stable ECS and Reflection identity for the minimal directional light. */
extern const std::string_view kDirectionalLightName;

/** @brief Stable field type identity for the engine's aligned three-float vector encoding. */
extern const std::string_view kVec3TypeName;
/** @brief Stable field type identity for the engine's XYZW quaternion encoding. */
extern const std::string_view kQuatTypeName;

/** @brief Current schema version shared by the built-in Runtime render components. */
extern const uint32_t kRenderComponentSchemaVersion;
}  // namespace aeg::engine::components
