/**
 * @file component_registration.hpp
 * @brief Explicit ECS registration entry points for Runtime render components.
 */
#pragma once

#include "ecs.hpp"
#include "reflection.hpp"

namespace aeg::engine::components {
/** @brief Registers non-reflected storage for ephemeral Runtime render components. */
[[nodiscard]] foundation::Result register_render_components(ecs::World& world);

/** @brief Registers reflected Runtime component storage in one World. */
[[nodiscard]] foundation::Result register_reflected_render_components(ecs::World& world,
                                                                      const reflection::TypeRegistry& registry);
}  // namespace aeg::engine::components
