/** @brief Engine-private access to committed command deliveries, never author-supplied provenance. */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "feature_graph.hpp"

namespace aeg::feature::detail {

inline constexpr uint32_t kFeatureOutboxMaxPendingEntries = kMaxOutboxIntentsPerCommit;
inline constexpr uint64_t kFeatureOutboxMaxPendingBytes = 16ull * 1024 * 1024;

enum class OutboxDeliveryState : uint8_t { kPending, kApplied, kRejected, kCancelled };

struct OutboxReceipt final {
    InvocationToken invocation{};
    uint32_t entry_index{0};
    OutboxDeliveryState state{OutboxDeliveryState::kPending};
    foundation::ResultCode error_code{foundation::ResultCode::kSuccess};
};

struct FeatureOutboxOwner;
struct FeatureOutboxEntry;
struct FeatureOutboxAccess;

/** Owning binding to one actual published owner and accepted command protocol. */
class FeatureOutboxBinding final {
public:
    FeatureOutboxBinding() noexcept = default;
    [[nodiscard]] const FeatureNodeProvenance* source() const noexcept;
    [[nodiscard]] RuntimeGeneration generation() const noexcept;
    [[nodiscard]] uint64_t definition_digest() const noexcept;
    [[nodiscard]] SideEffectId effect() const noexcept { return effect_; }
    [[nodiscard]] uint32_t version() const noexcept { return version_; }

private:
    FeatureOutboxBinding(std::shared_ptr<const FeatureOutboxOwner> owner, SideEffectId effect,
                         uint32_t version) noexcept;
    std::shared_ptr<const FeatureOutboxOwner> owner_{};
    SideEffectId effect_{};
    uint32_t version_{0};
    friend struct FeatureOutboxAccess;
};

/** Immutable committed bytes with a shared terminal receipt; default construction is invalid. */
class FeatureOutboxHandle final {
public:
    FeatureOutboxHandle() noexcept = default;
    [[nodiscard]] const OutboxIntent* intent() const noexcept;
    [[nodiscard]] const InvocationToken* invocation() const noexcept;
    [[nodiscard]] uint32_t entry_index() const noexcept;
    [[nodiscard]] OutboxReceipt receipt() const noexcept;

private:
    explicit FeatureOutboxHandle(std::shared_ptr<FeatureOutboxEntry> entry) noexcept;
    std::shared_ptr<FeatureOutboxEntry> entry_{};
    friend struct FeatureOutboxAccess;
};

/** Owner-thread access. Applications finish typed validation before entering the noexcept apply. */
struct FeatureOutboxAccess final {
    [[nodiscard]] static foundation::ResultValue<FeatureOutboxBinding> bind(
        FeatureRuntime& runtime, FeatureInstanceId instance, SideEffectId effect, uint32_t version);
    [[nodiscard]] static foundation::ResultValue<std::vector<FeatureOutboxHandle>> capture(
        FeatureRuntime& runtime, const FeatureOutboxBinding& binding);
    [[nodiscard]] static foundation::Result consume(
        FeatureRuntime& runtime, const FeatureOutboxBinding& binding, const FeatureOutboxHandle& handle,
        void (*apply)(void*, const OutboxIntent&) noexcept, void* user_data);
    [[nodiscard]] static foundation::Result reject(
        FeatureRuntime& runtime, const FeatureOutboxBinding& binding, const FeatureOutboxHandle& handle,
        foundation::ResultCode reason);

private:
    [[nodiscard]] static foundation::Result validate_binding(
        FeatureRuntime& runtime, const FeatureOutboxBinding& binding);
    [[nodiscard]] static foundation::ResultValue<std::size_t> pending_index(
        FeatureRuntime& runtime, const FeatureOutboxBinding& binding, const FeatureOutboxHandle& handle);
};

}  // namespace aeg::feature::detail
