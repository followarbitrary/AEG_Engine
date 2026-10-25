#include <type_traits>

#include "feature_module.hpp"

namespace {
template <typename Type>
concept HasLegacyPipelineMembers = requires(Type value) {
    value.pipelines;
    value.passes;
    value.bindings;
};
}

static_assert(!HasLegacyPipelineMembers<aeg::module::FeatureModuleDefinition>);
static_assert(std::is_same_v<decltype(aeg::module::FeatureModuleDefinition{}.descriptor.id), aeg::module::ModuleId>);
static_assert(std::is_same_v<decltype(aeg::module::FeatureModuleDefinition{}.descriptor.version), aeg::module::Version>);

int main() { return 0; }
