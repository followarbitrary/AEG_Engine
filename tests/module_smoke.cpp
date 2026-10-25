/**
 * @file module_smoke.cpp
 * @brief Exercises Feature module execution, registration transactions and dependencies.
 */
#include "feature_clock_test_fixture.hpp"
#include <iostream>
#include <memory>

#include "module.hpp"
#include "feature_runtime.hpp"

namespace {
using aeg::foundation::Result;
using aeg::module::ModuleId;
using aeg::feature::FeatureDefinitionId;

constexpr ModuleId kBaseModule("Game.Base");
constexpr ModuleId kDependentModule("Game.Dependent");
constexpr ModuleId kFailingModule("Game.Failing");
constexpr FeatureDefinitionId kBaseFeature("Game.BaseFeature");
constexpr FeatureDefinitionId kFailingFeature("Game.FailingFeature");
constexpr aeg::reflection::TypeId kBaseType("Game.BaseType");
constexpr aeg::reflection::TypeId kFailingType("Game.FailingType");
unsigned executed = 0;
unsigned unregistered = 0;
bool reached_intentional_failure = false;

template <typename Value>
bool require(const Value& condition, const char* message) {
    if (!static_cast<bool>(condition)) std::cerr << message << '\n';
    return static_cast<bool>(condition);
}

Result register_owned(aeg::module::ModuleRegistrationContext& context,
                      FeatureDefinitionId id, aeg::reflection::TypeId type) {
    using namespace aeg::feature;
    const auto reflected = context.register_type({type, "Game.Component", 1,
        aeg::reflection::TypeKind::kComponent, sizeof(uint32_t), alignof(uint32_t), {}});
    if (!reflected) return reflected;
    FeatureDefinition definition{};
    definition.id = id;
    definition.version = {1, 0, 0};
    definition.owner_module = context.module_id();
    definition.contract.definition_id = id;
    definition.contract.version = definition.version;
    definition.contract.domains = to_mask(FeatureDomain::kLogic);
    definition.contract.execution = ExecutionKind::kCall;
    definition.contract.scope = {ScopeId("World")};
    definition.contract.clock = aeg::feature::make_periodic_clock_contract(ClockId("Logic"), 60, 1, LateDataPolicy::kReject,
        aeg::feature::DeadlinePolicy::kObserve, definition.contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    definition.contract.permissions.version = 1;
    definition.execute = [](const InvocationToken& token, const StateSnapshot&,
                            std::span<const uint8_t>, std::span<const uint8_t>) {
        ++executed;
        return aeg::foundation::ResultValue<StateDelta>{Result::success(), StateDelta{token.state_version}};
    };
    return context.register_feature(std::move(definition));
}

class BaseModule final : public aeg::module::Module {
public:
    const aeg::module::ModuleDescriptor& descriptor() const noexcept override { return descriptor_; }
    Result register_module(aeg::module::ModuleRegistrationContext& context) override {
        return register_owned(context, kBaseFeature, kBaseType);
    }
    Result unregister_module() override { ++unregistered; return Result::success(); }
private:
    aeg::module::ModuleDescriptor descriptor_{kBaseModule, "Game.Base", {1, 0, 0}, {}};
};

class DependentModule final : public aeg::module::Module {
public:
    const aeg::module::ModuleDescriptor& descriptor() const noexcept override { return descriptor_; }
    Result register_module(aeg::module::ModuleRegistrationContext&) override { return Result::success(); }
    Result unregister_module() override { return Result::success(); }
private:
    aeg::module::ModuleDescriptor descriptor_{
        kDependentModule, "Game.Dependent", {1, 0, 0}, {{kBaseModule, {1, 0, 0}}}};
};

class FailingModule final : public aeg::module::Module {
public:
    const aeg::module::ModuleDescriptor& descriptor() const noexcept override { return descriptor_; }
    Result register_module(aeg::module::ModuleRegistrationContext& context) override {
        const auto registered = register_owned(context, kFailingFeature, kFailingType);
        if (!registered) return registered;
        reached_intentional_failure = true;
        return Result::failure(aeg::foundation::ResultCode::kUnknown, "intentional module failure");
    }
    Result unregister_module() override { return Result::success(); }
private:
    aeg::module::ModuleDescriptor descriptor_{kFailingModule, "Game.Failing", {1, 0, 0}, {}};
};
} // namespace

int main() {
    aeg::feature::FeatureRegistry features{aeg::feature::test::clock_test_host()};
    aeg::reflection::TypeRegistry types;
    aeg::module::ModuleRegistry modules(features, &types);
    if (!require(!modules.register_module([] { return std::make_unique<DependentModule>(); }),
                 "module dependency failure was not reported") ||
        !require(modules.register_module([] { return std::make_unique<BaseModule>(); }),
                 "base module registration failed") ||
        !require(modules.contains(kBaseModule), "registered module cannot be found") ||
        !require(features.find_definition(kBaseFeature) != nullptr, "module Feature was not registered") ||
        !require(features.find_definition(kBaseFeature)->owner_module == kBaseModule,
                 "module Feature lost its ownership") ||
        !require(types.contains(kBaseType), "module type was not registered")) return 1;

    {
        aeg::feature::FeatureRuntime runtime(features);
        const auto expanded = features.expand(kBaseFeature, {});
        if (!require(expanded, "module Feature expansion failed")) return 1;
        const auto instance = runtime.activate(expanded.value, aeg::feature::ScopeId("World"));
        if (!require(instance, "module Feature activation failed") ||
            !require(runtime.dispatch_call(instance.value), "module Feature callback failed") ||
            !require(executed == 1 && runtime.find_instance(instance.value)->state.commit_count == 1,
                     "module Feature did not execute and commit through FeatureRuntime")) return 1;
    }

    if (!require(modules.register_module([] { return std::make_unique<DependentModule>(); }),
                 "dependent module registration failed") ||
        !require(modules.list_modules().size() == 2 && modules.list_modules()[0].id < modules.list_modules()[1].id,
                 "module metadata snapshot is not stably ordered") ||
        !require(!modules.unregister_module(kBaseModule) && unregistered == 0,
                 "dependent module did not block base lifecycle and unload") ||
        !require(modules.unregister_module(kDependentModule), "dependent module unload failed") ||
        !require(modules.unregister_module(kBaseModule) && unregistered == 1, "base module lifecycle unload failed") ||
        !require(!features.find_definition(kBaseFeature), "module Feature survived unload") ||
        !require(!types.contains(kBaseType), "module type survived unload")) return 1;

    const auto failed = modules.register_module([] { return std::make_unique<FailingModule>(); });
    if (!require(!failed && failed.code == aeg::foundation::ResultCode::kUnknown && reached_intentional_failure,
                 "failing module did not reach its deliberate startup failure") ||
        !require(!modules.contains(kFailingModule), "failed module retained its registration") ||
        !require(!features.find_definition(kFailingFeature), "failed module Feature registration was not rolled back") ||
        !require(!types.contains(kFailingType), "failed module type registration was not rolled back")) return 1;
    std::cout << "Feature module execution, lifecycle, rollback and dependency smoke passed\n";
    return 0;
}
