#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "pipeline.hpp"
#include "pipeline_authoring_types.hpp"

namespace aeg::pipeline {

struct RenderPassInstanceIdTag;
using RenderPassInstanceId = StableId<RenderPassInstanceIdTag>;

/** Frame-local, pass-owned execution data. Never persisted in authoring documents. */
struct RenderPassRuntimeData {
    virtual ~RenderPassRuntimeData() = default;
};

struct RenderPassConfiguration final {
    RenderPassTypeId type{};
    RenderPassInstanceId instance{};
    rhi::QueueClass queue{rhi::QueueClass::kGraphics};
    bool enabled{true};
    ParameterBlock parameters{};
    std::unordered_map<uint64_t, uint64_t> bindings{};
    std::string source{};
    std::shared_ptr<const RenderPassRuntimeData> runtime_data{};
};

struct RenderPassDescriptor final {
    RenderPassTypeId type{};
    ModuleId owner_module{};
    std::string name{};
    Version version{};
    RenderGraphPassType graph_type{RenderGraphPassType::kGraphics};
    uint64_t allowed_queue_mask{0};
};

class RenderPass {
public:
    virtual ~RenderPass() = default;
    [[nodiscard]] virtual const RenderPassDescriptor& descriptor() const noexcept = 0;
    [[nodiscard]] virtual foundation::Result setup(RenderPassBuilder&, const RenderPassConfiguration&) const = 0;
    [[nodiscard]] virtual foundation::Result execute(RenderPassExecutionContext&,
                                                     const RenderPassConfiguration&) const = 0;
};

using RenderPassFactory = std::function<std::unique_ptr<RenderPass>()>;

class RenderPassRegistry final {
public:
    [[nodiscard]] foundation::Result register_pass(RenderPassFactory factory);
    [[nodiscard]] foundation::Result unregister_pass(RenderPassTypeId type);
    [[nodiscard]] foundation::ResultValue<std::shared_ptr<const RenderPass>> find_pass(RenderPassTypeId type) const;

private:
    std::map<RenderPassTypeId, RenderPassFactory> factories_{};
};

/** Process-wide registry used by static game-module registration macros. */
[[nodiscard]] RenderPassRegistry& global_render_pass_registry() noexcept;

template <class T>
struct StaticRenderPassRegistrar final {
    StaticRenderPassRegistrar() {
        static_cast<void>(global_render_pass_registry().register_pass([] { return std::make_unique<T>(); }));
    }
};

#define AEG_DETAIL_JOIN_RENDER_PASS(a, b) a##b
#define AEG_DETAIL_RENDER_PASS_NAME(a, b) AEG_DETAIL_JOIN_RENDER_PASS(a, b)
#define AEG_STATIC_REGISTER_RENDER_PASS(Type) AEG_DETAIL_REGISTER_RENDER_PASS(Type, __COUNTER__)
#define AEG_DETAIL_REGISTER_RENDER_PASS(Type, n)                                        \
    namespace {                                                                         \
    const ::aeg::pipeline::StaticRenderPassRegistrar<Type> AEG_DETAIL_RENDER_PASS_NAME( \
        aeg_static_render_pass_registrar_, n){};                                        \
    }
#define REGISTER_RENDER_PASS(Type) AEG_STATIC_REGISTER_RENDER_PASS(Type)

[[nodiscard]] foundation::ResultValue<RenderGraphPass> add_registered_render_pass(
    RenderGraphBuilder&, const RenderPassRegistry&, std::shared_ptr<const RenderPassConfiguration>);

}  // namespace aeg::pipeline
