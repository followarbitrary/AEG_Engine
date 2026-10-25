/** @brief Engine-private leases for projecting host-owned observations into invocation bytes. */
#pragma once

#include <functional>
#include <memory>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "feature_runtime.hpp"

namespace aeg::feature::detail {

/** Borrowed only for the synchronous owner-thread projector call. */
struct FeatureInputContext final {
    FeatureInstanceId owner{};
    RuntimeGeneration generation{};
    ClockId clock{};
    double elapsed_seconds{0.0};
    std::span<const uint8_t> routed_input{};
    std::span<const SideEffectContract> pending_effects{};
};

struct FeatureInputProjection final {
    bool ready{true};
    std::vector<uint8_t> bytes{};
};

using FeatureInputProjector =
    std::function<foundation::ResultValue<FeatureInputProjection>(const FeatureInputContext&)>;

struct FeatureInputLease;
struct FeatureInputAccess;

/** Retirement releases only a marker; it never calls or dereferences the Runtime. */
class FeatureInputBinding final {
public:
    FeatureInputBinding() noexcept = default;
    FeatureInputBinding(FeatureInputBinding&&) noexcept = default;
    FeatureInputBinding& operator=(FeatureInputBinding&&) noexcept = default;
    FeatureInputBinding(const FeatureInputBinding&) = delete;
    FeatureInputBinding& operator=(const FeatureInputBinding&) = delete;

private:
    explicit FeatureInputBinding(std::shared_ptr<const FeatureInputLease> lease) noexcept
        : lease_(std::move(lease)) {}
    std::shared_ptr<const FeatureInputLease> lease_{};
    friend struct FeatureInputAccess;
};

struct FeatureInputState final {
    struct Slot final {
        std::weak_ptr<const FeatureInputLease> lease{};
        RuntimeGeneration generation{};
        uint64_t definition_digest{0};
        PortId port{};
        ValueSchemaId schema{};
        uint32_t version{0};
        FeatureInputProjector projector{};
    };
    std::unordered_map<FeatureInstanceId, Slot, StableIdHash<FeatureInstanceIdTag>> slots{};
};

struct FeatureInputAccess final {
    [[nodiscard]] static foundation::ResultValue<FeatureInputBinding> bind(
        FeatureRuntime& runtime, FeatureInstanceId instance, PortId port,
        ValueSchemaId schema, uint32_t version, FeatureInputProjector projector);
};

}  // namespace aeg::feature::detail
