/** @brief Engine-private leases for the actual initialized World Scope generation. */
#pragma once

#include <utility>

#include "../public/world_scope.hpp"

namespace aeg::world::detail {

struct WorldGenerationAccess;

/** Weak ownership cannot keep a retired Scope generation active or revive it after reinitialization. */
class WorldGenerationBinding final {
public:
    WorldGenerationBinding() noexcept = default;

private:
    const WorldScope* scope_{nullptr};
    WorldHandle handle_{};
    std::weak_ptr<ecs::World> world_{};
    std::weak_ptr<std::atomic_bool> active_{};
    friend struct WorldGenerationAccess;
};

struct WorldGenerationAccess final {
    [[nodiscard]] static foundation::ResultValue<WorldGenerationBinding> capture(const WorldScope& scope) {
        if (!scope.state_ || !scope.state_->is_owner_thread())
            return {foundation::invalid_state("World generation binding requires the actual ECS owner thread"), {}};
        if (!scope.initialized_ || !scope.active_token_ ||
            !scope.active_token_->load(std::memory_order_acquire) || !scope.handle().is_valid())
            return {foundation::invalid_state("World generation binding requires an initialized Scope"), {}};
        WorldGenerationBinding binding;
        binding.scope_ = &scope;
        binding.handle_ = scope.handle();
        binding.world_ = scope.state_;
        binding.active_ = scope.active_token_;
        return {foundation::Result::success(), std::move(binding)};
    }

    [[nodiscard]] static foundation::ResultValue<std::shared_ptr<ecs::World>> lock(
        const WorldScope& scope, const WorldGenerationBinding& binding) {
        if (binding.scope_ != &scope)
            return {foundation::invalid_argument("World generation binding belongs to another Scope"), {}};
        auto world = binding.world_.lock();
        const auto active = binding.active_.lock();
        if (!world || !active || !active->load(std::memory_order_acquire))
            return {foundation::invalid_state("World generation binding has retired"), {}};
        if (!world->is_owner_thread())
            return {foundation::invalid_state("World generation access requires its actual ECS owner thread"), {}};
        if (!scope.initialized_ || scope.state_ != world || scope.active_token_ != active ||
            scope.handle() != binding.handle_ || world->handle() != binding.handle_)
            return {foundation::invalid_state("World generation binding no longer matches its actual owner"), {}};
        return {foundation::Result::success(), std::move(world)};
    }
};

}  // namespace aeg::world::detail
