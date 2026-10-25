#pragma once

#include <span>
#include <vector>

#include "feature_runtime.hpp"

namespace aeg::feature {

/** Pure CPU producer binding; physical resources belong to the current graph build. */
struct RenderLightListUse final {
    size_t consumer_output{0};
    uint32_t consumer_operation{0};
    size_t producer_output{0};
    uint32_t producer_operation{0};
};

struct RenderIntentExecutionPlan final {
    std::vector<size_t> output_order{};
    std::vector<RenderLightListUse> light_list_uses{};
};

} // namespace aeg::feature
