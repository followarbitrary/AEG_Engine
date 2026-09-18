/**
 * @file module.cpp
 * @brief Implements module registration transactions and unload ordering.
 */
#include "../public/module.hpp"

#include <algorithm>
#include <mutex>
#include <new>
#include <string>

#include "../../Pipeline/public/render_pass.hpp"

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
}  // namespace

foundation::Result ModuleRegistrationContext::register_phase(const pipeline::PhaseDescriptor& descriptor) {
    if (descriptor.id.is_valid() == false) {
        return invalid_argument("module phase id is invalid");
    }
    const foundation::Result result = pipeline_registry_.phase_registry().register_phase(descriptor);
    if (!result) {
        return result;
    }
    if (const foundation::Result record_result =
            push_id(phases_, descriptor.id, "module phase ownership allocation failed");
        !record_result) {
        static_cast<void>(pipeline_registry_.phase_registry().unregister_phase(descriptor.id));
        return record_result;
    }
    return foundation::Result::success();
}

foundation::Result ModuleRegistrationContext::register_pipeline(pipeline::PipelineFactory factory) {
    if (!factory) {
        return invalid_argument("module pipeline factory is empty");
    }

    std::unique_ptr<pipeline::Pipeline> preview;
    try {
        preview = factory();
    } catch (const std::exception& exception) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                           std::string("module pipeline factory threw: ") + exception.what());
    } catch (...) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                           "module pipeline factory threw an unknown exception");
    }
    if (!preview) {
        return invalid_argument("module pipeline factory returned null");
    }
    const pipeline::PipelineDescriptor descriptor = preview->descriptor();
    if (descriptor.owner_module != module_id_) {
        return invalid_argument("pipeline owner module does not match registration context");
    }

    const auto preview_holder = std::make_shared<std::unique_ptr<pipeline::Pipeline>>(std::move(preview));
    const auto factory_holder = std::make_shared<pipeline::PipelineFactory>(std::move(factory));
    const foundation::Result result = pipeline_registry_.register_pipeline(
        [preview_holder, factory_holder]() mutable -> std::unique_ptr<pipeline::Pipeline> {
            if (*preview_holder != nullptr) {
                return std::move(*preview_holder);
            }
            return (*factory_holder)();
        });
    if (!result) {
        return result;
    }
    if (const foundation::Result record_result =
            push_id(pipelines_, descriptor.id, "module pipeline ownership allocation failed");
        !record_result) {
        static_cast<void>(pipeline_registry_.unregister_pipeline(descriptor.id));
        return record_result;
    }
    return foundation::Result::success();
}

foundation::Result ModuleRegistrationContext::register_feature(pipeline::FeatureFactory factory) {
    if (!factory) {
        return invalid_argument("module feature factory is empty");
    }

    std::unique_ptr<pipeline::PipelineFeature> preview;
    try {
        preview = factory();
    } catch (const std::exception& exception) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                           std::string("module feature factory threw: ") + exception.what());
    } catch (...) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                           "module feature factory threw an unknown exception");
    }
    if (!preview) {
        return invalid_argument("module feature factory returned null");
    }
    const pipeline::FeatureDescriptor descriptor = preview->descriptor();
    if (descriptor.owner_module != module_id_) {
        return invalid_argument("feature owner module does not match registration context");
    }

    const auto preview_holder = std::make_shared<std::unique_ptr<pipeline::PipelineFeature>>(std::move(preview));
    const auto factory_holder = std::make_shared<pipeline::FeatureFactory>(std::move(factory));
    const foundation::Result result = pipeline_registry_.register_feature(
        [preview_holder, factory_holder]() mutable -> std::unique_ptr<pipeline::PipelineFeature> {
            if (*preview_holder != nullptr) {
                return std::move(*preview_holder);
            }
            return (*factory_holder)();
        });
    if (!result) {
        return result;
    }
    if (const foundation::Result record_result =
            push_id(features_, descriptor.id, "module feature ownership allocation failed");
        !record_result) {
        static_cast<void>(pipeline_registry_.unregister_feature(descriptor.id));
        return record_result;
    }
    return foundation::Result::success();
}

foundation::Result ModuleRegistrationContext::register_render_pass(pipeline::RenderPassFactory factory) {
    if (render_pass_registry_ == nullptr) {
        return unsupported("module registry has no render pass registry");
    }
    if (!factory) {
        return invalid_argument("module render pass factory is empty");
    }
    std::unique_ptr<pipeline::RenderPass> preview;
    try {
        preview = factory();
    } catch (const std::exception& exception) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                           std::string("module render pass factory threw: ") + exception.what());
    } catch (...) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                           "module render pass factory threw an unknown exception");
    }
    if (!preview) {
        return invalid_argument("module render pass factory returned null");
    }
    const auto descriptor = preview->descriptor();
    if (descriptor.owner_module != module_id_) {
        return invalid_argument("render pass owner module does not match registration context");
    }
    const auto preview_holder = std::make_shared<std::unique_ptr<pipeline::RenderPass>>(std::move(preview));
    const auto factory_holder = std::make_shared<pipeline::RenderPassFactory>(std::move(factory));
    const foundation::Result result = render_pass_registry_->register_pass(
        [preview_holder, factory_holder]() mutable -> std::unique_ptr<pipeline::RenderPass> {
            if (*preview_holder != nullptr) {
                return std::move(*preview_holder);
            }
            return (*factory_holder)();
        });
    if (!result) {
        return result;
    }
    if (const foundation::Result record_result =
            push_id(render_passes_, descriptor.type, "module render pass ownership allocation failed");
        !record_result) {
        static_cast<void>(render_pass_registry_->unregister_pass(descriptor.type));
        return record_result;
    }
    return foundation::Result::success();
}

foundation::Result ModuleRegistrationContext::bind_feature(const pipeline::FeatureBindingDescriptor& binding) {
    pipeline::FeatureBindingDescriptor normalized = binding;
    if (!normalized.instance.is_valid()) {
        normalized.instance = pipeline::make_feature_instance_id(normalized.feature, normalized.pipeline,
                                                                 normalized.scope, normalized.point);
    }
    const foundation::Result result = pipeline_registry_.bind_feature(normalized);
    if (!result) {
        return result;
    }
    if (const foundation::Result record_result =
            push_id(feature_bindings_, normalized.instance, "module feature binding allocation failed");
        !record_result) {
        static_cast<void>(pipeline_registry_.unbind_feature(normalized.instance));
        return record_result;
    }
    return foundation::Result::success();
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
    foundation::Result first_error = foundation::Result::success();
    for (auto iterator = feature_bindings_.rbegin(); iterator != feature_bindings_.rend(); ++iterator) {
        const foundation::Result result = pipeline_registry_.unbind_feature(*iterator);
        if (!result && first_error) {
            first_error = result;
        }
    }
    for (auto iterator = pipelines_.rbegin(); iterator != pipelines_.rend(); ++iterator) {
        const foundation::Result result = pipeline_registry_.unregister_pipeline(*iterator);
        if (!result && first_error) {
            first_error = result;
        }
    }
    for (auto iterator = features_.rbegin(); iterator != features_.rend(); ++iterator) {
        const foundation::Result result = pipeline_registry_.unregister_feature(*iterator);
        if (!result && first_error) {
            first_error = result;
        }
    }
    if (render_pass_registry_ != nullptr) {
        for (auto iterator = render_passes_.rbegin(); iterator != render_passes_.rend(); ++iterator) {
            const foundation::Result result = render_pass_registry_->unregister_pass(*iterator);
            if (!result && first_error) {
                first_error = result;
            }
        }
    }
    for (auto iterator = phases_.rbegin(); iterator != phases_.rend(); ++iterator) {
        const foundation::Result result = pipeline_registry_.phase_registry().unregister_phase(*iterator);
        if (!result && first_error) {
            first_error = result;
        }
    }
    if (reflection_registry_ != nullptr) {
        for (auto iterator = types_.rbegin(); iterator != types_.rend(); ++iterator) {
            const foundation::Result result = reflection_registry_->unregister_type(*iterator);
            if (!result && first_error) {
                first_error = result;
            }
        }
    }
    return first_error;
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

    ModuleRegistrationContext context(pipeline_registry_, reflection_registry_, render_pass_registry_, descriptor.id);
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
        registration.phases = context.phases_;
        registration.pipelines = context.pipelines_;
        registration.features = context.features_;
        registration.render_passes = context.render_passes_;
        registration.feature_bindings = context.feature_bindings_;
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
    foundation::Result first_error = foundation::Result::success();
    for (auto iterator = registration.feature_bindings.rbegin(); iterator != registration.feature_bindings.rend();
         ++iterator) {
        const foundation::Result result = pipeline_registry_.unbind_feature(*iterator);
        if (!result && first_error) {
            first_error = result;
        }
    }
    for (auto iterator = registration.pipelines.rbegin(); iterator != registration.pipelines.rend(); ++iterator) {
        const foundation::Result result = pipeline_registry_.unregister_pipeline(*iterator);
        if (!result && first_error) {
            first_error = result;
        }
    }
    for (auto iterator = registration.features.rbegin(); iterator != registration.features.rend(); ++iterator) {
        const foundation::Result result = pipeline_registry_.unregister_feature(*iterator);
        if (!result && first_error) {
            first_error = result;
        }
    }
    if (render_pass_registry_ != nullptr) {
        for (auto iterator = registration.render_passes.rbegin(); iterator != registration.render_passes.rend();
             ++iterator) {
            const foundation::Result result = render_pass_registry_->unregister_pass(*iterator);
            if (!result && first_error) {
                first_error = result;
            }
        }
    }
    for (auto iterator = registration.phases.rbegin(); iterator != registration.phases.rend(); ++iterator) {
        const foundation::Result result = pipeline_registry_.phase_registry().unregister_phase(*iterator);
        if (!result && first_error) {
            first_error = result;
        }
    }
    if (reflection_registry_ != nullptr) {
        for (auto iterator = registration.types.rbegin(); iterator != registration.types.rend(); ++iterator) {
            const foundation::Result result = reflection_registry_->unregister_type(*iterator);
            if (!result && first_error) {
                first_error = result;
            }
        }
    }
    return first_error;
}

foundation::Result ModuleRegistry::unregister_module(pipeline::ModuleId id) {
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

bool ModuleRegistry::contains(pipeline::ModuleId id) const noexcept {
    if (!id.is_valid()) {
        return false;
    }
    std::shared_lock lock(mutex_);
    return registrations_.find(id) != registrations_.end();
}

foundation::ResultValue<ModuleDescriptor> ModuleRegistry::find_module(pipeline::ModuleId id) const {
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
