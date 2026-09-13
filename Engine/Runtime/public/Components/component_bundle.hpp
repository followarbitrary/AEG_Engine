/**
 * @file component_bundle.hpp
 * @brief Coordinated Reflection and ECS registration for Runtime render components.
 */
#pragma once

#include "component_reflection.hpp"
#include "component_registration.hpp"

namespace aeg::engine::components {
/** @brief Registers the shared schemas and reflected storage required by one Runtime World. */
[[nodiscard]] foundation::Result register_render_component_bundle(ecs::World& world,
                                                                  reflection::TypeRegistry& registry);
}  // namespace aeg::engine::components
