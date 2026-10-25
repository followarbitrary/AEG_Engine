#pragma once

#include "pipeline.hpp"

namespace aeg::pipeline::detail {

/**
 * @brief Pipeline-owned access to one Scope host's execution plan lifecycle.
 *
 * Binding, rebuild, activation, and execution remain on PipelineScopeHost;
 * this bridge keeps borrowed plan snapshots and retirement bookkeeping out of
 * the public host contract.
 */
struct PipelineScopeHostExecution final {
    [[nodiscard]] static const PipelinePlan* active_plan(const PipelineScopeHost& host) noexcept;
    [[nodiscard]] static PipelinePlanHandle active_plan_snapshot(const PipelineScopeHost& host) noexcept;
    [[nodiscard]] static bool has_pending_plan(const PipelineScopeHost& host) noexcept;
    [[nodiscard]] static std::size_t retired_plan_count(const PipelineScopeHost& host) noexcept;
    [[nodiscard]] static const ArtifactScopeBridge* scope_bridge(const PipelineScopeHost& host) noexcept;
    static void set_scope_bridge(PipelineScopeHost& host, const ArtifactScopeBridge* scope_bridge) noexcept;
};

}  // namespace aeg::pipeline::detail
