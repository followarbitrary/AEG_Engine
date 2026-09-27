/**
 * @file runtime_bootstrap.hpp
 * @brief Package-only game startup boundary for static game executables.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "game_module.hpp"
#include "game_runtime.hpp"
#include "result.hpp"
#include "runtime_asset_resolver.hpp"
#include "window.hpp"

namespace aeg::engine {
class EngineFrameOrchestrator;
struct EngineFrameDesc;
}  // namespace aeg::engine
namespace aeg::render_graph {
class FrameContext;
class RenderGraph;
}  // namespace aeg::render_graph
namespace aeg::renderer {
class Renderer;
class RenderResourceResolver;
}  // namespace aeg::renderer
namespace aeg::world {
class WorldScope;
}

namespace aeg::application::runtime {

struct RuntimeBuildIdentity final {
    std::string build_id{};
    std::string target{};
    std::string configuration{};
};

/** @brief Optional Application-owned frame resources used for orchestrator execution. */
struct RuntimeFrameHostResources final {
    render_graph::FrameContext* frame_context{nullptr};
    render_graph::RenderGraph* render_graph{nullptr};
    renderer::Renderer* renderer{nullptr};
    /** @brief Caller-owned upload dependencies borrowed by RuntimeAssetResolver. */
    RuntimeAssetResolver::UploadContext* upload_context{nullptr};
    /** @brief Require the orchestrator to resolve renderer resources for this host. */
    bool require_resolved_render_resources{false};

    /** @brief Applies this host's resolver contract to an orchestrator frame description. */
    void apply_frame_desc(engine::EngineFrameDesc&, const renderer::RenderResourceResolver*) const noexcept;
};

struct RuntimeBootRequest final {
    std::filesystem::path manifest_path{};
    std::filesystem::path package_path{};
    uint32_t max_frames{};
    std::filesystem::path trace_path{};
    /** @brief Owned-host capture of the latest successful frame as binary PPM (P6, RGB8). */
    std::filesystem::path readback_path{};
    bool headless{false};
    bool offscreen{false};
    /** @brief Let RuntimeBootstrap own a native window and frame resources for windowed runs. */
    bool owned_host{false};
    std::string window_title{"AEG Game"};
    uint32_t width{1280};
    uint32_t height{720};
    float fixed_delta_seconds{1.0f / 60.0f};
    float max_frame_delta_seconds{0.25f};
    uint32_t max_fixed_steps_per_frame{8};
    bool vsync{true};
    /** @brief Optional explicit startup Scene identity for package-only requests. */
    asset::AssetId startup_scene{};
    /** @brief Shader asset identity expected by the fixed runtime material ABI. */
    asset::AssetId material_shader{};
    /** @brief Owned-host opaque shader package; empty uses the build's core shader package when available. */
    std::filesystem::path material_shader_package{};
    /** @brief Optional owned-host depth-only mesh shader package. */
    std::filesystem::path depth_mesh_shader_package{};
    /** @brief Optional owned-host GBuffer mesh shader package. */
    std::filesystem::path gbuffer_mesh_shader_package{};
    /** @brief Optional owned-host forward shadow shader package. */
    std::filesystem::path forward_shadow_shader_package{};
    /** @brief Optional owned-host forward clustered-light shader package. */
    std::filesystem::path forward_clustered_shader_package{};
    /** @brief Optional owned-host light-culling compute shader package. */
    std::filesystem::path light_culling_shader_package{};
    /** @brief Optional owned-host deferred-lighting shader package. */
    std::filesystem::path deferred_lighting_shader_package{};
    /** @brief Optional owned-host deferred-shadow shader package. */
    std::filesystem::path deferred_shadow_shader_package{};
    /** @brief Optional owned-host sky/post-process shader package. */
    std::filesystem::path post_process_shader_package{};
    /**
     * @brief Optional borrowed platform event source owned by the Application host.
     *
     * RuntimeBootstrap never creates or destroys this object. When supplied,
     * it is polled at the start of each frame and semantic events are routed
     * through submit_event() before InputSystem::begin_frame().
     */
    platform::IWindowSystem* window_system{nullptr};
    /** @brief Borrowed Application-owned frame resources; null keeps package-only execution. */
    RuntimeFrameHostResources* frame_host{nullptr};
    struct Clock final {
        double (*now_seconds)(void*) noexcept {nullptr};
        void* user_data{nullptr};
    } clock{};
};

enum class RuntimeMode : uint8_t { kStopped, kPlaying, kPaused };

using GameConfigurationCallback = foundation::Result (*)(RuntimeBootRequest&);

class RuntimeBootstrap final {
public:
    RuntimeBootstrap();
    ~RuntimeBootstrap();
    RuntimeBootstrap(const RuntimeBootstrap&) = delete;
    RuntimeBootstrap& operator=(const RuntimeBootstrap&) = delete;

    [[nodiscard]] foundation::Result initialize(const RuntimeBootRequest&, module::ModuleDefinition,
                                                const RuntimeBuildIdentity&);
    [[nodiscard]] foundation::Result initialize(const RuntimeBootRequest&, module::ModuleDefinition,
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
    /** @brief Number of startup Pipeline assets decoded and prepared from the package. */
    [[nodiscard]] std::size_t startup_pipeline_count() const noexcept;
    /** @brief Borrowed startup World Scope, if the package carried a decoded Scene. */
    [[nodiscard]] world::WorldScope* startup_world_scope() const noexcept;
    /** @brief Borrowed active frame host; valid only while the bootstrap is initialized. */
    [[nodiscard]] const RuntimeFrameHostResources* frame_host_resources() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_{};
};

/** @brief Applies project configuration, boots a static module, and runs bounded frames. */
[[nodiscard]] int run_game(int argc, char** argv, module::ModuleDefinition definition, const RuntimeBuildIdentity&,
                           GameConfigurationCallback configure = nullptr);

}  // namespace aeg::application::runtime
