#include "feature_contract_metadata.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <regex>
#include <unordered_set>

#include "asset_identity.hpp"
#include "serialization.hpp"

namespace aeg::feature {
namespace {
using foundation::ParameterValue;
using foundation::Result;
using foundation::ResultValue;
using foundation::invalid_argument;

constexpr long double kStepTolerance = 1e-6L;
constexpr double kUnitMagnitudeTolerance = 1e-6;

size_t dimension(ParameterType type) {
    switch (type) {
    case ParameterType::kVector2: return 2;
    case ParameterType::kVector3: return 3;
    case ParameterType::kVector4: case ParameterType::kColor: return 4;
    default: return 0;
    }
}
bool numeric(ParameterType type) {
    return type >= ParameterType::kInt32 && type <= ParameterType::kDouble;
}
Result valid_range(const NumericRange& range) {
    if (!std::isfinite(range.min) || !std::isfinite(range.max) || !std::isfinite(range.step) ||
        range.min > range.max || range.step < 0 ||
        (range.normalized && (range.min != 0 || range.max != 1)))
        return invalid_argument("parameter range is invalid");
    return Result::success();
}
Result range_value(double value, const NumericRange& range) {
    if (value < range.min || value > range.max) return invalid_argument("parameter is outside its range");
    if (range.step > 0) {
        const long double offset = static_cast<long double>(value) - range.min;
        const long double remainder = std::fmod(offset, static_cast<long double>(range.step));
        const long double tolerance = kStepTolerance * range.step;
        if (!std::isfinite(remainder) ||
            (std::abs(remainder) > tolerance && std::abs(remainder - range.step) > tolerance))
            return invalid_argument("parameter does not align with its step");
    }
    return Result::success();
}
Result unit_vector_range(const NumericRange& range, size_t dimensions) {
    const long double lower = range.min;
    const long double upper = range.max;
    long double minimum = lower > 0 ? lower : upper < 0 ? -upper : 0;
    long double maximum = (std::max)(std::abs(lower), std::abs(upper));
    if (range.step > 0) {
        const long double step = range.step;
        const long double extent = upper - lower;
        // Keep an outer bound on the values accepted by range_value, including
        // its grid tolerance and cancellation when locating the final grid point.
        const long double scale = (std::max)({std::abs(lower), std::abs(upper), step, 1.0L});
        const long double tolerance = kStepTolerance * step +
            16 * std::numeric_limits<long double>::epsilon() * scale;
        if (std::isfinite(extent)) {
            const long double remainder = std::fmod(extent, step);
            long double reachable_upper = upper;
            if (remainder > tolerance && step - remainder > tolerance)
                reachable_upper = (std::max)(lower, (std::min)(upper, upper - remainder + tolerance));
            maximum = (std::max)(std::abs(lower), std::abs(reachable_upper));
            if (upper < 0) {
                minimum = -reachable_upper;
            } else if (lower <= 0) {
                const long double zero_remainder = std::fmod(-lower, step);
                minimum = (std::max)(0.0L, (std::min)(zero_remainder, step - zero_remainder) - tolerance);
            }
        }
    }
    long double minimum_length = 0;
    long double maximum_length = 0;
    for (size_t component = 0; component < dimensions; ++component) {
        minimum_length = std::hypot(minimum_length, minimum);
        maximum_length = std::hypot(maximum_length, maximum);
    }
    const long double tolerance = kUnitMagnitudeTolerance + 32 * std::numeric_limits<double>::epsilon();
    if (minimum_length > 1 + tolerance || maximum_length < 1 - tolerance)
        return invalid_argument("vector component range cannot satisfy unit magnitude");
    // These are necessary norm bounds. A stepped range can still have gaps
    // between the lengths formed by different combinations of components.
    return Result::success();
}
Result constraints(const ParameterMetadata& metadata) {
    const auto type = metadata.type;
    if (type > ParameterType::kColor) return invalid_argument("unknown parameter type");
    const auto& constraint = metadata.constraint;
    if (const auto* range = std::get_if<NumericRange>(&constraint)) {
        if (!numeric(type)) return invalid_argument("numeric range requires a numeric parameter");
        if (const auto result = valid_range(*range); !result) return result;
        if (type == ParameterType::kFloat &&
            (std::abs(range->min) > (std::numeric_limits<float>::max)() ||
             std::abs(range->max) > (std::numeric_limits<float>::max)() ||
             range->step > (std::numeric_limits<float>::max)() ||
             (range->step > 0 && static_cast<float>(range->step) == 0)))
            return invalid_argument("float range exceeds its representable width");
        if (type <= ParameterType::kUInt64) {
            // Integer limits must be exactly representable; never compare an i64/u64 through double.
            const double limit = type == ParameterType::kInt32 ? 2147483648.0 :
                                 type == ParameterType::kUInt32 ? 4294967296.0 :
                                 type == ParameterType::kInt64 ? 9223372036854775808.0 : 18446744073709551616.0;
            const bool signed_type = type == ParameterType::kInt32 || type == ParameterType::kInt64;
            if (range->min < (signed_type ? -limit : 0) || range->max >= limit || range->step >= limit ||
                std::trunc(range->min) != range->min || std::trunc(range->max) != range->max ||
                std::trunc(range->step) != range->step)
                return invalid_argument("integer range must contain representable integral bounds and step");
        }
    } else if (const auto* choices = std::get_if<EnumChoices>(&constraint)) {
        if (type != ParameterType::kEnum || choices->labels.empty() ||
            (!choices->descriptions.empty() && choices->descriptions.size() != choices->labels.size()))
            return invalid_argument("enum choices are invalid");
        std::unordered_set<std::string> seen;
        for (const auto& label : choices->labels)
            if (label.empty() || !seen.insert(label).second) return invalid_argument("enum values must be nonempty and unique");
    } else if (const auto* pattern = std::get_if<StringPattern>(&constraint)) {
        if (type != ParameterType::kString || (pattern->max_length && pattern->min_length > pattern->max_length))
            return invalid_argument("string constraint is invalid");
        try { if (!pattern->regex.empty()) { const std::regex checked(pattern->regex); } }
        catch (const std::regex_error&) { return invalid_argument("parameter regex is invalid"); }
    } else if (const auto* asset = std::get_if<AssetConstraint>(&constraint)) {
        if (type != ParameterType::kAssetReference) return invalid_argument("asset constraint requires an asset reference");
        if (asset->kind && static_cast<uint8_t>(*asset->kind) > static_cast<uint8_t>(foundation::AssetKind::kShader))
            return invalid_argument("asset constraint has an unknown asset kind");
    } else if (const auto* vector = std::get_if<VectorConstraint>(&constraint)) {
        if (!dimension(type)) return invalid_argument("vector constraint requires a vector");
        if (vector->component_range) {
            if (const auto result = valid_range(*vector->component_range); !result) return result;
            if (vector->normalized) return unit_vector_range(*vector->component_range, dimension(type));
        }
    }
    return Result::success();
}
} // namespace

Result validate_parameter_value(const ParameterValue& value, const ParameterMetadata& metadata) {
    if (const auto result = constraints(metadata); !result) return result;
    const auto type = metadata.type;
    bool matches = false;
    switch (type) {
    case ParameterType::kBool: matches = std::holds_alternative<bool>(value); break;
    case ParameterType::kInt32: case ParameterType::kInt64:
        if (const auto* number = std::get_if<int64_t>(&value)) {
            matches = type == ParameterType::kInt64 || (*number >= INT32_MIN && *number <= INT32_MAX);
            if (const auto* range = std::get_if<NumericRange>(&metadata.constraint)) {
                if (*number < static_cast<int64_t>(range->min) || *number > static_cast<int64_t>(range->max) ||
                    (range->step > 0 && (static_cast<uint64_t>(*number) - static_cast<uint64_t>(static_cast<int64_t>(range->min))) % static_cast<uint64_t>(range->step)))
                    return invalid_argument("integer parameter violates its range or step");
            }
        }
        break;
    case ParameterType::kUInt32: case ParameterType::kUInt64:
        if (const auto* number = std::get_if<uint64_t>(&value)) {
            matches = type == ParameterType::kUInt64 || *number <= UINT32_MAX;
            if (const auto* range = std::get_if<NumericRange>(&metadata.constraint)) {
                if (*number < static_cast<uint64_t>(range->min) || *number > static_cast<uint64_t>(range->max) ||
                    (range->step > 0 && (*number - static_cast<uint64_t>(range->min)) % static_cast<uint64_t>(range->step)))
                    return invalid_argument("unsigned parameter violates its range or step");
            }
        }
        break;
    case ParameterType::kFloat: case ParameterType::kDouble:
        if (const auto* number = std::get_if<double>(&value)) {
            matches = std::isfinite(*number) && (type == ParameterType::kDouble || std::abs(*number) <= (std::numeric_limits<float>::max)());
            if (!matches) return invalid_argument("floating parameter is nonfinite or exceeds its width");
            if (const auto* range = std::get_if<NumericRange>(&metadata.constraint)) {
                auto effective_range = *range;
                double effective_value = *number;
                if (type == ParameterType::kFloat) {
                    effective_value = static_cast<float>(*number);
                    effective_range.min = static_cast<float>(range->min);
                    effective_range.max = static_cast<float>(range->max);
                    effective_range.step = 0; // Float grid membership is checked after wire rounding.
                    if (range->step > 0) {
                        const double position = (effective_value - range->min) / range->step;
                        const double grid_point = range->min + std::round(position) * range->step;
                        if (!std::isfinite(grid_point) || std::abs(grid_point) > (std::numeric_limits<float>::max)() ||
                            static_cast<float>(grid_point) != static_cast<float>(effective_value))
                            return invalid_argument("float parameter does not align with its step");
                    }
                }
                if (const auto result = range_value(effective_value, effective_range); !result) return result;
            }
        }
        break;
    case ParameterType::kString: case ParameterType::kEnum:
        if (const auto* text = std::get_if<std::string>(&value)) {
            matches = text->size() <= UINT32_MAX;
            if (const auto* pattern = std::get_if<StringPattern>(&metadata.constraint)) {
                if (text->size() < pattern->min_length || (pattern->max_length && text->size() > pattern->max_length) ||
                    (!pattern->regex.empty() && !std::regex_match(*text, std::regex(pattern->regex))))
                    return invalid_argument("string violates its pattern or length");
            }
            if (const auto* choices = std::get_if<EnumChoices>(&metadata.constraint))
                if (std::find(choices->labels.begin(), choices->labels.end(), *text) == choices->labels.end())
                    return invalid_argument("enum value is undeclared");
        }
        break;
    case ParameterType::kAssetReference:
        if (const auto* asset = std::get_if<foundation::AssetReference>(&value)) {
            const auto id = asset->uuid.empty()
                                ? ResultValue<foundation::Uuid>{Result::success(), {}}
                                : foundation::Uuid::parse(asset->uuid);
            matches = static_cast<bool>(id);
            if (const auto* constraint = std::get_if<AssetConstraint>(&metadata.constraint))
                if (id && id.value == foundation::Uuid{} && !constraint->optional)
                    return invalid_argument("asset reference cannot be null");
        }
        break;
    default:
        if (const auto* vector = std::get_if<std::vector<double>>(&value)) {
            matches = vector->size() == dimension(type) && std::all_of(vector->begin(), vector->end(), [](double n) { return std::isfinite(n); });
            if (const auto* constraint = std::get_if<VectorConstraint>(&metadata.constraint)) {
                if (constraint->component_range)
                    for (const double component : *vector)
                        if (const auto result = range_value(component, *constraint->component_range); !result) return result;
                if (constraint->normalized) {
                    double length = 0;
                    for (const double component : *vector) length = std::hypot(length, component);
                    if (!std::isfinite(length) || std::abs(length - 1) > kUnitMagnitudeTolerance) return invalid_argument("vector must have unit magnitude");
                }
            }
        }
        break;
    }
    return matches ? Result::success() : invalid_argument("parameter has the wrong type, width, dimension or a nonfinite value");
}

Result validate_parameter_value(const ParameterValue& value, const ParameterMetadata& metadata,
                                const AssetKindResolver& resolver) {
    if (const auto valid = validate_parameter_value(value, metadata); !valid) return valid;
    if (metadata.type != ParameterType::kAssetReference) return Result::success();
    const auto* constraint = std::get_if<AssetConstraint>(&metadata.constraint);
    const auto& reference = std::get<foundation::AssetReference>(value);
    if (reference.uuid.empty()) return Result::success();
    const auto id = foundation::AssetId::parse(reference.uuid);
    if (!id) return id.result;
    if (id.value == foundation::AssetId{}) return Result::success();
    if (!resolver) return constraint && constraint->kind
        ? foundation::invalid_state("typed asset reference requires an asset resolver") : Result::success();
    try {
        const auto actual = resolver(id.value);
        if (!actual) return actual.result;
        if (actual.value > foundation::AssetKind::kShader)
            return invalid_argument("asset resolver returned an unknown asset kind");
        if (constraint && constraint->kind && actual.value != *constraint->kind)
            return invalid_argument("asset reference kind does not match its declared constraint");
        return Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("asset kind resolver allocation failed");
    } catch (...) {
        return foundation::invalid_state("asset kind resolver threw an exception");
    }
}

Result validate_parameter_metadata(const ParameterMetadata& metadata) {
    if (const auto result = constraints(metadata); !result) return result;
    if (metadata.type == ParameterType::kEnum && !std::holds_alternative<EnumChoices>(metadata.constraint))
        return invalid_argument("typed enum metadata requires choices");
    if (metadata.default_value) return validate_parameter_value(*metadata.default_value, metadata);
    return Result::success();
}

ResultValue<std::vector<uint8_t>> serialize_parameter_value(const ParameterValue& value, ParameterType type) {
    ParameterMetadata metadata{};
    metadata.type = type;
    if (const auto result = validate_parameter_value(value, metadata); !result) return {result, {}};
    serialization::BinaryWriter writer;
    Result result = Result::success();
    switch (type) {
    case ParameterType::kBool: {
        const uint8_t byte = std::get<bool>(value) ? 1 : 0;
        result = writer.write_bytes({&byte, 1}); break;
    }
    case ParameterType::kInt32: result = writer.write_u32(std::bit_cast<uint32_t>(static_cast<int32_t>(std::get<int64_t>(value)))); break;
    case ParameterType::kInt64: result = writer.write_u64(std::bit_cast<uint64_t>(std::get<int64_t>(value))); break;
    case ParameterType::kUInt32: result = writer.write_u32(static_cast<uint32_t>(std::get<uint64_t>(value))); break;
    case ParameterType::kUInt64: result = writer.write_u64(std::get<uint64_t>(value)); break;
    case ParameterType::kFloat: result = writer.write_u32(std::bit_cast<uint32_t>(static_cast<float>(std::get<double>(value)))); break;
    case ParameterType::kDouble: result = writer.write_u64(std::bit_cast<uint64_t>(std::get<double>(value))); break;
    case ParameterType::kString: case ParameterType::kEnum: result = writer.write_string(std::get<std::string>(value)); break;
    case ParameterType::kAssetReference: {
        const auto& uuid = std::get<foundation::AssetReference>(value).uuid;
        foundation::Uuid id{};
        if (!uuid.empty()) id = foundation::Uuid::parse(uuid).value;
        result = writer.write_bytes(id.bytes); break;
    }
    default:
        for (const double component : std::get<std::vector<double>>(value)) {
            result = writer.write_u64(std::bit_cast<uint64_t>(component));
            if (!result) break;
        }
        break;
    }
    if (!result) return {result, {}};
    return {Result::success(), {writer.data().begin(), writer.data().end()}};
}

ResultValue<ParameterValue> deserialize_parameter_value(std::span<const uint8_t> bytes, ParameterType type) {
    serialization::BinaryReader reader(bytes);
    ParameterValue value{};
    switch (type) {
    case ParameterType::kBool: {
        const auto raw = reader.read_bytes(1);
        if (!raw || raw.value[0] > 1) return {invalid_argument("invalid boolean payload"), {}};
        value = raw.value[0] == 1; break;
    }
    case ParameterType::kInt32: case ParameterType::kUInt32: case ParameterType::kFloat: {
        const auto raw = reader.read_u32();
        if (!raw) return {raw.result, {}};
        if (type == ParameterType::kInt32) value = static_cast<int64_t>(std::bit_cast<int32_t>(raw.value));
        else if (type == ParameterType::kUInt32) value = static_cast<uint64_t>(raw.value);
        else value = static_cast<double>(std::bit_cast<float>(raw.value));
        break;
    }
    case ParameterType::kInt64: case ParameterType::kUInt64: case ParameterType::kDouble: {
        const auto raw = reader.read_u64();
        if (!raw) return {raw.result, {}};
        if (type == ParameterType::kInt64) value = std::bit_cast<int64_t>(raw.value);
        else if (type == ParameterType::kUInt64) value = raw.value;
        else value = std::bit_cast<double>(raw.value);
        break;
    }
    case ParameterType::kString: case ParameterType::kEnum: {
        const auto raw = reader.read_string();
        if (!raw) return {raw.result, {}};
        value = raw.value; break;
    }
    case ParameterType::kAssetReference: {
        const auto raw = reader.read_bytes(16);
        if (!raw) return {raw.result, {}};
        foundation::Uuid uuid{};
        std::copy(raw.value.begin(), raw.value.end(), uuid.bytes.begin());
        value = foundation::AssetReference{uuid == foundation::Uuid{} ? "" : uuid.to_string()};
        break;
    }
    default: {
        if (!dimension(type)) return {invalid_argument("unknown parameter type"), {}};
        std::vector<double> vector;
        for (size_t index = 0; index < dimension(type); ++index) {
            const auto raw = reader.read_u64();
            if (!raw) return {raw.result, {}};
            vector.push_back(std::bit_cast<double>(raw.value));
        }
        value = std::move(vector); break;
    }
    }
    if (reader.remaining()) return {invalid_argument("typed parameter payload has trailing bytes"), {}};
    ParameterMetadata metadata{};
    metadata.type = type;
    if (const auto result = validate_parameter_value(value, metadata); !result) return {result, {}};
    return {Result::success(), std::move(value)};
}

uint64_t parameter_metadata_digest(const ParameterMetadata& metadata) {
    uint64_t digest = 14695981039346656037ull;
    const auto mix = [&digest](uint64_t number) {
        for (unsigned shift = 0; shift < 64; shift += 8) { digest ^= (number >> shift) & 255; digest *= 1099511628211ull; }
    };
    const auto text = [&](const std::string& value) { mix(value.size()); for (unsigned char byte : value) mix(byte); };
    const auto range = [&](const NumericRange& value) {
        mix(std::bit_cast<uint64_t>(value.min == 0 ? 0.0 : value.min));
        mix(std::bit_cast<uint64_t>(value.max == 0 ? 0.0 : value.max));
        mix(std::bit_cast<uint64_t>(value.step == 0 ? 0.0 : value.step));
        mix(value.normalized);
    };
    mix(static_cast<uint8_t>(metadata.type));
    mix(metadata.default_value.has_value());
    if (metadata.default_value) {
        const auto bytes = serialize_parameter_value(*metadata.default_value, metadata.type);
        mix(bytes.value.size()); for (const auto byte : bytes.value) mix(byte);
    }
    mix(metadata.constraint.index());
    if (const auto* numeric_range = std::get_if<NumericRange>(&metadata.constraint)) range(*numeric_range);
    else if (const auto* choices = std::get_if<EnumChoices>(&metadata.constraint)) {
        mix(choices->labels.size()); for (const auto& label : choices->labels) text(label);
    } else if (const auto* pattern = std::get_if<StringPattern>(&metadata.constraint)) {
        text(pattern->regex); mix(pattern->min_length); mix(pattern->max_length);
    } else if (const auto* asset = std::get_if<AssetConstraint>(&metadata.constraint)) {
        mix(asset->optional);
        // Preserve historical digests for untyped references, while separating Mesh=0 from no kind.
        if (asset->kind) { mix(0x41535345544b494eull); mix(static_cast<uint8_t>(*asset->kind)); }
    }
    else if (const auto* vector = std::get_if<VectorConstraint>(&metadata.constraint)) {
        mix(vector->component_range.has_value()); if (vector->component_range) range(*vector->component_range);
        mix(vector->normalized);
    }
    return digest;
}
} // namespace aeg::feature
