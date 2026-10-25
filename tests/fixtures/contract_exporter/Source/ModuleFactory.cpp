#include "ModuleFactory.hpp"
#include "FeatureDefinition.hpp"
#include "Revision.hpp"

namespace exporter_fixture {
aeg::module::FeatureModuleDefinition make_module() {
    aeg::module::FeatureModuleDefinition definition;
    definition.descriptor = {aeg::module::ModuleId{"Exporter.Fixture"}, "Contract Exporter Fixture",
                             {kRevision, 0, 0}, {}};
    if (kRejectFactory) {
        definition.status = aeg::foundation::invalid_argument("intentional contract factory rejection");
        return definition;
    }
    if (const auto result = AEG_REGISTER_GAME_FEATURE(definition, make_feature()); !result)
        return definition;
    return definition;
}
}
