#pragma once

#include <memory>
#include <utility>

#include "asset_package_reader.hpp"
#include "ecs.hpp"
#include "reflection.hpp"
#include "runtime_asset_resolver.hpp"
#include "scene_asset.hpp"
#include "world_scope.hpp"

namespace aeg::application::runtime {

/** Owns the World and Scope created from one packaged startup Scene. */
struct RuntimeSceneLoadResult final {
    RuntimeSceneLoadResult() = default;
    RuntimeSceneLoadResult(const RuntimeSceneLoadResult&) = delete;
    RuntimeSceneLoadResult& operator=(const RuntimeSceneLoadResult&) = delete;
    RuntimeSceneLoadResult(RuntimeSceneLoadResult&& other) noexcept
        : asset(std::move(other.asset)),
          instance(other.instance),
          startup_camera(other.startup_camera),
          world(std::move(other.world)),
          scope(std::move(other.scope)),
          world_manager(std::move(other.world_manager)) {}
    RuntimeSceneLoadResult& operator=(RuntimeSceneLoadResult&& other) noexcept {
        asset = std::move(other.asset);
        instance = other.instance;
        startup_camera = other.startup_camera;
        world = std::move(other.world);
        scope = std::move(other.scope);
        world_manager = std::move(other.world_manager);
        return *this;
    }
    scene::SceneAsset asset{};
    scene::SceneInstance instance{};
    /** @brief First startup entity carrying the Runtime Camera component, if one was authored. */
    ecs::EntityId startup_camera{};
    std::shared_ptr<ecs::World> world{};
    std::unique_ptr<world::WorldScope> scope{};
    std::shared_ptr<ecs::WorldManager> world_manager{};
};

class RuntimeSceneLoader final {
public:
    /** Resolves the same stable entity UUID used by Scene relocations and explicit World command routes. */
    [[nodiscard]] static scene::SceneEntityId scene_entity_id(foundation::Uuid uuid) noexcept;
    [[nodiscard]] static foundation::ResultValue<RuntimeSceneLoadResult> load_startup_scene(
        const asset::AssetPackageReader& package, foundation::AssetId startup_scene,
        reflection::TypeRegistry& reflection_registry, RuntimeAssetResolver* resolver = nullptr);
};

}  // namespace aeg::application::runtime
