/**
 * @file world_scope.hpp
 * @brief Aggregate World ownership and immutable per-Frame snapshot views.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <shared_mutex>

#include "ecs.hpp"

namespace aeg::world {
using WorldHandle = ecs::WorldHandle;
namespace detail { struct WorldSnapshotExecution; struct WorldGenerationAccess; }

/** @brief Identity of one World view consumed during one output frame. */
struct WorldFrameKey final {
    uint64_t frame_number{0};
    WorldHandle world{};
    uint32_t view_id{0};

    [[nodiscard]] constexpr bool is_valid() const noexcept { return world.is_valid(); }
    friend constexpr bool operator==(WorldFrameKey, WorldFrameKey) = default;
};

class WorldScope;

/** @brief Frame-local immutable view of a committed World snapshot. */
class WorldFrameScope final {
public:
    WorldFrameScope() noexcept;
    ~WorldFrameScope();

    WorldFrameScope(const WorldFrameScope&) = delete;
    WorldFrameScope& operator=(const WorldFrameScope&) = delete;
    WorldFrameScope(WorldFrameScope&& other) noexcept;
    WorldFrameScope& operator=(WorldFrameScope&& other) noexcept;

    [[nodiscard]] bool is_valid() const noexcept;
    [[nodiscard]] const WorldFrameKey& key() const noexcept { return key_; }
    /** @brief Captures the snapshot published by this view's live World generation. */
    [[nodiscard]] foundation::Result capture();
    /** @brief Returns an owning immutable value, which shares the captured snapshot data. */
    [[nodiscard]] foundation::ResultValue<ecs::WorldSnapshot> read_snapshot() const;

private:
    struct Impl;
    WorldFrameScope(WorldFrameKey key, std::unique_ptr<Impl> impl) noexcept;

    WorldFrameKey key_{};
    std::unique_ptr<Impl> impl_{};

    friend class WorldScope;
    friend struct detail::WorldSnapshotExecution;
};

/** @brief Owns one mutable ECS aggregate, its commands and committed snapshot publication. */
class WorldScope final {
public:
    /** @brief Binds the aggregate to an existing WorldManager-owned World. */
    explicit WorldScope(std::shared_ptr<ecs::World> state);
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
    [[nodiscard]] bool is_initialized() const noexcept { return initialized_; }

    /** @brief Installs the canonical immutable World snapshot publication contract. */
    [[nodiscard]] foundation::Result initialize();
    /** @brief Retires this generation and clears unpublished commands and source snapshot ownership. */
    [[nodiscard]] foundation::Result shutdown();
    /** @brief Applies queued commands on the World owner thread. */
    [[nodiscard]] foundation::Result apply_commands(ecs::WorldCommandBuffer& commands,
                                                    uint32_t max_commands = std::numeric_limits<uint32_t>::max());
    /** @brief Commits ECS state and publishes its immutable World snapshot. */
    [[nodiscard]] foundation::ResultValue<std::shared_ptr<const ecs::WorldSnapshot>> commit(uint64_t frame_number = 0,
                                                                                            uint64_t fixed_tick = 0);
    /** @brief Publishes an already committed snapshot after validating its World and current revision. */
    [[nodiscard]] foundation::Result publish_snapshot(const ecs::WorldSnapshot& snapshot);
    /** @brief Returns the most recently committed snapshot, if one exists. */
    [[nodiscard]] foundation::ResultValue<std::shared_ptr<const ecs::WorldSnapshot>> latest_snapshot() const;
    /** @brief Creates a read-only view identified by frame number and view ID. */
    [[nodiscard]] foundation::ResultValue<WorldFrameScope> create_frame_scope(uint64_t frame_number,
                                                                              uint32_t view_id = 0) const;

private:
    struct Impl;
    [[nodiscard]] foundation::Result publish_snapshot_value(std::shared_ptr<const ecs::WorldSnapshot> snapshot);

    std::shared_ptr<ecs::World> state_{};
    ecs::WorldCommandBuffer command_buffer_{};
    std::unique_ptr<Impl> impl_{};
    mutable std::shared_mutex snapshot_mutex_;
    std::shared_ptr<const ecs::WorldSnapshot> latest_snapshot_{};
    std::shared_ptr<std::atomic_bool> active_token_{};
    bool initialized_{false};

    friend struct detail::WorldSnapshotExecution;
    friend struct detail::WorldGenerationAccess;
};
}  // namespace aeg::world
