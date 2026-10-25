#include "feature_clock_admission.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <new>
#include <unordered_map>
#include <unordered_set>

namespace aeg::feature::detail {
struct ClockAdmissionAccess final {
    static const ClockHostContextPtr& host(const FeatureRegistry& registry) { return registry.clock_host_; }
    static auto& bindings(FeatureRegistry& registry) { return registry.clock_bindings_; }
    static const auto& bindings(const FeatureRegistry& registry) { return registry.clock_bindings_; }
    static const auto& definitions(const FeatureRegistry& registry) { return registry.definitions_; }
};

namespace {
constexpr ClockId kToolingClock{"Engine.Tooling.ProjectOperationClock"};
constexpr uint64_t kToolingBackgroundLimitNs = 1'800'000'000'000ull;

bool background(const ClockHostLimits& limits, ClockId id) {
    return limits.allow_tooling_background && id == kToolingClock;
}

foundation::Result validate_request(const FeatureRegistry& registry, const FeatureContract& request) try {
    if (const auto valid = validate_feature_contract(request); !valid) return valid;
    const auto& host = ClockAdmissionAccess::host(registry);
    if (!host) return foundation::invalid_state("Clock request has no host approval context");
    const auto& limits = host->limits;
    const auto& clock = request.clock;
    if (clock.capability_version != limits.capability_version)
        return foundation::unsupported("Clock capability version is not supported by this host");
    if (clock.id == kToolingClock && !limits.allow_tooling_background)
        return foundation::unsupported("Tooling background Clock is not approved by this host");
    const bool is_background = background(limits, clock.id);
    const auto deadline_limit = is_background ? kToolingBackgroundLimitNs : limits.max_relative_deadline_ns;
    const auto budget_limit = is_background ? kToolingBackgroundLimitNs : limits.max_plan_budget_ns;
    if ((request.domains & ~limits.allowed_domains) != 0 || clock.frequency_hz > limits.max_frequency_hz ||
        clock.max_backlog > limits.max_backlog || clock.timing->relative_deadline_ns > deadline_limit ||
        (is_automatic_clock_work(request) && clock.work_budget_ns > budget_limit))
        return foundation::invalid_argument("Clock request exceeds host domain, rate, backlog, deadline or budget limits");
    if (is_background && clock.timing->deadline_policy != DeadlinePolicy::kObserve)
        return foundation::invalid_argument("Tooling background Clock requires explicit Observe policy");
    // Bindings retain capability approval, not a retired DLL's old schedule.
    // All currently registered users still have to agree on one schedule.
    for (const auto& [id, definition] : ClockAdmissionAccess::definitions(registry)) {
        if (id != request.definition_id && definition.contract.clock.id == clock.id &&
            !same_clock_schedule(definition.contract.clock, clock))
            return foundation::invalid_argument("Feature definitions sharing a ClockId require one Clock schedule");
    }
    return foundation::Result::success();
} catch (const std::bad_alloc&) {
    return foundation::out_of_memory("Clock request validation allocation failed");
}

bool has_binding(const FeatureRegistry& registry, const ClockContract& clock) {
    const auto& bindings = ClockAdmissionAccess::bindings(registry);
    return std::find(bindings.begin(), bindings.end(), std::pair{clock.id, clock.capability_version}) != bindings.end();
}
}  // namespace

foundation::ResultValue<ClockHostContextPtr> make_clock_host_context(const ClockHostLimits& limits) {
    if (limits.capability_version != Version{1, 0, 0} || limits.allowed_domains == 0 ||
        (limits.allowed_domains & ~kAllFeatureDomains) != 0 || limits.max_frequency_hz == 0 ||
        limits.max_backlog == 0 || limits.max_bindings == 0 || limits.max_relative_deadline_ns == 0 ||
        limits.max_plan_budget_ns == 0 || limits.max_domain_automatic_budget_per_second_ns == 0)
        return {foundation::invalid_argument("Clock host limits must declare supported v1 capability and finite positive bounds"), {}};
    try {
        return {foundation::Result::success(), std::make_shared<const ClockHostContext>(limits)};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("Clock host context allocation failed"), {}};
    }
}

foundation::ResultValue<ClockBindingChange> admit_clock_request(FeatureRegistry& registry, const FeatureContract& request) {
    if (const auto valid = validate_request(registry, request); !valid) return {valid, {}};
    const ClockBindingKey key{request.clock.id, request.clock.capability_version};
    if (has_binding(registry, request.clock)) return {foundation::Result::success(), {key, false}};
    auto& bindings = ClockAdmissionAccess::bindings(registry);
    if (bindings.size() >= ClockAdmissionAccess::host(registry)->limits.max_bindings)
        return {foundation::invalid_argument("Clock host binding capacity is exhausted"), {}};
    try {
        bindings.emplace_back(key.id, key.version);
        return {foundation::Result::success(), {key, true}};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("Clock binding allocation failed"), {}};
    }
}

foundation::Result rollback_new_clock_bindings(FeatureRegistry& registry, std::span<const ClockBindingKey> keys) {
    auto& bindings = ClockAdmissionAccess::bindings(registry);
    for (const auto& key : keys) {
        const auto& definitions = ClockAdmissionAccess::definitions(registry);
        const bool referenced = std::any_of(definitions.begin(), definitions.end(), [&](const auto& entry) {
            const auto& clock = entry.second.contract.clock;
            return clock.id == key.id && clock.capability_version == key.version;
        });
        if (!referenced) {
            const auto found = std::find(bindings.begin(), bindings.end(), std::pair{key.id, key.version});
            if (found != bindings.end()) bindings.erase(found);
        }
    }
    return foundation::Result::success();
}

foundation::Result validate_admitted_clock(const FeatureRegistry& registry, const FeatureContract& request) {
    if (const auto valid = validate_request(registry, request); !valid) return valid;
    if (!has_binding(registry, request.clock))
        return foundation::unsupported("Clock request has not been explicitly approved by this host");
    return foundation::Result::success();
}

bool is_automatic_clock_work(const FeatureContract& contract) noexcept {
    return contract.structure == FeatureStructure::kAtomic &&
        (contract.execution == ExecutionKind::kProcess || contract.execution == ExecutionKind::kContinuous);
}

foundation::Result validate_clock_workload(std::span<const ClockWorkDemand> demands, const FeatureRegistry& registry) {
    const auto& host = ClockAdmissionAccess::host(registry);
    if (!host) return foundation::invalid_state("Clock workload has no host approval context");
    struct Plan final { ClockContract schedule{}; uint64_t budget{0}; };
    try {
        std::unordered_set<uint64_t> identities;
        std::unordered_map<ClockId, Plan, StableIdHash<ClockIdTag>> plans;
        std::array<uint64_t, 5> domain_usage{};
        for (const auto& demand : demands) {
            if (demand.identity == 0 || !identities.insert(demand.identity).second)
                return foundation::invalid_argument("Clock workload requires unique nonzero owner identities");
            const auto& contract = demand.contract;
            if (const auto approved = validate_admitted_clock(registry, contract); !approved) return approved;
            const auto& clock = contract.clock;
            auto [entry, inserted] = plans.try_emplace(clock.id, Plan{clock});
            auto& plan = entry->second;
            if (!inserted && !same_clock_schedule(plan.schedule, clock))
                return foundation::invalid_argument("Candidate workload contains conflicting shared Clock schedules");
            if (!is_automatic_clock_work(contract)) continue;
            const bool is_background = background(host->limits, clock.id);
            const auto plan_limit = (std::min)(is_background ? kToolingBackgroundLimitNs : host->limits.max_plan_budget_ns,
                                                clock.timing->relative_deadline_ns);
            if (plan.budget > plan_limit || clock.work_budget_ns > plan_limit - plan.budget)
                return foundation::invalid_argument("Actual Clock owners exceed the approved plan budget or minimum deadline");
            plan.budget += clock.work_budget_ns;
            if (is_background) continue; // Only periodic utilization is exempted.
            const auto domain = std::countr_zero(static_cast<unsigned>(contract.domains));
            const auto domain_limit = host->limits.max_domain_automatic_budget_per_second_ns;
            // Divide before multiplying, then subtract before summing: no wrapped approval.
            if (clock.work_budget_ns > domain_limit / clock.frequency_hz)
                return foundation::invalid_argument("Clock automatic utilization exceeds the host domain budget");
            const auto usage = clock.work_budget_ns * clock.frequency_hz;
            if (usage > domain_limit - domain_usage[domain])
                return foundation::invalid_argument("Combined Clock automatic utilization exceeds the host domain budget");
            domain_usage[domain] += usage;
        }
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Clock workload validation allocation failed");
    }
}
}  // namespace aeg::feature::detail

namespace aeg::feature {
foundation::ResultValue<ClockHost> make_clock_host() {
    auto context = detail::make_clock_host_context({});
    if (!context) return {context.result, {}};
    try {
        return {foundation::Result::success(),
                ClockHost(std::make_shared<const ClockHost::State>(std::move(context.value)))};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("Clock host handle allocation failed"), {}};
    }
}
}  // namespace aeg::feature
