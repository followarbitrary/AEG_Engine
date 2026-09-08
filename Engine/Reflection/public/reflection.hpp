/**
 * @file reflection.hpp
 * @brief Runtime type metadata and field access used by tools and serialization.
 */
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

#include "result.hpp"

namespace aeg::reflection {

/** @brief Stable hash-backed identity for a reflected type or field. */
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

struct TypeIdTag;
struct FieldIdTag;

using TypeId = StableId<TypeIdTag>;
using FieldId = StableId<FieldIdTag>;

/** @brief Hash function for reflected identities. */
template <typename Tag>
struct StableIdHash final {
    [[nodiscard]] std::size_t operator()(StableId<Tag> id) const noexcept {
        return static_cast<std::size_t>(id.value());
    }
};

/** @brief Semantic category of a reflected type. */
enum class TypeKind : uint8_t {
    kStruct = 0,
    kClass = 1,
    kComponent = 2,
    kEnum = 3,
    kAsset = 4,
};

/** @brief Encoding category used by the built-in binary serializer. */
enum class FieldValueKind : uint8_t {
    kBool = 0,
    kInt32 = 1,
    kUInt32 = 2,
    kInt64 = 3,
    kUInt64 = 4,
    kFloat = 5,
    kDouble = 6,
    kString = 7,
    kObject = 8,
    kOpaque = 9,
};

/** @brief Editor and persistence policy attached to a reflected field. */
enum class FieldFlags : uint32_t {
    kNone = 0,
    kSerializable = 1u << 0,
    kEditable = 1u << 1,
    kTransient = 1u << 2,
    kReadOnly = 1u << 3,
};

constexpr FieldFlags operator|(FieldFlags left, FieldFlags right) noexcept {
    return static_cast<FieldFlags>(static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
}

constexpr FieldFlags operator&(FieldFlags left, FieldFlags right) noexcept {
    return static_cast<FieldFlags>(static_cast<uint32_t>(left) & static_cast<uint32_t>(right));
}

[[nodiscard]] constexpr bool has_flag(FieldFlags value, FieldFlags flag) noexcept {
    return (value & flag) != FieldFlags::kNone;
}

/** @brief Type-erased accessors for one reflected data member. */
struct FieldAccessor final {
    using MutableAccessFunction = std::function<void*(void*)>;
    using ConstAccessFunction = std::function<const void*(const void*)>;

    MutableAccessFunction mutable_access{};
    ConstAccessFunction const_access{};
    FieldValueKind value_kind{FieldValueKind::kOpaque};
    std::size_t value_size{0};
    std::size_t value_alignment{0};

    [[nodiscard]] bool is_valid() const noexcept {
        return static_cast<bool>(mutable_access) && static_cast<bool>(const_access) && value_size != 0 &&
               value_alignment != 0;
    }
};

/** @brief Metadata describing one editable and/or serializable field. */
struct FieldDescriptor final {
    FieldId id{};
    std::string name{};
    TypeId type{};
    FieldValueKind value_kind{FieldValueKind::kOpaque};
    FieldFlags flags{FieldFlags::kSerializable | FieldFlags::kEditable};
    FieldAccessor accessor{};
};

/** @brief Metadata describing one reflected type and its fields. */
struct TypeDescriptor final {
    TypeId id{};
    std::string name{};
    uint32_t version{1};
    TypeKind kind{TypeKind::kStruct};
    std::size_t size{0};
    std::size_t alignment{0};
    std::vector<FieldDescriptor> fields{};
};

namespace detail {
template <typename Type>
using RemoveCvrefT = std::remove_cv_t<std::remove_reference_t<Type>>;

template <typename Type>
[[nodiscard]] constexpr FieldValueKind field_value_kind() noexcept {
    using ValueType = RemoveCvrefT<Type>;
    if constexpr (std::is_same_v<ValueType, bool>) {
        return FieldValueKind::kBool;
    } else if constexpr (std::is_same_v<ValueType, int32_t>) {
        return FieldValueKind::kInt32;
    } else if constexpr (std::is_same_v<ValueType, uint32_t>) {
        return FieldValueKind::kUInt32;
    } else if constexpr (std::is_same_v<ValueType, int64_t>) {
        return FieldValueKind::kInt64;
    } else if constexpr (std::is_same_v<ValueType, uint64_t>) {
        return FieldValueKind::kUInt64;
    } else if constexpr (std::is_same_v<ValueType, float>) {
        return FieldValueKind::kFloat;
    } else if constexpr (std::is_same_v<ValueType, double>) {
        return FieldValueKind::kDouble;
    } else if constexpr (std::is_same_v<ValueType, std::string>) {
        return FieldValueKind::kString;
    } else {
        return FieldValueKind::kObject;
    }
}
}  // namespace detail

/** @brief Returns the stable built-in or RTTI-backed identity of a C++ type. */
template <typename Type>
[[nodiscard]] TypeId type_id() noexcept {
    using ValueType = detail::RemoveCvrefT<Type>;
    if constexpr (std::is_same_v<ValueType, bool>) {
        return TypeId("builtin.bool");
    } else if constexpr (std::is_same_v<ValueType, int32_t>) {
        return TypeId("builtin.int32");
    } else if constexpr (std::is_same_v<ValueType, uint32_t>) {
        return TypeId("builtin.uint32");
    } else if constexpr (std::is_same_v<ValueType, int64_t>) {
        return TypeId("builtin.int64");
    } else if constexpr (std::is_same_v<ValueType, uint64_t>) {
        return TypeId("builtin.uint64");
    } else if constexpr (std::is_same_v<ValueType, float>) {
        return TypeId("builtin.float");
    } else if constexpr (std::is_same_v<ValueType, double>) {
        return TypeId("builtin.double");
    } else if constexpr (std::is_same_v<ValueType, std::string>) {
        return TypeId("builtin.string");
    } else {
        return TypeId(std::string_view(typeid(ValueType).name()));
    }
}

/** @brief Creates a type-erased accessor for one C++ data member. */
template <typename Owner, typename Member>
[[nodiscard]] FieldAccessor make_member_accessor(Member Owner::* member) {
    using ValueType = detail::RemoveCvrefT<Member>;
    FieldAccessor accessor{};
    accessor.mutable_access = [member](void* object) -> void* {
        return object == nullptr ? nullptr : &(static_cast<Owner*>(object)->*member);
    };
    accessor.const_access = [member](const void* object) -> const void* {
        return object == nullptr ? nullptr : &(static_cast<const Owner*>(object)->*member);
    };
    accessor.value_kind = detail::field_value_kind<ValueType>();
    accessor.value_size = sizeof(ValueType);
    accessor.value_alignment = alignof(ValueType);
    return accessor;
}

/** @brief Fluent builder for one reflected type descriptor. */
class TypeBuilder final {
public:
    explicit TypeBuilder(TypeDescriptor descriptor) : descriptor_(std::move(descriptor)) {}

    TypeBuilder(const TypeBuilder&) = delete;
    TypeBuilder& operator=(const TypeBuilder&) = delete;
    TypeBuilder(TypeBuilder&&) noexcept = default;
    TypeBuilder& operator=(TypeBuilder&&) noexcept = default;

    /** @brief Adds one field and derives its stable identity from its name. */
    [[nodiscard]] foundation::Result add_field(std::string name, TypeId type, FieldAccessor accessor,
                                               FieldFlags flags = FieldFlags::kSerializable | FieldFlags::kEditable);

    /** @brief Finalizes the descriptor after validating field identities and accessors. */
    [[nodiscard]] foundation::ResultValue<TypeDescriptor> finish() &&;

private:
    TypeDescriptor descriptor_{};
    foundation::Result status_{};
};

/** @brief Thread-safe registry of reflected type descriptors. */
class TypeRegistry final {
public:
    TypeRegistry() = default;
    ~TypeRegistry() = default;

    TypeRegistry(const TypeRegistry&) = delete;
    TypeRegistry& operator=(const TypeRegistry&) = delete;
    TypeRegistry(TypeRegistry&&) = delete;
    TypeRegistry& operator=(TypeRegistry&&) = delete;

    /** @brief Registers one immutable type descriptor. */
    [[nodiscard]] foundation::Result register_type(TypeDescriptor descriptor);
    /** @brief Removes one type descriptor that is no longer needed by a module. */
    [[nodiscard]] foundation::Result unregister_type(TypeId id);
    /** @brief Tests whether a type identity is registered. */
    [[nodiscard]] bool contains(TypeId id) const noexcept;
    /** @brief Returns a copied descriptor safe to retain after the registry lock is released. */
    [[nodiscard]] foundation::ResultValue<std::shared_ptr<const TypeDescriptor>> find_type(TypeId id) const;
    /** @brief Returns a copied field descriptor from one registered type. */
    [[nodiscard]] foundation::ResultValue<FieldDescriptor> find_field(TypeId type, FieldId field) const;
    /** @brief Captures all registered descriptors in stable identity order. */
    [[nodiscard]] foundation::ResultValue<std::vector<TypeDescriptor>> snapshot() const;

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<TypeId, std::shared_ptr<const TypeDescriptor>, StableIdHash<TypeIdTag>> types_{};
};
}  // namespace aeg::reflection
