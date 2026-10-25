/** @file runtime_feature_control.hpp
 *  @brief Engine-only forwarding service for the Bootstrap-owned FeatureGraph host.
 */
#pragma once

#include <cstdint>
#include <functional>

#include "feature_graph_runtime_host.hpp"

namespace aeg::application::runtime {
class RuntimeBootstrap;

/** Owner-thread tools receive owning snapshots, never a mutable Runtime or executor. */
class RuntimeFeatureControl final {
public:
    /** Configure subsequent sessions while stopped. The source must be thread
     *  safe and monotonic; an empty source restores the default steady clock. */
    [[nodiscard]] static foundation::Result set_transport_time_source(
        RuntimeBootstrap& bootstrap, std::function<uint64_t()> now_ns);
    [[nodiscard]] static foundation::ResultValue<FeatureRuntimeNodeSnapshot> inspect_node(
        const RuntimeBootstrap& bootstrap, std::string_view asset_uuid, feature::FeatureNodeId node_id);
    [[nodiscard]] static foundation::Result control_process(
        RuntimeBootstrap& bootstrap, const RuntimeProcessCommand& command);
    [[nodiscard]] static foundation::ResultValue<std::vector<feature::RenderFeatureOutput>> capture_render_outputs(
        const RuntimeBootstrap& bootstrap);
    [[nodiscard]] static foundation::Result render_publication_status(
        const RuntimeBootstrap& bootstrap, feature::ClockId clock);

private:
    [[nodiscard]] static FeatureGraphRuntimeHost* host(const RuntimeBootstrap& bootstrap) noexcept;
};
}  // namespace aeg::application::runtime
