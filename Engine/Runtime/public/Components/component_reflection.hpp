/**
 * @file component_reflection.hpp
 * @brief Reflection schema entry points for the built-in Runtime render components.
 */
#pragma once

#include "reflection.hpp"

namespace aeg::engine::components {
/** @brief Returns the stable reflection descriptor for the Transform component. */
[[nodiscard]] foundation::ResultValue<reflection::TypeDescriptor> transform_reflection_descriptor();

/** @brief Returns the stable reflection descriptor for the Camera component. */
[[nodiscard]] foundation::ResultValue<reflection::TypeDescriptor> camera_reflection_descriptor();

/** @brief Returns the stable reflection descriptor for the MeshRenderer component. */
[[nodiscard]] foundation::ResultValue<reflection::TypeDescriptor> mesh_renderer_reflection_descriptor();

/** @brief Registers compatible Runtime component schemas in the supplied registry. */
[[nodiscard]] foundation::Result register_render_component_reflection(reflection::TypeRegistry& registry);
}  // namespace aeg::engine::components
