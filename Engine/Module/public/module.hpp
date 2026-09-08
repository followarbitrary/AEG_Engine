/**
 * @file module.hpp
 * @brief Static module lifecycle and registration ownership.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "pipeline.hpp"
#include "reflection.hpp"
#include "result.hpp"

namespace aeg::module {

/** @brief Minimum version required from one dependent module. */
struct ModuleDependency final {
    pipeline::ModuleId module{};
    pipeline::Version minimum_version{1, 0, 0};
};

/** @brief Immutable identity and dependency metadata for one engine module. */
struct ModuleDescriptor final {
    pipeline::ModuleId id{};
    std::string name{};
    pipeline::Version version{1, 0, 0};
    std::vector<ModuleDependency> dependencies{};
};

class Module;
class ModuleRegistry;

/** @brief Restricted registration surface exposed to one module during startup. */
class ModuleRegistrationContext final {
public:
    [[nodiscard]] pipeline::ModuleId module_id() const noexcept { return module_id_; }
    [[nodiscard]] pipeline::PipelineRegistry& pipeline_registry() const noexcept { return pipeline_registry_; }
    [[nodiscard]] reflection::TypeRegistry* reflection_registry() const noexcept { return reflection_registry_; }

    /** @brief Registers a custom global Phase owned by the current module. */
    [[nodiscard]] foundation::Result register_phase(const pipeline::PhaseDescriptor& descriptor);
    /** @brief Registers a Pipeline definition owned by the current module. */
    [[nodiscard]] foundation::Result register_pipeline(pipeline::PipelineFactory factory);
    /** @brief Registers a Feature definition owned by the current module. */
    [[nodiscard]] foundation::Result register_feature(pipeline::FeatureFactory factory);
    /** @brief Binds a Feature to a Pipeline extension point under module ownership. */
    [[nodiscard]] foundation::Result bind_feature(const pipeline::FeatureBindingDescriptor& binding);
    /** @brief Registers reflected type metadata and records it for module unload. */
    [[nodiscard]] foundation::Result register_type(reflection::TypeDescriptor descriptor);

private:
    ModuleRegistrationContext(pipeline::PipelineRegistry& pipeline_registry,
                              reflection::TypeRegistry* reflection_registry, pipeline::ModuleId module_id) noexcept
        : pipeline_registry_(pipeline_registry), reflection_registry_(reflection_registry), module_id_(module_id) {}

    [[nodiscard]] foundation::Result rollback() noexcept;

    pipeline::PipelineRegistry& pipeline_registry_;
    reflection::TypeRegistry* reflection_registry_{nullptr};
    pipeline::ModuleId module_id_{};
    std::vector<pipeline::PhaseId> phases_{};
    std::vector<pipeline::PipelineId> pipelines_{};
    std::vector<pipeline::FeatureId> features_{};
    std::vector<pipeline::FeatureInstanceId> feature_bindings_{};
    std::vector<reflection::TypeId> types_{};

    friend class ModuleRegistry;
};

/** @brief C++ module implementation used by static game and engine modules. */
class Module {
public:
    virtual ~Module() = default;

    [[nodiscard]] virtual const ModuleDescriptor& descriptor() const noexcept = 0;
    [[nodiscard]] virtual foundation::Result register_module(ModuleRegistrationContext& context) = 0;
    [[nodiscard]] virtual foundation::Result unregister_module() = 0;
};

using ModuleFactory = std::function<std::unique_ptr<Module>()>;

/** @brief Registry that owns module implementations and their registered definitions. */
class ModuleRegistry final {
public:
    explicit ModuleRegistry(pipeline::PipelineRegistry& pipeline_registry,
                            reflection::TypeRegistry* reflection_registry = nullptr) noexcept
        : pipeline_registry_(pipeline_registry), reflection_registry_(reflection_registry) {}
    ~ModuleRegistry() = default;

    ModuleRegistry(const ModuleRegistry&) = delete;
    ModuleRegistry& operator=(const ModuleRegistry&) = delete;
    ModuleRegistry(ModuleRegistry&&) = delete;
    ModuleRegistry& operator=(ModuleRegistry&&) = delete;

    /** @brief Constructs, validates, registers, and retains one module. */
    [[nodiscard]] foundation::Result register_module(ModuleFactory factory);
    /** @brief Safely unregisters one module and all definitions owned by it. */
    [[nodiscard]] foundation::Result unregister_module(pipeline::ModuleId id);
    /** @brief Tests whether a module is currently registered. */
    [[nodiscard]] bool contains(pipeline::ModuleId id) const noexcept;
    /** @brief Returns immutable metadata for one registered module. */
    [[nodiscard]] foundation::ResultValue<ModuleDescriptor> find_module(pipeline::ModuleId id) const;

private:
    struct Registration final {
        std::shared_ptr<Module> implementation{};
        ModuleDescriptor descriptor{};
        std::vector<pipeline::PhaseId> phases{};
        std::vector<pipeline::PipelineId> pipelines{};
        std::vector<pipeline::FeatureId> features{};
        std::vector<pipeline::FeatureInstanceId> feature_bindings{};
        std::vector<reflection::TypeId> types{};
    };

    [[nodiscard]] foundation::Result validate_descriptor(const ModuleDescriptor& descriptor) const;
    [[nodiscard]] foundation::Result unregister_owned(const Registration& registration) noexcept;

    pipeline::PipelineRegistry& pipeline_registry_;
    reflection::TypeRegistry* reflection_registry_{nullptr};
    mutable std::shared_mutex mutex_;
    std::unordered_map<pipeline::ModuleId, Registration, pipeline::StableIdHash<pipeline::ModuleIdTag>>
        registrations_{};
};
}  // namespace aeg::module
