/**
 * @file feature_graph_asset.hpp
 * @brief FeatureGraph binary asset envelope codec.
 */
#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "feature_graph.hpp"

namespace aeg::asset {

[[nodiscard]] foundation::ResultValue<std::vector<std::byte>> encode_feature_graph_asset(
    const feature::FeatureGraphDocument& graph);
[[nodiscard]] foundation::ResultValue<feature::FeatureGraphDocument> decode_feature_graph_asset(
    std::span<const std::byte> bytes);

}  // namespace aeg::asset
