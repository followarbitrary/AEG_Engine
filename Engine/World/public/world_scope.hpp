/**
 * @file world_scope.hpp
 * @brief World Scope ownership and World-to-Frame snapshot data flow.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <shared_mutex>

#include "ecs.hpp"
#include "pipeline.hpp"

namespace aeg::world {
using WorldHandle = ecs::WorldHandle;

inline constexpr pipeline::ArtifactId kWorldSnapshotArtifactId{"WorldSnapshot"};
inline constexpr pipeline::ArtifactTypeId kWorldSnapshotArtifactTypeId{"WorldSnapshot"};
inline constexpr uint32_t kWorldSnapshotArtifactVersion = 1;

/** @brief Identity of one World view consumed during one output frame. */
struct WorldFrameKey final {
    uint64_t frame_number{0};
    WorldHandle world{};
    uint32_t view_id{0};

    [[nodiscard]] constexpr bool is_valid() const noexcept { return world.is_valid(); }
    friend constexpr bool operator==(WorldFrameKey, WorldFrameKey) = default;
};

/** @brief Construction and artifact settings for one World Scope. */
struct WorldScopeDesc final {
    pipeline::PipelineInstanceId pipeline_instance{};
    pipeline::PipelineId pipeline{};
    pipeline::ArtifactId snapshot_artifact{kWorldSnapshotArtifactId};
    pipeline::ArtifactTypeId snapshot_type{kWorldSnapshotArtifactTypeId};
    uint32_t snapshot_version{kWorldSnapshotArtifactVersion};
};

class WorldScope;

/** @brief Frame-local read-only bridge for one World and one view. */
class WorldFrameScope final {
public:
    WorldFrameScope() noexcept = default;
    ~WorldFrameScope();

    WorldFrameScope(const WorldFrameScope&) = delete;
    WorldFrameScope& operator=(const WorldFrameScope&) = delete;
    WorldFrameScope(WorldFrameScope&& other) noexcept;
    WorldFrameScope& operator=(WorldFrameScope&& other) noexcept;

    [[nodiscard]] bool is_valid() const noexcept;
    [[nodiscard]] const WorldFrameKey& key() const noexcept { return key_; }
    [[nodiscard]] const pipeline::ArtifactScopeBridge* bridge() const noexcept { return bridge_.get(); }

    /** @brief Captures the World snapshot visible to this Frame view. */
    [[nodiscard]] foundation::Result capture();
    /** @brief Reads the captured WorldSnapshot Artifact. */
    [[nodiscard]] foundation::ResultValue<pipeline::ArtifactView> read_snapshot() const;

private:
    WorldFrameScope(WorldFrameKey key, pipeline::ArtifactId snapshot_artifact, pipeline::ArtifactTypeId snapshot_type,
                    uint32_t snapshot_version, std::unique_ptr<pipeline::ArtifactScopeBridge> bridge,
                    std::weak_ptr<std::atomic_bool> source_active) noexcept;

    WorldFrameKey key_{};
    pipeline::ArtifactId snapshot_artifact_{};
    pipeline::ArtifactTypeId snapshot_type_{};
    uint32_t snapshot_version_{0};
    std::unique_ptr<pipeline::ArtifactScopeBridge> bridge_{};
    std::weak_ptr<std::atomic_bool> source_active_{};

    friend class WorldScope;
};

/** @brief Owns one mutable ECS World and its World Scope pipeline instance. */
class WorldScope final {
public:
    /** @brief Binds the Scope to an existing World instance, normally obtained from WorldManager. */
    WorldScope(std::shared_ptr<ecs::World> state, pipeline::PipelineRegistry& registry,
               const WorldScopeDesc& desc = {});
    ~WorldScope();

    WorldScope(const WorldScope&) = delete;
    WorldScope& operator=(const WorldScope&) = delete;
    WorldScope(WorldScope&&) = delete;
    WorldScope& operator=(WorldScope&&) = delete;

    [[nodiscard]] WorldHandle handle() const noexcept { return state_ == nullptr ? WorldHandle{} : state_->handle(); }
    [[nodiscard]] ecs::World& state() noexcept { return *state_; }
    [[nodiscard]] const ecs::World& state() const noexcept { return *state_; }
    [[nodiscard]] const std::shared_ptr<ecs::World>& state_owner() const noexcept { return state_; }
    [[nodiscard]] ecs::WorldCommandBuffer& command_buffer() noexcept { return command_buffer_; }
    [[nodiscard]] const ecs::WorldCommandBuffer& command_buffer() const noexcept { return command_buffer_; }
    [[nodiscard]] pipeline::PipelineScopeHost& pipeline_host() noexcept { return pipeline_host_; }
    [[nodiscard]] const pipeline::PipelineScopeHost& pipeline_host() const noexcept { return pipeline_host_; }
    [[nodiscard]] pipeline::ArtifactId snapshot_artifact() const noexcept { return snapshot_artifact_; }
    [[nodiscard]] pipeline::ArtifactTypeId snapshot_type() const noexcept { return snapshot_type_; }
    [[nodiscard]] uint32_t snapshot_version() const noexcept { return snapshot_version_; }
    [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }
    [[nodiscard]] bool is_pipeline_active() const noexcept { return pipeline_host_.is_active(); }
    [[nodiscard]] bool has_pipeline() const noexcept { return pipeline_id_.is_valid(); }

    /** @brief Installs the World snapshot contract and prepares the optional pipeline plan. */
    [[nodiscard]] foundation::Result initialize(pipeline::CapabilityMask available_capabilities = 0);
    /** @brief Activates the prepared World pipeline instance. */
    [[nodiscard]] foundation::Result activate();
    /** @brief Deactivates the World pipeline instance. */
    [[nodiscard]] foundation::Result deactivate();
    /** @brief Releases World runtime state and deactivates the pipeline. */
    [[nodiscard]] foundation::Result shutdown();

    [[nodiscard]] foundation::Result bind_pipeline(pipeline::PipelineId pipeline);
    [[nodiscard]] foundation::Result rebuild(pipeline::CapabilityMask available_capabilities = 0);
    [[nodiscard]] foundation::Result apply_pending_plan();

    /** @brief Applies queued commands on the World owner thread. */
    [[nodiscard]] foundation::Result apply_commands(ecs::WorldCommandBuffer& commands,
                                                    uint32_t max_commands = std::numeric_limits<uint32_t>::max());
    /** @brief Commits ECS state and publishes the immutable WorldSnapshot Artifact. */
    [[nodiscard]] foundation::ResultValue<std::shared_ptr<const ecs::WorldSnapshot>> commit(uint64_t frame_number = 0,
                                                                                            uint64_t fixed_tick = 0);
    /** @brief Publishes an already committed snapshot after validating its World identity. */
    [[nodiscard]] foundation::Result publish_snapshot(const ecs::WorldSnapshot& snapshot);
    /** @brief Returns the most recently committed snapshot, if one exists. */
    [[nodiscard]] foundation::ResultValue<std::shared_ptr<const ecs::WorldSnapshot>> latest_snapshot() const;

    /** @brief Creates a World-to-Frame bridge without capturing a value yet. */
    [[nodiscard]] foundation::ResultValue<std::unique_ptr<pipeline::ArtifactScopeBridge>> create_frame_bridge() const;
    /** @brief Captures the current World snapshot into an existing Frame bridge. */
    [[nodiscard]] foundation::Result capture_frame_bridge(pipeline::ArtifactScopeBridge& bridge) const;
    /** @brief Creates a Frame view identified by frame number and view ID. */
    [[nodiscard]] foundation::ResultValue<WorldFrameScope> create_frame_scope(uint64_t frame_number,
                                                                              uint32_t view_id = 0) const;

private:
    [[nodiscard]] foundation::Result publish_snapshot_value(std::shared_ptr<const ecs::WorldSnapshot> snapshot);

    std::shared_ptr<ecs::World> state_{};
    ecs::WorldCommandBuffer command_buffer_{};
    pipeline::PipelineScopeHost pipeline_host_;
    pipeline::PipelineRegistry* registry_{nullptr};
    pipeline::PipelineId pipeline_id_{};
    pipeline::ArtifactId snapshot_artifact_{};
    pipeline::ArtifactTypeId snapshot_type_{};
    uint32_t snapshot_version_{0};
    mutable std::shared_mutex snapshot_mutex_;
    std::shared_ptr<const ecs::WorldSnapshot> latest_snapshot_{};
    std::shared_ptr<std::atomic_bool> active_token_{};
    bool initialized_{false};
};
}  // namespace aeg::world
