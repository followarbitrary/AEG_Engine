/**
 * @file scene_asset.hpp
 * @brief Stable scene document model and runtime instantiation contracts.
 */
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "asset_identity.hpp"
#include "ecs.hpp"
#include "reflection.hpp"
#include "result.hpp"
#include "serialization.hpp"

namespace aeg::scene {

/** @brief Distinguishes the storage domain of one 128-bit scene identity. */
template <typename Tag>
class Guid128 final {
public:
    constexpr Guid128() noexcept = default;
    constexpr Guid128(uint64_t high, uint64_t low) noexcept : high_(high), low_(low) {}

    [[nodiscard]] constexpr bool is_valid() const noexcept { return high_ != 0 || low_ != 0; }
    [[nodiscard]] constexpr uint64_t high() const noexcept { return high_; }
    [[nodiscard]] constexpr uint64_t low() const noexcept { return low_; }

    /** @brief Returns canonical UUID bytes with each high/low word in big-endian order. */
    [[nodiscard]] constexpr foundation::Uuid uuid() const noexcept {
        foundation::Uuid result{};
        for (std::size_t index = 0; index != 8; ++index) {
            const auto shift = static_cast<unsigned>((7 - index) * 8);
            result.bytes[index] = static_cast<uint8_t>(high_ >> shift);
            result.bytes[index + 8] = static_cast<uint8_t>(low_ >> shift);
        }
        return result;
    }

    /** @brief Restores the high/low words from canonical UUID bytes without changing the identity domain. */
    [[nodiscard]] static constexpr Guid128 from_uuid(const foundation::Uuid& uuid) noexcept {
        uint64_t high = 0;
        uint64_t low = 0;
        for (std::size_t index = 0; index != 8; ++index) {
            high = (high << 8u) | uuid.bytes[index];
            low = (low << 8u) | uuid.bytes[index + 8];
        }
        return Guid128{high, low};
    }

    friend constexpr bool operator==(Guid128, Guid128) = default;
    friend constexpr auto operator<=>(Guid128, Guid128) = default;

private:
    uint64_t high_{0};
    uint64_t low_{0};
};

struct SceneAssetIdTag;
struct SceneEntityIdTag;

using SceneAssetId = Guid128<SceneAssetIdTag>;
using SceneEntityId = Guid128<SceneEntityIdTag>;

/** @brief Hashes a scene identity for associative containers. */
template <typename Tag>
struct Guid128Hash final {
    [[nodiscard]] std::size_t operator()(Guid128<Tag> value) const noexcept {
        const std::size_t high = static_cast<std::size_t>(value.high());
        const std::size_t low = static_cast<std::size_t>(value.low());
        return high ^ (low + static_cast<std::size_t>(0x9e3779b9u) + (high << 6u) + (high >> 2u));
    }
};

/** @brief Editor visibility and locking state attached to a scene entity. */
enum class SceneEntityFlags : uint32_t {
    kNone = 0,
    kHidden = 1u << 0,
    kLocked = 1u << 1,
};

/** @brief Defines how descendants are handled when a Scene entity is removed. */
enum class SceneEntityRemovalPolicy : uint8_t {
    kCascade,
    kPromoteChildren,
};

constexpr SceneEntityFlags operator|(SceneEntityFlags left, SceneEntityFlags right) noexcept {
    return static_cast<SceneEntityFlags>(static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
}

constexpr SceneEntityFlags operator&(SceneEntityFlags left, SceneEntityFlags right) noexcept {
    return static_cast<SceneEntityFlags>(static_cast<uint32_t>(left) & static_cast<uint32_t>(right));
}

[[nodiscard]] constexpr bool has_flag(SceneEntityFlags value, SceneEntityFlags flag) noexcept {
    return (value & flag) != SceneEntityFlags::kNone;
}

/** @brief Stable scene identity and hierarchy metadata for one entity. */
struct SceneEntityRecord final {
    SceneEntityId id{};
    SceneEntityId parent{};
    std::string name{};
    SceneEntityFlags flags{SceneEntityFlags::kNone};
    std::string tag{};
};

/** @brief Serialized component payload attached to one stable scene entity. */
struct SceneComponentRecord final {
    SceneEntityId entity{};
    ecs::ComponentTypeId component_type{};
    reflection::TypeId schema_type{};
    uint32_t schema_version{0};
    std::vector<uint8_t> payload{};
};

/** @brief Limits applied while reading or writing a Scene Asset archive. */
struct SceneSerializationOptions final {
    uint32_t max_name_bytes{1u * 1024u * 1024u};
    uint32_t max_entities{1'000'000};
    uint32_t max_components{4'000'000};
    uint32_t max_component_payload_bytes{64u * 1024u * 1024u};
};

/** @brief Runtime mapping from a stable scene entity to a World entity. */
struct SceneEntityBinding final {
    SceneEntityId scene_entity{};
    ecs::EntityId runtime_entity{};
};

/** @brief Resulting runtime identity map after one Scene Asset is instantiated. */
class SceneInstance final {
public:
    SceneInstance() noexcept = default;
    ~SceneInstance() = default;

    [[nodiscard]] SceneAssetId asset() const noexcept { return asset_; }
    [[nodiscard]] std::size_t entity_count() const noexcept { return bindings_.size(); }
    [[nodiscard]] std::span<const SceneEntityBinding> entities() const noexcept { return bindings_; }

    /** @brief Resolves one stable scene entity to its current runtime EntityId. */
    [[nodiscard]] foundation::ResultValue<ecs::EntityId> runtime_entity(SceneEntityId scene_entity) const;

private:
    SceneAssetId asset_{};
    std::vector<SceneEntityBinding> bindings_{};

    friend class SceneAsset;
};

/** @brief Options controlling replacement or additive Scene Asset instantiation. */
struct SceneInstantiationOptions final {
    bool clear_world{true};
};

/** @brief Editable, versioned scene document independent from runtime EntityId values. */
class SceneAsset final {
public:
    SceneAsset() noexcept = default;
    explicit SceneAsset(SceneAssetId id, std::string name = {}, uint32_t version = 1);
    ~SceneAsset() = default;

    SceneAsset(const SceneAsset&) = default;
    SceneAsset& operator=(const SceneAsset&) = default;
    SceneAsset(SceneAsset&&) noexcept = default;
    SceneAsset& operator=(SceneAsset&&) noexcept = default;

    [[nodiscard]] SceneAssetId id() const noexcept { return id_; }
    [[nodiscard]] std::string_view name() const noexcept { return name_; }
    [[nodiscard]] uint32_t version() const noexcept { return version_; }
    [[nodiscard]] std::span<const SceneEntityRecord> entities() const noexcept { return entities_; }
    [[nodiscard]] std::span<const SceneComponentRecord> components() const noexcept { return components_; }

    /** @brief Replaces the asset identity and metadata after validating the version. */
    [[nodiscard]] foundation::Result set_identity(SceneAssetId id, std::string name, uint32_t version = 1);
    /** @brief Adds one stable entity record to the scene document. */
    [[nodiscard]] foundation::Result add_entity(SceneEntityRecord entity);
    /** @brief Updates the user-visible name of one stable scene entity. */
    [[nodiscard]] foundation::Result set_entity_name(SceneEntityId entity, std::string name);
    /** @brief Updates the editor tag stored on one stable scene entity. */
    [[nodiscard]] foundation::Result set_entity_tag(SceneEntityId entity, std::string tag);
    /** @brief Returns the direct children in their persisted sibling order. */
    [[nodiscard]] foundation::ResultValue<std::vector<SceneEntityId>> children(SceneEntityId parent) const;
    /** @brief Reparents entities and optionally inserts them at a sibling index. */
    [[nodiscard]] foundation::Result reparent_entities(std::span<const SceneEntityId> entities, SceneEntityId parent,
                                                       std::size_t sibling_index = static_cast<std::size_t>(-1));
    /** @brief Removes entities using either cascade or child-promotion semantics. */
    [[nodiscard]] foundation::Result remove_entities(std::span<const SceneEntityId> entities,
                                                     SceneEntityRemovalPolicy policy);
    /** @brief Adds one reflected component payload to the scene document. */
    [[nodiscard]] foundation::Result add_component(SceneComponentRecord component);
    /** @brief Replaces one reflected component's serialized payload in place. */
    [[nodiscard]] foundation::Result replace_component_payload(SceneEntityId entity,
                                                               ecs::ComponentTypeId component_type,
                                                               std::vector<uint8_t> payload);
    /** @brief Encodes one reflected component value and adds it to the scene document. */
    [[nodiscard]] foundation::Result add_reflected_component(SceneEntityId entity, ecs::ComponentTypeId component_type,
                                                             const reflection::TypeRegistry& registry,
                                                             reflection::TypeId schema_type, const void* value);
    /** @brief Removes one reflected component payload from a stable scene entity. */
    [[nodiscard]] foundation::Result remove_component(SceneEntityId entity, ecs::ComponentTypeId component_type);
    /** @brief Removes all entities and component records while retaining asset metadata. */
    void clear() noexcept;
    /** @brief Validates identities, hierarchy, component references, and duplicate records. */
    [[nodiscard]] foundation::Result validate() const;
    /** @brief Instantiates the scene into a World and returns stable-to-runtime bindings. */
    [[nodiscard]] foundation::ResultValue<SceneInstance> instantiate(
        ecs::World& world, const reflection::TypeRegistry& registry,
        const SceneInstantiationOptions& options = {}) const;

private:
    SceneAssetId id_{};
    std::string name_{};
    uint32_t version_{0};
    std::vector<SceneEntityRecord> entities_{};
    std::vector<SceneComponentRecord> components_{};
};

/** @brief Versioned binary archive codec for Scene Asset documents. */
class SceneSerializer final {
public:
    inline static constexpr uint64_t kMagic = 0x3154534547414541ull;
    inline static constexpr uint32_t kFormatVersion = 2;

    SceneSerializer() = delete;

    /** @brief Writes one deterministic Scene Asset archive without changing the writer on failure. */
    [[nodiscard]] static foundation::Result serialize(const SceneAsset& asset, serialization::BinaryWriter& writer,
                                                      const SceneSerializationOptions& options = {});
    /** @brief Reads one Scene Asset archive and leaves later reader bytes available. */
    [[nodiscard]] static foundation::ResultValue<SceneAsset> deserialize(serialization::BinaryReader& reader,
                                                                         const SceneSerializationOptions& options = {});
};
}  // namespace aeg::scene
