/**
 * @file stable_id.hpp
 * @brief Shared hash-backed stable identity primitive.
 */
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace aeg::foundation {

template <typename Tag>
class StableId final {
public:
    constexpr StableId() noexcept = default;
    explicit constexpr StableId(uint64_t value) noexcept : value_(value) {}
    explicit constexpr StableId(std::string_view name) noexcept : value_(hash_name(name)) {}

    [[nodiscard]] constexpr bool is_valid() const noexcept { return value_ != 0; }
    [[nodiscard]] constexpr uint64_t value() const noexcept { return value_; }

    friend constexpr bool operator==(StableId, StableId) = default;
    friend constexpr auto operator<=>(StableId, StableId) = default;

private:
    static constexpr uint64_t hash_name(std::string_view name) noexcept {
        uint64_t hash = 14695981039346656037ull;
        for (const char character : name) {
            hash ^= static_cast<uint8_t>(character);
            hash *= 1099511628211ull;
        }
        return hash == 0 ? 1 : hash;
    }

    uint64_t value_{0};
};

template <typename Tag>
struct StableIdHash final {
    [[nodiscard]] std::size_t operator()(StableId<Tag> id) const noexcept {
        return static_cast<std::size_t>(id.value());
    }
};

}  // namespace aeg::foundation
