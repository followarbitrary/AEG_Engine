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
#include <utility>
#include <vector>

#include "feature_registry.hpp"
#include "module_descriptor.hpp"
#include "reflection.hpp"
#include "result.hpp"

namespace aeg::module {

class Module;
class ModuleRegistry;

/** @brief Restricted registration surface exposed to one module during startup. */
class ModuleRegistrationContext final {
public:
    [[nodiscard]] ModuleId module_id() const noexcept { return module_id_; }
    [[nodiscard]] reflection::TypeRegistry* reflection_registry() const noexcept { return reflection_registry_; }

    /** @brief Registers a five-domain Feature contract and its module-owned implementation. */
    [[nodiscard]] foundation::Result register_feature(feature::FeatureDefinition definition);
    /** @brief Registers reflected type metadata and records it for module unload. */
    [[nodiscard]] foundation::Result register_type(reflection::TypeDescriptor descriptor);

private:
    ModuleRegistrationContext(feature::FeatureRegistry& feature_registry,
                              reflection::TypeRegistry* reflection_registry, ModuleId module_id) noexcept
        : feature_registry_(feature_registry),
          reflection_registry_(reflection_registry),
          module_id_(module_id) {}

    [[nodiscard]] foundation::Result rollback() noexcept;

    feature::FeatureRegistry& feature_registry_;
    reflection::TypeRegistry* reflection_registry_{nullptr};
    ModuleId module_id_{};
    std::vector<feature::FeatureDefinitionId> feature_definitions_{};
    std::vector<reflection::TypeId> types_{};
    // Only bindings first approved during this registration transaction.
    std::vector<std::pair<feature::ClockId, feature::Version>> new_clock_bindings_{};

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
    explicit ModuleRegistry(feature::FeatureRegistry& feature_registry,
                            reflection::TypeRegistry* reflection_registry = nullptr) noexcept
        : feature_registry_(feature_registry),
          reflection_registry_(reflection_registry) {}
    ~ModuleRegistry() = default;

    ModuleRegistry(const ModuleRegistry&) = delete;
    ModuleRegistry& operator=(const ModuleRegistry&) = delete;
    ModuleRegistry(ModuleRegistry&&) = delete;
    ModuleRegistry& operator=(ModuleRegistry&&) = delete;

    /** @brief Constructs, validates, registers, and retains one module. */
    [[nodiscard]] foundation::Result register_module(ModuleFactory factory);
    /** @brief Safely unregisters one module and all definitions owned by it. */
    [[nodiscard]] foundation::Result unregister_module(ModuleId id);
    /** @brief Tests whether a module is currently registered. */
    [[nodiscard]] bool contains(ModuleId id) const noexcept;
    /** @brief Returns immutable metadata for one registered module. */
    [[nodiscard]] foundation::ResultValue<ModuleDescriptor> find_module(ModuleId id) const;
    /** @brief Returns a stable snapshot of registered module metadata for tooling and diagnostics. */
    [[nodiscard]] std::vector<ModuleDescriptor> list_modules() const;

private:
    struct Registration final {
        std::shared_ptr<Module> implementation{};
        ModuleDescriptor descriptor{};
        std::vector<feature::FeatureDefinitionId> feature_definitions{};
        std::vector<reflection::TypeId> types{};
    };

    [[nodiscard]] foundation::Result validate_descriptor(const ModuleDescriptor& descriptor) const;
    [[nodiscard]] foundation::Result unregister_owned(const Registration& registration) noexcept;

    feature::FeatureRegistry& feature_registry_;
    reflection::TypeRegistry* reflection_registry_{nullptr};
    mutable std::shared_mutex mutex_;
    std::unordered_map<ModuleId, Registration, ModuleIdHash> registrations_{};
};
}  // namespace aeg::module
