#include "feature_parameters.hpp"

#include <algorithm>
#include <limits>
#include <unordered_set>

#include "serialization.hpp"

namespace aeg::feature {
namespace {

foundation::ParameterValue minimum_wire_value(ParameterType type) {
    switch (type) {
    case ParameterType::kBool: return false;
    case ParameterType::kInt32: case ParameterType::kInt64: return int64_t{0};
    case ParameterType::kUInt32: case ParameterType::kUInt64: return uint64_t{0};
    case ParameterType::kFloat: case ParameterType::kDouble: return 0.0;
    case ParameterType::kString: case ParameterType::kEnum: return std::string{};
    case ParameterType::kAssetReference: return foundation::AssetReference{};
    case ParameterType::kVector2: return std::vector<double>(2, 0);
    case ParameterType::kVector3: return std::vector<double>(3, 0);
    case ParameterType::kVector4: case ParameterType::kColor: return std::vector<double>(4, 0);
    }
    return false; // Unknown types have already failed metadata validation.
}

foundation::Result validate_records(std::span<const FeatureParameterRecord> values,
                                    std::span<const ParameterContract> parameters) {
    if (const auto result = validate_feature_parameter_schema(parameters); !result) return result;
    std::unordered_set<ParameterId, StableIdHash<ParameterIdTag>> seen;
    for (const auto& value : values) {
        const auto schema = std::find_if(parameters.begin(), parameters.end(), [&](const auto& parameter) {
            return parameter.id == value.id;
        });
        if (schema == parameters.end())
            return foundation::invalid_argument("Feature parameter envelope references an undeclared parameter");
        if (!seen.insert(value.id).second)
            return foundation::invalid_argument("Feature parameter envelope contains a duplicate parameter");
        if (value.version != schema->version)
            return foundation::invalid_argument("Feature parameter schema version does not match the contract");
        if (value.payload.size() > schema->max_size)
            return foundation::invalid_argument("Feature parameter payload exceeds its declared size");
        if (schema->metadata) {
            const auto decoded = deserialize_parameter_value(value.payload, schema->metadata->type);
            if (!decoded) return decoded.result;
            if (const auto result = validate_parameter_value(decoded.value, *schema->metadata); !result) return result;
        }
    }
    for (const auto& parameter : parameters)
        if (parameter.required && !seen.contains(parameter.id))
            return foundation::invalid_argument("required Feature parameter is missing");
    return foundation::Result::success();
}

foundation::Result complete_records(std::vector<FeatureParameterRecord>& values,
                                    std::span<const ParameterContract> parameters) {
    if (const auto result = validate_feature_parameter_schema(parameters); !result) return result;
    for (const auto& parameter : parameters) {
        if (!parameter.metadata || !parameter.metadata->default_value ||
            std::any_of(values.begin(), values.end(), [&](const auto& value) { return value.id == parameter.id; })) continue;
        const auto encoded = serialize_parameter_value(*parameter.metadata->default_value, parameter.metadata->type);
        if (!encoded) return encoded.result;
        values.push_back({parameter.id, parameter.version, encoded.value});
    }
    return validate_records(values, parameters);
}

}  // namespace

foundation::Result validate_feature_parameter_schema(std::span<const ParameterContract> parameters) {
    std::unordered_set<ParameterId, StableIdHash<ParameterIdTag>> ids;
    for (const auto& parameter : parameters) {
        if (!parameter.id.is_valid() || parameter.version == 0 || parameter.max_size == 0 ||
            !ids.insert(parameter.id).second)
            return foundation::invalid_argument("parameter schema ids, versions, sizes must be valid and unique");
        if (parameter.metadata) {
            if (const auto result = validate_parameter_metadata(*parameter.metadata); !result) return result;
            // Derive the fixed width (or mandatory string prefix) from the one
            // encoder even when the author has not supplied a default value.
            const auto minimum_payload = serialize_parameter_value(
                minimum_wire_value(parameter.metadata->type), parameter.metadata->type);
            if (!minimum_payload) return minimum_payload.result;
            if (minimum_payload.value.size() > parameter.max_size)
                return foundation::invalid_argument("Feature parameter size cannot hold its typed wire value");
            if (parameter.metadata->default_value) {
                const auto payload = serialize_parameter_value(*parameter.metadata->default_value, parameter.metadata->type);
                if (!payload) return payload.result;
                if (payload.value.size() > parameter.max_size)
                    return foundation::invalid_argument("Feature parameter default exceeds its declared size");
                const auto decoded = deserialize_parameter_value(payload.value, parameter.metadata->type);
                if (!decoded) return decoded.result;
                if (const auto result = validate_parameter_value(decoded.value, *parameter.metadata); !result) return result;
            }
        }
    }
    return foundation::Result::success();
}

foundation::ResultValue<std::vector<uint8_t>> encode_feature_parameters(
    std::span<const FeatureParameterRecord> values, std::span<const ParameterContract> parameters) {
    std::vector<FeatureParameterRecord> complete(values.begin(), values.end());
    if (const auto result = complete_records(complete, parameters); !result) return {result, {}};
    if (complete.empty()) return {foundation::Result::success(), {}};
    if (complete.size() > (std::numeric_limits<uint32_t>::max)())
        return {foundation::invalid_argument("Feature parameter record count exceeds the wire format"), {}};
    std::vector<const FeatureParameterRecord*> ordered;
    ordered.reserve(complete.size());
    for (const auto& value : complete) ordered.push_back(&value);
    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) { return left->id < right->id; });
    serialization::BinaryWriter writer;
    if (auto result = writer.write_u32(static_cast<uint32_t>(ordered.size())); !result) return {result, {}};
    for (const auto* value : ordered) {
        if (auto result = writer.write_u64(value->id.value()); !result) return {result, {}};
        if (auto result = writer.write_u32(value->version); !result) return {result, {}};
        if (auto result = writer.write_u32(static_cast<uint32_t>(value->payload.size())); !result) return {result, {}};
        if (auto result = writer.write_bytes(value->payload); !result) return {result, {}};
    }
    return {foundation::Result::success(), {writer.data().begin(), writer.data().end()}};
}

foundation::ResultValue<std::vector<FeatureParameterRecord>> decode_feature_parameters(
    std::span<const uint8_t> blob, std::span<const ParameterContract> parameters) {
    if (const auto result = validate_feature_parameter_schema(parameters); !result) return {result, {}};
    std::vector<FeatureParameterRecord> values;
    if (!blob.empty()) {
        serialization::BinaryReader reader(blob);
        const auto count = reader.read_u32();
        constexpr std::size_t record_header_size = sizeof(uint64_t) + 2 * sizeof(uint32_t);
        if (!count || count.value > parameters.size() || count.value > reader.remaining() / record_header_size)
            return {foundation::invalid_argument("Feature parameter record count is invalid or truncated"), {}};
        values.reserve(count.value);
        for (uint32_t index = 0; index < count.value; ++index) {
            const auto id = reader.read_u64();
            const auto version = reader.read_u32();
            const auto size = reader.read_u32();
            if (!id || !version || !size)
                return {foundation::invalid_argument("Feature parameter record header is truncated"), {}};
            const auto payload = reader.read_bytes(size.value);
            if (!payload)
                return {foundation::invalid_argument("Feature parameter payload is truncated"), {}};
            // Reject oversized/unknown records before allocating their payload.
            const auto schema = std::find_if(parameters.begin(), parameters.end(), [&](const auto& parameter) {
                return parameter.id.value() == id.value;
            });
            if (schema == parameters.end() || size.value > schema->max_size)
                return {foundation::invalid_argument("Feature parameter is undeclared or exceeds its declared size"), {}};
            values.push_back({ParameterId(id.value), version.value, {payload.value.begin(), payload.value.end()}});
        }
        if (reader.remaining() != 0)
            return {foundation::invalid_argument("Feature parameter envelope has trailing bytes"), {}};
    }
    if (const auto result = complete_records(values, parameters); !result) return {result, {}};
    return {foundation::Result::success(), std::move(values)};
}

foundation::Result validate_feature_parameter_blob(
    std::span<const uint8_t> blob, std::span<const ParameterContract> parameters) {
    return decode_feature_parameters(blob, parameters).result;
}

foundation::Result validate_feature_parameter_assets(
    std::span<const uint8_t> blob, std::span<const ParameterContract> parameters,
    const AssetKindResolver& resolver) {
    const auto records = decode_feature_parameters(blob, parameters);
    if (!records) return records.result;
    for (const auto& record : records.value) {
        const auto schema = std::find_if(parameters.begin(), parameters.end(), [&](const auto& parameter) {
            return parameter.id == record.id;
        });
        if (!schema->metadata || schema->metadata->type != ParameterType::kAssetReference) continue;
        const auto decoded = deserialize_parameter_value(record.payload, schema->metadata->type);
        if (!decoded) return decoded.result;
        if (const auto valid = validate_parameter_value(decoded.value, *schema->metadata, resolver); !valid)
            return valid;
    }
    return foundation::Result::success();
}

}  // namespace aeg::feature
