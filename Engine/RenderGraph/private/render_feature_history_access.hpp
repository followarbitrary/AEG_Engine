/**
 * @file render_feature_history_access.hpp
 * @brief RenderGraph-private access bridge for Feature history storage.
 */
#pragma once

#include "render_feature_history.hpp"

namespace aeg::render_graph::detail {

/**
 * @brief Keeps history transaction staging out of the typed runtime friend list.
 *
 * The Feature runtime may validate the owning device and stage a keyed
 * transaction, but the history store remains an implementation-only service.
 */
struct RenderFeatureHistoryRuntimeAccess final {
    [[nodiscard]] static bool uses_device(const RenderFeatureHistory& history,
                                          const rhi::Device& device) noexcept;
    [[nodiscard]] static std::shared_ptr<RenderFeatureHistoryTransaction> stage(
        RenderFeatureHistory& history, RenderFeatureHistoryKey key);
};

}  // namespace aeg::render_graph::detail
