/**
 * @file world_scope.cpp
 * @brief Implements aggregate World ownership and immutable snapshot capture.
 */
#include "../public/world_scope.hpp"
#include "world_snapshot_execution.hpp"

#include <exception>
#include <mutex>
#include <new>
#include <utility>

namespace aeg::world {
namespace {
using foundation::invalid_argument;
using foundation::invalid_state;
using foundation::not_ready;
using foundation::out_of_memory;

// Frame scopes pin the registry storage without retaining the mutable ECS World.
struct WorldSnapshotSource final {
    pipeline::ArtifactRegistry artifacts{pipeline::PipelineScope::kWorld};
};
} // namespace

struct WorldScope::Impl final {
    std::shared_ptr<WorldSnapshotSource> source{};
};

struct WorldFrameScope::Impl final {
    std::shared_ptr<WorldSnapshotSource> source_owner{};
    std::unique_ptr<pipeline::ArtifactScopeBridge> bridge{};
    std::weak_ptr<std::atomic_bool> source_active{};
};

WorldFrameScope::WorldFrameScope() noexcept = default;
WorldFrameScope::WorldFrameScope(WorldFrameKey key, std::unique_ptr<Impl> impl) noexcept
    : key_(key), impl_(std::move(impl)) {}
WorldFrameScope::~WorldFrameScope() = default;

WorldFrameScope::WorldFrameScope(WorldFrameScope&& other) noexcept
    : key_(other.key_), impl_(std::move(other.impl_)) {
    other.key_ = {};
}

WorldFrameScope& WorldFrameScope::operator=(WorldFrameScope&& other) noexcept {
    if (this != &other) {
        key_ = other.key_;
        impl_ = std::move(other.impl_);
        other.key_ = {};
    }
    return *this;
}

bool WorldFrameScope::is_valid() const noexcept {
    return key_.is_valid() && impl_ != nullptr && impl_->bridge != nullptr;
}

foundation::Result WorldFrameScope::capture() {
    if (!is_valid()) return invalid_state("World frame scope is not initialized");
    const auto source_active = impl_->source_active.lock();
    if (source_active == nullptr || !source_active->load(std::memory_order_acquire))
        return invalid_state("World Scope is no longer active");
    return impl_->bridge->capture();
}

foundation::ResultValue<ecs::WorldSnapshot> WorldFrameScope::read_snapshot() const {
    if (!is_valid()) return {invalid_state("World frame scope is not initialized"), {}};
    const auto view = impl_->bridge->read(detail::kWorldSnapshotArtifactId, detail::kWorldSnapshotArtifactTypeId,
                                         detail::kWorldSnapshotArtifactVersion);
    if (!view) return {view.result, {}};
    const auto* snapshot = view.value.as<ecs::WorldSnapshot>(detail::kWorldSnapshotArtifactTypeId);
    if (snapshot == nullptr) return {invalid_state("World frame scope does not contain a WorldSnapshot"), {}};
    // WorldSnapshot copies its shared immutable SnapshotData; no allocation or borrowed payload escapes.
    return {foundation::Result::success(), *snapshot};
}

WorldScope::WorldScope(std::shared_ptr<ecs::World> state)
    : state_(std::move(state)), impl_(std::make_unique<Impl>()) {}

WorldScope::~WorldScope() {
    if (active_token_ != nullptr) {
        active_token_->store(false, std::memory_order_release);
        active_token_.reset();
    }
}

foundation::Result WorldScope::initialize() {
    if (initialized_) return invalid_state("World Scope is already initialized");
    if (!handle().is_valid()) return invalid_argument("World Scope requires a valid WorldManager-owned World");
    std::shared_ptr<WorldSnapshotSource> source;
    std::shared_ptr<std::atomic_bool> token;
    try {
        source = std::make_shared<WorldSnapshotSource>();
        token = std::make_shared<std::atomic_bool>(false);
    } catch (const std::bad_alloc&) {
        return out_of_memory("World Scope snapshot storage allocation failed");
    }
    const pipeline::ArtifactDescriptor snapshot_descriptor{
        detail::kWorldSnapshotArtifactId,
        detail::kWorldSnapshotArtifactTypeId,
        pipeline::PipelineScope::kWorld,
        pipeline::ArtifactStorage::kImmutableSnapshot,
        pipeline::ArtifactLifetime::kWorld,
        pipeline::OwnershipPolicy::kSharedImmutable,
        detail::kWorldSnapshotArtifactVersion,
        0,
        false,
    };
    if (const auto result = source->artifacts.install_contract(snapshot_descriptor); !result) return result;
    impl_->source = std::move(source);
    active_token_ = std::move(token);
    initialized_ = true;
    active_token_->store(true, std::memory_order_release);
    return foundation::Result::success();
}

foundation::Result WorldScope::shutdown() {
    if (active_token_ != nullptr) {
        active_token_->store(false, std::memory_order_release);
        active_token_.reset();
    }
    const auto released = impl_->source != nullptr
        ? impl_->source->artifacts.release_lifetime(pipeline::ArtifactLifetime::kWorld)
        : foundation::Result::success();
    {
        std::unique_lock lock(snapshot_mutex_);
        latest_snapshot_.reset();
    }
    command_buffer_.clear();
    initialized_ = false;
    return released;
}

foundation::Result WorldScope::apply_commands(ecs::WorldCommandBuffer& commands, uint32_t max_commands) {
    if (!initialized_) {
        return invalid_state("World Scope is not initialized");
    }
    return state_->apply_commands(commands, max_commands);
}

foundation::ResultValue<std::shared_ptr<const ecs::WorldSnapshot>> WorldScope::commit(uint64_t frame_number,
                                                                                      uint64_t fixed_tick) {
    if (!initialized_) {
        return {invalid_state("World Scope is not initialized"), {}};
    }
    const foundation::ResultValue<ecs::WorldSnapshot> snapshot = state_->commit(frame_number, fixed_tick);
    if (!snapshot) {
        return {snapshot.result, {}};
    }
    try {
        auto value = std::make_shared<const ecs::WorldSnapshot>(std::move(snapshot.value));
        if (const foundation::Result result = publish_snapshot_value(value); !result) {
            return {result, {}};
        }
        return {foundation::Result::success(), std::move(value)};
    } catch (const std::bad_alloc&) {
        return {out_of_memory("World Scope snapshot ownership allocation failed"), {}};
    } catch (const std::exception& exception) {
        return {foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what()), {}};
    } catch (...) {
        return {foundation::Result::failure(foundation::ResultCode::kUnknown,
                                            "World Scope snapshot publication threw an unknown exception"),
                {}};
    }
}

foundation::Result WorldScope::publish_snapshot(const ecs::WorldSnapshot& snapshot) {
    if (!initialized_) {
        return invalid_state("World Scope is not initialized");
    }
    if (!snapshot.is_valid() || snapshot.world() != handle()) {
        return invalid_argument("World snapshot does not belong to this World Scope");
    }
    try {
        return publish_snapshot_value(std::make_shared<const ecs::WorldSnapshot>(snapshot));
    } catch (const std::bad_alloc&) {
        return out_of_memory("World snapshot Artifact allocation failed");
    } catch (const std::exception& exception) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what());
    } catch (...) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                           "World snapshot Artifact publication threw an unknown exception");
    }
}

foundation::Result WorldScope::publish_snapshot_value(std::shared_ptr<const ecs::WorldSnapshot> snapshot) {
    if (snapshot == nullptr || !snapshot->is_valid() || snapshot->world() != handle()) {
        return invalid_argument("World snapshot does not belong to this World Scope");
    }
    if (snapshot->revision() != state_->revision()) {
        return invalid_state("World snapshot revision is not the current World revision");
    }

    std::unique_lock lock(snapshot_mutex_);
    if (latest_snapshot_ != nullptr && snapshot->revision() < latest_snapshot_->revision()) {
        return invalid_state("World snapshot revision would move publication backwards");
    }

    std::shared_ptr<const ecs::WorldSnapshot> artifact_snapshot = snapshot;
    if (const foundation::Result result = impl_->source->artifacts.publish(
            detail::kWorldSnapshotArtifactId,
            pipeline::ArtifactPayload::owned(detail::kWorldSnapshotArtifactTypeId, std::move(artifact_snapshot), false));
        !result) {
        return result;
    }
    latest_snapshot_ = std::move(snapshot);
    return foundation::Result::success();
}

foundation::ResultValue<std::shared_ptr<const ecs::WorldSnapshot>> WorldScope::latest_snapshot() const {
    std::shared_lock lock(snapshot_mutex_);
    if (latest_snapshot_ == nullptr) {
        return {not_ready("World Scope has not committed a snapshot"), {}};
    }
    return {foundation::Result::success(), latest_snapshot_};
}

foundation::ResultValue<WorldFrameScope> WorldScope::create_frame_scope(uint64_t frame_number, uint32_t view_id) const {
    if (!initialized_) return {invalid_state("World Scope is not initialized"), {}};
    auto bridge = detail::WorldSnapshotExecution::create_frame_bridge(*this);
    if (!bridge) return {bridge.result, {}};
    try {
        auto frame = std::make_unique<WorldFrameScope::Impl>();
        frame->source_owner = impl_->source;
        frame->bridge = std::move(bridge.value);
        frame->source_active = active_token_;
        return {foundation::Result::success(),
                WorldFrameScope(WorldFrameKey{frame_number, handle(), view_id}, std::move(frame))};
    } catch (const std::bad_alloc&) {
        return {out_of_memory("World frame scope allocation failed"), {}};
    }
}

const pipeline::ArtifactScopeBridge* detail::WorldSnapshotExecution::bridge(const WorldFrameScope& frame) noexcept {
    return frame.impl_ != nullptr ? frame.impl_->bridge.get() : nullptr;
}

foundation::ResultValue<std::unique_ptr<pipeline::ArtifactScopeBridge>>
    detail::WorldSnapshotExecution::create_frame_bridge(const WorldScope& scope) {
    if (!scope.initialized_ || scope.impl_->source == nullptr)
        return {invalid_state("World Scope is not initialized"), {}};
    try {
        auto bridge = std::make_unique<pipeline::ArtifactScopeBridge>(
            pipeline::PipelineScope::kWorld, pipeline::PipelineScope::kFrame, scope.impl_->source->artifacts);
        if (const auto result = bridge->expose(kWorldSnapshotArtifactId); !result) return {result, {}};
        return {foundation::Result::success(), std::move(bridge)};
    } catch (const std::bad_alloc&) {
        return {out_of_memory("World-to-Frame bridge allocation failed"), {}};
    }
}

foundation::Result detail::WorldSnapshotExecution::capture_frame_bridge(const WorldScope& scope,
                                                                        pipeline::ArtifactScopeBridge& bridge) {
    if (!scope.initialized_ || scope.impl_->source == nullptr) return invalid_state("World Scope is not initialized");
    if (bridge.source_scope() != pipeline::PipelineScope::kWorld ||
        bridge.target_scope() != pipeline::PipelineScope::kFrame || !bridge.is_bound_to(scope.impl_->source->artifacts))
        return invalid_argument("World frame bridge has an invalid direction");
    return bridge.capture();
}
} // namespace aeg::world
