#include "feature_world_transform.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <exception>
#include <limits>
#include <new>
#include <utility>

#include "Components/component_reflection.hpp"
#include "Components/render_components.hpp"
#include "serialization.hpp"

namespace aeg::feature {
namespace {
using NativeTransform = engine::components::Transform;

foundation::ResultValue<reflection::TypeId> register_transform_schema(reflection::TypeRegistry& registry) {
    auto schema = engine::components::transform_reflection_descriptor();
    if (!schema) return {schema.result, {}};
    const auto id = schema.value.id;
    if (const auto result = registry.register_type(std::move(schema.value)); !result) return {result, {}};
    return {foundation::Result::success(), id};
}

bool finite_transform(const WorldTransformValue& value) noexcept {
    for (const float component : value.translation) if (!std::isfinite(component)) return false;
    for (const float component : value.rotation) if (!std::isfinite(component)) return false;
    for (const float component : value.scale) if (!std::isfinite(component)) return false;
    return true;
}

bool finite_times(const WorldTransformObservation& observation) noexcept {
    return std::isfinite(observation.elapsed_seconds) && observation.elapsed_seconds >= 0 &&
           std::isfinite(observation.frame_delta_seconds) && observation.frame_delta_seconds >= 0;
}

NativeTransform native_transform(const WorldTransformValue& value) noexcept {
    NativeTransform result;
    result.translation = {value.translation[0], value.translation[1], value.translation[2]};
    result.rotation = {value.rotation[0], value.rotation[1], value.rotation[2], value.rotation[3]};
    result.scale = {value.scale[0], value.scale[1], value.scale[2]};
    return result;
}

WorldTransformValue public_transform(const NativeTransform& value) noexcept {
    return {{value.translation.x, value.translation.y, value.translation.z},
            {value.rotation.x, value.rotation.y, value.rotation.z, value.rotation.w},
            {value.scale.x, value.scale.y, value.scale.z}};
}

foundation::Result serialize_transform(const reflection::TypeRegistry& registry, reflection::TypeId schema,
                                       const WorldTransformValue& value, serialization::BinaryWriter& writer) {
    if (!finite_transform(value))
        return foundation::invalid_argument("World Transform protocol requires finite semantic components");
    const auto native = native_transform(value);
    return serialization::serialize_object(registry, schema, &native, writer);
}

foundation::ResultValue<WorldTransformValue> deserialize_transform(
    const reflection::TypeRegistry& registry, reflection::TypeId schema, std::span<const uint8_t> bytes) {
    NativeTransform native;
    serialization::BinaryReader reader(bytes);
    if (const auto result = serialization::deserialize_object(registry, schema, reader, &native); !result)
        return {result, {}};
    if (reader.remaining() != 0)
        return {foundation::invalid_argument("World Transform reflected payload contains trailing bytes"), {}};
    auto value = public_transform(native);
    if (!finite_transform(value))
        return {foundation::invalid_argument("World Transform reflected payload contains non-finite components"), {}};
    return {foundation::Result::success(), value};
}

foundation::Result write_blob(serialization::BinaryWriter& writer, std::span<const uint8_t> bytes) {
    if (bytes.size() > (std::numeric_limits<uint32_t>::max)())
        return foundation::invalid_argument("World Transform protocol blob is too large");
    if (const auto result = writer.write_u32(static_cast<uint32_t>(bytes.size())); !result) return result;
    return writer.write_bytes(bytes);
}

foundation::ResultValue<std::span<const uint8_t>> read_blob(serialization::BinaryReader& reader, uint32_t limit) {
    const auto size = reader.read_u32();
    if (!size) return {size.result, {}};
    if (size.value > limit)
        return {foundation::invalid_argument("World Transform protocol blob exceeds its byte limit"), {}};
    return reader.read_bytes(size.value);
}

bool same_bytes(std::span<const uint8_t> left, std::span<const uint8_t> right) noexcept {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
}
}  // namespace

foundation::Result validate_world_transform_component(std::span<const uint8_t> bytes, uint32_t schema_version) {
    try {
        reflection::TypeRegistry registry;
        const auto schema = register_transform_schema(registry);
        if (!schema) return schema.result;
        const auto descriptor = registry.find_type(schema.value);
        if (!descriptor) return descriptor.result;
        if (schema_version == 0 || schema_version > descriptor.value->version)
            return foundation::unsupported("World Transform component schema version is unsupported");
        serialization::BinaryReader header(bytes);
        const auto type = header.read_u64();
        const auto version = header.read_u32();
        if (!type || !version || type.value != schema.value.value() || version.value != schema_version)
            return foundation::invalid_argument("World Transform component payload identity or version is inconsistent");
        // Preserve Scene's load-compatible field handling; command wire records
        // have a separate, stricter canonical re-encoding requirement.
        return deserialize_transform(registry, schema.value, bytes).result;
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("World Transform component validation allocation failed");
    } catch (const std::exception& error) {
        return foundation::unknown_exception(error);
    } catch (...) {
        return foundation::invalid_state("World Transform component validation threw an exception");
    }
}

foundation::ResultValue<OutboxIntent> encode_world_transform_command(const WorldTransformCommand& command) {
    try {
        reflection::TypeRegistry registry;
        const auto schema = register_transform_schema(registry);
        if (!schema) return {schema.result, {}};
        serialization::BinaryWriter expected;
        if (const auto result = serialize_transform(registry, schema.value, command.expected, expected); !result)
            return {result, {}};
        serialization::BinaryWriter replacement;
        if (const auto result = serialize_transform(registry, schema.value, command.replacement, replacement); !result)
            return {result, {}};
        serialization::BinaryWriter writer;
        if (const auto result = writer.write_u64(command.expected_world_revision); !result) return {result, {}};
        if (const auto result = write_blob(writer, expected.data()); !result) return {result, {}};
        if (const auto result = write_blob(writer, replacement.data()); !result) return {result, {}};
        if (writer.size() > kMaxOutboxIntentBytes)
            return {foundation::invalid_argument("World Transform command exceeds the outbox intent limit"), {}};
        return {foundation::Result::success(),
                {kWorldTransformCommandEffect, kWorldTransformCommandVersion, {writer.data().begin(), writer.data().end()}}};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("World Transform command encoding allocation failed"), {}};
    } catch (const std::exception& error) {
        return {foundation::unknown_exception(error), {}};
    } catch (...) {
        return {foundation::invalid_state("World Transform command encoding threw an exception"), {}};
    }
}

foundation::ResultValue<WorldTransformCommand> decode_world_transform_command(const OutboxIntent& intent) {
    try {
        if (intent.effect != kWorldTransformCommandEffect || intent.version != kWorldTransformCommandVersion ||
            intent.payload.size() > kMaxOutboxIntentBytes)
            return {foundation::invalid_argument("World Transform command protocol or payload size is invalid"), {}};
        reflection::TypeRegistry registry;
        const auto schema = register_transform_schema(registry);
        if (!schema) return {schema.result, {}};
        serialization::BinaryReader reader(intent.payload);
        const auto revision = reader.read_u64();
        if (!revision) return {revision.result, {}};
        const auto expected = read_blob(reader, kMaxOutboxIntentBytes);
        if (!expected) return {expected.result, {}};
        const auto replacement = read_blob(reader, kMaxOutboxIntentBytes);
        if (!replacement) return {replacement.result, {}};
        if (reader.remaining() != 0)
            return {foundation::invalid_argument("World Transform command contains trailing bytes"), {}};
        const auto expected_value = deserialize_transform(registry, schema.value, expected.value);
        if (!expected_value) return {expected_value.result, {}};
        const auto replacement_value = deserialize_transform(registry, schema.value, replacement.value);
        if (!replacement_value) return {replacement_value.result, {}};
        WorldTransformCommand command{revision.value, expected_value.value, replacement_value.value};
        const auto canonical = encode_world_transform_command(command);
        if (!canonical) return {canonical.result, {}};
        if (canonical.value.payload != intent.payload)
            return {foundation::invalid_argument("World Transform command is not a complete canonical reflected record"), {}};
        return {foundation::Result::success(), std::move(command)};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("World Transform command decoding allocation failed"), {}};
    } catch (const std::exception& error) {
        return {foundation::unknown_exception(error), {}};
    } catch (...) {
        return {foundation::invalid_state("World Transform command decoding threw an exception"), {}};
    }
}

foundation::ResultValue<std::vector<uint8_t>> encode_world_transform_input(const WorldTransformInput& input) {
    try {
        if (!finite_times(input.observation))
            return {foundation::invalid_argument("World Transform observation times must be finite and non-negative"), {}};
        if (input.channel_input.size() > kMaxWorldTransformInputBytes)
            return {foundation::invalid_argument("World Transform input exceeds its byte limit"), {}};
        reflection::TypeRegistry registry;
        const auto schema = register_transform_schema(registry);
        if (!schema) return {schema.result, {}};
        serialization::BinaryWriter before;
        if (const auto result = serialize_transform(registry, schema.value, input.observation.before, before); !result)
            return {result, {}};
        constexpr std::size_t envelope_bytes = 44;
        if (before.size() > kMaxWorldTransformInputBytes - envelope_bytes ||
            input.channel_input.size() > kMaxWorldTransformInputBytes - envelope_bytes - before.size())
            return {foundation::invalid_argument("World Transform input exceeds its total envelope byte limit"), {}};
        serialization::BinaryWriter writer;
        if (const auto result = writer.write_u64(kWorldTransformInputSchema.value()); !result) return {result, {}};
        if (const auto result = writer.write_u32(kWorldTransformInputVersion); !result) return {result, {}};
        if (const auto result = writer.write_u64(input.observation.world_revision); !result) return {result, {}};
        if (const auto result = writer.write_u64(std::bit_cast<uint64_t>(input.observation.elapsed_seconds)); !result)
            return {result, {}};
        if (const auto result = writer.write_u64(std::bit_cast<uint64_t>(input.observation.frame_delta_seconds)); !result)
            return {result, {}};
        if (const auto result = write_blob(writer, before.data()); !result) return {result, {}};
        if (const auto result = write_blob(writer, input.channel_input); !result) return {result, {}};
        return {foundation::Result::success(), {writer.data().begin(), writer.data().end()}};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("World Transform input encoding allocation failed"), {}};
    } catch (const std::exception& error) {
        return {foundation::unknown_exception(error), {}};
    } catch (...) {
        return {foundation::invalid_state("World Transform input encoding threw an exception"), {}};
    }
}

foundation::ResultValue<WorldTransformInput> decode_world_transform_input(std::span<const uint8_t> bytes) {
    try {
        if (bytes.size() > kMaxWorldTransformInputBytes)
            return {foundation::invalid_argument("World Transform input exceeds its byte limit"), {}};
        serialization::BinaryReader reader(bytes);
        const auto wire_schema = reader.read_u64();
        if (!wire_schema) return {wire_schema.result, {}};
        const auto version = reader.read_u32();
        if (!version) return {version.result, {}};
        if (wire_schema.value != kWorldTransformInputSchema.value() || version.value != kWorldTransformInputVersion)
            return {foundation::invalid_argument("World Transform input schema or version is unsupported"), {}};
        const auto revision = reader.read_u64();
        if (!revision) return {revision.result, {}};
        const auto elapsed = reader.read_u64();
        if (!elapsed) return {elapsed.result, {}};
        const auto frame_delta = reader.read_u64();
        if (!frame_delta) return {frame_delta.result, {}};
        WorldTransformInput input;
        input.observation.world_revision = revision.value;
        input.observation.elapsed_seconds = std::bit_cast<double>(elapsed.value);
        input.observation.frame_delta_seconds = std::bit_cast<double>(frame_delta.value);
        if (!finite_times(input.observation))
            return {foundation::invalid_argument("World Transform input times must be finite and non-negative"), {}};
        const auto before = read_blob(reader, kMaxWorldTransformInputBytes);
        if (!before) return {before.result, {}};
        const auto channels = read_blob(reader, kMaxWorldTransformInputBytes);
        if (!channels) return {channels.result, {}};
        if (reader.remaining() != 0)
            return {foundation::invalid_argument("World Transform input contains trailing bytes"), {}};
        reflection::TypeRegistry registry;
        const auto schema = register_transform_schema(registry);
        if (!schema) return {schema.result, {}};
        const auto value = deserialize_transform(registry, schema.value, before.value);
        if (!value) return {value.result, {}};
        input.observation.before = value.value;
        input.channel_input.assign(channels.value.begin(), channels.value.end());
        const auto canonical = encode_world_transform_input(input);
        if (!canonical) return {canonical.result, {}};
        if (!same_bytes(canonical.value, bytes))
            return {foundation::invalid_argument("World Transform input is not a complete canonical reflected record"), {}};
        return {foundation::Result::success(), std::move(input)};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("World Transform input decoding allocation failed"), {}};
    } catch (const std::exception& error) {
        return {foundation::unknown_exception(error), {}};
    } catch (...) {
        return {foundation::invalid_state("World Transform input decoding threw an exception"), {}};
    }
}
}  // namespace aeg::feature
