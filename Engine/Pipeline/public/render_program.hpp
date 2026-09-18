#pragma once

#include <memory>
#include <span>
#include <string>

#include "pipeline_authoring_types.hpp"
#include "rhi_struct.hpp"

namespace aeg::pipeline {

struct RenderProgramRequest final {
    AssetReference shader{};
    std::string variant{};
    RenderGraphPassType type{RenderGraphPassType::kGraphics};
    rhi::Format color_format{rhi::Format::kUnknown};
};

struct ResolvedRenderProgramResource final {
    uint64_t port_id{};
    rhi::TextureViewHandle texture{};
    rhi::BufferViewHandle buffer{};
};

struct PreparedRenderProgram final {
    rhi::PipelineHandle pipeline{};
    rhi::BindingSetHandle bindings{};
    uint32_t group{};
    std::shared_ptr<const void> owner{};
};

class RenderProgramProvider {
public:
    virtual ~RenderProgramProvider() = default;
    [[nodiscard]] virtual foundation::ResultValue<PreparedRenderProgram> prepare(
        const RenderProgramRequest&, const ParameterBlock&, std::span<const ResolvedRenderProgramResource>) = 0;
};

}  // namespace aeg::pipeline
