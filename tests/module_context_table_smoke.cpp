#include <iostream>

#include "../Engine/Module/private/module_context_table.hpp"

int main() {
    using aeg::foundation::ResultCode;
    using aeg::module::ModuleContextKind;
    using aeg::module::ModuleContextTable;

    ModuleContextTable table(0xAEE6u);
    auto feature = table.allocate(ModuleContextKind::kFeatureDefinition);
    if (!feature || table.active_count() != 1)
        return 1;
    if (!table.validate(feature.value, ModuleContextKind::kFeatureDefinition))
        return 2;
    if (table.validate(feature.value, ModuleContextKind::kFeatureInstance).code != ResultCode::kInvalidArgument)
        return 3;

    auto foreign = feature.value;
    foreign.owner_cookie += 1;
    if (table.validate(foreign, ModuleContextKind::kFeatureDefinition).code != ResultCode::kInvalidState)
        return 4;

    if (!table.release(feature.value, ModuleContextKind::kFeatureDefinition))
        return 5;
    if (table.validate(feature.value, ModuleContextKind::kFeatureDefinition).code != ResultCode::kInvalidState)
        return 6;
    auto recycled = table.allocate(ModuleContextKind::kFeatureDefinition);
    if (!recycled || recycled.value.slot != feature.value.slot ||
        recycled.value.generation == feature.value.generation)
        return 7;

    {
        auto lease = table.acquire(ModuleContextKind::kRenderGraph);
        if (!lease || !lease.value.valid() || table.active_count() != 2)
            return 8;
        if (!table.validate(lease.value.context(), ModuleContextKind::kRenderGraph))
            return 9;
    }
    if (table.active_count() != 1)
        return 10;
    std::cout << "module context table smoke passed\n";
    return 0;
}
