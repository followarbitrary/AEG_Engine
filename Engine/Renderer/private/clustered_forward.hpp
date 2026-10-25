/** @brief Shared creation of the two fixed clustered material pipeline variants. */
#pragma once

#include <array>
#include <filesystem>

#include "renderer.hpp"
#include "shader_contract.hpp"

namespace aeg::renderer {
struct ClusteredForwardPipelines final {
    RenderMaterialPipelineVariant shadowed{}, unshadowed{};
    rhi::BindingLayoutHandle shadowed_inputs{}, unshadowed_inputs{};
};

/** Pipelines are caller-owned; reflected layouts borrow the existing ShaderManager cache. */
[[nodiscard]] foundation::ResultValue<ClusteredForwardPipelines> create_clustered_forward_pipelines(
    rhi::Device& device, shader::ShaderManager& shaders, const rhi::GraphicsPipelineState& material_graphics,
    const std::filesystem::path& shadowed_package, const std::filesystem::path& unshadowed_package);
}  // namespace aeg::renderer
