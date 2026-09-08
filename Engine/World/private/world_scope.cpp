/**
 * @file world_scope.cpp
 * @brief Implements World Scope ownership and snapshot bridge creation.
 */
#include "../public/world_scope.hpp"

#include <exception>
#include <mutex>
#include <new>
#include <string>
#include <utility>

namespace aeg::world {
namespace {
using foundation::invalid_argument;
using foundation::invalid_state;
using foundation::not_ready;
using foundation::out_of_memory;

[[nodiscard]] pipeline::PipelineInstanceId make_world_pipeline_instance_id(WorldHandle handle) noexcept {
    uint64_t value = 14695981039346656037ull;
    value ^= handle.index;
    value *= 1099511628211ull;
    value ^= handle.generation;
    value *= 1099511628211ull;
    return pipeline::PipelineInstanceId(value == 0 ? 1 : value);
}

}  // namespace

WorldFrameScope::WorldFrameScope(WorldFrameKey key, pipeline::ArtifactId snapshot_artifact,
                                 pipeline::ArtifactTypeId snapshot_type, uint32_t snapshot_version,
                                 std::unique_ptr<pipeline::ArtifactScopeBridge> bridge,
                                 std::weak_ptr<std::atomic_bool> source_active) noexcept
    : key_(key),
      snapshot_artifact_(snapshot_artifact),
      snapshot_type_(snapshot_type),
      snapshot_version_(snapshot_version),
      bridge_(std::move(bridge)),
      source_active_(std::move(source_active)) {}

WorldFrameScope::~WorldFrameScope() = default;

WorldFrameScope::WorldFrameScope(WorldFrameScope&& other) noexcept
    : key_(other.key_),
      snapshot_artifact_(other.snapshot_artifact_),
      snapshot_type_(other.snapshot_type_),
      snapshot_version_(other.snapshot_version_),
      bridge_(std::move(other.bridge_)),
      source_active_(std::move(other.source_active_)) {
    other.key_ = {};
    other.snapshot_artifact_ = {};
    other.snapshot_type_ = {};
    other.snapshot_version_ = 0;
    other.source_active_.reset();
}

WorldFrameScope& WorldFrameScope::operator=(WorldFrameScope&& other) noexcept {
    if (this != &other) {
        key_ = other.key_;
        snapshot_artifact_ = other.snapshot_artifact_;
        snapshot_type_ = other.snapshot_type_;
        snapshot_version_ = other.snapshot_version_;
        bridge_ = std::move(other.bridge_);
        source_active_ = std::move(other.source_active_);
        other.key_ = {};
        other.snapshot_artifact_ = {};
        other.snapshot_type_ = {};
        other.snapshot_version_ = 0;
        other.source_active_.reset();
    }
    return *this;
}

bool WorldFrameScope::is_valid() const noexcept {
    return key_.is_valid() && bridge_ != nullptr;
}

foundation::Result WorldFrameScope::capture() {
    if (!is_valid()) {
        return invalid_state("World frame scope is not initialized");
    }
    const std::shared_ptr<std::atomic_bool> source_active = source_active_.lock();
    if (source_active == nullptr || !source_active->load(std::memory_order_acquire)) {
        return invalid_state("World Scope is no longer active");
    }
    return bridge_->capture();
}

foundation::ResultValue<pipeline::ArtifactView> WorldFrameScope::read_snapshot() const {
    if (!is_valid()) {
        return {invalid_state("World frame scope is not initialized"), {}};
    }
    return bridge_->read(snapshot_artifact_, snapshot_type_, snapshot_version_);
}

WorldScope::WorldScope(std::shared_ptr<ecs::World> state, pipeline::PipelineRegistry& registry,
                       const WorldScopeDesc& desc)
    : state_(std::move(state)),
      pipeline_host_(pipeline::PipelineScope::kWorld,
                     desc.pipeline_instance.is_valid()
                         ? desc.pipeline_instance
                         : make_world_pipeline_instance_id(state_ == nullptr ? WorldHandle{} : state_->handle()),
                     state_.get()),
      registry_(&registry),
      pipeline_id_(desc.pipeline),
      snapshot_artifact_(desc.snapshot_artifact),
      snapshot_type_(desc.snapshot_type),
      snapshot_version_(desc.snapshot_version) {}

WorldScope::~WorldScope() {
    if (active_token_ != nullptr) {
        active_token_->store(false, std::memory_order_release);
        active_token_.reset();
    }
    if (pipeline_host_.is_active()) {
        static_cast<void>(pipeline_host_.deactivate({}));
    }
}

foundation::Result WorldScope::initialize(pipeline::CapabilityMask available_capabilities) {
    if (initialized_) {
        return invalid_state("World Scope is already initialized");
    }
    if (!handle().is_valid()) {
        return invalid_argument("World Scope requires a valid WorldManager-owned World");
    }
    if (registry_ == nullptr) {
        return invalid_state("World Scope has no pipeline registry");
    }
    if (!snapshot_artifact_.is_valid() || !snapshot_type_.is_valid() || snapshot_version_ == 0) {
        return invalid_argument("World snapshot artifact contract is invalid");
    }

    try {
        active_token_ = std::make_shared<std::atomic_bool>(false);
    } catch (const std::bad_alloc&) {
        return out_of_memory("World Scope activity token allocation failed");
    }

    const pipeline::ArtifactDescriptor snapshot_descriptor{
        snapshot_artifact_,
        snapshot_type_,
        pipeline::PipelineScope::kWorld,
        pipeline::ArtifactStorage::kImmutableSnapshot,
        pipeline::ArtifactLifetime::kWorld,
        pipeline::OwnershipPolicy::kSharedImmutable,
        snapshot_version_,
        0,
        false,
    };
    if (const foundation::Result result = pipeline_host_.artifacts().install_contract(snapshot_descriptor); !result) {
        return result;
    }

    if (pipeline_id_.is_valid()) {
        if (const foundation::Result result = pipeline_host_.bind_pipeline(pipeline_id_); !result) {
            return result;
        }
        if (const foundation::Result result = pipeline_host_.rebuild(*registry_, available_capabilities); !result) {
            return result;
        }
    }
    initialized_ = true;
    active_token_->store(true, std::memory_order_release);
    return foundation::Result::success();
}

foundation::Result WorldScope::activate() {
    if (!initialized_) {
        return invalid_state("World Scope is not initialized");
    }
    if (pipeline_host_.active_plan_snapshot() == nullptr) {
        return foundation::Result::success();
    }
    if (pipeline_host_.is_active()) {
        return foundation::Result::success();
    }
    return pipeline_host_.activate({});
}

foundation::Result WorldScope::deactivate() {
    if (!pipeline_host_.is_active()) {
        return foundation::Result::success();
    }
    return pipeline_host_.deactivate({});
}

foundation::Result WorldScope::shutdown() {
    if (active_token_ != nullptr) {
        active_token_->store(false, std::memory_order_release);
        active_token_.reset();
    }
    foundation::Result first_error = foundation::Result::success();
    if (const foundation::Result result = deactivate(); !result) {
        first_error = result;
    }
    const foundation::Result release_result =
        pipeline_host_.artifacts().release_lifetime(pipeline::ArtifactLifetime::kWorld);
    if (!release_result && first_error) {
        first_error = release_result;
    }
    {
        std::unique_lock lock(snapshot_mutex_);
        latest_snapshot_.reset();
    }
    command_buffer_.clear();
    initialized_ = false;
    return first_error;
}

foundation::Result WorldScope::bind_pipeline(pipeline::PipelineId pipeline) {
    if (!initialized_) {
        return invalid_state("World Scope is not initialized");
    }
    if (!pipeline.is_valid()) {
        return invalid_argument("World pipeline id is invalid");
    }
    if (const foundation::Result result = pipeline_host_.bind_pipeline(pipeline); !result) {
        return result;
    }
    pipeline_id_ = pipeline;
    return foundation::Result::success();
}

foundation::Result WorldScope::rebuild(pipeline::CapabilityMask available_capabilities) {
    if (!initialized_) {
        return invalid_state("World Scope is not initialized");
    }
    if (registry_ == nullptr) {
        return invalid_state("World Scope has no pipeline registry");
    }
    if (!pipeline_id_.is_valid()) {
        return invalid_state("World Scope has no bound pipeline");
    }
    return pipeline_host_.rebuild(*registry_, available_capabilities);
}

foundation::Result WorldScope::apply_pending_plan() {
    if (!initialized_) {
        return invalid_state("World Scope is not initialized");
    }
    return pipeline_host_.apply_pending_plan();
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
    if (const foundation::Result result = pipeline_host_.artifacts().publish(
            snapshot_artifact_, pipeline::ArtifactPayload::owned(snapshot_type_, std::move(artifact_snapshot), false));
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

foundation::ResultValue<std::unique_ptr<pipeline::ArtifactScopeBridge>> WorldScope::create_frame_bridge() const {
    if (!initialized_) {
        return {invalid_state("World Scope is not initialized"), {}};
    }
    try {
        auto bridge = std::make_unique<pipeline::ArtifactScopeBridge>(
            pipeline::PipelineScope::kWorld, pipeline::PipelineScope::kFrame, pipeline_host_.artifacts());
        if (const foundation::Result result = bridge->expose(snapshot_artifact_); !result) {
            return {result, {}};
        }
        return {foundation::Result::success(), std::move(bridge)};
    } catch (const std::bad_alloc&) {
        return {out_of_memory("World-to-Frame bridge allocation failed"), {}};
    }
}

foundation::Result WorldScope::capture_frame_bridge(pipeline::ArtifactScopeBridge& bridge) const {
    if (!initialized_) {
        return invalid_state("World Scope is not initialized");
    }
    if (bridge.source_scope() != pipeline::PipelineScope::kWorld ||
        bridge.target_scope() != pipeline::PipelineScope::kFrame || !bridge.is_bound_to(pipeline_host_.artifacts())) {
        return invalid_argument("World frame bridge has an invalid direction");
    }
    return bridge.capture();
}

foundation::ResultValue<WorldFrameScope> WorldScope::create_frame_scope(uint64_t frame_number, uint32_t view_id) const {
    if (!initialized_) {
        return {invalid_state("World Scope is not initialized"), {}};
    }
    foundation::ResultValue<std::unique_ptr<pipeline::ArtifactScopeBridge>> bridge = create_frame_bridge();
    if (!bridge) {
        return {bridge.result, {}};
    }
    return {foundation::Result::success(),
            WorldFrameScope(WorldFrameKey{frame_number, handle(), view_id}, snapshot_artifact_, snapshot_type_,
                            snapshot_version_, std::move(bridge.value), active_token_)};
}
}  // namespace aeg::world
