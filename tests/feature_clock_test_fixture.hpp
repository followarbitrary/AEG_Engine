#pragma once

#include <utility>

#include "../Engine/Feature/private/feature_clock_admission.hpp"

namespace aeg::feature::test {

/** Test hosts request the same bounded, private approval used by production hosts. */
inline detail::ClockHostContextPtr clock_test_host(bool allow_tooling_background = false) {
    detail::ClockHostLimits limits{};
    limits.allow_tooling_background = allow_tooling_background;
    const auto host = detail::make_clock_host_context(limits);
    return host ? host.value : detail::ClockHostContextPtr{};
}

inline foundation::Result register_clock_test_definition(FeatureRegistry& registry, FeatureDefinition definition) {
    const auto admission = detail::admit_clock_request(registry, definition.contract);
    if (!admission) return admission.result;
    return registry.register_definition(std::move(definition));
}

inline foundation::Result replace_clock_test_definition(FeatureRegistry& registry, FeatureDefinition definition) {
    const auto admission = detail::admit_clock_request(registry, definition.contract);
    if (!admission) return admission.result;
    return registry.replace_definition(std::move(definition));
}

}  // namespace aeg::feature::test
