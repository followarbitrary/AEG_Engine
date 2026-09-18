#include <iostream>

#include "../Engine/Module/private/module_context_table.hpp"

int main() {
    using aeg::foundation::ResultCode;
    using aeg::module::ModuleContextKind;
    using aeg::module::ModuleContextTable;

    ModuleContextTable table(0xAEE6u);
    auto pipeline = table.allocate(ModuleContextKind::kPipeline);
    if (!pipeline || table.active_count() != 1)
        return 1;
    if (!table.validate(pipeline.value, ModuleContextKind::kPipeline))
        return 2;
    if (table.validate(pipeline.value, ModuleContextKind::kFeature).code != ResultCode::kInvalidArgument)
        return 3;

    auto foreign = pipeline.value;
    foreign.owner_cookie += 1;
    if (table.validate(foreign, ModuleContextKind::kPipeline).code != ResultCode::kInvalidState)
        return 4;

    if (!table.release(pipeline.value, ModuleContextKind::kPipeline))
        return 5;
    if (table.validate(pipeline.value, ModuleContextKind::kPipeline).code != ResultCode::kInvalidState)
        return 6;
    auto recycled = table.allocate(ModuleContextKind::kPipeline);
    if (!recycled || recycled.value.slot != pipeline.value.slot ||
        recycled.value.generation == pipeline.value.generation)
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
