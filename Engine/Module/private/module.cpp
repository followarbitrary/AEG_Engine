/**
 * @file module.cpp
 * @brief Implements module registration transactions and unload ordering.
 */
#include "../public/module.hpp"
#include "../../Feature/private/feature_clock_admission.hpp"

#include <algorithm>
#include <mutex>
#include <new>
#include <span>
#include <string>

namespace aeg::module {
namespace {
using foundation::invalid_argument;
using foundation::invalid_state;
using foundation::not_found;
using foundation::out_of_memory;
using foundation::unsupported;

[[nodiscard]] foundation::Result module_registration_failure(const char* action, const foundation::Result& result) {
    return foundation::Result::failure(result.code, std::string(action) + ": " + result.message);
}

template <typename Type>
[[nodiscard]] foundation::Result push_id(std::vector<Type>& values, Type value, const char* message) {
    try {
        values.push_back(value);
    } catch (const std::bad_alloc&) {
        return out_of_memory(message);
    }
    return foundation::Result::success();
}
[[nodiscard]] foundation::Result unregister_definitions(
    feature::FeatureRegistry& feature_registry, reflection::TypeRegistry* reflection_registry,
    std::span<const feature::FeatureDefinitionId> features, std::span<const reflection::TypeId> types) noexcept try {
    foundation::Result first_error = foundation::Result::success();
    // A Composite may precede its child in registration order. Remove every
    // currently removable definition, then retry its remaining dependencies.
    size_t remaining = features.size();
    while (remaining != 0) {
        bool progress = false;
        foundation::Result last_error = foundation::Result::success();
        for (auto iterator = features.rbegin(); iterator != features.rend(); ++iterator) {
            if (feature_registry.find_definition(*iterator) == nullptr) continue;
            const auto result = feature_registry.unregister_definition(*iterator);
            if (result) {
                progress = true;
            } else {
                last_error = result;
            }
        }
        remaining = static_cast<size_t>(std::count_if(features.begin(), features.end(), [&](auto id) {
            return feature_registry.find_definition(id) != nullptr;
        }));
        if (!progress) {
            first_error = last_error;
            break;
        }
    }
    if (reflection_registry != nullptr) {
        for (auto iterator = types.rbegin(); iterator != types.rend(); ++iterator) {
            const auto result = reflection_registry->unregister_type(*iterator);
            if (!result && first_error) first_error = result;
        }
    }
    return first_error;
} catch (const std::bad_alloc&) {
    // Rollback must not terminate while reporting an allocation failure.
    return foundation::Result{foundation::ResultCode::kOutOfMemory, {}};
}
}  // namespace

foundation::Result ModuleRegistrationContext::register_feature(feature::FeatureDefinition definition) {
    if (definition.contract.definition_id != definition.id || !definition.id.is_valid()) {
        return invalid_argument("module Feature definition identity is invalid");
    }
    if (definition.owner_module != module_id_) {
        return invalid_argument("Feature owner module does not match registration context");
    }
    const auto id = definition.id;
    try {
        // Reserve every ownership record before granting any new capability.
        feature_definitions_.reserve(feature_definitions_.size() + 1);
        new_clock_bindings_.reserve(new_clock_bindings_.size() + 1);
    } catch (const std::bad_alloc&) {
        return out_of_memory("module Feature ownership reservation failed");
    }
    feature::detail::ClockBindingChange approval{};
    const auto undo_approval = [&]() {
        if (!approval.inserted) return;
        static_cast<void>(feature::detail::rollback_new_clock_bindings(feature_registry_,
            std::span<const feature::detail::ClockBindingKey>(&approval.key, 1)));
        new_clock_bindings_.pop_back();
    };
    try {
        const auto approved = feature::detail::admit_clock_request(feature_registry_, definition.contract);
        if (!approved) return approved.result;
        approval = approved.value;
        if (approval.inserted) new_clock_bindings_.emplace_back(approval.key.id, approval.key.version);
        const auto registered = feature_registry_.register_definition(std::move(definition));
        if (!registered) {
            undo_approval();
            return registered;
        }
        feature_definitions_.push_back(id); // Reserved above; StableId copy cannot allocate.
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        undo_approval();
        return out_of_memory("module Feature registration allocation failed");
    }
}

foundation::Result ModuleRegistrationContext::register_type(reflection::TypeDescriptor descriptor) {
    if (reflection_registry_ == nullptr) {
        return unsupported("module registry has no reflection registry");
    }
    const reflection::TypeId id = descriptor.id;
    const foundation::Result result = reflection_registry_->register_type(std::move(descriptor));
    if (!result) {
        return result;
    }
    if (const foundation::Result record_result = push_id(types_, id, "module type ownership allocation failed");
        !record_result) {
        static_cast<void>(reflection_registry_->unregister_type(id));
        return record_result;
    }
    return foundation::Result::success();
}

foundation::Result ModuleRegistrationContext::rollback() noexcept {
    auto result = unregister_definitions(feature_registry_, reflection_registry_, feature_definitions_, types_);
    for (auto iterator = new_clock_bindings_.rbegin(); iterator != new_clock_bindings_.rend(); ++iterator) {
        const feature::detail::ClockBindingKey key{iterator->first, iterator->second};
        const auto removed = feature::detail::rollback_new_clock_bindings(feature_registry_, {&key, 1});
        if (!removed && result) result = removed;
    }
    return result;
}

foundation::Result ModuleRegistry::validate_descriptor(const ModuleDescriptor& descriptor) const {
    if (!descriptor.id.is_valid() || descriptor.name.empty() || !descriptor.version.is_valid()) {
        return invalid_argument("module descriptor is invalid");
    }
    for (const ModuleDependency& dependency : descriptor.dependencies) {
        if (!dependency.module.is_valid() || !dependency.minimum_version.is_valid() ||
            dependency.module == descriptor.id) {
            return invalid_argument("module dependency is invalid");
        }
    }
    return foundation::Result::success();
}

foundation::Result ModuleRegistry::register_module(ModuleFactory factory) {
    if (!factory) {
        return invalid_argument("module factory is empty");
    }
    std::unique_ptr<Module> module;
    try {
        module = factory();
    } catch (const std::exception& exception) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                           std::string("module factory threw: ") + exception.what());
    } catch (...) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                           "module factory threw an unknown exception");
    }
    if (!module) {
        return invalid_argument("module factory returned null");
    }
    const ModuleDescriptor descriptor = module->descriptor();
    if (const foundation::Result result = validate_descriptor(descriptor); !result) {
        return result;
    }

    {
        std::shared_lock lock(mutex_);
        if (registrations_.find(descriptor.id) != registrations_.end()) {
            return invalid_state("module is already registered");
        }
        for (const ModuleDependency& dependency : descriptor.dependencies) {
            const auto iterator = registrations_.find(dependency.module);
            if (iterator == registrations_.end() || iterator->second.descriptor.version < dependency.minimum_version) {
                return not_found("module dependency is not registered at the required version");
            }
        }
    }

    ModuleRegistrationContext context(feature_registry_, reflection_registry_, descriptor.id);
    foundation::Result registration_result;
    try {
        registration_result = module->register_module(context);
    } catch (const std::exception& exception) {
        registration_result = foundation::Result::failure(
            foundation::ResultCode::kUnknown, std::string("module registration threw: ") + exception.what());
    } catch (...) {
        registration_result = foundation::Result::failure(foundation::ResultCode::kUnknown,
                                                          "module registration threw an unknown exception");
    }
    if (!registration_result) {
        const foundation::Result rollback_result = context.rollback();
        if (!rollback_result) {
            return module_registration_failure("module registration failed and rollback was incomplete",
                                               rollback_result);
        }
        return module_registration_failure("module registration failed", registration_result);
    }

    Registration registration{};
    try {
        registration.implementation = std::shared_ptr<Module>(std::move(module));
        registration.descriptor = descriptor;
        registration.feature_definitions = context.feature_definitions_;
        registration.types = context.types_;
        std::unique_lock lock(mutex_);
        if (registrations_.find(descriptor.id) != registrations_.end()) {
            static_cast<void>(context.rollback());
            return invalid_state("module was registered concurrently");
        }
        registrations_.emplace(descriptor.id, std::move(registration));
    } catch (const std::bad_alloc&) {
        static_cast<void>(context.rollback());
        return out_of_memory("module registration ownership allocation failed");
    }
    return foundation::Result::success();
}

foundation::Result ModuleRegistry::unregister_owned(const Registration& registration) noexcept {
    return unregister_definitions(feature_registry_, reflection_registry_,
                                  registration.feature_definitions, registration.types);
}

foundation::Result ModuleRegistry::unregister_module(ModuleId id) {
    if (!id.is_valid()) {
        return invalid_argument("module id is invalid");
    }

    Registration registration{};
    {
        std::shared_lock lock(mutex_);
        const auto iterator = registrations_.find(id);
        if (iterator == registrations_.end()) {
            return not_found("module is not registered");
        }
        for (const auto& [candidate_id, candidate] : registrations_) {
            static_cast<void>(candidate_id);
            const auto dependency =
                std::find_if(candidate.descriptor.dependencies.begin(), candidate.descriptor.dependencies.end(),
                             [id](const ModuleDependency& value) { return value.module == id; });
            if (dependency != candidate.descriptor.dependencies.end()) {
                return invalid_state("module is still required by a registered dependent module");
            }
        }
        registration = iterator->second;
    }

    foundation::Result module_result;
    try {
        module_result = registration.implementation->unregister_module();
    } catch (const std::exception& exception) {
        module_result = foundation::Result::failure(foundation::ResultCode::kUnknown,
                                                    std::string("module unregistration threw: ") + exception.what());
    } catch (...) {
        module_result = foundation::Result::failure(foundation::ResultCode::kUnknown,
                                                    "module unregistration threw an unknown exception");
    }
    if (!module_result) {
        return module_registration_failure("module unregistration failed", module_result);
    }
    if (const foundation::Result result = unregister_owned(registration); !result) {
        return module_registration_failure("module owned registrations could not be removed", result);
    }

    std::unique_lock lock(mutex_);
    registrations_.erase(id);
    return foundation::Result::success();
}

bool ModuleRegistry::contains(ModuleId id) const noexcept {
    if (!id.is_valid()) {
        return false;
    }
    std::shared_lock lock(mutex_);
    return registrations_.find(id) != registrations_.end();
}

foundation::ResultValue<ModuleDescriptor> ModuleRegistry::find_module(ModuleId id) const {
    if (!id.is_valid()) {
        return {invalid_argument("module id is invalid"), {}};
    }
    std::shared_lock lock(mutex_);
    const auto iterator = registrations_.find(id);
    if (iterator == registrations_.end()) {
        return {not_found("module is not registered"), {}};
    }
    return {foundation::Result::success(), iterator->second.descriptor};
}

std::vector<ModuleDescriptor> ModuleRegistry::list_modules() const {
    std::shared_lock lock(mutex_);
    std::vector<ModuleDescriptor> result;
    result.reserve(registrations_.size());
    for (const auto& [id, registration] : registrations_)
        result.push_back(registration.descriptor);
    std::sort(result.begin(), result.end(),
              [](const ModuleDescriptor& a, const ModuleDescriptor& b) { return a.id.value() < b.id.value(); });
    return result;
}
}  // namespace aeg::module
