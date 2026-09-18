#include <iostream>
#include <memory>

#include "render_pass.hpp"

namespace aeg_test {
class MacroPass final : public aeg::pipeline::RenderPass {
public:
    const aeg::pipeline::RenderPassDescriptor& descriptor() const noexcept override { return descriptor_; }
    aeg::foundation::Result setup(aeg::pipeline::RenderPassBuilder&,
                                  const aeg::pipeline::RenderPassConfiguration&) const override {
        return {};
    }
    aeg::foundation::Result execute(aeg::pipeline::RenderPassExecutionContext&,
                                    const aeg::pipeline::RenderPassConfiguration&) const override {
        return {};
    }

private:
    aeg::pipeline::RenderPassDescriptor descriptor_{
        aeg::pipeline::RenderPassTypeId("Game.Macro.Pass"),
        aeg::pipeline::ModuleId("Game.Main"),
        "Game.Macro.Pass",
        {1, 0, 0},
        aeg::pipeline::RenderGraphPassType::kGraphics,
        uint64_t{1} << static_cast<uint8_t>(aeg::rhi::QueueClass::kGraphics)};
};
}  // namespace aeg_test
REGISTER_RENDER_PASS(aeg_test::MacroPass)
namespace {
class CountingPass final : public aeg::pipeline::RenderPass {
public:
    explicit CountingPass(uint32_t& destroyed) : destroyed_(&destroyed) {
        descriptor_.type = aeg::pipeline::RenderPassTypeId("Game.Test.Pass");
        descriptor_.owner_module = aeg::pipeline::ModuleId("Game.Main");
        descriptor_.name = "Game.Test.Pass";
        descriptor_.version = {1, 0, 0};
        descriptor_.allowed_queue_mask = uint64_t{1} << static_cast<uint8_t>(aeg::rhi::QueueClass::kGraphics);
    }
    ~CountingPass() override { ++*destroyed_; }
    const aeg::pipeline::RenderPassDescriptor& descriptor() const noexcept override { return descriptor_; }
    aeg::foundation::Result setup(aeg::pipeline::RenderPassBuilder&,
                                  const aeg::pipeline::RenderPassConfiguration&) const override {
        return aeg::foundation::Result::success();
    }
    aeg::foundation::Result execute(aeg::pipeline::RenderPassExecutionContext&,
                                    const aeg::pipeline::RenderPassConfiguration&) const override {
        return aeg::foundation::Result::success();
    }

private:
    uint32_t* destroyed_;
    aeg::pipeline::RenderPassDescriptor descriptor_{};
};
}  // namespace

int main() {
    if (!aeg::pipeline::global_render_pass_registry().find_pass(aeg::pipeline::RenderPassTypeId("Game.Macro.Pass")))
        return 1;
    aeg::pipeline::RenderPassRegistry registry;
    uint32_t destroyed = 0;
    auto factory = [&] { return std::make_unique<CountingPass>(destroyed); };
    if (!registry.register_pass(factory) || registry.register_pass(factory))
        return 1;
    auto retained = registry.find_pass(aeg::pipeline::RenderPassTypeId("Game.Test.Pass"));
    if (!retained || !registry.unregister_pass(aeg::pipeline::RenderPassTypeId("Game.Test.Pass")))
        return 2;
    const auto before_reset = destroyed;
    retained.value.reset();
    if (destroyed != before_reset + 1)
        return 3;
    std::cout << "render pass registry smoke passed\n";
    return 0;
}
