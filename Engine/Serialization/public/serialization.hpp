/**
 * @file serialization.hpp
 * @brief Reflection-driven binary serialization primitives.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "reflection.hpp"
#include "result.hpp"

namespace aeg::serialization {

/** @brief Append-only byte writer using the engine's little-endian wire format. */
class BinaryWriter final {
public:
    BinaryWriter() = default;
    ~BinaryWriter() = default;

    BinaryWriter(const BinaryWriter&) = delete;
    BinaryWriter& operator=(const BinaryWriter&) = delete;
    BinaryWriter(BinaryWriter&&) noexcept = default;
    BinaryWriter& operator=(BinaryWriter&&) noexcept = default;

    /** @brief Removes all encoded bytes. */
    void clear() noexcept;
    /** @brief Returns the encoded byte span. */
    [[nodiscard]] std::span<const uint8_t> data() const noexcept;
    /** @brief Returns the current encoded byte count. */
    [[nodiscard]] std::size_t size() const noexcept;
    /** @brief Writes one byte sequence to the stream. */
    [[nodiscard]] foundation::Result write_bytes(std::span<const uint8_t> bytes);
    /** @brief Writes a length-prefixed byte string with an explicit size limit. */
    [[nodiscard]] foundation::Result write_string(
        std::string_view value, uint32_t max_bytes = (std::numeric_limits<uint32_t>::max)());
    /** @brief Writes one unsigned 16-bit little-endian value. */
    [[nodiscard]] foundation::Result write_u16(uint16_t value);
    /** @brief Writes one unsigned 32-bit little-endian value. */
    [[nodiscard]] foundation::Result write_u32(uint32_t value);
    /** @brief Writes one unsigned 64-bit little-endian value. */
    [[nodiscard]] foundation::Result write_u64(uint64_t value);

private:
    void truncate(std::size_t size) noexcept;

    std::vector<uint8_t> data_{};

    friend foundation::Result serialize_object(const reflection::TypeRegistry& registry, reflection::TypeId type,
                                               const void* object, BinaryWriter& writer);
};

/** @brief Bounds-checked reader for the engine's little-endian wire format. */
class BinaryReader final {
public:
    explicit BinaryReader(std::span<const uint8_t> data) noexcept : data_(data) {}

    /** @brief Returns the unread byte count. */
    [[nodiscard]] std::size_t remaining() const noexcept;
    /** @brief Reads one byte sequence without copying it. */
    [[nodiscard]] foundation::ResultValue<std::span<const uint8_t>> read_bytes(std::size_t size) noexcept;
    /** @brief Reads a length-prefixed byte string with an explicit size limit. */
    [[nodiscard]] foundation::ResultValue<std::string> read_string(
        uint32_t max_bytes = (std::numeric_limits<uint32_t>::max)());
    /** @brief Reads one unsigned 16-bit little-endian value. */
    [[nodiscard]] foundation::ResultValue<uint16_t> read_u16() noexcept;
    /** @brief Reads one unsigned 32-bit little-endian value. */
    [[nodiscard]] foundation::ResultValue<uint32_t> read_u32() noexcept;
    /** @brief Reads one unsigned 64-bit little-endian value. */
    [[nodiscard]] foundation::ResultValue<uint64_t> read_u64() noexcept;

private:
    std::span<const uint8_t> data_{};
    std::size_t offset_{0};
};

/** @brief Serializes one reflected object into a self-describing binary record. */
[[nodiscard]] foundation::Result serialize_object(const reflection::TypeRegistry& registry, reflection::TypeId type,
                                                  const void* object, BinaryWriter& writer);

/** @brief Deserializes one reflected object and skips unknown serialized fields. */
[[nodiscard]] foundation::Result deserialize_object(const reflection::TypeRegistry& registry, reflection::TypeId type,
                                                    BinaryReader& reader, void* object);
}  // namespace aeg::serialization
