#pragma once

#include <string>
#include <string_view>
#include "feature_contract.hpp"

namespace aeg::feature {
/** JSON structure codec. Incomplete authoring drafts can roundtrip; publication
 * separately requires validate_feature_contract. Executable callbacks are never serialized. */
[[nodiscard]] foundation::ResultValue<std::string> encode_feature_contract_json(const FeatureContract& contract);
[[nodiscard]] foundation::ResultValue<FeatureContract> decode_feature_contract_json(std::string_view json);
}  // namespace aeg::feature
