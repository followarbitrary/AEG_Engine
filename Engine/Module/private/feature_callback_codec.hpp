#pragma once

#include "feature_contract.hpp"

namespace aeg::module::detail {
/** Versioned Module ABI response; host-only message provenance is never serialized. */
[[nodiscard]] foundation::ResultValue<std::vector<uint8_t>> encode_feature_callback_result(
    const foundation::ResultValue<feature::StateDelta>& result);
[[nodiscard]] foundation::ResultValue<feature::StateDelta> decode_feature_callback_result(
    std::span<const uint8_t> bytes);
}
