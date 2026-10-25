/** @brief Public World value protocol interoperates with the actual existing Transform v1 codec. */
#include "feature_world_transform.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

#include "Components/component_reflection.hpp"
#include "Components/render_components.hpp"
#include "serialization.hpp"
#include "../Engine/Runtime/private/feature_world_commands.hpp"

namespace {
using namespace aeg;
using Value = feature::WorldTransformValue;
using Command = feature::WorldTransformCommand;
using Input = feature::WorldTransformInput;
using Native = engine::components::Transform;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template <class T> void succeeds(const T& result, const char* message) { require(bool(result), message); }

Value before_value() {
    return {{5.f, -2.f, 3.5f}, {0.f, 0.f, .6f, .8f}, {1.f, 2.f, 3.f}};
}
Value after_value() {
    return {{7.f, -4.f, -2.f}, {0.f, .6f, 0.f, .8f}, {2.f, 3.f, 4.f}};
}
Native native(const Value& value) {
    Native result;
    result.translation = {value.translation[0], value.translation[1], value.translation[2]};
    result.rotation = {value.rotation[0], value.rotation[1], value.rotation[2], value.rotation[3]};
    result.scale = {value.scale[0], value.scale[1], value.scale[2]};
    return result;
}
bool same(const Native& actual, const Value& expected) {
    return actual.translation.x == expected.translation[0] && actual.translation.y == expected.translation[1] &&
        actual.translation.z == expected.translation[2] && actual.rotation.x == expected.rotation[0] &&
        actual.rotation.y == expected.rotation[1] && actual.rotation.z == expected.rotation[2] &&
        actual.rotation.w == expected.rotation[3] && actual.scale.x == expected.scale[0] &&
        actual.scale.y == expected.scale[1] && actual.scale.z == expected.scale[2];
}
std::vector<uint8_t> reflected(const reflection::TypeRegistry& registry, const Native& value) {
    serialization::BinaryWriter writer;
    succeeds(serialization::serialize_object(registry, reflection::TypeId("Game.Transform"), &value, writer),
             "serialize the actual Game.Transform schema");
    return {writer.data().begin(), writer.data().end()};
}
void blob(serialization::BinaryWriter& writer, std::span<const uint8_t> bytes) {
    succeeds(writer.write_u32(static_cast<uint32_t>(bytes.size())), "write protocol fixture blob length");
    succeeds(writer.write_bytes(bytes), "write protocol fixture blob bytes");
}
feature::OutboxIntent command_wire(uint64_t revision, std::span<const uint8_t> expected,
                                  std::span<const uint8_t> replacement) {
    serialization::BinaryWriter writer;
    succeeds(writer.write_u64(revision), "write command revision");
    blob(writer, expected); blob(writer, replacement);
    return {feature::SideEffectId("World.Transform"), 1, {writer.data().begin(), writer.data().end()}};
}
std::vector<uint8_t> input_wire(const feature::WorldTransformObservation& observation,
                               std::span<const uint8_t> before, std::span<const uint8_t> channels,
                               feature::ValueSchemaId schema = feature::ValueSchemaId("World.Transform.Observation"),
                               uint32_t version = 1) {
    serialization::BinaryWriter writer;
    succeeds(writer.write_u64(schema.value()), "write input schema");
    succeeds(writer.write_u32(version), "write input version");
    succeeds(writer.write_u64(observation.world_revision), "write observed World revision");
    succeeds(writer.write_u64(std::bit_cast<uint64_t>(observation.elapsed_seconds)), "write clock quantum");
    succeeds(writer.write_u64(std::bit_cast<uint64_t>(observation.frame_delta_seconds)), "write actual frame delta");
    blob(writer, before); blob(writer, channels);
    return {writer.data().begin(), writer.data().end()};
}

struct Field final { uint64_t id; std::vector<uint8_t> bytes; };
struct Archive final { uint64_t type; uint32_t version; std::vector<Field> fields; };
Archive read_archive(std::span<const uint8_t> bytes) {
    serialization::BinaryReader reader(bytes);
    const auto type = reader.read_u64(); const auto version = reader.read_u32(); const auto count = reader.read_u32();
    require(type && version && count, "real reflected archive header missing");
    Archive archive{type.value, version.value, {}};
    for (uint32_t index = 0; index != count.value; ++index) {
        const auto id = reader.read_u64(); const auto size = reader.read_u32();
        require(id && size, "real reflected field header missing");
        const auto payload = reader.read_bytes(size.value); succeeds(payload, "read actual field payload");
        archive.fields.push_back({id.value, {payload.value.begin(), payload.value.end()}});
    }
    require(reader.remaining() == 0 && archive.fields.size() == 3, "unexpected actual Transform archive");
    return archive;
}
std::vector<uint8_t> write_archive(const Archive& archive) {
    serialization::BinaryWriter writer;
    succeeds(writer.write_u64(archive.type), "write altered reflected type");
    succeeds(writer.write_u32(archive.version), "write altered reflected version");
    succeeds(writer.write_u32(static_cast<uint32_t>(archive.fields.size())), "write altered field count");
    for (const auto& field : archive.fields) {
        succeeds(writer.write_u64(field.id), "write altered field identity"); blob(writer, field.bytes);
    }
    return {writer.data().begin(), writer.data().end()};
}

void test_public_command_matches_actual_private_v1(const reflection::TypeRegistry& registry) {
    const Value identity;
    require(identity.translation == std::array<float, 3>{0.f, 0.f, 0.f} &&
        identity.rotation == std::array<float, 4>{0.f, 0.f, 0.f, 1.f} &&
        identity.scale == std::array<float, 3>{1.f, 1.f, 1.f} && identity == Value{},
        "neutral Transform defaults are not semantic identity");
    require(feature::kWorldTransformCommandEffect == feature::SideEffectId("World.Transform") &&
        feature::kWorldTransformCommandVersion == 1 &&
        feature::kWorldTransformInputSchema == feature::ValueSchemaId("World.Transform.Observation") &&
        feature::kWorldTransformInputVersion == 1 && feature::kMaxWorldTransformInputBytes == 1024u * 1024u,
        "public protocol identities or versions drifted");
    const Command command{0x0102030405060708ull, before_value(), after_value()};
    const auto encoded = feature::encode_world_transform_command(command); succeeds(encoded, "public command encoder");
    const auto private_encoded = engine::detail::encode_world_transform_command(registry,
        {command.expected_world_revision, native(command.expected), native(command.replacement)});
    succeeds(private_encoded, "actual private v1 command encoder");
    require(encoded.value.effect == private_encoded.value.effect && encoded.value.version == 1 &&
        encoded.value.payload == private_encoded.value.payload && encoded.value.payload.size() >= 8 &&
        std::equal(encoded.value.payload.begin(), encoded.value.payload.begin() + 8,
                   std::array<uint8_t, 8>{8, 7, 6, 5, 4, 3, 2, 1}.begin()),
        "public command changed the actual v1 bytes or little-endian revision");
    const auto private_decoded = engine::detail::decode_world_transform_command(registry, encoded.value);
    succeeds(private_decoded, "private consumer decodes public v1 bytes");
    require(private_decoded.value.expected_world_revision == 0x0102030405060708ull &&
        same(private_decoded.value.expected, before_value()) && same(private_decoded.value.replacement, after_value()),
        "private consumer lost literal public translation/rotation/scale");
    const auto public_decoded = feature::decode_world_transform_command(private_encoded.value);
    succeeds(public_decoded, "public decoder reads actual private v1 bytes");
    require(public_decoded.value.expected_world_revision == 0x0102030405060708ull &&
        public_decoded.value.expected == before_value() && public_decoded.value.replacement == after_value(),
        "public decoder lost literal private translation/rotation/scale");
    auto zero = command; zero.expected_world_revision = 0;
    const auto initial = feature::encode_world_transform_command(zero); succeeds(initial, "initial World revision zero is valid");
    const auto initial_decoded = feature::decode_world_transform_command(initial.value);
    require(initial_decoded && initial_decoded.value.expected_world_revision == 0, "public codec rejected initial World revision");
}

void test_strict_commands_and_reflected_input(const reflection::TypeRegistry& registry) {
    const Command command{17, before_value(), after_value()};
    const auto canonical = feature::encode_world_transform_command(command); succeeds(canonical, "encode command guard control");
    const auto before = reflected(registry, native(command.expected));
    const auto after = reflected(registry, native(command.replacement));
    const feature::WorldTransformObservation observation{31, before_value(), .025, .125};
    const std::vector<uint8_t> channels{9, 0, 255, 4};
    const auto rejected_by_both = [&](const feature::OutboxIntent& intent) {
        require(!feature::decode_world_transform_command(intent) &&
                !engine::detail::decode_world_transform_command(registry, intent), "public/private command guards disagree");
    };
    auto bad = canonical.value; bad.effect = feature::SideEffectId("World.Other"); rejected_by_both(bad);
    bad = canonical.value; bad.version = 2; rejected_by_both(bad);
    bad = canonical.value; bad.payload.push_back(0xa5); rejected_by_both(bad);
    bad = canonical.value; std::fill(bad.payload.begin() + 8, bad.payload.begin() + 12, 0xff); rejected_by_both(bad);
    bad = canonical.value; bad.payload.resize(feature::kMaxOutboxIntentBytes + 1); rejected_by_both(bad);
    for (std::size_t size = 0; size != canonical.value.payload.size(); ++size) {
        bad = canonical.value; bad.payload.resize(size); rejected_by_both(bad);
    }
    const auto reject_archive = [&](const std::vector<uint8_t>& bytes) {
        rejected_by_both(command_wire(17, bytes, after));
        rejected_by_both(command_wire(17, before, bytes));
        require(!feature::decode_world_transform_input(input_wire(observation, bytes, channels)),
                "public input decoder accepted a noncanonical actual reflected Transform");
    };
    auto archive = read_archive(before); archive.type = reflection::TypeId("Fixture.Other").value(); reject_archive(write_archive(archive));
    archive = read_archive(before); archive.version = 2; reject_archive(write_archive(archive));
    archive = read_archive(before); archive.fields.erase(archive.fields.begin()); reject_archive(write_archive(archive));
    archive = read_archive(before); archive.fields.push_back(archive.fields.front()); reject_archive(write_archive(archive));
    archive = read_archive(before); archive.fields.front().id = reflection::FieldId("unknown_transform_field").value();
    reject_archive(write_archive(archive));
    archive = read_archive(before); std::reverse(archive.fields.begin(), archive.fields.end()); reject_archive(write_archive(archive));
    archive = read_archive(before); archive.fields.front().bytes.push_back(0); reject_archive(write_archive(archive));
    auto altered = native(before_value()); altered.translation.x = (std::numeric_limits<float>::quiet_NaN)();
    reject_archive(reflected(registry, altered));
    altered = native(before_value()); altered.rotation.w = (std::numeric_limits<float>::infinity)();
    reject_archive(reflected(registry, altered));
    altered = native(before_value()); altered.scale.y = -(std::numeric_limits<float>::infinity)();
    reject_archive(reflected(registry, altered));
    auto invalid = command; invalid.expected.translation[0] = (std::numeric_limits<float>::quiet_NaN)();
    require(!feature::encode_world_transform_command(invalid), "public command encoder accepted NaN translation");
    invalid = command; invalid.replacement.rotation[3] = (std::numeric_limits<float>::infinity)();
    require(!feature::encode_world_transform_command(invalid), "public command encoder accepted infinite rotation");
    invalid = command; invalid.replacement.scale[2] = -(std::numeric_limits<float>::infinity)();
    require(!feature::encode_world_transform_command(invalid), "public command encoder accepted infinite scale");
}

void test_input_preserves_owned_channel_and_strict_envelope(const reflection::TypeRegistry& registry) {
    Input input{{47, before_value(), 1.0 / 60.0, .125}, {9, 0, 255, 4, 0, 17}};
    const auto before = reflected(registry, native(input.observation.before));
    auto encoded = feature::encode_world_transform_input(input); succeeds(encoded, "encode public World input");
    require(encoded.value == input_wire(input.observation, before, input.channel_input),
            "input did not preserve schema/version, independent time values, actual Transform and Channel bytes");
    const auto original = encoded.value;
    auto decoded = feature::decode_world_transform_input(encoded.value); succeeds(decoded, "decode public World input");
    input.observation.before.translation[0] = 999.f; input.channel_input.assign(1, 42);
    std::fill(encoded.value.begin(), encoded.value.end(), 0xa5);
    require(decoded.value.observation.world_revision == 47 && decoded.value.observation.before == before_value() &&
        decoded.value.observation.elapsed_seconds == 1.0 / 60.0 && decoded.value.observation.frame_delta_seconds == .125 &&
        decoded.value.channel_input == std::vector<uint8_t>{9, 0, 255, 4, 0, 17},
        "decoded observation or Channel bytes borrowed caller storage");
    const auto reencoded = feature::encode_world_transform_input(decoded.value);
    require(reencoded && reencoded.value == original, "owning input changed its canonical bytes");
    const Input direct{{0, Value{}, 0.0, 0.0}, {}};
    const auto direct_bytes = feature::encode_world_transform_input(direct); succeeds(direct_bytes, "encode direct dispatch with zero times/revision");
    const auto direct_decoded = feature::decode_world_transform_input(direct_bytes.value);
    require(direct_decoded && direct_decoded.value.observation.world_revision == 0 &&
        direct_decoded.value.observation.before == Value{} && direct_decoded.value.observation.elapsed_seconds == 0 &&
        direct_decoded.value.observation.frame_delta_seconds == 0 && direct_decoded.value.channel_input.empty(),
        "direct dispatch zeros or empty Channel were rejected/defaulted");
    const auto rejected = [&](const std::vector<uint8_t>& bytes) {
        require(!feature::decode_world_transform_input(bytes), "input decoder accepted invalid envelope bytes");
    };
    rejected({9, 0, 255, 4});  // Ordinary Channel bytes are not implicitly guessed to be World observations.
    rejected(input_wire(decoded.value.observation, before, decoded.value.channel_input, feature::ValueSchemaId("World.Other")));
    rejected(input_wire(decoded.value.observation, before, decoded.value.channel_input, feature::kWorldTransformInputSchema, 2));
    auto bad = original; bad.push_back(0xa5); rejected(bad);
    bad = original; bad.resize(feature::kMaxWorldTransformInputBytes + 1); rejected(bad);
    for (std::size_t size = 0; size != original.size(); ++size)
        require(!feature::decode_world_transform_input(std::span<const uint8_t>(original).first(size)),
                "input decoder accepted a truncated envelope prefix");
    // The reflected blob length starts after schema/version/revision/two double bit patterns.
    bad = original; std::fill(bad.begin() + 36, bad.begin() + 40, 0xff); rejected(bad);
    const auto channel_length = original.size() - decoded.value.channel_input.size() - 4;
    bad = original; std::fill(bad.begin() + static_cast<std::ptrdiff_t>(channel_length),
        bad.begin() + static_cast<std::ptrdiff_t>(channel_length + 4), 0xff); rejected(bad);
    auto invalid = decoded.value;
    for (const double value : {-1.0, (std::numeric_limits<double>::quiet_NaN)(), (std::numeric_limits<double>::infinity)()}) {
        invalid = decoded.value; invalid.observation.elapsed_seconds = value;
        require(!feature::encode_world_transform_input(invalid), "input encoder accepted invalid clock quantum");
        rejected(input_wire(invalid.observation, before, invalid.channel_input));
        invalid = decoded.value; invalid.observation.frame_delta_seconds = value;
        require(!feature::encode_world_transform_input(invalid), "input encoder accepted invalid frame delta");
        rejected(input_wire(invalid.observation, before, invalid.channel_input));
    }
    invalid = decoded.value; invalid.observation.before.rotation[1] = (std::numeric_limits<float>::quiet_NaN)();
    require(!feature::encode_world_transform_input(invalid), "input encoder accepted non-finite observed Transform");
    invalid = decoded.value; invalid.channel_input.resize(feature::kMaxWorldTransformInputBytes);
    require(!feature::encode_world_transform_input(invalid), "input encoder ignored total envelope byte limit");
}
}  // namespace

int main() try {
    reflection::TypeRegistry registry;
    const auto schema = engine::components::transform_reflection_descriptor(); succeeds(schema, "obtain actual existing Transform factory");
    succeeds(registry.register_type(schema.value), "register actual Transform schema");
    test_public_command_matches_actual_private_v1(registry);
    test_strict_commands_and_reflected_input(registry);
    test_input_preserves_owned_channel_and_strict_envelope(registry);
    std::cout << "Public World Transform v1 interop and owning observation input protocol passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "World Transform protocol: " << error.what() << '\n';
    return 73;
}
