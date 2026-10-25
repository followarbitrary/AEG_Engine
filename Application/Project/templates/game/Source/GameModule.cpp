#include <span>
#include <utility>

#include "GameModule.hpp"
#include "feature_contract.hpp"
#include "module_export.hpp"

namespace {
using namespace aeg::feature;

FeatureDefinition make_render_feature() {
    FeatureDefinition definition{};
    definition.id = FeatureDefinitionId("Game.Render.Outline");
    definition.version = {1, 0, 0};
    definition.contract.definition_id = definition.id;
    definition.contract.version = definition.version;
    definition.contract.structure = FeatureStructure::kAtomic;
    definition.contract.domains = to_mask(FeatureDomain::kRender);
    definition.contract.execution = ExecutionKind::kContinuous;
    definition.contract.state = StatePolicy::kInstanceState;
    definition.contract.scope = ScopeContract{ScopeId("Frame")};
    definition.contract.clock = aeg::feature::make_periodic_clock_contract(ClockId("RenderFrame"), 60, 2, LateDataPolicy::kUseLatest,
        aeg::feature::DeadlinePolicy::kObserve, definition.contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    definition.contract.permissions.version = 1;
    definition.contract.ports = {
        {PortId("SceneColor"), PortDirection::kInput, ValueSchemaId("Texture.v1"), 1},
        {PortId("ColorOut"), PortDirection::kOutput, ValueSchemaId("Texture.v1"), 1},
    };
    definition.execute = [](const InvocationToken&, const StateSnapshot&, std::span<const uint8_t>, std::span<const uint8_t>)
        -> aeg::foundation::ResultValue<StateDelta> {
        return {aeg::foundation::Result::success(), StateDelta{}};
    };
    return definition;
}

FeatureDefinition make_logic_process() {
    FeatureDefinition definition{};
    definition.id = FeatureDefinitionId("Game.Logic.Gameplay");
    definition.version = {1, 0, 0};
    definition.contract.definition_id = definition.id;
    definition.contract.version = definition.version;
    definition.contract.structure = FeatureStructure::kAtomic;
    definition.contract.domains = to_mask(FeatureDomain::kLogic);
    definition.contract.execution = ExecutionKind::kProcess;
    definition.contract.state = StatePolicy::kProcessState;
    definition.contract.scope = ScopeContract{ScopeId("World")};
    definition.contract.clock = aeg::feature::make_periodic_clock_contract(ClockId("LogicTick"), 60, 8, LateDataPolicy::kQueue,
        aeg::feature::DeadlinePolicy::kObserve, definition.contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    definition.contract.permissions.version = 1;
    definition.contract.has_continuation = true;
    definition.execute = [](const InvocationToken&, const StateSnapshot& state, std::span<const uint8_t>, std::span<const uint8_t>)
        -> aeg::foundation::ResultValue<StateDelta> {
        // The starter FeatureGraph declares these two transitions. After a real
        // Build/Load/Play, Runtime commits one transition per Logic invocation
        // and completes the Process when it reaches the authored terminal Done.
        StateDelta delta;
        if (state.current_state == "Ready") delta.next_state = "Running";
        else if (state.current_state == "Running") delta.next_state = "Done";
        return {aeg::foundation::Result::success(), std::move(delta)};
    };
    return definition;
}
}  // namespace

aeg::module::FeatureModuleDefinition aeg_define_GameModule() {
    aeg::module::FeatureModuleDefinition definition{};
    definition.descriptor.id = aeg::module::ModuleId("GameModule");
    definition.descriptor.name = "Game Module";
    definition.descriptor.version = {1, 0, 0};
    if (auto result = AEG_REGISTER_GAME_FEATURE(definition, make_render_feature()); !result)
        return definition;
    if (auto result = AEG_REGISTER_GAME_FEATURE(definition, make_logic_process()); !result)
        return definition;
    return definition;
}

#if !defined(AEG_FEATURE_CONTRACT_EXPORTER)
AEG_EXPORT_GAME_MODULE_API(aeg_define_GameModule);
#endif
