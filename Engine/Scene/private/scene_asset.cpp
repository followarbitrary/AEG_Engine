/**
 * @file scene_asset.cpp
 * @brief Implements Scene Asset validation, instantiation, and archive encoding.
 */
#include "../public/scene_asset.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace aeg::scene {
namespace {
using foundation::invalid_argument;
using foundation::invalid_state;
using foundation::not_found;
using foundation::out_of_memory;
using foundation::unsupported;

constexpr uint32_t kKnownSceneEntityFlags =
    static_cast<uint32_t>(SceneEntityFlags::kHidden) | static_cast<uint32_t>(SceneEntityFlags::kLocked);

using SceneEntityIndex = std::unordered_map<SceneEntityId, std::size_t, Guid128Hash<SceneEntityIdTag>>;

struct SceneComponentKey final {
    SceneEntityId entity{};
    ecs::ComponentTypeId component{};

    friend bool operator==(const SceneComponentKey&, const SceneComponentKey&) = default;
};

struct SceneComponentKeyHash final {
    [[nodiscard]] std::size_t operator()(const SceneComponentKey& key) const noexcept {
        const std::size_t entity_hash = Guid128Hash<SceneEntityIdTag>{}(key.entity);
        const std::size_t component_hash = ecs::ComponentTypeIdHash{}(key.component);
        return entity_hash ^
               (component_hash + static_cast<std::size_t>(0x9e3779b9u) + (entity_hash << 6u) + (entity_hash >> 2u));
    }
};

[[nodiscard]] foundation::Result validate_serialization_options(const SceneSerializationOptions& options) {
    if (options.max_name_bytes == 0 || options.max_entities == 0 || options.max_components == 0 ||
        options.max_component_payload_bytes == 0) {
        return invalid_argument("Scene serialization limits must be non-zero");
    }
    return foundation::Result::success();
}

[[nodiscard]] foundation::Result write_guid(serialization::BinaryWriter& writer, SceneEntityId id) {
    if (const foundation::Result result = writer.write_u64(id.high()); !result) {
        return result;
    }
    return writer.write_u64(id.low());
}

[[nodiscard]] foundation::Result write_asset_guid(serialization::BinaryWriter& writer, SceneAssetId id) {
    if (const foundation::Result result = writer.write_u64(id.high()); !result) {
        return result;
    }
    return writer.write_u64(id.low());
}

[[nodiscard]] foundation::ResultValue<SceneEntityId> read_guid(serialization::BinaryReader& reader) {
    const auto high = reader.read_u64();
    const auto low = reader.read_u64();
    if (!high || !low) {
        return {invalid_argument("Scene archive identity is truncated"), {}};
    }
    return {foundation::Result::success(), SceneEntityId{high.value, low.value}};
}

[[nodiscard]] foundation::ResultValue<SceneAssetId> read_asset_guid(serialization::BinaryReader& reader) {
    const auto high = reader.read_u64();
    const auto low = reader.read_u64();
    if (!high || !low) {
        return {invalid_argument("Scene archive asset identity is truncated"), {}};
    }
    return {foundation::Result::success(), SceneAssetId{high.value, low.value}};
}

[[nodiscard]] foundation::ResultValue<SceneEntityIndex> build_entity_index(const SceneAsset& asset) {
    SceneEntityIndex index;
    try {
        index.reserve(asset.entities().size());
        for (std::size_t entity_index = 0; entity_index < asset.entities().size(); ++entity_index) {
            const SceneEntityRecord& entity = asset.entities()[entity_index];
            if (!index.emplace(entity.id, entity_index).second) {
                return {invalid_state("Scene entity identity is duplicated"), {}};
            }
        }
    } catch (const std::bad_alloc&) {
        return {out_of_memory("Scene entity index allocation failed"), {}};
    }
    return {foundation::Result::success(), std::move(index)};
}

struct HierarchyLists final {
    SceneEntityIndex index{};
    std::vector<std::vector<SceneEntityId>> children{};
    std::vector<SceneEntityId> roots{};
};

[[nodiscard]] foundation::ResultValue<HierarchyLists> build_hierarchy_lists(
    std::span<const SceneEntityRecord> records) {
    HierarchyLists result;
    try {
        result.index.reserve(records.size());
        result.children.resize(records.size());
        for (std::size_t index = 0; index < records.size(); ++index) {
            if (!result.index.emplace(records[index].id, index).second) {
                return {invalid_state("Scene entity identity is duplicated"), {}};
            }
        }
        result.roots.reserve(records.size());
        for (const SceneEntityRecord& entity : records) {
            if (!entity.parent.is_valid()) {
                result.roots.push_back(entity.id);
                continue;
            }
            const auto parent = result.index.find(entity.parent);
            if (parent == result.index.end()) {
                return {not_found("Scene entity parent is not present in the asset"), {}};
            }
            result.children[parent->second].push_back(entity.id);
        }
    } catch (const std::bad_alloc&) {
        return {out_of_memory("Scene hierarchy index allocation failed"), {}};
    }
    return {foundation::Result::success(), std::move(result)};
}

[[nodiscard]] foundation::ResultValue<std::vector<SceneEntityRecord>> flatten_hierarchy(
    std::span<const SceneEntityRecord> records, const HierarchyLists& lists) {
    std::vector<SceneEntityRecord> result;
    try {
        result.reserve(records.size());
        std::vector<uint8_t> visited(records.size(), 0);
        std::function<foundation::Result(SceneEntityId)> append = [&](SceneEntityId entity) {
            const auto iterator = lists.index.find(entity);
            if (iterator == lists.index.end()) {
                return not_found("Scene hierarchy references an unknown entity");
            }
            if (visited[iterator->second] != 0) {
                return invalid_state("Scene entity hierarchy contains a cycle");
            }
            visited[iterator->second] = 1;
            result.push_back(records[iterator->second]);
            for (const SceneEntityId child : lists.children[iterator->second]) {
                if (const foundation::Result child_result = append(child); !child_result) {
                    return child_result;
                }
            }
            return foundation::Result::success();
        };
        for (const SceneEntityId root : lists.roots) {
            if (const foundation::Result root_result = append(root); !root_result) {
                return {root_result, {}};
            }
        }
        if (result.size() != records.size()) {
            return {invalid_state("Scene entity hierarchy contains an unreachable entity"), {}};
        }
    } catch (const std::bad_alloc&) {
        return {out_of_memory("Scene hierarchy ordering allocation failed"), {}};
    }
    return {foundation::Result::success(), std::move(result)};
}

[[nodiscard]] foundation::Result write_entity(serialization::BinaryWriter& writer, const SceneEntityRecord& entity,
                                              const SceneSerializationOptions& options) {
    if (const foundation::Result result = write_guid(writer, entity.id); !result) {
        return result;
    }
    if (const foundation::Result result = write_guid(writer, entity.parent); !result) {
        return result;
    }
    if (const foundation::Result result = writer.write_u32(static_cast<uint32_t>(entity.flags)); !result) {
        return result;
    }
    if (const foundation::Result result = writer.write_string(entity.name, options.max_name_bytes); !result) {
        return result;
    }
    return writer.write_string(entity.tag, options.max_name_bytes);
}

[[nodiscard]] foundation::Result write_component(serialization::BinaryWriter& writer,
                                                 const SceneComponentRecord& component,
                                                 const SceneSerializationOptions& options) {
    if (const foundation::Result result = write_guid(writer, component.entity); !result) {
        return result;
    }
    if (const foundation::Result result = writer.write_u64(component.component_type.value()); !result) {
        return result;
    }
    if (const foundation::Result result = writer.write_u64(component.schema_type.value()); !result) {
        return result;
    }
    if (const foundation::Result result = writer.write_u32(component.schema_version); !result) {
        return result;
    }
    if (component.payload.size() > options.max_component_payload_bytes ||
        component.payload.size() > std::numeric_limits<uint32_t>::max()) {
        return invalid_state("Scene component payload exceeds the configured size limit");
    }
    if (const foundation::Result result = writer.write_u32(static_cast<uint32_t>(component.payload.size())); !result) {
        return result;
    }
    return writer.write_bytes(component.payload);
}

[[nodiscard]] foundation::Result instantiate_components(
    ecs::World& world, const reflection::TypeRegistry& registry, const SceneAsset& asset,
    const std::unordered_map<SceneEntityId, ecs::EntityId, Guid128Hash<SceneEntityIdTag>>& entity_remap) {
    for (const SceneComponentRecord& component : asset.components()) {
        const auto descriptor = world.find_component_descriptor(component.component_type);
        if (!descriptor) {
            return descriptor.result;
        }
        if (descriptor.value.schema_type != component.schema_type ||
            descriptor.value.schema_version < component.schema_version) {
            return unsupported("World component registration does not satisfy the Scene Asset schema");
        }
        const auto reflected_type = registry.find_type(component.schema_type);
        if (!reflected_type) {
            return reflected_type.result;
        }
        if (reflected_type.value->kind != reflection::TypeKind::kComponent ||
            reflected_type.value->version < component.schema_version) {
            return unsupported("Scene Asset component schema is not compatible with the registered reflection type");
        }
        const auto remapped = entity_remap.find(component.entity);
        if (remapped == entity_remap.end()) {
            return invalid_state("Scene Asset entity remap is incomplete");
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
        serialization::BinaryReader payload(component.payload);
        if (const foundation::Result result =
                serialization::deserialize_object(registry, component.schema_type, payload, value.value);
            !result) {
            return result;
        }
        if (payload.remaining() != 0) {
            return invalid_argument("Scene Asset component payload contains trailing bytes");
        }
    }
    return foundation::Result::success();
}

void rollback_entities(ecs::World& world, std::span<const SceneEntityBinding> bindings) noexcept {
    for (const SceneEntityBinding& binding : bindings) {
        static_cast<void>(world.destroy_entity(binding.runtime_entity));
    }
}
}  // namespace

foundation::ResultValue<ecs::EntityId> SceneInstance::runtime_entity(SceneEntityId scene_entity) const {
    if (!scene_entity.is_valid()) {
        return {invalid_argument("Scene entity identity is invalid"), {}};
    }
    const auto iterator = std::lower_bound(
        bindings_.begin(), bindings_.end(), scene_entity,
        [](const SceneEntityBinding& binding, SceneEntityId value) { return binding.scene_entity < value; });
    if (iterator == bindings_.end() || iterator->scene_entity != scene_entity) {
        return {not_found("Scene entity is not present in the instance"), {}};
    }
    return {foundation::Result::success(), iterator->runtime_entity};
}

SceneAsset::SceneAsset(SceneAssetId id, std::string name, uint32_t version)
    : id_(id), name_(std::move(name)), version_(version) {}

foundation::Result SceneAsset::set_identity(SceneAssetId id, std::string name, uint32_t version) {
    if (!id.is_valid() || version == 0) {
        return invalid_argument("Scene Asset identity or version is invalid");
    }
    try {
        std::string new_name = std::move(name);
        id_ = id;
        name_ = std::move(new_name);
        version_ = version;
    } catch (const std::bad_alloc&) {
        return out_of_memory("Scene Asset identity allocation failed");
    }
    return foundation::Result::success();
}

foundation::Result SceneAsset::add_entity(SceneEntityRecord entity) {
    if (!entity.id.is_valid()) {
        return invalid_argument("Scene entity identity is invalid");
    }
    const auto duplicate = std::find_if(entities_.begin(), entities_.end(),
                                        [id = entity.id](const SceneEntityRecord& value) { return value.id == id; });
    if (duplicate != entities_.end()) {
        return invalid_state("Scene entity identity is already present");
    }
    try {
        entities_.push_back(std::move(entity));
    } catch (const std::bad_alloc&) {
        return out_of_memory("Scene entity allocation failed");
    }
    return foundation::Result::success();
}

foundation::Result SceneAsset::set_entity_name(SceneEntityId entity, std::string name) {
    const auto found =
        std::find_if(entities_.begin(), entities_.end(), [entity](const auto& value) { return value.id == entity; });
    if (found == entities_.end()) {
        return not_found("Scene entity name target is not present in the asset");
    }
    if (name.empty()) {
        return invalid_argument("Scene entity name cannot be empty");
    }
    try {
        found->name = std::move(name);
    } catch (const std::bad_alloc&) {
        return out_of_memory("Scene entity name allocation failed");
    }
    return foundation::Result::success();
}

foundation::Result SceneAsset::set_entity_tag(SceneEntityId entity, std::string tag) {
    const auto found =
        std::find_if(entities_.begin(), entities_.end(), [entity](const auto& value) { return value.id == entity; });
    if (found == entities_.end()) {
        return not_found("Scene entity tag target is not present in the asset");
    }
    try {
        found->tag = std::move(tag);
    } catch (const std::bad_alloc&) {
        return out_of_memory("Scene entity tag allocation failed");
    }
    return foundation::Result::success();
}

foundation::ResultValue<std::vector<SceneEntityId>> SceneAsset::children(SceneEntityId parent) const {
    if (parent.is_valid()) {
        const auto parent_exists = std::find_if(entities_.begin(), entities_.end(),
                                                [parent](const auto& entity) { return entity.id == parent; });
        if (parent_exists == entities_.end()) {
            return {not_found("Scene hierarchy parent is not present in the asset"), {}};
        }
    }
    try {
        std::vector<SceneEntityId> result;
        for (const SceneEntityRecord& entity : entities_) {
            if (entity.parent == parent) {
                result.push_back(entity.id);
            }
        }
        return {foundation::Result::success(), std::move(result)};
    } catch (const std::bad_alloc&) {
        return {out_of_memory("Scene child list allocation failed"), {}};
    }
}

foundation::Result SceneAsset::reparent_entities(std::span<const SceneEntityId> entities, SceneEntityId parent,
                                                 std::size_t sibling_index) {
    if (entities.empty()) {
        return invalid_argument("Scene reparent operation has no entities");
    }
    if (parent.is_valid() && std::find_if(entities_.begin(), entities_.end(), [parent](const auto& entity) {
                                 return entity.id == parent;
                             }) == entities_.end()) {
        return not_found("Scene reparent target parent is not present in the asset");
    }
    const auto lists_result = build_hierarchy_lists(entities_);
    if (!lists_result) {
        return lists_result.result;
    }
    try {
        std::vector<SceneEntityId> sources;
        sources.reserve(entities.size());
        std::unordered_set<SceneEntityId, Guid128Hash<SceneEntityIdTag>> source_set;
        source_set.reserve(entities.size());
        for (const SceneEntityId entity : entities) {
            if (!entity.is_valid() || lists_result.value.index.find(entity) == lists_result.value.index.end()) {
                return not_found("Scene reparent source is not present in the asset");
            }
            if (!source_set.emplace(entity).second) {
                return invalid_argument("Scene reparent sources contain a duplicate entity");
            }
            sources.push_back(entity);
        }
        for (const SceneEntityId source : sources) {
            if (source == parent) {
                return invalid_state("Scene entity cannot be its own parent");
            }
            SceneEntityId current = parent;
            while (current.is_valid()) {
                if (current == source) {
                    return invalid_state("Scene entity cannot be parented below its descendant");
                }
                const auto current_index = lists_result.value.index.find(current);
                if (current_index == lists_result.value.index.end()) {
                    return not_found("Scene hierarchy references an unknown parent");
                }
                current = entities_[current_index->second].parent;
            }
        }
        for (const SceneEntityId source : sources) {
            SceneEntityId current = source;
            while (current.is_valid()) {
                const auto current_index = lists_result.value.index.find(current);
                if (current_index == lists_result.value.index.end()) {
                    break;
                }
                current = entities_[current_index->second].parent;
                if (source_set.contains(current)) {
                    return invalid_argument("Scene reparent sources cannot contain nested entities");
                }
            }
        }

        HierarchyLists lists = lists_result.value;
        std::vector<SceneEntityRecord> records(entities_.begin(), entities_.end());
        for (const SceneEntityId source : sources) {
            const auto source_index = lists.index.find(source);
            records[source_index->second].parent = parent;
        }
        for (std::vector<SceneEntityId>& siblings : lists.children) {
            siblings.erase(std::remove_if(siblings.begin(), siblings.end(),
                                          [&source_set](SceneEntityId value) { return source_set.contains(value); }),
                           siblings.end());
        }
        lists.roots.erase(std::remove_if(lists.roots.begin(), lists.roots.end(),
                                         [&source_set](SceneEntityId value) { return source_set.contains(value); }),
                          lists.roots.end());
        auto& destination = parent.is_valid() ? lists.children[lists.index.find(parent)->second] : lists.roots;
        const std::size_t insertion = sibling_index == static_cast<std::size_t>(-1)
                                          ? destination.size()
                                          : std::min(sibling_index, destination.size());
        destination.insert(destination.begin() + static_cast<std::ptrdiff_t>(insertion), sources.begin(),
                           sources.end());
        const auto flattened = flatten_hierarchy(records, lists);
        if (!flattened) {
            return flattened.result;
        }
        entities_ = flattened.value;
    } catch (const std::bad_alloc&) {
        return out_of_memory("Scene reparent operation allocation failed");
    }
    return foundation::Result::success();
}

foundation::Result SceneAsset::remove_entities(std::span<const SceneEntityId> entities,
                                               SceneEntityRemovalPolicy policy) {
    if (entities.empty()) {
        return invalid_argument("Scene remove operation has no entities");
    }
    if (policy != SceneEntityRemovalPolicy::kCascade && policy != SceneEntityRemovalPolicy::kPromoteChildren) {
        return invalid_argument("Scene remove operation has an unknown removal policy");
    }
    const auto lists_result = build_hierarchy_lists(entities_);
    if (!lists_result) {
        return lists_result.result;
    }
    try {
        std::unordered_set<SceneEntityId, Guid128Hash<SceneEntityIdTag>> sources;
        sources.reserve(entities.size());
        for (const SceneEntityId entity : entities) {
            if (!entity.is_valid() || lists_result.value.index.find(entity) == lists_result.value.index.end()) {
                return not_found("Scene remove source is not present in the asset");
            }
            if (!sources.emplace(entity).second) {
                return invalid_argument("Scene remove sources contain a duplicate entity");
            }
        }
        for (const SceneEntityId source : sources) {
            SceneEntityId current = entities_[lists_result.value.index.find(source)->second].parent;
            while (current.is_valid()) {
                if (sources.contains(current)) {
                    return invalid_argument("Scene remove sources cannot contain nested entities");
                }
                current = entities_[lists_result.value.index.find(current)->second].parent;
            }
        }
        std::unordered_set<SceneEntityId, Guid128Hash<SceneEntityIdTag>> removed = sources;
        if (policy == SceneEntityRemovalPolicy::kCascade) {
            std::function<void(SceneEntityId)> mark_descendants = [&](SceneEntityId entity) {
                const auto index = lists_result.value.index.find(entity);
                if (index == lists_result.value.index.end()) {
                    return;
                }
                for (const SceneEntityId child : lists_result.value.children[index->second]) {
                    if (removed.emplace(child).second) {
                        mark_descendants(child);
                    }
                }
            };
            for (const SceneEntityId source : sources) {
                mark_descendants(source);
            }
        }
        std::vector<SceneEntityRecord> remaining;
        remaining.reserve(entities_.size() - removed.size());
        for (const SceneEntityRecord& entity : entities_) {
            if (!removed.contains(entity.id)) {
                remaining.push_back(entity);
            }
        }
        if (policy == SceneEntityRemovalPolicy::kPromoteChildren) {
            for (SceneEntityRecord& entity : remaining) {
                const auto removed_parent = sources.find(entity.parent);
                if (removed_parent == sources.end()) {
                    continue;
                }
                const auto parent_index = lists_result.value.index.find(*removed_parent);
                if (parent_index == lists_result.value.index.end()) {
                    return invalid_state("Scene removal hierarchy index is incomplete");
                }
                entity.parent = entities_[parent_index->second].parent;
            }
        }
        std::vector<SceneComponentRecord> remaining_components;
        remaining_components.reserve(components_.size());
        for (const SceneComponentRecord& component : components_) {
            if (!removed.contains(component.entity)) {
                remaining_components.push_back(component);
            }
        }
        const auto remaining_lists = build_hierarchy_lists(remaining);
        if (!remaining_lists) {
            return remaining_lists.result;
        }
        const auto flattened = flatten_hierarchy(remaining, remaining_lists.value);
        if (!flattened) {
            return flattened.result;
        }
        entities_ = flattened.value;
        components_ = std::move(remaining_components);
    } catch (const std::bad_alloc&) {
        return out_of_memory("Scene remove operation allocation failed");
    }
    return foundation::Result::success();
}

foundation::Result SceneAsset::add_component(SceneComponentRecord component) {
    if (!component.entity.is_valid() || !component.component_type.is_valid() || !component.schema_type.is_valid() ||
        component.schema_version == 0) {
        return invalid_argument("Scene component record is invalid");
    }
    const auto duplicate =
        std::find_if(components_.begin(), components_.end(), [&component](const SceneComponentRecord& value) {
            return value.entity == component.entity && value.component_type == component.component_type;
        });
    if (duplicate != components_.end()) {
        return invalid_state("Scene component record is already present");
    }
    try {
        components_.push_back(std::move(component));
    } catch (const std::bad_alloc&) {
        return out_of_memory("Scene component allocation failed");
    }
    return foundation::Result::success();
}

foundation::Result SceneAsset::replace_component_payload(SceneEntityId entity, ecs::ComponentTypeId component_type,
                                                         std::vector<uint8_t> payload) {
    if (!entity.is_valid() || !component_type.is_valid()) {
        return invalid_argument("Scene component payload replacement identity is invalid");
    }
    const auto found =
        std::find_if(components_.begin(), components_.end(), [entity, component_type](const auto& value) {
            return value.entity == entity && value.component_type == component_type;
        });
    if (found == components_.end()) {
        return not_found("Scene component is not present on the entity");
    }
    if (payload.empty()) {
        return invalid_argument("Scene component payload replacement is empty");
    }
    found->payload = std::move(payload);
    return foundation::Result::success();
}

foundation::Result SceneAsset::add_reflected_component(SceneEntityId entity, ecs::ComponentTypeId component_type,
                                                       const reflection::TypeRegistry& registry,
                                                       reflection::TypeId schema_type, const void* value) {
    if (!entity.is_valid() || !component_type.is_valid() || !schema_type.is_valid() || value == nullptr) {
        return invalid_argument("Scene reflected component input is invalid");
    }
    const auto descriptor = registry.find_type(schema_type);
    if (!descriptor) {
        return descriptor.result;
    }
    if (descriptor.value->kind != reflection::TypeKind::kComponent || descriptor.value->version == 0) {
        return invalid_argument("Scene reflected component schema must describe a component type");
    }
    serialization::BinaryWriter payload;
    if (const foundation::Result result = serialization::serialize_object(registry, schema_type, value, payload);
        !result) {
        return result;
    }
    try {
        return add_component(SceneComponentRecord{entity, component_type, schema_type, descriptor.value->version,
                                                  std::vector<uint8_t>(payload.data().begin(), payload.data().end())});
    } catch (const std::bad_alloc&) {
        return out_of_memory("Scene reflected component payload allocation failed");
    }
}

foundation::Result SceneAsset::remove_component(SceneEntityId entity, ecs::ComponentTypeId component_type) {
    if (!entity.is_valid() || !component_type.is_valid()) {
        return invalid_argument("Scene component removal input is invalid");
    }
    const auto found =
        std::find_if(components_.begin(), components_.end(), [entity, component_type](const auto& value) {
            return value.entity == entity && value.component_type == component_type;
        });
    if (found == components_.end()) {
        return not_found("Scene component is not present on the entity");
    }
    components_.erase(found);
    return foundation::Result::success();
}

void SceneAsset::clear() noexcept {
    entities_.clear();
    components_.clear();
}

foundation::Result SceneAsset::validate() const {
    if (!id_.is_valid() || version_ == 0) {
        return invalid_argument("Scene Asset identity or version is invalid");
    }

    const auto entity_index = build_entity_index(*this);
    if (!entity_index) {
        return entity_index.result;
    }
    try {
        std::vector<uint8_t> hierarchy_states(entities_.size(), 0);
        for (const SceneEntityRecord& entity : entities_) {
            if ((static_cast<uint32_t>(entity.flags) & ~kKnownSceneEntityFlags) != 0) {
                return invalid_argument("Scene entity flags contain unsupported bits");
            }
            if (entity.parent == entity.id) {
                return invalid_state("Scene entity cannot be its own parent");
            }
            if (entity.parent.is_valid() && entity_index.value.find(entity.parent) == entity_index.value.end()) {
                return not_found("Scene entity parent is not present in the asset");
            }
        }

        std::vector<std::size_t> hierarchy_chain;
        hierarchy_chain.reserve(entities_.size());
        for (std::size_t start_index = 0; start_index < entities_.size(); ++start_index) {
            if (hierarchy_states[start_index] == 2) {
                continue;
            }
            std::size_t current_index = start_index;
            while (true) {
                if (hierarchy_states[current_index] == 1) {
                    return invalid_state("Scene entity hierarchy contains a cycle");
                }
                if (hierarchy_states[current_index] == 2) {
                    break;
                }
                hierarchy_states[current_index] = 1;
                hierarchy_chain.push_back(current_index);
                const SceneEntityId parent = entities_[current_index].parent;
                if (!parent.is_valid()) {
                    break;
                }
                const auto iterator = entity_index.value.find(parent);
                if (iterator == entity_index.value.end()) {
                    return not_found("Scene entity hierarchy references an unknown entity");
                }
                current_index = iterator->second;
            }
            for (const std::size_t entity_index_value : hierarchy_chain) {
                hierarchy_states[entity_index_value] = 2;
            }
            hierarchy_chain.clear();
        }
        std::unordered_map<SceneComponentKey, bool, SceneComponentKeyHash> component_keys;
        component_keys.reserve(components_.size());
        for (const SceneComponentRecord& component : components_) {
            if (!component.entity.is_valid() || entity_index.value.find(component.entity) == entity_index.value.end()) {
                return not_found("Scene component references an unknown entity");
            }
            if (!component.component_type.is_valid() || !component.schema_type.is_valid() ||
                component.schema_version == 0) {
                return invalid_argument("Scene component schema metadata is invalid");
            }
            if (!component_keys.emplace(SceneComponentKey{component.entity, component.component_type}, true).second) {
                return invalid_state("Scene component identity is duplicated");
            }
        }
    } catch (const std::bad_alloc&) {
        return out_of_memory("Scene Asset validation allocation failed");
    }
    return foundation::Result::success();
}

foundation::ResultValue<SceneInstance> SceneAsset::instantiate(ecs::World& world,
                                                               const reflection::TypeRegistry& registry,
                                                               const SceneInstantiationOptions& options) const {
    if (const foundation::Result result = validate(); !result) {
        return {result, {}};
    }
    if (options.clear_world) {
        if (const foundation::Result result = world.clear(); !result) {
            return {result, {}};
        }
    }

    SceneInstance instance;
    instance.asset_ = id_;
    std::unordered_map<SceneEntityId, ecs::EntityId, Guid128Hash<SceneEntityIdTag>> entity_remap;
    try {
        entity_remap.reserve(entities_.size());
        instance.bindings_.reserve(entities_.size());
        for (const SceneEntityRecord& entity : entities_) {
            const auto runtime_entity = world.create_entity();
            if (!runtime_entity) {
                if (options.clear_world) {
                    static_cast<void>(world.clear());
                } else {
                    rollback_entities(world, instance.bindings_);
                }
                return {runtime_entity.result, {}};
            }
            entity_remap.emplace(entity.id, runtime_entity.value);
            instance.bindings_.push_back(SceneEntityBinding{entity.id, runtime_entity.value});
        }
        std::sort(instance.bindings_.begin(), instance.bindings_.end(),
                  [](const SceneEntityBinding& left, const SceneEntityBinding& right) {
                      return left.scene_entity < right.scene_entity;
                  });
    } catch (const std::bad_alloc&) {
        if (options.clear_world) {
            static_cast<void>(world.clear());
        } else {
            rollback_entities(world, instance.bindings_);
        }
        return {out_of_memory("Scene instance allocation failed"), {}};
    }

    if (const foundation::Result result = instantiate_components(world, registry, *this, entity_remap); !result) {
        if (options.clear_world) {
            static_cast<void>(world.clear());
        } else {
            rollback_entities(world, instance.bindings_);
        }
        return {result, {}};
    }
    return {foundation::Result::success(), std::move(instance)};
}

foundation::Result SceneSerializer::serialize(const SceneAsset& asset, serialization::BinaryWriter& writer,
                                              const SceneSerializationOptions& options) {
    if (const foundation::Result result = validate_serialization_options(options); !result) {
        return result;
    }
    if (const foundation::Result result = asset.validate(); !result) {
        return result;
    }
    if (asset.name().size() > options.max_name_bytes || asset.entities().size() > options.max_entities ||
        asset.components().size() > options.max_components ||
        asset.entities().size() > std::numeric_limits<uint32_t>::max() ||
        asset.components().size() > std::numeric_limits<uint32_t>::max()) {
        return invalid_state("Scene Asset exceeds the configured archive limits");
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
        if (const foundation::Result result = write_asset_guid(encoded, asset.id()); !result) {
            return result;
        }
        if (const foundation::Result result = encoded.write_u32(asset.version()); !result) {
            return result;
        }
        if (const foundation::Result result = encoded.write_string(asset.name(), options.max_name_bytes); !result) {
            return result;
        }
        if (const foundation::Result result = encoded.write_u32(static_cast<uint32_t>(asset.entities().size()));
            !result) {
            return result;
        }
        if (const foundation::Result result = encoded.write_u32(static_cast<uint32_t>(asset.components().size()));
            !result) {
            return result;
        }

        std::vector<const SceneComponentRecord*> components;
        components.reserve(asset.components().size());
        for (const SceneComponentRecord& component : asset.components()) {
            components.push_back(&component);
        }
        const auto hierarchy = build_hierarchy_lists(asset.entities());
        if (!hierarchy) {
            return hierarchy.result;
        }
        const auto entities = flatten_hierarchy(asset.entities(), hierarchy.value);
        if (!entities) {
            return entities.result;
        }
        std::sort(components.begin(), components.end(),
                  [](const SceneComponentRecord* left, const SceneComponentRecord* right) {
                      if (left->entity != right->entity) {
                          return left->entity < right->entity;
                      }
                      return left->component_type < right->component_type;
                  });
        for (const SceneEntityRecord& entity : entities.value) {
            if (const foundation::Result result = write_entity(encoded, entity, options); !result) {
                return result;
            }
        }
        for (const SceneComponentRecord* component : components) {
            if (const foundation::Result result = write_component(encoded, *component, options); !result) {
                return result;
            }
        }
    } catch (const std::bad_alloc&) {
        return out_of_memory("Scene Asset archive allocation failed");
    }
    return writer.write_bytes(encoded.data());
}

foundation::ResultValue<SceneAsset> SceneSerializer::deserialize(serialization::BinaryReader& reader,
                                                                 const SceneSerializationOptions& options) {
    if (const foundation::Result result = validate_serialization_options(options); !result) {
        return {result, {}};
    }
    try {
        const auto magic = reader.read_u64();
        const auto format_version = reader.read_u32();
        const auto reserved = reader.read_u32();
        if (!magic || !format_version || !reserved) {
            return {invalid_argument("Scene archive header is truncated"), {}};
        }
        if (magic.value != kMagic || (format_version.value != 1u && format_version.value != kFormatVersion) ||
            reserved.value != 0) {
            return {unsupported("Scene archive format is not supported"), {}};
        }
        const auto asset_id = read_asset_guid(reader);
        const auto asset_version = reader.read_u32();
        if (!asset_id || !asset_version) {
            return {invalid_argument("Scene archive asset metadata is truncated"), {}};
        }
        if (!asset_id.value.is_valid() || asset_version.value == 0) {
            return {invalid_state("Scene archive asset metadata is invalid"), {}};
        }
        const auto name = reader.read_string(options.max_name_bytes);
        const auto entity_count = reader.read_u32();
        const auto component_count = reader.read_u32();
        if (!name || !entity_count || !component_count) {
            return {invalid_argument("Scene archive counts are truncated"), {}};
        }
        if (entity_count.value > options.max_entities || component_count.value > options.max_components) {
            return {invalid_state("Scene archive exceeds the configured count limits"), {}};
        }

        SceneAsset asset(asset_id.value, std::move(name.value), asset_version.value);
        for (uint32_t index = 0; index < entity_count.value; ++index) {
            const auto entity_id = read_guid(reader);
            const auto parent = read_guid(reader);
            const auto flags = reader.read_u32();
            if (!entity_id || !parent || !flags) {
                return {invalid_argument("Scene archive entity record is truncated"), {}};
            }
            if (!entity_id.value.is_valid() || (flags.value & ~kKnownSceneEntityFlags) != 0) {
                return {invalid_state("Scene archive entity record is invalid"), {}};
            }
            const auto entity_name = reader.read_string(options.max_name_bytes);
            if (!entity_name) {
                return {entity_name.result, {}};
            }
            std::string entity_tag;
            if (format_version.value >= 2u) {
                const auto tag = reader.read_string(options.max_name_bytes);
                if (!tag) {
                    return {tag.result, {}};
                }
                entity_tag = std::move(tag.value);
            }
            if (const foundation::Result result = asset.add_entity(
                    SceneEntityRecord{entity_id.value, parent.value, std::move(entity_name.value),
                                      static_cast<SceneEntityFlags>(flags.value), std::move(entity_tag)});
                !result) {
                return {result, {}};
            }
        }
        for (uint32_t index = 0; index < component_count.value; ++index) {
            const auto entity = read_guid(reader);
            const auto component_type = reader.read_u64();
            const auto schema_type = reader.read_u64();
            const auto schema_version = reader.read_u32();
            const auto payload_size = reader.read_u32();
            if (!entity || !component_type || !schema_type || !schema_version || !payload_size) {
                return {invalid_argument("Scene archive component record is truncated"), {}};
            }
            if (!entity.value.is_valid() || component_type.value == 0 || schema_type.value == 0 ||
                schema_version.value == 0 || payload_size.value > options.max_component_payload_bytes) {
                return {invalid_state("Scene archive component record is invalid"), {}};
            }
            const auto payload = reader.read_bytes(payload_size.value);
            if (!payload) {
                return {payload.result, {}};
            }
            std::vector<uint8_t> payload_copy(payload.value.begin(), payload.value.end());
            if (const foundation::Result result = asset.add_component(SceneComponentRecord{
                    entity.value, ecs::ComponentTypeId(component_type.value), reflection::TypeId(schema_type.value),
                    schema_version.value, std::move(payload_copy)});
                !result) {
                return {result, {}};
            }
        }
        if (const foundation::Result result = asset.validate(); !result) {
            return {result, {}};
        }
        return {foundation::Result::success(), std::move(asset)};
    } catch (const std::bad_alloc&) {
        return {out_of_memory("Scene archive allocation failed"), {}};
    }
}
}  // namespace aeg::scene
