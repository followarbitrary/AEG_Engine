/**
 * @file serialization.cpp
 * @brief Implements the reflection-driven binary wire format.
 */
#include "../public/serialization.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <new>
#include <string>

namespace aeg::serialization {
namespace {
using foundation::invalid_argument;
using foundation::invalid_state;
using foundation::not_found;
using foundation::out_of_memory;
using foundation::unsupported;

constexpr std::size_t kMaxObjectDepth = 64;
constexpr std::size_t kMaxFieldBytes = 64u * 1024u * 1024u;

[[nodiscard]] foundation::Result read_failure() {
    return foundation::Result::failure(foundation::ResultCode::kInvalidArgument, "serialized input is truncated");
}

[[nodiscard]] foundation::Result validate_object(const reflection::TypeDescriptor& descriptor, const void* object) {
    if (object == nullptr) {
        return invalid_argument("serialized object pointer is null");
    }
    if (!descriptor.id.is_valid() || descriptor.version == 0) {
        return invalid_state("serialized type descriptor is invalid");
    }
    return foundation::Result::success();
}

[[nodiscard]] foundation::Result write_scalar(const reflection::FieldDescriptor& field, const void* value,
                                              BinaryWriter& writer) {
    if (value == nullptr) {
        return invalid_argument("serialized field value is null");
    }
    switch (field.value_kind) {
        case reflection::FieldValueKind::kBool:
            return writer.write_bytes({static_cast<const uint8_t*>(value), 1});
        case reflection::FieldValueKind::kInt32: {
            uint32_t bits = 0;
            std::memcpy(&bits, value, sizeof(bits));
            return writer.write_u32(bits);
        }
        case reflection::FieldValueKind::kUInt32: {
            uint32_t value_copy = 0;
            std::memcpy(&value_copy, value, sizeof(value_copy));
            return writer.write_u32(value_copy);
        }
        case reflection::FieldValueKind::kInt64: {
            uint64_t bits = 0;
            std::memcpy(&bits, value, sizeof(bits));
            return writer.write_u64(bits);
        }
        case reflection::FieldValueKind::kUInt64: {
            uint64_t value_copy = 0;
            std::memcpy(&value_copy, value, sizeof(value_copy));
            return writer.write_u64(value_copy);
        }
        case reflection::FieldValueKind::kFloat: {
            uint32_t bits = std::bit_cast<uint32_t>(*static_cast<const float*>(value));
            return writer.write_u32(bits);
        }
        case reflection::FieldValueKind::kDouble: {
            uint64_t bits = std::bit_cast<uint64_t>(*static_cast<const double*>(value));
            return writer.write_u64(bits);
        }
        case reflection::FieldValueKind::kString: {
            const auto& string = *static_cast<const std::string*>(value);
            return writer.write_bytes({reinterpret_cast<const uint8_t*>(string.data()), string.size()});
        }
        case reflection::FieldValueKind::kObject:
        case reflection::FieldValueKind::kOpaque:
            return unsupported("field requires a reflected nested object or supported scalar type");
    }
    return unsupported("field value kind is unknown");
}

[[nodiscard]] foundation::Result read_scalar(const reflection::FieldDescriptor& field, BinaryReader& reader,
                                             void* value) {
    if (value == nullptr) {
        return invalid_argument("deserialized field value is null");
    }
    switch (field.value_kind) {
        case reflection::FieldValueKind::kBool: {
            const auto bytes = reader.read_bytes(1);
            if (!bytes) {
                return bytes.result;
            }
            *static_cast<bool*>(value) = bytes.value[0] != 0;
            return foundation::Result::success();
        }
        case reflection::FieldValueKind::kInt32: {
            const auto bits = reader.read_u32();
            if (!bits) {
                return bits.result;
            }
            std::memcpy(value, &bits.value, sizeof(bits.value));
            return foundation::Result::success();
        }
        case reflection::FieldValueKind::kUInt32: {
            const auto value_result = reader.read_u32();
            if (!value_result) {
                return value_result.result;
            }
            std::memcpy(value, &value_result.value, sizeof(value_result.value));
            return foundation::Result::success();
        }
        case reflection::FieldValueKind::kInt64: {
            const auto bits = reader.read_u64();
            if (!bits) {
                return bits.result;
            }
            std::memcpy(value, &bits.value, sizeof(bits.value));
            return foundation::Result::success();
        }
        case reflection::FieldValueKind::kUInt64: {
            const auto value_result = reader.read_u64();
            if (!value_result) {
                return value_result.result;
            }
            std::memcpy(value, &value_result.value, sizeof(value_result.value));
            return foundation::Result::success();
        }
        case reflection::FieldValueKind::kFloat: {
            const auto bits = reader.read_u32();
            if (!bits) {
                return bits.result;
            }
            *static_cast<float*>(value) = std::bit_cast<float>(bits.value);
            return foundation::Result::success();
        }
        case reflection::FieldValueKind::kDouble: {
            const auto bits = reader.read_u64();
            if (!bits) {
                return bits.result;
            }
            *static_cast<double*>(value) = std::bit_cast<double>(bits.value);
            return foundation::Result::success();
        }
        case reflection::FieldValueKind::kString: {
            const auto bytes = reader.read_bytes(reader.remaining());
            if (!bytes) {
                return bytes.result;
            }
            try {
                *static_cast<std::string*>(value) =
                    std::string(reinterpret_cast<const char*>(bytes.value.data()), bytes.value.size());
            } catch (const std::bad_alloc&) {
                return out_of_memory("deserialized string allocation failed");
            }
            return foundation::Result::success();
        }
        case reflection::FieldValueKind::kObject:
        case reflection::FieldValueKind::kOpaque:
            return unsupported("field requires a reflected nested object or supported scalar type");
    }
    return unsupported("field value kind is unknown");
}

[[nodiscard]] foundation::Result write_object(const reflection::TypeRegistry& registry,
                                              const reflection::TypeDescriptor& descriptor, const void* object,
                                              BinaryWriter& writer, std::size_t depth) {
    if (depth > kMaxObjectDepth) {
        return invalid_state("serialized object nesting is too deep");
    }
    if (const foundation::Result result = validate_object(descriptor, object); !result) {
        return result;
    }

    std::size_t serializable_field_count = 0;
    for (const reflection::FieldDescriptor& field : descriptor.fields) {
        if (reflection::has_flag(field.flags, reflection::FieldFlags::kSerializable) &&
            !reflection::has_flag(field.flags, reflection::FieldFlags::kTransient)) {
            ++serializable_field_count;
        }
    }
    if (serializable_field_count > std::numeric_limits<uint32_t>::max()) {
        return invalid_state("serialized field count exceeds wire format capacity");
    }

    if (const foundation::Result result = writer.write_u64(descriptor.id.value()); !result) {
        return result;
    }
    if (const foundation::Result result = writer.write_u32(descriptor.version); !result) {
        return result;
    }
    if (const foundation::Result result = writer.write_u32(static_cast<uint32_t>(serializable_field_count)); !result) {
        return result;
    }

    for (const reflection::FieldDescriptor& field : descriptor.fields) {
        if (!reflection::has_flag(field.flags, reflection::FieldFlags::kSerializable) ||
            reflection::has_flag(field.flags, reflection::FieldFlags::kTransient)) {
            continue;
        }
        const void* field_value = field.accessor.const_access(object);
        if (field_value == nullptr) {
            return invalid_state("reflected field accessor returned null");
        }

        BinaryWriter payload;
        foundation::Result field_result = foundation::Result::success();
        if (field.value_kind == reflection::FieldValueKind::kObject) {
            const auto nested = registry.find_type(field.type);
            if (!nested) {
                return nested.result;
            }
            field_result = write_object(registry, *nested.value, field_value, payload, depth + 1);
        } else {
            field_result = write_scalar(field, field_value, payload);
        }
        if (!field_result) {
            return field_result;
        }
        if (payload.size() > kMaxFieldBytes || payload.size() > std::numeric_limits<uint32_t>::max()) {
            return invalid_state("serialized field exceeds wire format capacity");
        }
        if (const foundation::Result result = writer.write_u64(field.id.value()); !result) {
            return result;
        }
        if (const foundation::Result result = writer.write_u32(static_cast<uint32_t>(payload.size())); !result) {
            return result;
        }
        if (const foundation::Result result = writer.write_bytes(payload.data()); !result) {
            return result;
        }
    }
    return foundation::Result::success();
}

[[nodiscard]] foundation::Result read_object(const reflection::TypeRegistry& registry,
                                             const reflection::TypeDescriptor& descriptor, BinaryReader& reader,
                                             void* object, std::size_t depth) {
    if (depth > kMaxObjectDepth) {
        return invalid_state("serialized object nesting is too deep");
    }
    if (const foundation::Result result = validate_object(descriptor, object); !result) {
        return result;
    }

    const auto serialized_type = reader.read_u64();
    const auto serialized_version = reader.read_u32();
    const auto field_count = reader.read_u32();
    if (!serialized_type || !serialized_version || !field_count) {
        return read_failure();
    }
    if (serialized_type.value != descriptor.id.value()) {
        return invalid_argument("serialized type id does not match the requested type");
    }
    if (serialized_version.value == 0 || serialized_version.value > descriptor.version) {
        return unsupported("serialized type version is newer than the registered schema");
    }

    for (uint32_t index = 0; index < field_count.value; ++index) {
        const auto field_id = reader.read_u64();
        const auto payload_size = reader.read_u32();
        if (!field_id || !payload_size) {
            return read_failure();
        }
        if (payload_size.value > kMaxFieldBytes) {
            return invalid_state("serialized field exceeds configured size limit");
        }
        const auto payload = reader.read_bytes(payload_size.value);
        if (!payload) {
            return payload.result;
        }

        const auto field_iterator = std::find_if(
            descriptor.fields.begin(), descriptor.fields.end(),
            [field_id](const reflection::FieldDescriptor& field) { return field.id.value() == field_id.value; });
        if (field_iterator == descriptor.fields.end() ||
            !reflection::has_flag(field_iterator->flags, reflection::FieldFlags::kSerializable) ||
            reflection::has_flag(field_iterator->flags, reflection::FieldFlags::kTransient)) {
            continue;
        }
        void* field_value = field_iterator->accessor.mutable_access(object);
        if (field_value == nullptr) {
            return invalid_state("reflected mutable field accessor returned null");
        }
        BinaryReader field_reader(payload.value);
        foundation::Result field_result = foundation::Result::success();
        if (field_iterator->value_kind == reflection::FieldValueKind::kObject) {
            const auto nested = registry.find_type(field_iterator->type);
            if (!nested) {
                return nested.result;
            }
            field_result = read_object(registry, *nested.value, field_reader, field_value, depth + 1);
        } else {
            field_result = read_scalar(*field_iterator, field_reader, field_value);
        }
        if (!field_result) {
            return field_result;
        }
        if (field_reader.remaining() != 0) {
            return invalid_argument("serialized field payload contains trailing bytes");
        }
    }
    return foundation::Result::success();
}
}  // namespace

void BinaryWriter::clear() noexcept {
    data_.clear();
}

std::span<const uint8_t> BinaryWriter::data() const noexcept {
    return data_;
}

std::size_t BinaryWriter::size() const noexcept {
    return data_.size();
}

foundation::Result BinaryWriter::write_bytes(std::span<const uint8_t> bytes) {
    try {
        data_.insert(data_.end(), bytes.begin(), bytes.end());
    } catch (const std::bad_alloc&) {
        return out_of_memory("binary writer allocation failed");
    }
    return foundation::Result::success();
}

foundation::Result BinaryWriter::write_u32(uint32_t value) {
    const uint8_t bytes[] = {static_cast<uint8_t>(value & 0xffu), static_cast<uint8_t>((value >> 8u) & 0xffu),
                             static_cast<uint8_t>((value >> 16u) & 0xffu),
                             static_cast<uint8_t>((value >> 24u) & 0xffu)};
    return write_bytes(bytes);
}

foundation::Result BinaryWriter::write_u64(uint64_t value) {
    const uint8_t bytes[] = {
        static_cast<uint8_t>(value & 0xffu),          static_cast<uint8_t>((value >> 8u) & 0xffu),
        static_cast<uint8_t>((value >> 16u) & 0xffu), static_cast<uint8_t>((value >> 24u) & 0xffu),
        static_cast<uint8_t>((value >> 32u) & 0xffu), static_cast<uint8_t>((value >> 40u) & 0xffu),
        static_cast<uint8_t>((value >> 48u) & 0xffu), static_cast<uint8_t>((value >> 56u) & 0xffu)};
    return write_bytes(bytes);
}

void BinaryWriter::truncate(std::size_t size) noexcept {
    if (size < data_.size()) {
        data_.resize(size);
    }
}

std::size_t BinaryReader::remaining() const noexcept {
    return offset_ <= data_.size() ? data_.size() - offset_ : 0;
}

foundation::ResultValue<std::span<const uint8_t>> BinaryReader::read_bytes(std::size_t size) noexcept {
    if (size > remaining()) {
        return {read_failure(), {}};
    }
    const std::span<const uint8_t> result = data_.subspan(offset_, size);
    offset_ += size;
    return {foundation::Result::success(), result};
}

foundation::ResultValue<uint32_t> BinaryReader::read_u32() noexcept {
    const auto bytes = read_bytes(4);
    if (!bytes) {
        return {bytes.result, 0};
    }
    const uint32_t value = static_cast<uint32_t>(bytes.value[0]) | (static_cast<uint32_t>(bytes.value[1]) << 8u) |
                           (static_cast<uint32_t>(bytes.value[2]) << 16u) |
                           (static_cast<uint32_t>(bytes.value[3]) << 24u);
    return {foundation::Result::success(), value};
}

foundation::ResultValue<uint64_t> BinaryReader::read_u64() noexcept {
    const auto bytes = read_bytes(8);
    if (!bytes) {
        return {bytes.result, 0};
    }
    uint64_t value = 0;
    for (uint32_t index = 0; index < 8; ++index) {
        value |= static_cast<uint64_t>(bytes.value[index]) << (index * 8u);
    }
    return {foundation::Result::success(), value};
}

foundation::Result serialize_object(const reflection::TypeRegistry& registry, reflection::TypeId type,
                                    const void* object, BinaryWriter& writer) {
    if (!type.is_valid()) {
        return invalid_argument("serialized type id is invalid");
    }
    const auto descriptor = registry.find_type(type);
    if (!descriptor) {
        return descriptor.result;
    }
    const std::size_t initial_size = writer.size();
    const foundation::Result result = write_object(registry, *descriptor.value, object, writer, 0);
    if (!result) {
        writer.truncate(initial_size);
    }
    return result;
}

foundation::Result deserialize_object(const reflection::TypeRegistry& registry, reflection::TypeId type,
                                      BinaryReader& reader, void* object) {
    if (!type.is_valid()) {
        return invalid_argument("deserialized type id is invalid");
    }
    const auto descriptor = registry.find_type(type);
    if (!descriptor) {
        return descriptor.result;
    }
    return read_object(registry, *descriptor.value, reader, object, 0);
}
}  // namespace aeg::serialization
