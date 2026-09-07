/**
 * @file ecs.cpp
 * @brief Implements entity-component storage, World commands, and snapshots.
 */
#include "../public/ecs.hpp"

#include <atomic>
#include <deque>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace aeg::ecs {
namespace {
[[nodiscard]] foundation::Result invalid_argument(std::string message) {
    return foundation::Result::failure(foundation::ResultCode::kInvalidArgument, std::move(message));
}

[[nodiscard]] foundation::Result invalid_state(std::string message) {
    return foundation::Result::failure(foundation::ResultCode::kInvalidState, std::move(message));
}

[[nodiscard]] foundation::Result not_found(std::string message) {
    return foundation::Result::failure(foundation::ResultCode::kNotFound, std::move(message));
}

[[nodiscard]] foundation::Result not_ready(std::string message) {
    return foundation::Result::failure(foundation::ResultCode::kNotReady, std::move(message));
}

[[nodiscard]] foundation::Result out_of_memory(std::string message) {
    return foundation::Result::failure(foundation::ResultCode::kOutOfMemory, std::move(message));
}

[[nodiscard]] foundation::Result unsupported(std::string message) {
    return foundation::Result::failure(foundation::ResultCode::kUnsupported, std::move(message));
}

[[nodiscard]] uint32_t next_generation(uint32_t generation) noexcept {
    ++generation;
    return generation == 0 ? 1 : generation;
}

}  // namespace

struct WorldSnapshot::SnapshotData final {
    WorldHandle world{};
    uint64_t revision{0};
    uint64_t frame_number{0};
    uint64_t fixed_tick{0};
    std::vector<EntityId> entities{};
    std::vector<uint32_t> entity_generations{};
    std::unordered_map<std::type_index, std::shared_ptr<const detail::ComponentSnapshotBase>> components{};
};

struct WorldCommandBuffer::Impl final {
    mutable std::mutex mutex;
    std::deque<CommandFunction> commands{};
};

struct EntityRecord final {
    uint32_t generation{1};
    bool alive{false};
};

struct ComponentRecord final {
    ComponentDescriptor descriptor{};
    std::string name{};
    std::type_index cpp_type{typeid(void)};
    std::unique_ptr<detail::ComponentPoolBase> pool{};
};

struct World::Impl final {
    WorldHandle handle{};
    std::string name{};
    bool enforce_owner_thread{true};
    std::thread::id owner_thread{};
    std::atomic<uint64_t> revision{0};
    std::vector<EntityRecord> entities{};
    std::vector<uint32_t> free_entities{};
    std::size_t alive_entity_count{0};
    uint32_t active_iteration_count{0};
    std::unordered_map<ComponentTypeId, ComponentRecord, ComponentTypeIdHash> components{};
    std::unordered_map<std::type_index, ComponentTypeId> component_types{};
    mutable std::shared_mutex snapshot_mutex;
    std::shared_ptr<const WorldSnapshot::SnapshotData> last_snapshot{};
};

struct WorldManager::Impl final {
    struct Slot final {
        uint32_t generation{1};
        std::shared_ptr<World> world{};
    };

    mutable std::shared_mutex mutex;
    std::vector<Slot> slots{};
    std::vector<uint32_t> free_slots{};
};

bool WorldSnapshot::is_valid() const noexcept {
    return data_ != nullptr && data_->world.is_valid() && data_->revision != 0;
}

WorldHandle WorldSnapshot::world() const noexcept {
    return data_ == nullptr ? WorldHandle{} : data_->world;
}

uint64_t WorldSnapshot::revision() const noexcept {
    return data_ == nullptr ? 0 : data_->revision;
}

uint64_t WorldSnapshot::frame_number() const noexcept {
    return data_ == nullptr ? 0 : data_->frame_number;
}

uint64_t WorldSnapshot::fixed_tick() const noexcept {
    return data_ == nullptr ? 0 : data_->fixed_tick;
}

std::size_t WorldSnapshot::entity_count() const noexcept {
    return data_ == nullptr ? 0 : data_->entities.size();
}

std::size_t WorldSnapshot::component_type_count() const noexcept {
    return data_ == nullptr ? 0 : data_->components.size();
}

EntityId WorldSnapshot::entity_at(std::size_t index) const noexcept {
    return data_ == nullptr || index >= data_->entities.size() ? EntityId{} : data_->entities[index];
}

bool WorldSnapshot::contains(EntityId entity) const noexcept {
    if (!entity.is_valid() || data_ == nullptr) {
        return false;
    }
    return entity.index < data_->entity_generations.size() &&
           data_->entity_generations[entity.index] == entity.generation;
}

const detail::ComponentSnapshotBase* WorldSnapshot::find_component(std::type_index type) const noexcept {
    if (data_ == nullptr) {
        return nullptr;
    }
    const auto iterator = data_->components.find(type);
    return iterator == data_->components.end() ? nullptr : iterator->second.get();
}

const void* WorldSnapshot::get_component(EntityId entity, std::type_index type) const noexcept {
    const detail::ComponentSnapshotBase* component = find_component(type);
    return component == nullptr ? nullptr : component->get(entity);
}

WorldCommandBuffer::WorldCommandBuffer() : impl_(std::make_unique<Impl>()) {}

WorldCommandBuffer::~WorldCommandBuffer() = default;

foundation::Result WorldCommandBuffer::enqueue(CommandFunction command) {
    if (impl_ == nullptr) {
        return invalid_state("world command buffer is not initialized");
    }
    if (!command) {
        return invalid_argument("world command is empty");
    }
    try {
        std::lock_guard lock(impl_->mutex);
        impl_->commands.push_back(std::move(command));
    } catch (const std::bad_alloc&) {
        return out_of_memory("world command allocation failed");
    }
    return foundation::Result::success();
}

foundation::Result WorldCommandBuffer::apply(World& world, uint32_t max_commands) {
    if (impl_ == nullptr) {
        return invalid_state("world command buffer is not initialized");
    }
    if (const foundation::Result owner_result = world.validate_owner_access(); !owner_result) {
        return owner_result;
    }
    if (const foundation::Result mutation_result = world.validate_structural_mutation(); !mutation_result) {
        return mutation_result;
    }
    if (max_commands == 0) {
        return foundation::Result::success();
    }

    std::vector<CommandFunction> commands;
    try {
        std::lock_guard lock(impl_->mutex);
        const std::size_t count = std::min<std::size_t>(max_commands, impl_->commands.size());
        commands.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            commands.push_back(std::move(impl_->commands.front()));
            impl_->commands.pop_front();
        }
    } catch (const std::bad_alloc&) {
        return out_of_memory("world command batch allocation failed");
    }

    foundation::Result first_error = foundation::Result::success();
    for (CommandFunction& command : commands) {
        foundation::Result result;
        try {
            result = command(world);
        } catch (const std::exception& exception) {
            result = foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what());
        } catch (...) {
            result = foundation::Result::failure(foundation::ResultCode::kUnknown,
                                                 "world command threw an unknown exception");
        }
        if (!result && first_error) {
            first_error = std::move(result);
        }
    }
    return first_error;
}

std::size_t WorldCommandBuffer::size() const noexcept {
    if (impl_ == nullptr) {
        return 0;
    }
    std::lock_guard lock(impl_->mutex);
    return impl_->commands.size();
}

bool WorldCommandBuffer::empty() const noexcept {
    return size() == 0;
}

void WorldCommandBuffer::clear() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    std::lock_guard lock(impl_->mutex);
    impl_->commands.clear();
}

foundation::Result WorldCommandBuffer::enqueue_destroy(EntityId entity) {
    return enqueue([entity](World& world) { return world.destroy_entity(entity); });
}

World::World(WorldHandle handle, const WorldDesc& desc) : impl_(std::make_unique<Impl>()) {
    impl_->handle = handle;
    impl_->name = desc.name;
    impl_->enforce_owner_thread = desc.enforce_owner_thread;
    impl_->owner_thread = std::this_thread::get_id();
}

World::~World() = default;

WorldHandle World::handle() const noexcept {
    return impl_ == nullptr ? WorldHandle{} : impl_->handle;
}

std::string_view World::name() const noexcept {
    return impl_ == nullptr ? std::string_view{} : std::string_view(impl_->name);
}

bool World::is_owner_thread() const noexcept {
    return impl_ != nullptr && (!impl_->enforce_owner_thread || impl_->owner_thread == std::this_thread::get_id());
}

foundation::Result World::claim_owner_thread() noexcept {
    if (impl_ == nullptr) {
        return invalid_state("world is not initialized");
    }
    impl_->owner_thread = std::this_thread::get_id();
    return foundation::Result::success();
}

uint64_t World::revision() const noexcept {
    return impl_ == nullptr ? 0 : impl_->revision.load(std::memory_order_acquire);
}

std::size_t World::entity_count() const noexcept {
    if (impl_ == nullptr || !validate_owner_access()) {
        return 0;
    }
    return impl_->alive_entity_count;
}

std::size_t World::component_type_count() const noexcept {
    return impl_ == nullptr || !validate_owner_access() ? 0 : impl_->components.size();
}

foundation::Result World::validate_owner_access() const noexcept {
    if (impl_ == nullptr) {
        return invalid_state("world is not initialized");
    }
    if (impl_->enforce_owner_thread && impl_->owner_thread != std::this_thread::get_id()) {
        return invalid_state("world mutable state is accessed from a non-owner thread");
    }
    return foundation::Result::success();
}

foundation::Result World::validate_structural_mutation() const noexcept {
    if (impl_ == nullptr) {
        return invalid_state("world is not initialized");
    }
    if (impl_->active_iteration_count != 0) {
        return invalid_state("structural ECS mutation is not allowed during component iteration");
    }
    return foundation::Result::success();
}

foundation::Result World::register_component_type(const ComponentDescriptor& descriptor, std::type_index cpp_type,
                                                  ComponentPoolFactory factory) {
    if (const foundation::Result owner_result = validate_owner_access(); !owner_result) {
        return owner_result;
    }
    if (const foundation::Result mutation_result = validate_structural_mutation(); !mutation_result) {
        return mutation_result;
    }
    if (!descriptor.id.is_valid() || descriptor.size == 0 || descriptor.alignment == 0) {
        return invalid_argument("component descriptor is invalid");
    }
    if (!descriptor.snapshotable) {
        return unsupported("non-snapshotable components are not supported by this World implementation");
    }
    if (!factory) {
        return invalid_argument("component pool factory is empty");
    }
    if (impl_->components.find(descriptor.id) != impl_->components.end()) {
        return invalid_state("component type id is already registered");
    }
    if (impl_->component_types.find(cpp_type) != impl_->component_types.end()) {
        return invalid_state("C++ component type is already registered");
    }

    ComponentRecord record;
    try {
        record.name = std::string(descriptor.name);
        record.descriptor = descriptor;
        record.descriptor.name = {};
        record.cpp_type = cpp_type;
        record.pool = factory(descriptor.id);
        if (record.pool == nullptr) {
            return out_of_memory("component pool construction failed");
        }

        auto [component_iterator, inserted] = impl_->components.emplace(descriptor.id, std::move(record));
        if (!inserted) {
            return invalid_state("component type id is already registered");
        }
        component_iterator->second.descriptor.name = component_iterator->second.name;
        try {
            impl_->component_types.emplace(cpp_type, descriptor.id);
        } catch (...) {
            impl_->components.erase(component_iterator);
            throw;
        }
    } catch (const std::bad_alloc&) {
        return out_of_memory("component registration allocation failed");
    } catch (const std::exception& exception) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what());
    } catch (...) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                           "component registration threw an unknown exception");
    }
    return foundation::Result::success();
}

foundation::ResultValue<ComponentTypeId> World::find_component_type(std::type_index cpp_type) const {
    if (impl_ == nullptr) {
        return {invalid_state("world is not initialized"), {}};
    }
    if (const foundation::Result owner_result = validate_owner_access(); !owner_result) {
        return {owner_result, {}};
    }
    const auto iterator = impl_->component_types.find(cpp_type);
    if (iterator == impl_->component_types.end()) {
        return {not_found("component type is not registered"), {}};
    }
    return {foundation::Result::success(), iterator->second};
}

foundation::ResultValue<ComponentDescriptor> World::find_component_descriptor(ComponentTypeId id) const {
    if (impl_ == nullptr) {
        return {invalid_state("world is not initialized"), {}};
    }
    if (const foundation::Result owner_result = validate_owner_access(); !owner_result) {
        return {owner_result, {}};
    }
    if (!id.is_valid()) {
        return {invalid_argument("component type id is invalid"), {}};
    }
    const auto iterator = impl_->components.find(id);
    if (iterator == impl_->components.end()) {
        return {not_found("component type is not registered"), {}};
    }
    return {foundation::Result::success(), iterator->second.descriptor};
}

foundation::ResultValue<EntityId> World::create_entity() {
    if (const foundation::Result owner_result = validate_owner_access(); !owner_result) {
        return {owner_result, {}};
    }
    if (const foundation::Result mutation_result = validate_structural_mutation(); !mutation_result) {
        return {mutation_result, {}};
    }

    try {
        uint32_t index = kInvalidEntityIndex;
        if (!impl_->free_entities.empty()) {
            index = impl_->free_entities.back();
            impl_->free_entities.pop_back();
        } else {
            index = static_cast<uint32_t>(impl_->entities.size());
            impl_->entities.push_back({1, false});
        }
        EntityRecord& record = impl_->entities[index];
        record.alive = true;
        if (record.generation == 0) {
            record.generation = 1;
        }
        ++impl_->alive_entity_count;
        return {foundation::Result::success(), EntityId{index, record.generation}};
    } catch (const std::bad_alloc&) {
        return {out_of_memory("entity allocation failed"), {}};
    }
}

foundation::Result World::destroy_entity(EntityId entity) {
    if (const foundation::Result owner_result = validate_owner_access(); !owner_result) {
        return owner_result;
    }
    if (const foundation::Result mutation_result = validate_structural_mutation(); !mutation_result) {
        return mutation_result;
    }
    if (!is_alive(entity)) {
        return not_found("entity is not alive in this World");
    }

    try {
        impl_->free_entities.push_back(entity.index);
    } catch (const std::bad_alloc&) {
        return out_of_memory("entity free-list allocation failed");
    }

    for (auto& [type, record] : impl_->components) {
        static_cast<void>(type);
        static_cast<void>(record.pool->remove(entity));
    }
    EntityRecord& record = impl_->entities[entity.index];
    record.alive = false;
    record.generation = next_generation(record.generation);
    --impl_->alive_entity_count;
    return foundation::Result::success();
}

bool World::is_alive(EntityId entity) const noexcept {
    if (impl_ == nullptr || !validate_owner_access() || !entity.is_valid() || entity.index >= impl_->entities.size()) {
        return false;
    }
    const EntityRecord& record = impl_->entities[entity.index];
    return record.alive && record.generation == entity.generation;
}

foundation::ResultValue<void*> World::emplace_component(EntityId entity, ComponentTypeId type, void* value) {
    if (const foundation::Result mutation_result = validate_structural_mutation(); !mutation_result) {
        return {mutation_result, nullptr};
    }
    if (!entity.is_valid() || !is_alive(entity)) {
        return {not_found("entity is not alive in this World"), nullptr};
    }
    if (!type.is_valid()) {
        return {not_found("component type is not registered"), nullptr};
    }
    const auto iterator = impl_->components.find(type);
    if (iterator == impl_->components.end()) {
        return {not_found("component type is not registered"), nullptr};
    }
    return iterator->second.pool->emplace_move(entity, value);
}

void* World::get_component(EntityId entity, ComponentTypeId type) noexcept {
    return const_cast<void*>(static_cast<const World*>(this)->get_component(entity, type));
}

const void* World::get_component(EntityId entity, ComponentTypeId type) const noexcept {
    if (!is_alive(entity) || !type.is_valid()) {
        return nullptr;
    }
    const auto iterator = impl_->components.find(type);
    return iterator == impl_->components.end() ? nullptr : iterator->second.pool->get(entity);
}

bool World::has_component(EntityId entity, ComponentTypeId type) const noexcept {
    if (!is_alive(entity) || !type.is_valid()) {
        return false;
    }
    const auto iterator = impl_->components.find(type);
    return iterator != impl_->components.end() && iterator->second.pool->has(entity);
}

foundation::Result World::remove_component(EntityId entity, ComponentTypeId type) {
    if (const foundation::Result mutation_result = validate_structural_mutation(); !mutation_result) {
        return mutation_result;
    }
    if (!entity.is_valid() || !is_alive(entity)) {
        return not_found("entity is not alive in this World");
    }
    const auto iterator = impl_->components.find(type);
    if (iterator == impl_->components.end()) {
        return not_found("component type is not registered");
    }
    if (!iterator->second.pool->remove(entity)) {
        return not_found("entity does not own this component");
    }
    return foundation::Result::success();
}

foundation::Result World::for_each_component(std::type_index cpp_type, MutableComponentIterationFunction function,
                                             void* user_data) {
    if (function == nullptr || user_data == nullptr) {
        return invalid_argument("ECS query callback is invalid");
    }
    if (const foundation::Result owner_result = validate_owner_access(); !owner_result) {
        return owner_result;
    }
    const auto type_iterator = impl_->component_types.find(cpp_type);
    if (type_iterator == impl_->component_types.end()) {
        return foundation::Result::success();
    }
    const auto component_iterator = impl_->components.find(type_iterator->second);
    if (component_iterator == impl_->components.end()) {
        return foundation::Result::success();
    }
    ++impl_->active_iteration_count;
    class IterationGuard final {
    public:
        explicit IterationGuard(uint32_t& count) noexcept : count_(count) {}
        ~IterationGuard() { --count_; }

    private:
        uint32_t& count_;
    } guard(impl_->active_iteration_count);
    detail::ComponentPoolBase& pool = *component_iterator->second.pool;
    for (std::size_t index = 0; index < pool.size(); ++index) {
        void* value = pool.value_at(index);
        if (value != nullptr) {
            if (const foundation::Result result = function(pool.entity_at(index), value, user_data); !result) {
                return result;
            }
        }
    }
    return foundation::Result::success();
}

foundation::Result World::for_each_component(std::type_index cpp_type, ConstComponentIterationFunction function,
                                             void* user_data) const {
    if (function == nullptr || user_data == nullptr) {
        return invalid_argument("ECS query callback is invalid");
    }
    if (const foundation::Result owner_result = validate_owner_access(); !owner_result) {
        return owner_result;
    }
    const auto type_iterator = impl_->component_types.find(cpp_type);
    if (type_iterator == impl_->component_types.end()) {
        return foundation::Result::success();
    }
    const auto component_iterator = impl_->components.find(type_iterator->second);
    if (component_iterator == impl_->components.end()) {
        return foundation::Result::success();
    }
    ++impl_->active_iteration_count;
    class IterationGuard final {
    public:
        explicit IterationGuard(uint32_t& count) noexcept : count_(count) {}
        ~IterationGuard() { --count_; }

    private:
        uint32_t& count_;
    } guard(impl_->active_iteration_count);
    const detail::ComponentPoolBase& pool = *component_iterator->second.pool;
    for (std::size_t index = 0; index < pool.size(); ++index) {
        const void* value = pool.value_at(index);
        if (value != nullptr) {
            if (const foundation::Result result = function(pool.entity_at(index), value, user_data); !result) {
                return result;
            }
        }
    }
    return foundation::Result::success();
}

foundation::Result World::apply_commands(WorldCommandBuffer& commands, uint32_t max_commands) {
    if (const foundation::Result owner_result = validate_owner_access(); !owner_result) {
        return owner_result;
    }
    if (const foundation::Result mutation_result = validate_structural_mutation(); !mutation_result) {
        return mutation_result;
    }
    return commands.apply(*this, max_commands);
}

foundation::ResultValue<WorldSnapshot> World::commit(uint64_t frame_number, uint64_t fixed_tick) {
    if (const foundation::Result owner_result = validate_owner_access(); !owner_result) {
        return {owner_result, {}};
    }

    try {
        auto data = std::make_shared<WorldSnapshot::SnapshotData>();
        data->world = impl_->handle;
        data->revision = impl_->revision.load(std::memory_order_relaxed) + 1;
        data->frame_number = frame_number;
        data->fixed_tick = fixed_tick;
        data->entities.reserve(impl_->alive_entity_count);
        data->entity_generations.assign(impl_->entities.size(), 0);
        for (uint32_t index = 0; index < impl_->entities.size(); ++index) {
            const EntityRecord& record = impl_->entities[index];
            if (record.alive) {
                data->entities.push_back(EntityId{index, record.generation});
                data->entity_generations[index] = record.generation;
            }
        }
        data->components.reserve(impl_->components.size());
        for (const auto& [type, record] : impl_->components) {
            data->components.emplace(record.cpp_type, record.pool->snapshot());
            static_cast<void>(type);
        }

        impl_->revision.store(data->revision, std::memory_order_release);
        {
            std::unique_lock snapshot_lock(impl_->snapshot_mutex);
            impl_->last_snapshot = data;
        }
        return {foundation::Result::success(), WorldSnapshot(std::move(data))};
    } catch (const std::bad_alloc&) {
        return {out_of_memory("World snapshot allocation failed"), {}};
    } catch (const std::exception& exception) {
        return {foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what()), {}};
    } catch (...) {
        return {foundation::Result::failure(foundation::ResultCode::kUnknown,
                                            "World snapshot construction threw an unknown exception"),
                {}};
    }
}

foundation::ResultValue<WorldSnapshot> World::last_snapshot() const {
    if (impl_ == nullptr) {
        return {invalid_state("world is not initialized"), {}};
    }
    std::shared_lock snapshot_lock(impl_->snapshot_mutex);
    if (impl_->last_snapshot == nullptr) {
        return {not_ready("World has not committed a snapshot"), {}};
    }
    return {foundation::Result::success(), WorldSnapshot(impl_->last_snapshot)};
}

WorldManager::WorldManager() : impl_(std::make_unique<Impl>()) {}

WorldManager::~WorldManager() = default;

foundation::ResultValue<WorldHandle> WorldManager::create_world(const WorldDesc& desc) {
    if (impl_ == nullptr) {
        return {invalid_state("World manager is not initialized"), {}};
    }
    std::unique_lock lock(impl_->mutex);
    uint32_t index = kInvalidWorldIndex;
    bool reused_slot = false;
    if (!impl_->free_slots.empty()) {
        try {
            impl_->free_slots.reserve(impl_->free_slots.size() + 1);
        } catch (const std::bad_alloc&) {
            return {out_of_memory("World free-list allocation failed"), {}};
        }
        index = impl_->free_slots.back();
        impl_->free_slots.pop_back();
        reused_slot = true;
    } else {
        index = static_cast<uint32_t>(impl_->slots.size());
        try {
            impl_->slots.push_back({1, nullptr});
        } catch (const std::bad_alloc&) {
            return {out_of_memory("World slot allocation failed"), {}};
        }
    }

    Impl::Slot& slot = impl_->slots[index];
    if (slot.generation == 0) {
        slot.generation = 1;
    }
    const WorldHandle handle{index, slot.generation};
    try {
        slot.world = std::make_shared<World>(handle, desc);
    } catch (const std::bad_alloc&) {
        if (reused_slot) {
            impl_->free_slots.push_back(index);
        } else {
            impl_->slots.pop_back();
        }
        return {out_of_memory("World allocation failed"), {}};
    } catch (const std::exception& exception) {
        if (reused_slot) {
            impl_->free_slots.push_back(index);
        } else {
            impl_->slots.pop_back();
        }
        return {foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what()), {}};
    }
    return {foundation::Result::success(), handle};
}

foundation::Result WorldManager::destroy_world(WorldHandle handle) {
    if (impl_ == nullptr) {
        return invalid_state("World manager is not initialized");
    }
    if (!handle.is_valid()) {
        return invalid_argument("World handle is invalid");
    }
    std::unique_lock lock(impl_->mutex);
    if (handle.index >= impl_->slots.size()) {
        return not_found("World handle is not registered");
    }
    Impl::Slot& slot = impl_->slots[handle.index];
    if (slot.generation != handle.generation || slot.world == nullptr) {
        return not_found("World handle generation is stale");
    }
    try {
        if (impl_->free_slots.size() == impl_->free_slots.capacity()) {
            impl_->free_slots.reserve(impl_->free_slots.size() + 1);
        }
    } catch (const std::bad_alloc&) {
        return out_of_memory("World free-list allocation failed");
    }
    slot.world.reset();
    slot.generation = next_generation(slot.generation);
    impl_->free_slots.push_back(handle.index);
    return foundation::Result::success();
}

foundation::ResultValue<std::shared_ptr<World>> WorldManager::find(WorldHandle handle) const {
    if (impl_ == nullptr) {
        return {invalid_state("World manager is not initialized"), {}};
    }
    if (!handle.is_valid()) {
        return {invalid_argument("World handle is invalid"), {}};
    }
    std::shared_lock lock(impl_->mutex);
    if (handle.index >= impl_->slots.size()) {
        return {not_found("World handle is not registered"), {}};
    }
    const Impl::Slot& slot = impl_->slots[handle.index];
    if (slot.generation != handle.generation || slot.world == nullptr) {
        return {not_found("World handle generation is stale"), {}};
    }
    return {foundation::Result::success(), slot.world};
}

foundation::ResultValue<std::shared_ptr<const World>> WorldManager::find_const(WorldHandle handle) const {
    const foundation::ResultValue<std::shared_ptr<World>> result = find(handle);
    if (!result) {
        return {result.result, {}};
    }
    return {foundation::Result::success(), std::shared_ptr<const World>(result.value)};
}

bool WorldManager::contains(WorldHandle handle) const noexcept {
    if (impl_ == nullptr || !handle.is_valid()) {
        return false;
    }
    std::shared_lock lock(impl_->mutex);
    return handle.index < impl_->slots.size() && impl_->slots[handle.index].generation == handle.generation &&
           impl_->slots[handle.index].world != nullptr;
}

std::size_t WorldManager::world_count() const noexcept {
    if (impl_ == nullptr) {
        return 0;
    }
    std::shared_lock lock(impl_->mutex);
    return static_cast<std::size_t>(std::count_if(impl_->slots.begin(), impl_->slots.end(),
                                                  [](const Impl::Slot& slot) { return slot.world != nullptr; }));
}
}  // namespace aeg::ecs
