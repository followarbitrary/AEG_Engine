#include "FeatureDefinition.hpp"
#include "Revision.hpp"

#include <cstdlib>
#include <fstream>
#include <string>
#include <utility>

namespace exporter_fixture {
aeg::feature::FeatureDefinition make_feature() {
    using namespace aeg::feature;
    FeatureDefinition definition;
    definition.id = FeatureDefinitionId{"Exporter.Fixture.Feature"};
    definition.version = {kRevision, 0, 0};
    auto& contract = definition.contract;
    contract.definition_id = definition.id;
    contract.version = definition.version;
    contract.domains = to_mask(FeatureDomain::kLogic);
    contract.execution = ExecutionKind::kCall;
    contract.state = StatePolicy::kInstanceState;
    contract.scope = {ScopeId{"World"}};
    contract.clock = make_periodic_clock_contract(ClockId{"Exporter.Fixture.Clock"}, 100, 4,
        LateDataPolicy::kReject, DeadlinePolicy::kObserve, 250'000, 0);
    contract.permissions.version = 1;
    contract.ports = {{PortId{"Exporter.Output"}, PortDirection::kOutput,
                      ValueSchemaId{std::string{"Exporter.Byte."} + std::to_string(kRevision)}, 1}};
    definition.validate_state_payload = [](std::span<const uint8_t> bytes) {
        return bytes.empty() || (bytes.size() == 1 && bytes.front() == kRevision)
            ? aeg::foundation::Result::success()
            : aeg::foundation::invalid_argument("exported fixture state has the wrong revision");
    };
    definition.execute = [](const InvocationToken& token, const StateSnapshot&, auto, auto) {
        // Exporting contracts must never execute this callback. The actual DLL
        // verifier runs it only after the script clears this environment value.
        if (const char* marker = std::getenv("AEG_CONTRACT_EXPORT_CALLBACK_MARKER"); marker && *marker)
            std::ofstream(marker, std::ios::binary) << "callback executed";
        StateDelta delta{token.state_version};
        delta.state_payload = std::vector<uint8_t>{static_cast<uint8_t>(kRevision)};
        return aeg::foundation::ResultValue<StateDelta>{aeg::foundation::Result::success(), std::move(delta)};
    };
    return definition;
}
}
