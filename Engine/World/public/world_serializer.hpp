/**
 * @file world_serializer.hpp
 * @brief Versioned serialization of ECS World snapshots.
 */
#pragma once

#include <cstdint>

#include "ecs.hpp"
#include "reflection.hpp"
#include "serialization.hpp"

namespace aeg::world {

/** @brief Resource limits applied while encoding or decoding a World archive. */
struct WorldSerializationOptions final {
    uint32_t max_entities{1'000'000};
    uint32_t max_component_types{4'096};
    uint32_t max_component_values{1'000'000};
    uint32_t max_component_payload_bytes{64u * 1024u * 1024u};
};

/** @brief Serializes and restores ECS World snapshots using reflected component schemas. */
class WorldSerializer final {
public:
    inline static constexpr uint64_t kMagic = 0x3145564948474541ull;
    inline static constexpr uint32_t kFormatVersion = 1;

    WorldSerializer() = delete;

    /** @brief Writes one self-describing World snapshot archive. */
    [[nodiscard]] static foundation::Result serialize(const ecs::WorldSnapshot& snapshot,
                                                      const reflection::TypeRegistry& registry,
                                                      serialization::BinaryWriter& writer,
                                                      const WorldSerializationOptions& options = {});

    /** @brief Validates and restores one World archive into an existing World. */
    [[nodiscard]] static foundation::Result deserialize(ecs::World& world, const reflection::TypeRegistry& registry,
                                                        serialization::BinaryReader& reader,
                                                        const WorldSerializationOptions& options = {});
};
}  // namespace aeg::world
