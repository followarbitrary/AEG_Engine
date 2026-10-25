/** @file feature_parameters.hpp
 *  @brief Versioned, Feature-owned parameter envelopes.
 */
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "feature_types.hpp"
#include "feature_contract_metadata.hpp"
#include "result.hpp"

namespace aeg::feature {

struct ParameterContract final {
    ParameterId id{};
    uint32_t version{0};
    uint32_t max_size{0};
    bool required{false};
    /** Optional typed payload semantics; absent metadata leaves bytes Feature-owned. */
    std::optional<ParameterMetadata> metadata{};

    friend bool operator==(const ParameterContract&, const ParameterContract&) = default;
};

/** One parameter record. Only its Feature interprets the payload schema. */
struct FeatureParameterRecord final {
    ParameterId id{};
    uint32_t version{0};
    std::vector<uint8_t> payload{};

    friend bool operator==(const FeatureParameterRecord&, const FeatureParameterRecord&) = default;
};

[[nodiscard]] foundation::Result validate_feature_parameter_schema(std::span<const ParameterContract> parameters);

/** Little-endian record count and (id, version, size, bytes), ordered by id.
 *  Missing typed defaults are materialized. An empty schema has an empty envelope.
 *  Duplicate, undeclared, required-without-default, stale and oversized records
 *  are rejected before an envelope is returned.
 */
[[nodiscard]] foundation::ResultValue<std::vector<uint8_t>> encode_feature_parameters(
    std::span<const FeatureParameterRecord> values, std::span<const ParameterContract> parameters);

/** Strictly consumes a complete envelope; returned payloads own their bytes. */
[[nodiscard]] foundation::ResultValue<std::vector<FeatureParameterRecord>> decode_feature_parameters(
    std::span<const uint8_t> blob, std::span<const ParameterContract> parameters);

[[nodiscard]] foundation::Result validate_feature_parameter_blob(
    std::span<const uint8_t> blob, std::span<const ParameterContract> parameters);

/** Validates effective values, including typed defaults, against the project/package asset resolver. */
[[nodiscard]] foundation::Result validate_feature_parameter_assets(
    std::span<const uint8_t> blob, std::span<const ParameterContract> parameters,
    const AssetKindResolver& resolver);

}  // namespace aeg::feature
