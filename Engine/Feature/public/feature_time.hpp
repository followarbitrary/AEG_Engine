#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include "feature_types.hpp"
#include "result.hpp"

namespace aeg::feature {
struct TransportTimePoint final {
    RuntimeGeneration domain{};
    uint64_t nanoseconds{0};
    friend bool operator==(const TransportTimePoint&, const TransportTimePoint&) = default;
};
struct TransportDuration final {
    uint64_t nanoseconds{0};
    friend bool operator==(const TransportDuration&, const TransportDuration&) = default;
};
struct TransportTiming final {
    TransportTimePoint origin{};
    std::optional<TransportTimePoint> expires_at{};
    friend bool operator==(const TransportTiming&, const TransportTiming&) = default;
};

[[nodiscard]] inline foundation::ResultValue<TransportDuration> source_period_duration(
    uint32_t ticks, uint32_t frequency_hz) {
    if (frequency_hz == 0) return {foundation::invalid_argument("transport source frequency is zero"), {}};
    // uint32 periods * 1e9 fits uint64. Divide before rounding to avoid addition overflow.
    const uint64_t numerator = static_cast<uint64_t>(ticks) * 1'000'000'000;
    return {foundation::Result::success(), {numerator / frequency_hz + (numerator % frequency_hz != 0)}};
}
[[nodiscard]] inline foundation::ResultValue<TransportTimePoint> transport_add(
    TransportTimePoint origin, TransportDuration duration) {
    if (!origin.domain.is_valid()) return {foundation::invalid_argument("transport time domain is missing"), {}};
    if (duration.nanoseconds > (std::numeric_limits<uint64_t>::max)() - origin.nanoseconds)
        return {foundation::invalid_argument("transport expiry overflows nanoseconds"), {}};
    return {foundation::Result::success(), {origin.domain, origin.nanoseconds + duration.nanoseconds}};
}
[[nodiscard]] inline foundation::Result validate_transport_timing(
    const TransportTiming& timing, TransportTimePoint now) {
    if (!now.domain.is_valid() || timing.origin.domain != now.domain || timing.origin.nanoseconds > now.nanoseconds)
        return foundation::invalid_argument("transport origin is missing, future or belongs to another time domain");
    if (timing.expires_at) {
        if (timing.expires_at->domain != now.domain || timing.expires_at->nanoseconds < timing.origin.nanoseconds)
            return foundation::invalid_argument("transport expiry has an invalid domain or precedes its origin");
        if (now.nanoseconds > timing.expires_at->nanoseconds)
            return foundation::Result::failure(foundation::ResultCode::kTimeout, "transport deadline expired");
    }
    return foundation::Result::success();
}
}
