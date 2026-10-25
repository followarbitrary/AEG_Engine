/** @brief Engine-private control for retiring issued Feature work without admitting new work. */
#pragma once

#include "feature_runtime.hpp"

namespace aeg::feature::detail {

struct FeatureExecutionControl final {
    /** Completes ready issued work while leaving deferred, retry, and clock backlog work queued. */
    [[nodiscard]] static foundation::Result drain_issued_completions(FeatureRuntime& runtime) {
        return runtime.pump_completions_impl(false);
    }
};

}  // namespace aeg::feature::detail
