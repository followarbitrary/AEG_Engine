#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "pipeline.hpp"
#include "parameter_value.hpp"

namespace aeg::pipeline {

/** Frame-local, pass-owned execution data. Never persisted in authoring documents. */
struct RenderPassRuntimeData {
    virtual ~RenderPassRuntimeData() = default;
};

struct RenderPassConfiguration final {
    RenderPassTypeId type{};
    RenderPassInstanceId instance{};
    rhi::QueueClass queue{rhi::QueueClass::kGraphics};
    bool enabled{true};
    foundation::ParameterBlock parameters{};
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

class RenderPassRegistry;
namespace detail {
struct RenderPassRegistryExecution;
}

class RenderPassRegistry final {
public:
    [[nodiscard]] foundation::Result register_pass(RenderPassFactory factory);
    [[nodiscard]] foundation::Result unregister_pass(RenderPassTypeId type);
private:
    friend struct detail::RenderPassRegistryExecution;
    std::map<RenderPassTypeId, RenderPassFactory> factories_{};
    uint64_t execution_epoch_{1};
};

}  // namespace aeg::pipeline
