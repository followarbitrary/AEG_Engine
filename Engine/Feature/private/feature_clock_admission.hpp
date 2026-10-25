/** Engine-owned Clock approval and actual-owner workload checks. Not an SDK approval API. */
#pragma once

#include <cstddef>
#include <memory>
#include <span>

#include "feature_registry.hpp"

namespace aeg::feature::detail {
struct ClockHostLimits final {
    Version capability_version{1, 0, 0};
    FeatureDomainMask allowed_domains{kAllFeatureDomains};
    uint32_t max_frequency_hz{192000};
    uint32_t max_backlog{4096};
    size_t max_bindings{4096};
    uint64_t max_relative_deadline_ns{600'000'000'000};
    uint64_t max_plan_budget_ns{1'000'000'000};
    uint64_t max_domain_automatic_budget_per_second_ns{1'000'000'000};
    /** Only the Tooling host opts into its fixed private background Clock. */
    bool allow_tooling_background{false};
};

struct ClockHostContext final {
    explicit ClockHostContext(ClockHostLimits value) : limits(std::move(value)) {}
    const ClockHostLimits limits;
};

struct ClockBindingKey final {
    ClockId id{};
    Version version{};
    friend bool operator==(const ClockBindingKey&, const ClockBindingKey&) = default;
};
struct ClockBindingChange final { ClockBindingKey key{}; bool inserted{false}; };
struct ClockWorkDemand final {
    /** Nonzero collection-local owner identity; distinct instances stay distinct. */
    uint64_t identity{0};
    FeatureContract contract{};
    OwnerPath owner_path{};
};

[[nodiscard]] foundation::ResultValue<ClockHostContextPtr> make_clock_host_context(const ClockHostLimits&);
[[nodiscard]] foundation::ResultValue<ClockBindingChange> admit_clock_request(FeatureRegistry&, const FeatureContract&);
[[nodiscard]] foundation::Result rollback_new_clock_bindings(FeatureRegistry&, std::span<const ClockBindingKey>);
[[nodiscard]] foundation::Result validate_clock_workload(std::span<const ClockWorkDemand>, const FeatureRegistry&);
/** Consumes an existing host binding; never grants approval. */
[[nodiscard]] foundation::Result validate_admitted_clock(const FeatureRegistry&, const FeatureContract&);
[[nodiscard]] bool is_automatic_clock_work(const FeatureContract&) noexcept;
}  // namespace aeg::feature::detail

namespace aeg::feature {
struct ClockHost::State final {
    explicit State(detail::ClockHostContextPtr value) : context(std::move(value)) {}
    const detail::ClockHostContextPtr context;
};
}  // namespace aeg::feature
