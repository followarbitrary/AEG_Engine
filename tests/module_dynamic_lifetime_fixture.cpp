#include "module_export.hpp"
#include "module_dynamic_feature_fixture.hpp"
#include "module_dynamic_feature_probe.h"
#ifndef AEG_DYNAMIC_FIXTURE_REVISION
#define AEG_DYNAMIC_FIXTURE_REVISION 1
#endif
namespace {
AegDynamicFeatureProbe probe{};
aeg::module::FeatureModuleDefinition fixture_definition() {
    if (probe.factory_entered) probe.factory_entered(probe.user_data);
    auto result = dynamic_feature_test::definition(AEG_DYNAMIC_FIXTURE_REVISION);
    for (auto& item : result.feature_definitions) {
        item.execute = [callback = std::move(item.execute), id = item.id.value()](const auto& token, const auto& state,
                        std::span<const uint8_t> input, std::span<const uint8_t> parameters) {
            if (probe.execute_entered) probe.execute_entered(probe.user_data, id);
            return callback(token, state, input, parameters);
        };
        item.validate_state_payload = [callback = std::move(item.validate_state_payload), id = item.id.value()](
                                      std::span<const uint8_t> payload) {
            if (probe.state_validated) probe.state_validated(probe.user_data, id);
            return callback(payload);
        };
    }
    return result;
}
}
extern "C" AEG_MODULE_EXPORT void AEG_ABI_CALL aeg_dynamic_test_bind_probe(AegDynamicFeatureProbe value) { probe = value; }
AEG_EXPORT_GAME_MODULE_API(fixture_definition);
