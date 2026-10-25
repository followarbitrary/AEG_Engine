#include "feature_clock_test_fixture.hpp"
#include <cassert>
#include <iostream>
#include <span>
#include <utility>

#include "feature_module.hpp"
#include "feature_registry.hpp"
#include "module.hpp"

namespace {
template <typename Type>
concept HasLegacyPipelineMembers = requires(Type value) {
    value.pipelines;
    value.passes;
};

aeg::feature::FeatureDefinition make_feature() {
    aeg::feature::FeatureDefinition definition{};
    definition.id = aeg::feature::FeatureDefinitionId("Smoke.Game.Feature");
    definition.version = {1, 0, 0};
    definition.contract.definition_id = definition.id;
    definition.contract.version = definition.version;
    definition.contract.domains = aeg::feature::to_mask(aeg::feature::FeatureDomain::kLogic);
    definition.contract.execution = aeg::feature::ExecutionKind::kCall;
    definition.contract.scope = aeg::feature::ScopeContract{aeg::feature::ScopeId("World")};
    definition.contract.clock = aeg::feature::make_periodic_clock_contract(aeg::feature::ClockId("Logic"), 60, 1, aeg::feature::LateDataPolicy::kReject,
        aeg::feature::DeadlinePolicy::kObserve, definition.contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    definition.contract.permissions.version = 1;
    definition.execute = [](const aeg::feature::InvocationToken&, const aeg::feature::StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>)
        -> aeg::foundation::ResultValue<aeg::feature::StateDelta> {
        return {aeg::foundation::Result::success(), {}};
    };
    return definition;
}
}  // namespace

int main() {
    using namespace aeg;
    static_assert(!HasLegacyPipelineMembers<module::FeatureModuleDefinition>);
    module::FeatureModuleDefinition definition{};
    definition.descriptor.id = module::ModuleId("Smoke.FeatureOnly");
    definition.descriptor.name = "Feature-only smoke module";
    assert(module::add_feature_definition(definition, make_feature()));
    assert(definition.feature_definitions.size() == 1);
    assert(definition.feature_catalog.entries().size() == 1);
    assert(definition.feature_definitions.front().owner_module == definition.descriptor.id);
    assert(definition.feature_catalog.entries().front().owner_module == definition.descriptor.id);

    feature::FeatureRegistry replacement_registry{aeg::feature::test::clock_test_host()};
    auto owned_definition = make_feature();
    owned_definition.owner_module = module::ModuleId("Smoke.FeatureOnly");
    assert(aeg::feature::test::register_clock_test_definition(replacement_registry, owned_definition));
    auto mismatched_replacement = owned_definition;
    mismatched_replacement.owner_module = module::ModuleId("Smoke.OtherOwner");
    assert(!aeg::feature::test::replace_clock_test_definition(replacement_registry, std::move(mismatched_replacement)));
    auto ownerless_replacement = owned_definition;
    ownerless_replacement.owner_module = {};
    assert(aeg::feature::test::replace_clock_test_definition(replacement_registry, std::move(ownerless_replacement)));
    assert(replacement_registry.find_definition(owned_definition.id)->owner_module ==
           module::ModuleId("Smoke.FeatureOnly"));

    module::FeatureModuleDefinition owner_mismatch{};
    owner_mismatch.descriptor.id = module::ModuleId("Smoke.OwnerMismatch");
    auto owned_by_other_module = make_feature();
    owned_by_other_module.owner_module = module::ModuleId("Smoke.OtherModule");
    assert(!module::add_feature_definition(owner_mismatch, std::move(owned_by_other_module)));

    feature::FeatureRegistry feature_registry{aeg::feature::test::clock_test_host()};
    module::ModuleRegistry modules(feature_registry);
    module::FeatureModuleDefinition invalid_definition{};
    invalid_definition.descriptor.id = module::ModuleId("Smoke.InvalidFeatureOnly");
    assert(module::add_feature_definition(invalid_definition, make_feature()));
    invalid_definition.feature_definitions.pop_back();
    assert(!module::register_static_module(modules, std::move(invalid_definition)));

    module::FeatureModuleDefinition unpaired_definition{};
    unpaired_definition.descriptor.id = module::ModuleId("Smoke.UnpairedFeatureOnly");
    assert(module::add_feature_definition(unpaired_definition, make_feature()));
    auto replacement = make_feature();
    replacement.id = feature::FeatureDefinitionId("Smoke.UnpairedReplacement");
    replacement.contract.definition_id = replacement.id;
    unpaired_definition.feature_definitions.front() = replacement;
    assert(!module::register_static_module(modules, std::move(unpaired_definition)));

    assert(module::register_static_module(modules, std::move(definition)));
    assert(feature_registry.find_definition(feature::FeatureDefinitionId("Smoke.Game.Feature")) != nullptr);
    assert(modules.unregister_module(module::ModuleId("Smoke.FeatureOnly")));
    std::cout << "feature-only module authoring smoke passed\n";
    return 0;
}
