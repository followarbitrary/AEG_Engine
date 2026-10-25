#pragma once

#include "feature_catalog.hpp"
#include <nlohmann/json.hpp>

namespace aeg::feature::detail {
[[nodiscard]] nlohmann::json encode_catalog_entry_json(const FeatureCatalogEntry& entry);
[[nodiscard]] foundation::ResultValue<FeatureCatalogEntry> decode_catalog_entry_json(const nlohmann::json& value);
}  // namespace aeg::feature::detail
