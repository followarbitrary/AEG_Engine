/**
 * @file game_runtime_bootstrap.hpp
 * @brief Minimal game-facing runtime startup facade.
 *
 * This header is the only Runtime bootstrap contract exposed through
 * AEG::GameRuntime.  Rendering, Pipeline, and Concurrency implementation
 * types stay behind the engine-owned runtime target.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "asset_identity.hpp"
#include "feature_module.hpp"
#include "../game_runtime.hpp"
#include "result.hpp"

namespace aeg::platform {
class IWindowSystem;
struct WindowEvent;
}  // namespace aeg::platform

namespace aeg::world {
class WorldScope;
}  // namespace aeg::world

namespace aeg::application::runtime {
class RuntimeFeatureControl;
struct RuntimeFrameContext;
struct RuntimeFrameHostResources;

struct RuntimeBuildIdentity final {
    std::string build_id{};
    std::string target{};
    std::string configuration{};
};

/** Explicit startup routing from one authored Feature owner to one stable Scene entity. */
struct RuntimeWorldCommandBinding final {
    std::string graph_asset_uuid{};
    feature::FeatureNodeId node_id{};
    /** Root parent path, or parent path + authored root node + descendant definition suffix. */
    feature::OwnerPath owner_path{};
    foundation::AssetId scene_asset{};
    foundation::Uuid scene_entity{};
    feature::SideEffectId effect{};
    uint32_t version{0};
    /** Optional input port receiving the immutable World.Transform.Observation/1 contract. */
    feature::PortId observation_port{};
};

/** @brief Package and host settings accepted by the game runtime facade. */
struct RuntimeBootRequest final {
    std::filesystem::path manifest_path{};
    std::filesystem::path package_path{};
    uint32_t max_frames{};
    std::filesystem::path trace_path{};
    std::filesystem::path readback_path{};
    bool headless{false};
    bool offscreen{false};
    bool owned_host{false};
    std::string window_title{"AEG Game"};
    uint32_t width{1280};
    uint32_t height{720};
    float fixed_delta_seconds{1.0f / 60.0f};
    float max_frame_delta_seconds{0.25f};
    uint32_t max_fixed_steps_per_frame{8};
    bool vsync{true};
    foundation::AssetId startup_scene{};
    /** Host routes are used when the cooked manifest has none. Two nonempty sources are rejected.
     *  Empty sources leave Feature commands pending without an implicit World/entity route. */
    std::vector<RuntimeWorldCommandBinding> world_command_bindings{};
    foundation::AssetId material_shader{};
    std::filesystem::path material_shader_package{};
    std::filesystem::path depth_mesh_shader_package{};
    std::filesystem::path gbuffer_mesh_shader_package{};
    std::filesystem::path forward_shadow_shader_package{};
    std::filesystem::path forward_clustered_shader_package{};
    std::filesystem::path forward_clustered_unshadowed_shader_package{};
    std::filesystem::path light_culling_shader_package{};
    std::filesystem::path deferred_lighting_shader_package{};
    std::filesystem::path deferred_shadow_shader_package{};
    std::filesystem::path post_process_shader_package{};
    platform::IWindowSystem* window_system{nullptr};
    RuntimeFrameHostResources* frame_host{nullptr};
    struct Clock final {
        double (*now_seconds)(void*) noexcept{nullptr};
        void* user_data{nullptr};
    } clock{};
};

enum class RuntimeMode : uint8_t { kStopped, kPlaying, kPaused };

using GameConfigurationCallback = foundation::Result (*)(RuntimeBootRequest&);

/** @brief Feature-only startup object with no rendering or task scheduler API. */
class RuntimeBootstrap final {
public:
    RuntimeBootstrap();
    ~RuntimeBootstrap();
    RuntimeBootstrap(const RuntimeBootstrap&) = delete;
    RuntimeBootstrap& operator=(const RuntimeBootstrap&) = delete;

    [[nodiscard]] foundation::Result initialize(const RuntimeBootRequest&, std::optional<module::FeatureModuleDefinition>,
                                                const RuntimeBuildIdentity&);
    [[nodiscard]] foundation::Result initialize(const RuntimeBootRequest&, std::optional<module::FeatureModuleDefinition>,
                                                const RuntimeBuildIdentity&, const GameRuntimeCallbacks& callbacks);
    void set_callbacks(const GameRuntimeCallbacks& callbacks) noexcept;
    void register_callbacks(const GameRuntimeCallbacks& callbacks) noexcept { set_callbacks(callbacks); }
    [[nodiscard]] RuntimeMode mode() const noexcept;
    void request_quit() noexcept;
    [[nodiscard]] foundation::Result submit_event(const platform::WindowEvent& event);
    [[nodiscard]] const RuntimeFrameContext& last_frame_context() const noexcept;
    [[nodiscard]] foundation::Result tick();
    [[nodiscard]] foundation::Result shutdown();
    [[nodiscard]] uint32_t frame_count() const noexcept;
    [[nodiscard]] std::size_t startup_feature_graph_count() const noexcept;
    [[nodiscard]] world::WorldScope* startup_world_scope() const noexcept;
    [[nodiscard]] const RuntimeFrameHostResources* frame_host_resources() const noexcept;

private:
    friend class RuntimeFeatureControl;
    struct Impl;
    std::unique_ptr<Impl> impl_{};
};

[[nodiscard]] int run_game(int argc, char** argv, std::optional<module::FeatureModuleDefinition> definition,
                           const RuntimeBuildIdentity&, GameConfigurationCallback configure = nullptr);
/** Starts a cooked game by loading its Feature module bundle from GameBuildManifest. */
[[nodiscard]] int run_game(int argc, char** argv, const RuntimeBuildIdentity&,
                           GameConfigurationCallback configure = nullptr);

}  // namespace aeg::application::runtime
