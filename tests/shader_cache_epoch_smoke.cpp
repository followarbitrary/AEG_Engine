#include <cstddef>
#include <iostream>
#include <utility>
#include <vector>

#include "rhi.hpp"
#include "shader_contract.hpp"

int main() {
    const auto device_result = aeg::rhi::create_device(
        {aeg::rhi::BackendType::kVulkan, "AEG Shader Cache Epoch Smoke"});
    if (!device_result) {
        std::cerr << "create_device failed: " << device_result.result.message << '\n';
        return 1;
    }

    aeg::shader::ShaderProgram program{};
    program.name = "cache-epoch";
    program.stages.push_back({aeg::rhi::ShaderStage::kCompute, "main", std::vector<std::byte>(4)});
    program.contract.reflection.resources.push_back({
        .group = 0,
        .binding = 0,
        .type = aeg::rhi::BindingType::kStorageBuffer,
        .shader_stages = aeg::rhi::ShaderStage::kCompute,
        .name = "Data",
    });

    aeg::shader::ShaderManager manager(*device_result.value);
    const auto empty_identity = manager.pipeline_layout_identity({});
    if (empty_identity || empty_identity.result.code != aeg::foundation::ResultCode::kInvalidState ||
        empty_identity.value.contract_hash != 0 || empty_identity.value.cache_epoch != 0)
        return 15;
    const auto first = manager.build_pipeline_layout(program);
    if (!first)
        return 2;
    const auto first_identity = manager.pipeline_layout_identity(first.value);
    if (!first_identity || first_identity.value.cache_epoch != manager.cache_epoch() ||
        first_identity.value.contract_hash == 0 || manager.cache_epoch() == 0)
        return 16;
    {
        aeg::shader::ShaderManager foreign_manager(*device_result.value);
        const auto foreign_identity = foreign_manager.pipeline_layout_identity(first.value);
        if (foreign_identity || foreign_identity.result.code != aeg::foundation::ResultCode::kInvalidState ||
            foreign_identity.value.contract_hash != 0 || foreign_identity.value.cache_epoch != 0)
            return 17;
    }
    auto changed_program = program;
    changed_program.contract.reflection.resources.front().binding = 1;
    const auto changed_layout = manager.build_pipeline_layout(changed_program);
    if (!changed_layout)
        return 18;
    const auto changed_identity = manager.pipeline_layout_identity(changed_layout.value);
    if (!changed_identity || changed_identity.value.contract_hash == first_identity.value.contract_hash ||
        changed_identity.value.cache_epoch != first_identity.value.cache_epoch)
        return 19;
    if (!manager.binding_layout(first.value, 0))
        return 3;

    const auto first_binding = manager.binding_layout(first.value, 0);
    if (!first_binding)
        return 9;
    const auto live_set = device_result.value->create_binding_set(
        {first_binding.value, aeg::rhi::BindingSetLifetime::kPersistent,
         aeg::rhi::DebugName{"ShaderCacheEpoch.LiveDependency"}});
    if (!live_set)
        return 10;

    const uint64_t retired_epoch = manager.cache_epoch();
    const auto blocked_clear = manager.clear_layout_cache();
    if (blocked_clear || blocked_clear.code != aeg::foundation::ResultCode::kInvalidState ||
        manager.cache_epoch() == retired_epoch)
        return 4;
    if (manager.cache_epoch() == 0)
        return 5;
    const auto retired_identity = manager.pipeline_layout_identity(first.value);
    if (first.value.is_valid() || manager.binding_layout(first.value, 0) || retired_identity ||
        retired_identity.result.code != aeg::foundation::ResultCode::kInvalidState ||
        retired_identity.value.contract_hash != 0 || retired_identity.value.cache_epoch != 0)
        return 6;

    const auto second = manager.build_pipeline_layout(program);
    if (!second)
        return 7;
    const auto second_identity = manager.pipeline_layout_identity(second.value);
    if (!second_identity || second_identity.value.cache_epoch != manager.cache_epoch() ||
        second_identity.value.cache_epoch == first_identity.value.cache_epoch ||
        second_identity.value.contract_hash != first_identity.value.contract_hash)
        return 20;
    const auto second_binding = manager.binding_layout(second.value, 0);
    if (!second_binding || second_binding.value == first_binding.value)
        return 8;

    if (!device_result.value->destroy_binding_set(live_set.value))
        return 11;
    if (const auto result = manager.clear_layout_cache(); !result)
        return 12;

    aeg::shader::ShaderPipelineLayout orphaned_layout{};
    {
        aeg::shader::ShaderManager temporary_manager(*device_result.value);
        const auto temporary_layout = temporary_manager.build_pipeline_layout(program);
        if (!temporary_layout)
            return 13;
        orphaned_layout = std::move(temporary_layout.value);
    }
    const auto orphaned_identity = manager.pipeline_layout_identity(orphaned_layout);
    if (orphaned_layout.is_valid() || orphaned_identity ||
        orphaned_identity.result.code != aeg::foundation::ResultCode::kInvalidState ||
        orphaned_identity.value.contract_hash != 0 || orphaned_identity.value.cache_epoch != 0)
        return 14;

    std::cout << "Shader cache epoch invalidates stale layouts and accepts rebuilt layouts\n";
    return 0;
}
