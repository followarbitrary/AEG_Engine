#include "feature_clock_test_fixture.hpp"
#include <cassert>
#include <iostream>
#include <type_traits>

#include "feature_registry.hpp"
#include "feature_module.hpp"
#include "module.hpp"

namespace aeg::pipeline { class PipelineRegistry; }

namespace {
template <typename Context>
constexpr bool exposes_legacy_registration =
    requires { &Context::pipeline_registry; } ||
    requires { &Context::render_pass_registry; } ||
    requires { &Context::register_phase; } ||
    requires { &Context::register_pipeline; } ||
    requires { &Context::register_pipeline_feature; } ||
    requires { &Context::register_render_pass; } ||
    requires { &Context::bind_feature; };
}

int main() {
    using namespace aeg;

    if (exposes_legacy_registration<module::ModuleRegistrationContext> ||
        !std::is_constructible_v<module::ModuleRegistry, feature::FeatureRegistry&,
                                 reflection::TypeRegistry*> ||
        std::is_constructible_v<module::ModuleRegistry, pipeline::PipelineRegistry&,
                                feature::FeatureRegistry*>) {
        std::cerr << "module registration still requires or exposes the legacy Pipeline/Pass hierarchy\n";
        return 1;
    }

    module::FeatureModuleDefinition definition{};
    definition.descriptor.id = module::ModuleId("GameModule");
    definition.descriptor.name = "Game Module";
    definition.descriptor.version = {1, 0, 0};

    feature::FeatureDefinition feature_definition{};
    feature_definition.id = feature::FeatureDefinitionId("Game.Logic.Tick");
    feature_definition.version = {1, 0, 0};
    feature_definition.contract.definition_id = feature_definition.id;
    feature_definition.contract.version = feature_definition.version;
    feature_definition.contract.domains = feature::to_mask(feature::FeatureDomain::kLogic);
    feature_definition.contract.execution = feature::ExecutionKind::kProcess;
    feature_definition.contract.state = feature::StatePolicy::kProcessState;
    feature_definition.contract.scope = feature::ScopeContract{feature::ScopeId("World")};
    feature_definition.contract.clock = aeg::feature::make_periodic_clock_contract(feature::ClockId("LogicTick"), 60, 4, feature::LateDataPolicy::kReject,
        aeg::feature::DeadlinePolicy::kObserve, feature_definition.contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    feature_definition.contract.permissions.version = 1;
    feature_definition.contract.has_continuation = true;
    assert(module::add_feature_definition(definition, std::move(feature_definition)));

    feature::FeatureRegistry feature_registry{aeg::feature::test::clock_test_host()};
    module::ModuleRegistry modules(feature_registry);
    assert(module::register_static_module(modules, std::move(definition)));
    assert(feature_registry.find_definition(feature::FeatureDefinitionId("Game.Logic.Tick")) != nullptr);
    assert(modules.unregister_module(module::ModuleId("GameModule")));
    assert(feature_registry.find_definition(feature::FeatureDefinitionId("Game.Logic.Tick")) == nullptr);
    std::cout << "module feature registration smoke passed\n";
    return 0;
}
