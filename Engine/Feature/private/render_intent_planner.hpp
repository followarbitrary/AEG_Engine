/**
 * @file render_intent_planner.hpp
 * @brief Feature-private RenderIntent validation and dependency planner.
 */
#pragma once

#include "../public/render_intent_plan.hpp"

namespace aeg::feature {

class RenderIntentPlanner final {
public:
    /** An exact reference to a previously accepted producer yields NotReady, never a mixed plan. */
    [[nodiscard]] foundation::ResultValue<RenderIntentExecutionPlan> plan(
        std::span<const RenderFeatureOutput> outputs,
        std::span<const RenderFeatureOutput> accepted = {}) const;

private:
    friend class FeatureRuntime;
    [[nodiscard]] foundation::ResultValue<RenderIntentExecutionPlan> plan_with_retired_sources(
        std::span<const RenderFeatureOutput> outputs, std::span<const RenderFeatureOutput> accepted,
        std::span<const std::shared_ptr<const detail::RenderLightListEvidence>> retired) const;
};

}  // namespace aeg::feature
