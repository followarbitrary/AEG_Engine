/**
 * @file world_serializer.cpp
 * @brief Implements versioned ECS World snapshot serialization.
 */
#include "../public/world_serializer.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aeg::world {
namespace {
using foundation::invalid_argument;
using foundation::invalid_state;
using foundation::not_found;
using foundation::out_of_memory;
using foundation::unsupported;

struct ComponentArchive final {
    ecs::ComponentTypeId component_type{};
    reflection::TypeId schema_type{};
    uint32_t schema_version{0};
    std::vector<ecs::EntityId> entities{};
    std::vector<std::vector<uint8_t>> payloads{};
};

struct WorldArchive final {
    uint64_t frame_number{0};
    uint64_t fixed_tick{0};
    std::vector<ecs::EntityId> entities{};
    std::vector<ComponentArchive> components{};
};

[[nodiscard]] foundation::Result validate_options(const WorldSerializationOptions& options) {
    if (options.max_entities == 0 || options.max_component_types == 0 || options.max_component_values == 0 ||
        options.max_component_payload_bytes == 0) {
        return invalid_argument("World serialization limits must be non-zero");
    }
    return foundation::Result::success();
}

[[nodiscard]] foundation::Result validate_snapshot(const ecs::WorldSnapshot& snapshot,
                                                   const WorldSerializationOptions& options) {
    if (!snapshot.is_valid()) {
        return invalid_argument("World snapshot is invalid");
    }
    if (snapshot.entity_count() > options.max_entities) {
        return invalid_state("World snapshot exceeds the configured entity limit");
    }
    if (snapshot.component_type_count() > options.max_component_types) {
        return invalid_state("World snapshot exceeds the configured component type limit");
    }
    return foundation::Result::success();
}

[[nodiscard]] std::size_t serialized_component_count(const ecs::WorldSnapshot& snapshot) noexcept {
    std::size_t count = 0;
    for (std::size_t index = 0; index < snapshot.component_type_count(); ++index) {
        if (snapshot.component_value_count(snapshot.component_type_at(index)) != 0) {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] foundation::Result write_component(const ecs::WorldSnapshot& snapshot, ecs::ComponentTypeId component,
                                                 const reflection::TypeRegistry& registry,
                                                 serialization::BinaryWriter& writer,
                                                 const WorldSerializationOptions& options) {
    const reflection::TypeId schema_type = snapshot.component_schema_type(component);
    const uint32_t schema_version = snapshot.component_schema_version(component);
    if (!schema_type.is_valid() || schema_version == 0) {
        return unsupported("World archive requires every populated component to have a reflected schema");
    }
    const auto descriptor = registry.find_type(schema_type);
    if (!descriptor) {
        return descriptor.result;
    }
    if (descriptor.value->version != schema_version) {
        return invalid_state("World component snapshot schema version does not match the registered schema");
    }

    const std::size_t value_count = snapshot.component_value_count(component);
    if (value_count > options.max_component_values || value_count > std::numeric_limits<uint32_t>::max()) {
        return invalid_state("World component exceeds the configured value limit");
    }
    if (const foundation::Result result = writer.write_u64(component.value()); !result) {
        return result;
    }
    if (const foundation::Result result = writer.write_u64(schema_type.value()); !result) {
        return result;
    }
    if (const foundation::Result result = writer.write_u32(schema_version); !result) {
        return result;
    }
    if (const foundation::Result result = writer.write_u32(static_cast<uint32_t>(value_count)); !result) {
        return result;
    }

    for (std::size_t index = 0; index < value_count; ++index) {
        const ecs::EntityId entity = snapshot.component_entity_at(component, index);
        const void* value = snapshot.component_value_at(component, index);
        if (!entity.is_valid() || value == nullptr) {
            return invalid_state("World component snapshot contains an invalid value");
        }
        serialization::BinaryWriter payload;
        if (const foundation::Result result = serialization::serialize_object(registry, schema_type, value, payload);
            !result) {
            return result;
        }
        if (payload.size() > options.max_component_payload_bytes ||
            payload.size() > std::numeric_limits<uint32_t>::max()) {
            return invalid_state("World component payload exceeds the configured size limit");
        }
        if (const foundation::Result result = writer.write_u32(entity.index); !result) {
            return result;
        }
        if (const foundation::Result result = writer.write_u32(entity.generation); !result) {
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

[[nodiscard]] foundation::Result read_archive(serialization::BinaryReader& reader,
                                              const reflection::TypeRegistry& registry, WorldArchive& archive,
                                              const WorldSerializationOptions& options) {
    const auto magic = reader.read_u64();
    const auto format_version = reader.read_u32();
    const auto reserved = reader.read_u32();
    const auto frame_number = reader.read_u64();
    const auto fixed_tick = reader.read_u64();
    const auto entity_count = reader.read_u32();
    const auto component_count = reader.read_u32();
    if (!magic || !format_version || !reserved || !frame_number || !fixed_tick || !entity_count || !component_count) {
        return invalid_argument("World archive header is truncated");
    }
    if (magic.value != WorldSerializer::kMagic || format_version.value != WorldSerializer::kFormatVersion ||
        reserved.value != 0) {
        return unsupported("World archive format is not supported");
    }
    if (entity_count.value > options.max_entities || component_count.value > options.max_component_types) {
        return invalid_state("World archive exceeds the configured count limits");
    }

    try {
        archive.frame_number = frame_number.value;
        archive.fixed_tick = fixed_tick.value;
        archive.entities.reserve(entity_count.value);
        archive.components.reserve(component_count.value);
    } catch (const std::bad_alloc&) {
        return out_of_memory("World archive allocation failed");
    }

    std::unordered_map<uint64_t, bool> entity_ids;
    try {
        entity_ids.reserve(entity_count.value);
        for (uint32_t index = 0; index < entity_count.value; ++index) {
            const auto entity_index = reader.read_u32();
            const auto generation = reader.read_u32();
            if (!entity_index || !generation || generation.value == 0 ||
                entity_index.value == ecs::kInvalidEntityIndex) {
                return invalid_argument("World archive contains an invalid entity identity");
            }
            const ecs::EntityId entity{entity_index.value, generation.value};
            const uint64_t identity = (static_cast<uint64_t>(entity.index) << 32u) | entity.generation;
            if (!entity_ids.emplace(identity, true).second) {
                return invalid_state("World archive contains a duplicate entity identity");
            }
            archive.entities.push_back(entity);
        }

        std::unordered_map<uint64_t, bool> component_types;
        component_types.reserve(component_count.value);
        for (uint32_t component_index = 0; component_index < component_count.value; ++component_index) {
            const auto component_type = reader.read_u64();
            const auto schema_type = reader.read_u64();
            const auto schema_version = reader.read_u32();
            const auto value_count = reader.read_u32();
            if (!component_type || !schema_type || !schema_version || !value_count) {
                return invalid_argument("World archive component header is truncated");
            }
            if (component_type.value == 0 || schema_type.value == 0 || schema_version.value == 0 ||
                value_count.value > options.max_component_values) {
                return invalid_state("World archive component header is invalid");
            }
            if (!component_types.emplace(component_type.value, true).second) {
                return invalid_state("World archive contains a duplicate component type");
            }
            const reflection::TypeId reflected_type(schema_type.value);
            const auto descriptor = registry.find_type(reflected_type);
            if (!descriptor) {
                return descriptor.result;
            }
            if (descriptor.value->version < schema_version.value) {
                return unsupported("World archive component schema is newer than the registered schema");
            }

            ComponentArchive component;
            component.component_type = ecs::ComponentTypeId(component_type.value);
            component.schema_type = reflected_type;
            component.schema_version = schema_version.value;
            component.entities.reserve(value_count.value);
            component.payloads.reserve(value_count.value);
            std::unordered_map<uint64_t, bool> component_entities;
            component_entities.reserve(value_count.value);
            for (uint32_t value_index = 0; value_index < value_count.value; ++value_index) {
                const auto entity_index = reader.read_u32();
                const auto generation = reader.read_u32();
                const auto payload_size = reader.read_u32();
                if (!entity_index || !generation || !payload_size || generation.value == 0 ||
                    entity_index.value == ecs::kInvalidEntityIndex) {
                    return invalid_argument("World archive component value header is invalid");
                }
                if (payload_size.value > options.max_component_payload_bytes) {
                    return invalid_state("World archive component payload exceeds the configured size limit");
                }
                const ecs::EntityId entity{entity_index.value, generation.value};
                const uint64_t identity = (static_cast<uint64_t>(entity.index) << 32u) | entity.generation;
                if (entity_ids.find(identity) == entity_ids.end()) {
                    return invalid_state("World archive component references an unknown entity");
                }
                if (!component_entities.emplace(identity, true).second) {
                    return invalid_state("World archive component contains a duplicate entity value");
                }
                const auto payload = reader.read_bytes(payload_size.value);
                if (!payload) {
                    return payload.result;
                }
                component.entities.push_back(entity);
                component.payloads.emplace_back(payload.value.begin(), payload.value.end());
            }
            archive.components.push_back(std::move(component));
        }
    } catch (const std::bad_alloc&) {
        return out_of_memory("World archive allocation failed");
    }
    return foundation::Result::success();
}

[[nodiscard]] foundation::Result apply_archive(ecs::World& world, const reflection::TypeRegistry& registry,
                                               const WorldArchive& archive) {
    if (const foundation::Result result = world.clear(); !result) {
        return result;
    }

    std::unordered_map<uint64_t, ecs::EntityId> entity_remap;
    try {
        entity_remap.reserve(archive.entities.size());
        for (const ecs::EntityId source_entity : archive.entities) {
            const auto created = world.create_entity();
            if (!created) {
                return created.result;
            }
            const uint64_t identity = (static_cast<uint64_t>(source_entity.index) << 32u) | source_entity.generation;
            entity_remap.emplace(identity, created.value);
        }

        for (const ComponentArchive& component : archive.components) {
            const auto descriptor = world.find_component_descriptor(component.component_type);
            if (!descriptor) {
                return descriptor.result;
            }
            if (descriptor.value.schema_type != component.schema_type ||
                descriptor.value.schema_version < component.schema_version) {
                return unsupported("World component registration does not satisfy the archive schema");
            }
            const auto reflected_type = registry.find_type(component.schema_type);
            if (!reflected_type) {
                return reflected_type.result;
            }
            for (std::size_t index = 0; index < component.entities.size(); ++index) {
                const ecs::EntityId source_entity = component.entities[index];
                const uint64_t identity =
                    (static_cast<uint64_t>(source_entity.index) << 32u) | source_entity.generation;
                const auto remapped = entity_remap.find(identity);
                if (remapped == entity_remap.end()) {
                    return invalid_state("World archive entity remap is incomplete");
                }
                if (const foundation::Result result =
                        world.emplace_default_component(remapped->second, component.component_type);
                    !result) {
                    return result;
                }
                const auto value = world.get_component_data(remapped->second, component.component_type);
                if (!value) {
                    return value.result;
                }
                serialization::BinaryReader payload(component.payloads[index]);
                if (const foundation::Result result =
                        serialization::deserialize_object(registry, component.schema_type, payload, value.value);
                    !result) {
                    return result;
                }
                if (payload.remaining() != 0) {
                    return invalid_argument("World archive component payload contains trailing bytes");
                }
            }
        }
    } catch (const std::bad_alloc&) {
        return out_of_memory("World archive restore allocation failed");
    }
    return foundation::Result::success();
}
}  // namespace

foundation::Result WorldSerializer::serialize(const ecs::WorldSnapshot& snapshot,
                                              const reflection::TypeRegistry& registry,
                                              serialization::BinaryWriter& writer,
                                              const WorldSerializationOptions& options) {
    if (const foundation::Result result = validate_options(options); !result) {
        return result;
    }
    if (const foundation::Result result = validate_snapshot(snapshot, options); !result) {
        return result;
    }

    const std::size_t component_count = serialized_component_count(snapshot);
    if (component_count > options.max_component_types || component_count > std::numeric_limits<uint32_t>::max()) {
        return invalid_state("World snapshot exceeds the configured serialized component type limit");
    }

    serialization::BinaryWriter encoded;
    try {
        if (const foundation::Result result = encoded.write_u64(kMagic); !result) {
            return result;
        }
        if (const foundation::Result result = encoded.write_u32(kFormatVersion); !result) {
            return result;
        }
        if (const foundation::Result result = encoded.write_u32(0); !result) {
            return result;
        }
        if (const foundation::Result result = encoded.write_u64(snapshot.frame_number()); !result) {
            return result;
        }
        if (const foundation::Result result = encoded.write_u64(snapshot.fixed_tick()); !result) {
            return result;
        }
        if (const foundation::Result result = encoded.write_u32(static_cast<uint32_t>(snapshot.entity_count()));
            !result) {
            return result;
        }
        if (const foundation::Result result = encoded.write_u32(static_cast<uint32_t>(component_count)); !result) {
            return result;
        }
        for (std::size_t index = 0; index < snapshot.entity_count(); ++index) {
            const ecs::EntityId entity = snapshot.entity_at(index);
            if (!entity.is_valid()) {
                return invalid_state("World snapshot contains an invalid entity");
            }
            if (const foundation::Result result = encoded.write_u32(entity.index); !result) {
                return result;
            }
            if (const foundation::Result result = encoded.write_u32(entity.generation); !result) {
                return result;
            }
        }
        for (std::size_t index = 0; index < snapshot.component_type_count(); ++index) {
            const ecs::ComponentTypeId component = snapshot.component_type_at(index);
            if (snapshot.component_value_count(component) == 0) {
                continue;
            }
            if (const foundation::Result result = write_component(snapshot, component, registry, encoded, options);
                !result) {
                return result;
            }
        }
    } catch (const std::bad_alloc&) {
        return out_of_memory("World archive serialization allocation failed");
    }
    return writer.write_bytes(encoded.data());
}

foundation::Result WorldSerializer::deserialize(ecs::World& world, const reflection::TypeRegistry& registry,
                                                serialization::BinaryReader& reader,
                                                const WorldSerializationOptions& options) {
    if (const foundation::Result result = validate_options(options); !result) {
        return result;
    }
    WorldArchive archive;
    if (const foundation::Result result = read_archive(reader, registry, archive, options); !result) {
        return result;
    }
    const foundation::Result result = apply_archive(world, registry, archive);
    if (!result) {
        static_cast<void>(world.clear());
    }
    return result;
}
}  // namespace aeg::world
