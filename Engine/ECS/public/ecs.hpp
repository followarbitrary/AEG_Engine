/**
 * @file ecs.hpp
 * @brief Entity-component storage, command buffering, and immutable World snapshots.
 */
#pragma once

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>

#include "result.hpp"

namespace aeg::ecs {
inline constexpr uint32_t kInvalidEntityIndex = std::numeric_limits<uint32_t>::max();
inline constexpr uint32_t kInvalidWorldIndex = std::numeric_limits<uint32_t>::max();
inline constexpr uint32_t kInvalidDenseIndex = std::numeric_limits<uint32_t>::max();

/** @brief Generation-checked identity of one World instance. */
struct WorldHandle final {
    uint32_t index{kInvalidWorldIndex};
    uint32_t generation{0};

    [[nodiscard]] constexpr bool is_valid() const noexcept { return index != kInvalidWorldIndex && generation != 0; }

    constexpr explicit operator bool() const noexcept { return is_valid(); }
    friend constexpr bool operator==(WorldHandle, WorldHandle) = default;
};

/** @brief Generation-checked identity of one entity in a World. */
struct EntityId final {
    uint32_t index{kInvalidEntityIndex};
    uint32_t generation{0};

    [[nodiscard]] constexpr bool is_valid() const noexcept { return index != kInvalidEntityIndex && generation != 0; }

    constexpr explicit operator bool() const noexcept { return is_valid(); }
    friend constexpr bool operator==(EntityId, EntityId) = default;
};

/** @brief Stable identity of a registered component type. */
class ComponentTypeId final {
public:
    constexpr ComponentTypeId() noexcept = default;
    explicit constexpr ComponentTypeId(uint64_t value) noexcept : value_(value) {}
    explicit constexpr ComponentTypeId(std::string_view name) noexcept : value_(hash_name(name)) {}

    [[nodiscard]] constexpr bool is_valid() const noexcept { return value_ != 0; }
    [[nodiscard]] constexpr uint64_t value() const noexcept { return value_; }

    friend constexpr bool operator==(ComponentTypeId, ComponentTypeId) = default;
    friend constexpr auto operator<=>(ComponentTypeId, ComponentTypeId) = default;

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

/** @brief Hash function for ComponentTypeId values. */
struct ComponentTypeIdHash final {
    [[nodiscard]] std::size_t operator()(ComponentTypeId id) const noexcept {
        return static_cast<std::size_t>(id.value());
    }
};

/** @brief Static contract describing one component type. */
struct ComponentDescriptor final {
    ComponentTypeId id{};
    std::string_view name{};
    std::size_t size{0};
    std::size_t alignment{0};
    bool snapshotable{true};
};

/** @brief Construction options for one World. */
struct WorldDesc final {
    std::string_view name{};
    bool enforce_owner_thread{true};
};

namespace detail {
/** @brief Type-erased immutable component storage used by WorldSnapshot. */
class ComponentSnapshotBase {
public:
    virtual ~ComponentSnapshotBase() = default;

    [[nodiscard]] virtual const void* get(EntityId entity) const noexcept = 0;
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    [[nodiscard]] virtual EntityId entity_at(std::size_t index) const noexcept = 0;
    [[nodiscard]] virtual const void* value_at(std::size_t index) const noexcept = 0;
};

/** @brief Typed immutable dense component storage. */
template <typename Type>
class ComponentSnapshot final : public ComponentSnapshotBase {
public:
    ComponentSnapshot(ComponentTypeId type, const std::vector<EntityId>& entities, const std::vector<Type>& values)
        : type_(type), entities_(entities), values_(values), sparse_(max_entity_index(entities), kInvalidDenseIndex) {
        for (uint32_t index = 0; index < entities_.size(); ++index) {
            sparse_[entities_[index].index] = index;
        }
    }

    [[nodiscard]] const void* get(EntityId entity) const noexcept override {
        if (!entity.is_valid() || entity.index >= sparse_.size()) {
            return nullptr;
        }
        const uint32_t dense_index = sparse_[entity.index];
        if (dense_index == kInvalidDenseIndex || dense_index >= entities_.size() || entities_[dense_index] != entity) {
            return nullptr;
        }
        return &values_[dense_index];
    }

    [[nodiscard]] std::size_t size() const noexcept override { return values_.size(); }

    [[nodiscard]] EntityId entity_at(std::size_t index) const noexcept override {
        return index < entities_.size() ? entities_[index] : EntityId{};
    }

    [[nodiscard]] const void* value_at(std::size_t index) const noexcept override {
        return index < values_.size() ? &values_[index] : nullptr;
    }

    [[nodiscard]] ComponentTypeId type() const noexcept { return type_; }

private:
    [[nodiscard]] static std::size_t max_entity_index(const std::vector<EntityId>& entities) noexcept {
        std::size_t maximum = 0;
        for (const EntityId entity : entities) {
            maximum = std::max(maximum, static_cast<std::size_t>(entity.index) + 1);
        }
        return maximum;
    }

    ComponentTypeId type_{};
    std::vector<EntityId> entities_{};
    std::vector<Type> values_{};
    std::vector<uint32_t> sparse_{};
};

/** @brief Type-erased mutable component pool interface. */
class ComponentPoolBase {
public:
    virtual ~ComponentPoolBase() = default;

    [[nodiscard]] virtual foundation::ResultValue<void*> emplace_move(EntityId entity, void* value) = 0;
    [[nodiscard]] virtual bool has(EntityId entity) const noexcept = 0;
    [[nodiscard]] virtual void* get(EntityId entity) noexcept = 0;
    [[nodiscard]] virtual const void* get(EntityId entity) const noexcept = 0;
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    [[nodiscard]] virtual EntityId entity_at(std::size_t index) const noexcept = 0;
    [[nodiscard]] virtual void* value_at(std::size_t index) noexcept = 0;
    [[nodiscard]] virtual const void* value_at(std::size_t index) const noexcept = 0;
    virtual bool remove(EntityId entity) noexcept = 0;
    [[nodiscard]] virtual std::shared_ptr<const ComponentSnapshotBase> snapshot() const = 0;
};

/** @brief Dense/sparse mutable storage for one concrete component type. */
template <typename Type>
class ComponentPool final : public ComponentPoolBase {
public:
    explicit ComponentPool(ComponentTypeId type) : type_(type) {}

    [[nodiscard]] foundation::ResultValue<void*> emplace_move(EntityId entity, void* value) override {
        if (!entity.is_valid() || value == nullptr) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                "component entity or value is invalid"),
                    nullptr};
        }
        if (has(entity)) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                                "entity already owns this component"),
                    nullptr};
        }

        try {
            if (entity.index >= sparse_.size()) {
                sparse_.resize(static_cast<std::size_t>(entity.index) + 1, kInvalidDenseIndex);
            }
            const uint32_t dense_index = static_cast<uint32_t>(values_.size());
            entities_.push_back(entity);
            try {
                values_.push_back(std::move(*static_cast<Type*>(value)));
            } catch (...) {
                entities_.pop_back();
                throw;
            }
            sparse_[entity.index] = dense_index;
            return {foundation::Result::success(), &values_.back()};
        } catch (const std::bad_alloc&) {
            return {foundation::Result::failure(foundation::ResultCode::kOutOfMemory, "component allocation failed"),
                    nullptr};
        } catch (const std::exception& exception) {
            return {foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what()), nullptr};
        } catch (...) {
            return {foundation::Result::failure(foundation::ResultCode::kUnknown,
                                                "component construction threw an unknown exception"),
                    nullptr};
        }
    }

    [[nodiscard]] bool has(EntityId entity) const noexcept override {
        if (!entity.is_valid() || entity.index >= sparse_.size()) {
            return false;
        }
        const uint32_t dense_index = sparse_[entity.index];
        return dense_index != kInvalidDenseIndex && dense_index < entities_.size() && entities_[dense_index] == entity;
    }

    [[nodiscard]] void* get(EntityId entity) noexcept override {
        return const_cast<void*>(static_cast<const ComponentPool*>(this)->get(entity));
    }

    [[nodiscard]] const void* get(EntityId entity) const noexcept override {
        if (!has(entity)) {
            return nullptr;
        }
        return &values_[sparse_[entity.index]];
    }

    [[nodiscard]] std::size_t size() const noexcept override { return values_.size(); }

    [[nodiscard]] EntityId entity_at(std::size_t index) const noexcept override {
        return index < entities_.size() ? entities_[index] : EntityId{};
    }

    [[nodiscard]] void* value_at(std::size_t index) noexcept override {
        return index < values_.size() ? &values_[index] : nullptr;
    }

    [[nodiscard]] const void* value_at(std::size_t index) const noexcept override {
        return index < values_.size() ? &values_[index] : nullptr;
    }

    bool remove(EntityId entity) noexcept override {
        if (!has(entity)) {
            return false;
        }
        const uint32_t dense_index = sparse_[entity.index];
        const uint32_t last_index = static_cast<uint32_t>(values_.size() - 1);
        if (dense_index != last_index) {
            entities_[dense_index] = entities_[last_index];
            values_[dense_index] = std::move(values_[last_index]);
            sparse_[entities_[dense_index].index] = dense_index;
        }
        entities_.pop_back();
        values_.pop_back();
        sparse_[entity.index] = kInvalidDenseIndex;
        return true;
    }

    [[nodiscard]] std::shared_ptr<const ComponentSnapshotBase> snapshot() const override {
        return std::make_shared<const ComponentSnapshot<Type>>(type_, entities_, values_);
    }

private:
    ComponentTypeId type_{};
    std::vector<EntityId> entities_{};
    std::vector<Type> values_{};
    std::vector<uint32_t> sparse_{};
};
}  // namespace detail

/** @brief Immutable, generation-checked view of one committed World state. */
class WorldSnapshot final {
public:
    WorldSnapshot() noexcept = default;

    [[nodiscard]] bool is_valid() const noexcept;
    [[nodiscard]] WorldHandle world() const noexcept;
    [[nodiscard]] uint64_t revision() const noexcept;
    [[nodiscard]] uint64_t frame_number() const noexcept;
    [[nodiscard]] uint64_t fixed_tick() const noexcept;
    [[nodiscard]] std::size_t entity_count() const noexcept;
    [[nodiscard]] std::size_t component_type_count() const noexcept;
    [[nodiscard]] EntityId entity_at(std::size_t index) const noexcept;
    [[nodiscard]] bool contains(EntityId entity) const noexcept;

    template <typename Type>
    [[nodiscard]] const Type* get(EntityId entity) const noexcept {
        return static_cast<const Type*>(get_component(entity, std::type_index(typeid(Type))));
    }

    /** @brief Iterates one immutable component pool in dense storage order. */
    template <typename Type, typename Function>
    void for_each(Function&& function) const {
        const detail::ComponentSnapshotBase* component = find_component(std::type_index(typeid(Type)));
        if (component == nullptr) {
            return;
        }
        for (std::size_t index = 0; index < component->size(); ++index) {
            const Type* value = static_cast<const Type*>(component->value_at(index));
            if (value != nullptr) {
                function(component->entity_at(index), *value);
            }
        }
    }

private:
    struct SnapshotData;

    explicit WorldSnapshot(std::shared_ptr<const SnapshotData> data) noexcept : data_(std::move(data)) {}

    [[nodiscard]] const detail::ComponentSnapshotBase* find_component(std::type_index type) const noexcept;
    [[nodiscard]] const void* get_component(EntityId entity, std::type_index type) const noexcept;

    std::shared_ptr<const SnapshotData> data_{};

    friend class World;
};

/** @brief Thread-safe producer queue for commands applied by a World owner. */
class WorldCommandBuffer final {
public:
    using CommandFunction = std::function<foundation::Result(class World&)>;

    WorldCommandBuffer();
    ~WorldCommandBuffer();

    WorldCommandBuffer(const WorldCommandBuffer&) = delete;
    WorldCommandBuffer& operator=(const WorldCommandBuffer&) = delete;
    WorldCommandBuffer(WorldCommandBuffer&&) = delete;
    WorldCommandBuffer& operator=(WorldCommandBuffer&&) = delete;

    [[nodiscard]] foundation::Result enqueue(CommandFunction command);
    [[nodiscard]] foundation::Result apply(class World& world,
                                           uint32_t max_commands = std::numeric_limits<uint32_t>::max());
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    void clear() noexcept;

    [[nodiscard]] foundation::Result enqueue_destroy(EntityId entity);

    template <typename Type, typename... Arguments>
    [[nodiscard]] foundation::Result enqueue_emplace(EntityId entity, Arguments&&... arguments);

    template <typename Type>
    [[nodiscard]] foundation::Result enqueue_remove(EntityId entity);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_{};
};

/** @brief Mutable entity-component state owned by one World Scope. */
class World final {
public:
    explicit World(WorldHandle handle, const WorldDesc& desc = {});
    ~World();

    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = delete;
    World& operator=(World&&) = delete;

    [[nodiscard]] WorldHandle handle() const noexcept;
    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] bool is_owner_thread() const noexcept;
    [[nodiscard]] foundation::Result claim_owner_thread() noexcept;
    [[nodiscard]] uint64_t revision() const noexcept;
    [[nodiscard]] std::size_t entity_count() const noexcept;
    [[nodiscard]] std::size_t component_type_count() const noexcept;

    /** @brief Iterates one mutable component pool while rejecting structural ECS changes. */
    template <typename Type, typename Function>
    [[nodiscard]] foundation::Result for_each(Function&& function) {
        using Callback = std::remove_reference_t<Function>;
        struct Adapter final {
            static foundation::Result invoke(EntityId entity, void* value, void* user_data) {
                Callback& callback = *static_cast<Callback*>(user_data);
                try {
                    if constexpr (std::is_same_v<std::invoke_result_t<Callback&, EntityId, Type&>,
                                                 foundation::Result>) {
                        return callback(entity, *static_cast<Type*>(value));
                    } else {
                        callback(entity, *static_cast<Type*>(value));
                        return foundation::Result::success();
                    }
                } catch (const std::bad_alloc&) {
                    return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                                       "mutable ECS query callback allocation failed");
                } catch (const std::exception& exception) {
                    return foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what());
                } catch (...) {
                    return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                                       "mutable ECS query callback threw an unknown exception");
                }
            }
        };
        return for_each_component(std::type_index(typeid(Type)), &Adapter::invoke, &function);
    }

    template <typename Type, typename Function>
    [[nodiscard]] foundation::Result for_each(Function&& function) const {
        using Callback = std::remove_reference_t<Function>;
        struct Adapter final {
            static foundation::Result invoke(EntityId entity, const void* value, void* user_data) {
                Callback& callback = *static_cast<Callback*>(user_data);
                try {
                    if constexpr (std::is_same_v<std::invoke_result_t<Callback&, EntityId, const Type&>,
                                                 foundation::Result>) {
                        return callback(entity, *static_cast<const Type*>(value));
                    } else {
                        callback(entity, *static_cast<const Type*>(value));
                        return foundation::Result::success();
                    }
                } catch (const std::bad_alloc&) {
                    return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                                       "const ECS query callback allocation failed");
                } catch (const std::exception& exception) {
                    return foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what());
                } catch (...) {
                    return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                                       "const ECS query callback threw an unknown exception");
                }
            }
        };
        return for_each_component(std::type_index(typeid(Type)), &Adapter::invoke, &function);
    }

    template <typename Type>
    [[nodiscard]] foundation::Result register_component(ComponentDescriptor descriptor) {
        static_assert(std::is_copy_constructible_v<Type>, "World snapshot components must be copy constructible");
        static_assert(std::is_nothrow_move_constructible_v<Type> && std::is_nothrow_move_assignable_v<Type>,
                      "World components must be nothrow move constructible and move assignable");
        static_assert(std::is_nothrow_destructible_v<Type>, "World components must be nothrow destructible");
        if (descriptor.size == 0) {
            descriptor.size = sizeof(Type);
        }
        if (descriptor.alignment == 0) {
            descriptor.alignment = alignof(Type);
        }
        return register_component_type(descriptor, std::type_index(typeid(Type)), [](ComponentTypeId id) {
            return std::make_unique<detail::ComponentPool<Type>>(id);
        });
    }

    template <typename Type>
    [[nodiscard]] foundation::Result register_component(ComponentTypeId id, std::string_view name = {}) {
        return register_component<Type>(ComponentDescriptor{id, name, sizeof(Type), alignof(Type), true});
    }

    template <typename Type>
    [[nodiscard]] ComponentTypeId component_type_id() const noexcept {
        const auto result = find_component_type(std::type_index(typeid(Type)));
        return result ? result.value : ComponentTypeId{};
    }

    [[nodiscard]] foundation::ResultValue<ComponentDescriptor> find_component_descriptor(ComponentTypeId id) const;

    [[nodiscard]] foundation::ResultValue<EntityId> create_entity();
    [[nodiscard]] foundation::Result destroy_entity(EntityId entity);
    [[nodiscard]] bool is_alive(EntityId entity) const noexcept;

    template <typename Type, typename... Arguments>
    [[nodiscard]] foundation::ResultValue<Type*> emplace(EntityId entity, Arguments&&... arguments) {
        if (const foundation::Result owner_result = validate_owner_access(); !owner_result) {
            return {owner_result, nullptr};
        }
        try {
            Type value(std::forward<Arguments>(arguments)...);
            const ComponentTypeId type = component_type_id<Type>();
            const foundation::ResultValue<void*> result = emplace_component(entity, type, &value);
            return {result.result, static_cast<Type*>(result.value)};
        } catch (const std::bad_alloc&) {
            return {foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                                "component construction allocation failed"),
                    nullptr};
        } catch (const std::exception& exception) {
            return {foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what()), nullptr};
        } catch (...) {
            return {foundation::Result::failure(foundation::ResultCode::kUnknown,
                                                "component construction threw an unknown exception"),
                    nullptr};
        }
    }

    template <typename Type>
    [[nodiscard]] Type* get(EntityId entity) noexcept {
        if (!validate_owner_access()) {
            return nullptr;
        }
        return static_cast<Type*>(get_component(entity, component_type_id<Type>()));
    }

    template <typename Type>
    [[nodiscard]] const Type* get(EntityId entity) const noexcept {
        if (!validate_owner_access()) {
            return nullptr;
        }
        return static_cast<const Type*>(get_component(entity, component_type_id<Type>()));
    }

    template <typename Type>
    [[nodiscard]] bool has(EntityId entity) const noexcept {
        if (!validate_owner_access()) {
            return false;
        }
        return has_component(entity, component_type_id<Type>());
    }

    template <typename Type>
    [[nodiscard]] foundation::Result remove(EntityId entity) {
        if (const foundation::Result owner_result = validate_owner_access(); !owner_result) {
            return owner_result;
        }
        return remove_component(entity, component_type_id<Type>());
    }

    [[nodiscard]] foundation::Result apply_commands(WorldCommandBuffer& commands,
                                                    uint32_t max_commands = std::numeric_limits<uint32_t>::max());
    [[nodiscard]] foundation::ResultValue<WorldSnapshot> commit(uint64_t frame_number = 0, uint64_t fixed_tick = 0);
    [[nodiscard]] foundation::ResultValue<WorldSnapshot> last_snapshot() const;

private:
    struct Impl;

    using ComponentPoolFactory = std::function<std::unique_ptr<detail::ComponentPoolBase>(ComponentTypeId)>;

    [[nodiscard]] foundation::Result validate_owner_access() const noexcept;
    [[nodiscard]] foundation::Result validate_structural_mutation() const noexcept;
    [[nodiscard]] foundation::Result register_component_type(const ComponentDescriptor& descriptor,
                                                             std::type_index cpp_type, ComponentPoolFactory factory);
    [[nodiscard]] foundation::ResultValue<ComponentTypeId> find_component_type(std::type_index cpp_type) const;
    [[nodiscard]] foundation::ResultValue<void*> emplace_component(EntityId entity, ComponentTypeId type, void* value);
    [[nodiscard]] void* get_component(EntityId entity, ComponentTypeId type) noexcept;
    [[nodiscard]] const void* get_component(EntityId entity, ComponentTypeId type) const noexcept;
    [[nodiscard]] bool has_component(EntityId entity, ComponentTypeId type) const noexcept;
    [[nodiscard]] foundation::Result remove_component(EntityId entity, ComponentTypeId type);

    using MutableComponentIterationFunction = foundation::Result (*)(EntityId, void*, void*);
    using ConstComponentIterationFunction = foundation::Result (*)(EntityId, const void*, void*);

    [[nodiscard]] foundation::Result for_each_component(std::type_index cpp_type,
                                                        MutableComponentIterationFunction function, void* user_data);
    [[nodiscard]] foundation::Result for_each_component(std::type_index cpp_type,
                                                        ConstComponentIterationFunction function,
                                                        void* user_data) const;

    std::unique_ptr<Impl> impl_{};

    friend class WorldCommandBuffer;
    friend class WorldManager;
};

/** @brief Thread-safe owner of multiple independently isolated World instances. */
class WorldManager final {
public:
    WorldManager();
    ~WorldManager();

    WorldManager(const WorldManager&) = delete;
    WorldManager& operator=(const WorldManager&) = delete;
    WorldManager(WorldManager&&) = delete;
    WorldManager& operator=(WorldManager&&) = delete;

    [[nodiscard]] foundation::ResultValue<WorldHandle> create_world(const WorldDesc& desc = {});
    [[nodiscard]] foundation::Result destroy_world(WorldHandle handle);
    [[nodiscard]] foundation::ResultValue<std::shared_ptr<World>> find(WorldHandle handle) const;
    [[nodiscard]] foundation::ResultValue<std::shared_ptr<const World>> find_const(WorldHandle handle) const;
    [[nodiscard]] bool contains(WorldHandle handle) const noexcept;
    [[nodiscard]] std::size_t world_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_{};
};

template <typename Type, typename... Arguments>
foundation::Result WorldCommandBuffer::enqueue_emplace(EntityId entity, Arguments&&... arguments) {
    try {
        auto values = std::make_shared<std::tuple<std::decay_t<Arguments>...>>(std::forward<Arguments>(arguments)...);
        return enqueue([entity, values](World& world) {
            return std::apply(
                [entity, &world](auto&... arguments) {
                    const auto result = world.emplace<Type>(entity, arguments...);
                    return result.result;
                },
                *values);
        });
    } catch (const std::bad_alloc&) {
        return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                           "world command argument allocation failed");
    } catch (const std::exception& exception) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what());
    } catch (...) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                           "world command argument construction threw an unknown exception");
    }
}

template <typename Type>
foundation::Result WorldCommandBuffer::enqueue_remove(EntityId entity) {
    return enqueue([entity](World& world) { return world.remove<Type>(entity); });
}
}  // namespace aeg::ecs
