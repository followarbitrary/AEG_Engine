/** @file render_pass_authoring_smoke.cpp @brief Explicit Engine-owned technical pass registry contracts. */
#include <iostream>
#include <memory>
#include <type_traits>
#include <utility>

#include "../Engine/Pipeline/private/render_pass_registry_execution.hpp"
#include "render_pass.hpp"

namespace {
using namespace aeg;
constexpr pipeline::RenderPassTypeId kPassType{"Renderer.RegistrySmoke.Pass"};
static_assert(std::is_same_v<decltype(pipeline::RenderPassConfiguration{}.parameters), foundation::ParameterBlock>);

pipeline::RenderPassDescriptor descriptor(const char* name) {
    return {kPassType, pipeline::ModuleId("Renderer"), name, {1, 0, 0},
            pipeline::RenderGraphPassType::kGraphics,
            uint64_t{1} << static_cast<uint8_t>(rhi::QueueClass::kGraphics)};
}

class CountingPass final : public pipeline::RenderPass {
public:
    CountingPass(uint32_t& destroyed, pipeline::RenderPassDescriptor description)
        : destroyed_(&destroyed), descriptor_(std::move(description)) {}
    ~CountingPass() override { ++*destroyed_; }
    const pipeline::RenderPassDescriptor& descriptor() const noexcept override { return descriptor_; }
    foundation::Result setup(pipeline::RenderPassBuilder&,
                             const pipeline::RenderPassConfiguration&) const override {
        return foundation::Result::success();
    }
    foundation::Result execute(pipeline::RenderPassExecutionContext&,
                               const pipeline::RenderPassConfiguration&) const override {
        return foundation::Result::success();
    }

private:
    uint32_t* destroyed_;
    pipeline::RenderPassDescriptor descriptor_{};
};
}  // namespace

int main() {
    using namespace aeg;
    using Execution = pipeline::detail::RenderPassRegistryExecution;
    using foundation::ResultCode;
    uint32_t destroyed_first = 0, destroyed_second = 0;
    pipeline::RenderPassRegistry first, second;
    const auto first_initial_epoch = Execution::execution_epoch(first);
    const auto second_initial_epoch = Execution::execution_epoch(second);

    // Constructing two hosts must not import any process-wide registration.
    if (Execution::find_pass(first, kPassType).result.code != ResultCode::kNotFound ||
        Execution::find_pass(second, kPassType).result.code != ResultCode::kNotFound ||
        destroyed_first != 0 || destroyed_second != 0)
        return 1;
    if (first.register_pass({}).code != ResultCode::kInvalidArgument ||
        first.register_pass([] { return std::unique_ptr<pipeline::RenderPass>{}; }).code != ResultCode::kInvalidArgument)
        return 2;
    for (unsigned invalid_case = 0; invalid_case < 5; ++invalid_case) {
        auto invalid = descriptor("Renderer.RegistrySmoke.First");
        switch (invalid_case) {
            case 0: invalid.type = {}; break;
            case 1: invalid.owner_module = {}; break;
            case 2: invalid.name.clear(); break;
            case 3: invalid.version = {0, 0, 0}; break;
            case 4: invalid.allowed_queue_mask = 0; break;
        }
        if (first.register_pass([&destroyed_first, invalid] {
                return std::make_unique<CountingPass>(destroyed_first, invalid);
            }).code != ResultCode::kInvalidArgument)
            return 3;
    }
    if (Execution::execution_epoch(first) != first_initial_epoch ||
        Execution::execution_epoch(second) != second_initial_epoch)
        return 4;

    const auto first_factory = [&destroyed_first] {
        return std::make_unique<CountingPass>(destroyed_first, descriptor("Renderer.RegistrySmoke.First"));
    };
    const auto second_factory = [&destroyed_second] {
        return std::make_unique<CountingPass>(destroyed_second, descriptor("Renderer.RegistrySmoke.Second"));
    };
    if (!first.register_pass(first_factory) || first.register_pass(first_factory).code != ResultCode::kInvalidArgument ||
        Execution::execution_epoch(first) == first_initial_epoch ||
        Execution::execution_epoch(second) != second_initial_epoch ||
        Execution::find_pass(second, kPassType).result.code != ResultCode::kNotFound)
        return 5;

    // The same technical type can be supplied independently by two host registries.
    const auto first_registered_epoch = Execution::execution_epoch(first);
    if (!second.register_pass(second_factory) || Execution::execution_epoch(first) != first_registered_epoch)
        return 6;
    auto retained_first = Execution::find_pass(first, kPassType);
    auto retained_second = Execution::find_pass(second, kPassType);
    if (!retained_first || !retained_second ||
        retained_first.value->descriptor().name != "Renderer.RegistrySmoke.First" ||
        retained_second.value->descriptor().name != "Renderer.RegistrySmoke.Second" ||
        retained_first.value->descriptor().owner_module != pipeline::ModuleId("Renderer"))
        return 7;

    const auto second_registered_epoch = Execution::execution_epoch(second);
    if (!first.unregister_pass(kPassType) ||
        first.unregister_pass(kPassType).code != ResultCode::kNotFound ||
        Execution::find_pass(first, kPassType).result.code != ResultCode::kNotFound ||
        Execution::execution_epoch(second) != second_registered_epoch ||
        !Execution::find_pass(second, kPassType))
        return 8;
    if (!second.unregister_pass(kPassType) ||
        Execution::find_pass(second, kPassType).result.code != ResultCode::kNotFound)
        return 9;
    // Removing a factory must not destroy objects already retained by a graph.
    const auto first_before_reset = destroyed_first;
    const auto second_before_reset = destroyed_second;
    if (retained_first.value->descriptor().type != kPassType ||
        retained_second.value->descriptor().type != kPassType)
        return 10;
    retained_first.value.reset();
    if (destroyed_first != first_before_reset + 1 || destroyed_second != second_before_reset)
        return 11;
    retained_second.value.reset();
    if (destroyed_second != second_before_reset + 1)
        return 12;

    std::cout << "explicit Engine pass registry smoke passed\n";
    return 0;
}
