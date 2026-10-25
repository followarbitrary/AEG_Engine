/** @brief Creates clustered variants through the host's existing shader/layout cache. */
#include "clustered_forward.hpp"

#include "../../Shader/private/shader_pipeline_layout_access.hpp"
#include "post_process.hpp"

namespace aeg::renderer {
foundation::ResultValue<ClusteredForwardPipelines> create_clustered_forward_pipelines(
    rhi::Device& device, shader::ShaderManager& shaders, const rhi::GraphicsPipelineState& material_graphics,
    const std::filesystem::path& shadowed_package, const std::filesystem::path& unshadowed_package) {
    if (shadowed_package.empty() || unshadowed_package.empty())
        return {foundation::invalid_argument("both clustered shader packages are required"), {}};
    if (material_graphics.depth_stencil_format == rhi::Format::kUnknown)
        return {foundation::invalid_argument("clustered material requires a camera depth attachment"), {}};
    const std::array colors{kSceneLightingFormat};
    auto graphics = material_graphics;
    graphics.color_formats = colors;
    const RenderMaterialAttachmentContract contract{graphics.color_formats.front(), graphics.samples,
        graphics.depth_stencil_format};
    ClusteredForwardPipelines result{};
    const std::array packages{&shadowed_package, &unshadowed_package};
    const std::array variants{&result.shadowed, &result.unshadowed};
    const std::array inputs{&result.shadowed_inputs, &result.unshadowed_inputs};
    const auto fail = [&](foundation::Result error) -> foundation::ResultValue<ClusteredForwardPipelines> {
        for (const auto* variant : variants)
            if (variant->pipeline.is_valid()) (void)device.destroy_pipeline(variant->pipeline);
        return {std::move(error), {}};
    };
    for (size_t index = 0; index < variants.size(); ++index) {
        const auto program = shader::load_shader_program(*packages[index]);
        if (!program) return fail(program.result);
        size_t input_count = 0;
        for (const auto& resource : program.value.contract.reflection.resources) {
            if (resource.group != 3) continue;
            ++input_count;
            const bool correct = resource.binding == 0 ? index == 0 && resource.type == rhi::BindingType::kSampledTexture
                : resource.binding == 1 ? resource.type == rhi::BindingType::kUniformBuffer
                : (resource.binding == 2 || resource.binding == 3) && resource.type == rhi::BindingType::kStorageBuffer;
            if (!correct)
                return fail(foundation::invalid_argument("clustered shader input ABI does not match its shadow mode"));
        }
        if (input_count != (index == 0 ? 4u : 3u))
            return fail(foundation::invalid_argument("clustered shader input ABI is incomplete"));
        const auto layout = shaders.build_pipeline_layout(program.value);
        if (!layout) return fail(layout.result);
        const auto input = shaders.binding_layout(layout.value, 3);
        if (!input) return fail(input.result);
        const auto pipeline = shaders.create_pipeline(program.value, layout.value,
            {rhi::PipelineType::kGraphics, graphics,
             rhi::DebugName{index == 0 ? "Renderer.Clustered.Shadowed" : "Renderer.Clustered.Unshadowed"}});
        if (!pipeline) return fail(pipeline.result);
        variants[index]->pipeline = pipeline.value;
        variants[index]->attachment_contract = contract;
        variants[index]->profile_contract = {
            {kRendererProfileContractVersion, RendererProfile::kForwardClustered,
             shader::detail::ShaderPipelineLayoutAccess::contract_hash(layout.value),
             shader::detail::ShaderPipelineLayoutAccess::cache_epoch(layout.value)},
            contract,
            shader::detail::ShaderPipelineLayoutAccess::cache_epoch(layout.value)};
        *inputs[index] = input.value;
    }
    return {foundation::Result::success(), result};
}
}  // namespace aeg::renderer
