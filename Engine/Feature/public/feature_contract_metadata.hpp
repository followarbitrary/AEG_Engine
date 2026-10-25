/** @file feature_contract_metadata.hpp
 *  @brief Typed parameter semantics and editor hints owned by a Feature contract.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "parameter_value.hpp"
#include "asset_identity.hpp"
#include "result.hpp"

namespace aeg::feature {

struct NumericRange final {
    double min{0};
    double max{1};
    double step{0}; // Zero is continuous; a positive step constrains allowed values.
    bool normalized{false};
    friend bool operator==(const NumericRange&, const NumericRange&) = default;
};
struct EnumChoices final {
    std::vector<std::string> labels{}; // Stable values, in UI order.
    std::vector<std::string> descriptions{};
    friend bool operator==(const EnumChoices&, const EnumChoices&) = default;
};
struct StringPattern final {
    std::string regex{};
    uint32_t min_length{0}; // Byte lengths; zero max means unbounded.
    uint32_t max_length{0};
    bool is_file_path{false};
    bool is_color{false};
    friend bool operator==(const StringPattern&, const StringPattern&) = default;
};
struct AssetConstraint final {
    bool optional{false}; // Empty UUID is a null reference.
    std::optional<foundation::AssetKind> kind{};
    friend bool operator==(const AssetConstraint&, const AssetConstraint&) = default;
};
struct VectorConstraint final {
    std::optional<NumericRange> component_range{};
    bool normalized{false}; // Unit magnitude; dimension comes only from ParameterType.
    friend bool operator==(const VectorConstraint&, const VectorConstraint&) = default;
};
using ParameterConstraint = std::variant<std::monostate, NumericRange, EnumChoices,
                                         StringPattern, AssetConstraint, VectorConstraint>;

enum class ParameterType : uint8_t {
    kBool, kInt32, kInt64, kUInt32, kUInt64, kFloat, kDouble,
    kString, kEnum, kAssetReference, kVector2, kVector3, kVector4, kColor,
};

/** Identity, schema version, required and size belong only to ParameterContract. */
struct ParameterMetadata final {
    ParameterType type{ParameterType::kFloat};
    std::optional<foundation::ParameterValue> default_value{};
    ParameterConstraint constraint{};
    std::string display_name{};
    std::string description{};
    bool expert_only{false};
    std::string ui_group{};
    uint32_t ui_order{0};
    friend bool operator==(const ParameterMetadata&, const ParameterMetadata&) = default;
};

[[nodiscard]] foundation::Result validate_parameter_metadata(const ParameterMetadata& metadata);
[[nodiscard]] foundation::Result validate_parameter_value(const foundation::ParameterValue& value,
                                                          const ParameterMetadata& metadata);
/** Project/package owners resolve canonical UUIDs; schema validation stays independent of storage. */
using AssetKindResolver = std::function<foundation::ResultValue<foundation::AssetKind>(foundation::AssetId)>;
/** Host validation resolves every nonnull asset when a resolver is supplied.
 *  A declared kind requires a resolver; untyped values without one retain the
 *  schema-only behavior of the two-argument overload.
 */
[[nodiscard]] foundation::Result validate_parameter_value(const foundation::ParameterValue& value,
                                                          const ParameterMetadata& metadata,
                                                          const AssetKindResolver& resolver);
/** Little-endian fixed widths, length-prefixed string bytes, full 16-byte UUID,
 *  and fixed-dimension float64 vectors. Never accepts absent or nonfinite values.
 */
[[nodiscard]] foundation::ResultValue<std::vector<uint8_t>> serialize_parameter_value(
    const foundation::ParameterValue& value, ParameterType type);
/** Strictly consumes one typed payload; empty bytes are never a missing value. */
[[nodiscard]] foundation::ResultValue<foundation::ParameterValue> deserialize_parameter_value(
    std::span<const uint8_t> bytes, ParameterType type);
/** Execution semantics only: UI labels/groups/order do not change the digest. */
[[nodiscard]] uint64_t parameter_metadata_digest(const ParameterMetadata& metadata);

} // namespace aeg::feature
