/**
 * @file feature_clock_host.hpp
 * @brief Opaque ownership of the engine's bounded default Clock policy.
 */
#pragma once

#include <memory>
#include <utility>

#include "result.hpp"

namespace aeg::feature {

class FeatureRegistry;

/**
 * Copyable host lifetime shared by candidate registries. An empty handle has no
 * policy. A valid handle alone grants no Clock binding or definition approval.
 */
class ClockHost final {
public:
    ClockHost() = default;
    [[nodiscard]] explicit operator bool() const noexcept { return state_ != nullptr; }

private:
    struct State;
    friend class FeatureRegistry;
    friend foundation::ResultValue<ClockHost> make_clock_host();
    explicit ClockHost(std::shared_ptr<const State> state) : state_(std::move(state)) {}
    std::shared_ptr<const State> state_{};
};

/**
 * Creates the fixed engine v1 policy for all five domains. Custom limits,
 * Tooling background exemption and individual Clock approvals remain internal.
 * Failure returns an empty handle; safe to share between host owner threads.
 */
[[nodiscard]] foundation::ResultValue<ClockHost> make_clock_host();

}  // namespace aeg::feature
