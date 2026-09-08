/**
 * @file reflection.cpp
 * @brief Implements reflected type descriptor construction and lookup.
 */
#include "../public/reflection.hpp"

#include <algorithm>
#include <mutex>
#include <new>

namespace aeg::reflection {
namespace {
using foundation::invalid_argument;
using foundation::invalid_state;
using foundation::not_found;
using foundation::out_of_memory;

[[nodiscard]] foundation::Result validate_descriptor(const TypeDescriptor& descriptor) {
    if (!descriptor.id.is_valid() || descriptor.name.empty() || descriptor.version == 0 || descriptor.size == 0 ||
        descriptor.alignment == 0) {
        return invalid_argument("reflected type descriptor is invalid");
    }

    std::unordered_map<FieldId, bool, StableIdHash<FieldIdTag>> fields;
    try {
        fields.reserve(descriptor.fields.size());
        for (const FieldDescriptor& field : descriptor.fields) {
            if (!field.id.is_valid() || field.name.empty() || !field.type.is_valid() || !field.accessor.is_valid()) {
                return invalid_argument("reflected field descriptor is invalid");
            }
            if (!fields.emplace(field.id, true).second) {
                return invalid_state("reflected field identity is duplicated");
            }
        }
    } catch (const std::bad_alloc&) {
        return out_of_memory("reflected field validation allocation failed");
    }
    return foundation::Result::success();
}
}  // namespace

foundation::Result TypeBuilder::add_field(std::string name, TypeId type, FieldAccessor accessor, FieldFlags flags) {
    if (!status_) {
        return status_;
    }
    if (name.empty() || !type.is_valid() || !accessor.is_valid()) {
        status_ = invalid_argument("reflected field declaration is invalid");
        return status_;
    }
    const FieldId id(name);
    const auto duplicate = std::find_if(descriptor_.fields.begin(), descriptor_.fields.end(),
                                        [id](const FieldDescriptor& field) { return field.id == id; });
    if (duplicate != descriptor_.fields.end()) {
        status_ = invalid_state("reflected field is already declared");
        return status_;
    }

    try {
        descriptor_.fields.push_back(
            FieldDescriptor{id, std::move(name), type, accessor.value_kind, flags, std::move(accessor)});
    } catch (const std::bad_alloc&) {
        status_ = out_of_memory("reflected field allocation failed");
    }
    return status_;
}

foundation::ResultValue<TypeDescriptor> TypeBuilder::finish() && {
    if (!status_) {
        return {status_, {}};
    }
    if (const foundation::Result result = validate_descriptor(descriptor_); !result) {
        return {result, {}};
    }
    return {foundation::Result::success(), std::move(descriptor_)};
}

foundation::Result TypeRegistry::register_type(TypeDescriptor descriptor) {
    if (const foundation::Result result = validate_descriptor(descriptor); !result) {
        return result;
    }
    try {
        auto definition = std::make_shared<const TypeDescriptor>(std::move(descriptor));
        std::unique_lock lock(mutex_);
        if (types_.find(definition->id) != types_.end()) {
            return invalid_state("reflected type is already registered");
        }
        types_.emplace(definition->id, std::move(definition));
    } catch (const std::bad_alloc&) {
        return out_of_memory("reflected type registration allocation failed");
    }
    return foundation::Result::success();
}

foundation::Result TypeRegistry::unregister_type(TypeId id) {
    if (!id.is_valid()) {
        return invalid_argument("reflected type id is invalid");
    }
    std::unique_lock lock(mutex_);
    const auto iterator = types_.find(id);
    if (iterator == types_.end()) {
        return not_found("reflected type is not registered");
    }
    types_.erase(iterator);
    return foundation::Result::success();
}

bool TypeRegistry::contains(TypeId id) const noexcept {
    if (!id.is_valid()) {
        return false;
    }
    std::shared_lock lock(mutex_);
    return types_.find(id) != types_.end();
}

foundation::ResultValue<std::shared_ptr<const TypeDescriptor>> TypeRegistry::find_type(TypeId id) const {
    if (!id.is_valid()) {
        return {invalid_argument("reflected type id is invalid"), {}};
    }
    std::shared_lock lock(mutex_);
    const auto iterator = types_.find(id);
    if (iterator == types_.end()) {
        return {not_found("reflected type is not registered"), {}};
    }
    return {foundation::Result::success(), iterator->second};
}

foundation::ResultValue<FieldDescriptor> TypeRegistry::find_field(TypeId type, FieldId field) const {
    const foundation::ResultValue<std::shared_ptr<const TypeDescriptor>> type_result = find_type(type);
    if (!type_result) {
        return {type_result.result, {}};
    }
    const auto iterator = std::find_if(type_result.value->fields.begin(), type_result.value->fields.end(),
                                       [field](const FieldDescriptor& candidate) { return candidate.id == field; });
    if (iterator == type_result.value->fields.end()) {
        return {not_found("reflected field is not registered"), {}};
    }
    return {foundation::Result::success(), *iterator};
}

foundation::ResultValue<std::vector<TypeDescriptor>> TypeRegistry::snapshot() const {
    std::vector<TypeDescriptor> result;
    try {
        std::shared_lock lock(mutex_);
        result.reserve(types_.size());
        for (const auto& [id, descriptor] : types_) {
            static_cast<void>(id);
            result.push_back(*descriptor);
        }
        std::sort(result.begin(), result.end(),
                  [](const TypeDescriptor& left, const TypeDescriptor& right) { return left.id < right.id; });
    } catch (const std::bad_alloc&) {
        return {out_of_memory("reflected type snapshot allocation failed"), {}};
    }
    return {foundation::Result::success(), std::move(result)};
}
}  // namespace aeg::reflection
