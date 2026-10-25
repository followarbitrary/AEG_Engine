/** @file runtime_material_vulkan_smoke.cpp
 *  @brief Validates the cooked material to Vulkan TextureMaterial ABI path.
 */
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <span>
#include <vector>

#include "asset_package.hpp"
#include "cooked_asset.hpp"
#include "frame_context.hpp"
#include "material.hpp"
#include "render_graph.hpp"
#include "resource_uploader.hpp"
#include "rhi.hpp"
#include "runtime_asset_resolver.hpp"
#include "runtime_asset_upload_host.hpp"
#include "../Engine/Shader/private/shader_pipeline_layout_access.hpp"
#include "shader_contract.hpp"
#include "clustered_forward.hpp"
#include "texture_uploader.hpp"

namespace {
using namespace aeg;

template <typename T>
void append(std::vector<std::byte>& bytes, T value) {
    const auto* first = reinterpret_cast<const std::byte*>(&value);
    bytes.insert(bytes.end(), first, first + sizeof(value));
}

void append_uuid(std::vector<std::byte>& bytes, foundation::AssetId id) {
    for (const uint8_t value : id.value.bytes)
        append(bytes, value);
}

template <typename Value>
bool require(const Value& condition, const char* message) {
    const bool valid = static_cast<bool>(condition);
    if (!valid)
        std::cerr << message << '\n';
    return valid;
}

std::vector<std::byte> material_payload(foundation::AssetId shader, foundation::AssetId texture) {
    std::vector<std::byte> bytes;
    append<uint32_t>(bytes, 0x314d4741u);  // AGM1
    append<uint32_t>(bytes, 1u);
    append_uuid(bytes, shader);
    append<uint32_t>(bytes, 1u);
    append<uint64_t>(bytes, 1u);  // canonical sampled-texture binding
    append_uuid(bytes, texture);
    // FNV-1a 64-bit hash of the reflected material_tint field.
    append<uint32_t>(bytes, 1u);
    append<uint64_t>(bytes, 5955903723798558642ull);
    append<uint8_t>(bytes, 5u);  // float4 vector
    append<uint32_t>(bytes, 4u);
    append<double>(bytes, 0.5);
    append<double>(bytes, 0.75);
    append<double>(bytes, 0.25);
    append<double>(bytes, 1.0);
    return bytes;
}

std::vector<std::byte> texture_payload() {
    std::vector<std::byte> bytes(16u, std::byte{0xff});
    const uint32_t one = 1u;
    const uint32_t four = 4u;
    std::memcpy(bytes.data(), &one, sizeof(one));
    std::memcpy(bytes.data() + 4u, &one, sizeof(one));
    std::memcpy(bytes.data() + 8u, &four, sizeof(four));
    return bytes;
}

bool has_binding(const shader::ShaderProgram& program, uint16_t group, uint16_t binding, rhi::BindingType type) {
    for (const auto& resource : program.contract.reflection.resources) {
        if (resource.group == group && resource.binding == binding && resource.type == type)
            return true;
    }
    return false;
}
}  // namespace

int main() {
    const auto material_id = foundation::AssetId::parse("d0112233-4455-6677-8899-aabbccddeeff");
    const auto texture_id = foundation::AssetId::parse("d1112233-4455-6677-8899-aabbccddeeff");
    const auto shader_id = foundation::AssetId::parse("d2112233-4455-6677-8899-aabbccddeeff");
    const auto wrong_shader_id = foundation::AssetId::parse("d3112233-4455-6677-8899-aabbccddeeff");
    if (!require(material_id && texture_id && shader_id && wrong_shader_id, "material Vulkan fixture IDs invalid"))
        return 1;

    const auto cooked_material = asset::encode_cooked_asset(
        {material_id.value, foundation::AssetKind::kMaterial, 1, {}, material_payload(shader_id.value, texture_id.value)});
    const auto cooked_texture =
        asset::encode_cooked_asset({texture_id.value, foundation::AssetKind::kTexture, 1, {}, texture_payload()});
    if (!require(cooked_material && cooked_texture, "material Vulkan fixture cook failed"))
        return 2;

    const auto package_path = std::filesystem::temp_directory_path() / "aeg_runtime_material_vulkan.aegbin";
    const std::vector<tooling::AssetPackageEntry> entries{
        {material_id.value, foundation::AssetKind::kMaterial, 1, cooked_material.value},
        {texture_id.value, foundation::AssetKind::kTexture, 1, cooked_texture.value},
    };
    if (!require(static_cast<bool>(tooling::AssetPackageWriter{}.write(package_path, entries)),
                 "material Vulkan package write failed"))
        return 3;
    auto resolver_result = application::runtime::RuntimeAssetResolver::open(package_path);
    if (!require(resolver_result, "material Vulkan package open failed"))
        return 4;
    auto resolver = std::move(resolver_result.value);
    const auto material_handle = resolver.bind_material(material_id.value);
    if (!require(material_handle, "material Vulkan bind failed"))
        return 5;

    auto device_result = rhi::create_device({rhi::BackendType::kVulkan, "AEG Runtime Material Vulkan Smoke"});
    if (!require(device_result, "material Vulkan device creation failed"))
        return 6;
    auto device = std::move(device_result.value);
    auto shader_program_result = shader::load_shader_program(AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH);
    if (!require(shader_program_result, "material Vulkan shader package load failed"))
        return 7;
    shader::ShaderProgram shader_program = std::move(shader_program_result.value);
    if (!require(has_binding(shader_program, 1, 0, rhi::BindingType::kSampledTexture) &&
                     has_binding(shader_program, 1, 1, rhi::BindingType::kSampler) &&
                     has_binding(shader_program, 1, 2, rhi::BindingType::kUniformBuffer),
                 "material Vulkan shader binding 0/1/2 contract missing"))
        return 8;
    if (!require(shader_program.contract.reflection.parameters.size() == 1u &&
                     shader_program.contract.reflection.parameters.front().name == "material_tint",
                 "material Vulkan shader parameter reflection missing"))
        return 8;

    shader::ShaderManager shader_manager(*device);
    const auto shader_layout =
        shader_manager.build_pipeline_layout(shader_program, rhi::DebugName{"Runtime.Material.Layout"});
    if (!require(shader_layout, "material Vulkan pipeline layout creation failed"))
        return 9;
    const auto material_layout = shader_manager.binding_layout(shader_layout.value, 1);
    if (!require(material_layout, "material Vulkan material binding layout lookup failed"))
        return 10;

    const std::array<rhi::VertexBindingDesc, 1> vertex_bindings{
        rhi::VertexBindingDesc{0, sizeof(float) * 9u, rhi::VertexInputRate::kVertex}};
    const std::array<rhi::VertexAttributeDesc, 3> vertex_attributes{
        rhi::VertexAttributeDesc{0, 0, rhi::Format::kRgba32Float, 0},
        rhi::VertexAttributeDesc{1, 0, rhi::Format::kRg32Float, sizeof(float) * 4u},
        rhi::VertexAttributeDesc{2, 0, rhi::Format::kRgb32Float, sizeof(float) * 6u}};
    const std::array<rhi::Format, 1> color_formats{rhi::Format::kRgba8Unorm};
    rhi::GraphicsPipelineState graphics{};
    graphics.vertex_bindings = vertex_bindings;
    graphics.vertex_attributes = vertex_attributes;
    graphics.topology = rhi::PrimitiveTopology::kTriangleList;
    graphics.cull_mode = rhi::CullMode::kNone;
    graphics.depth_test_enable = false;
    graphics.depth_write_enable = false;
    graphics.color_formats = color_formats;
    const auto pipeline = shader_manager.create_pipeline(
        shader_program, shader_layout.value,
        {rhi::PipelineType::kGraphics, graphics, rhi::DebugName{"Runtime.Material.Pipeline"}});
    if (!require(pipeline, "material Vulkan pipeline creation failed"))
        return 11;

    auto color_depth_graphics = graphics;
    color_depth_graphics.depth_test_enable = color_depth_graphics.depth_write_enable = true;
    color_depth_graphics.depth_stencil_format = rhi::Format::kD32Float;
    const auto color_depth_pipeline = shader_manager.create_pipeline(shader_program, shader_layout.value,
        {rhi::PipelineType::kGraphics, color_depth_graphics, {"Runtime.Material.ColorDepth"}});
    const std::array replacement_colors{rhi::Format::kBgra8Unorm};
    auto replacement_color_depth_graphics = color_depth_graphics;
    replacement_color_depth_graphics.color_formats = replacement_colors;
    const auto replacement_color_depth_pipeline = shader_manager.create_pipeline(shader_program, shader_layout.value,
        {rhi::PipelineType::kGraphics, replacement_color_depth_graphics, {"Runtime.Material.ColorDepthReplacement"}});
    if (!require(color_depth_pipeline && replacement_color_depth_pipeline,
                 "material Vulkan real color/depth PSOs failed")) return 39;
    const renderer::RenderMaterialAttachmentContract color_depth_attachment{
        color_depth_graphics.color_formats[0], color_depth_graphics.samples, color_depth_graphics.depth_stencil_format};
    const renderer::RenderMaterialAttachmentContract replacement_color_depth_attachment{
        replacement_color_depth_graphics.color_formats[0], replacement_color_depth_graphics.samples,
        replacement_color_depth_graphics.depth_stencil_format};

    const auto shadow_program = shader::load_shader_program(AEG_FORWARD_SHADOW_SHADER_PACKAGE_PATH);
    if (!require(shadow_program, "material Vulkan ForwardShadow shader load failed")) return 39;
    const auto shadow_layout = shader_manager.build_pipeline_layout(shadow_program.value, {"Runtime.ForwardShadow.Layout"});
    if (!require(shadow_layout, "material Vulkan ForwardShadow layout failed")) return 39;
    const auto shadow_pipeline = shader_manager.create_pipeline(shadow_program.value, shadow_layout.value,
        {rhi::PipelineType::kGraphics, color_depth_graphics, {"Runtime.ForwardShadow.Pipeline"}});
    const auto replacement_shadow_pipeline = shader_manager.create_pipeline(shadow_program.value, shadow_layout.value,
        {rhi::PipelineType::kGraphics, replacement_color_depth_graphics, {"Runtime.ForwardShadow.Replacement"}});
    if (!require(shadow_pipeline && replacement_shadow_pipeline,
                 "material Vulkan real ForwardShadow PSOs failed")) return 39;
    const renderer::RenderMaterialShaderContract shadow_shader_contract{
        renderer::kRenderMaterialShaderContractVersion,
        shader::detail::ShaderPipelineLayoutAccess::contract_hash(shadow_layout.value),
        shader::detail::ShaderPipelineLayoutAccess::cache_epoch(shadow_layout.value)};
    const auto initial_variant_epoch = shader_manager.cache_epoch();

    const auto depth_program = shader::load_shader_program(AEG_DEPTH_MESH_SHADER_PACKAGE_PATH);
    if (!require(depth_program, "material Vulkan depth shader load failed")) return 36;
    const auto depth_layout = shader_manager.build_pipeline_layout(depth_program.value, {"Runtime.Depth.Layout"});
    if (!require(depth_layout, "material Vulkan depth layout failed")) return 36;
    auto depth_graphics = graphics;
    depth_graphics.vertex_attributes = std::span<const rhi::VertexAttributeDesc>{vertex_attributes.data(), 1};
    depth_graphics.color_formats = {};
    depth_graphics.depth_test_enable = depth_graphics.depth_write_enable = true;
    depth_graphics.depth_stencil_format = rhi::Format::kD32Float;
    const auto depth_pipeline = shader_manager.create_pipeline(depth_program.value, depth_layout.value,
        {rhi::PipelineType::kGraphics, depth_graphics, {"Runtime.Depth.Pipeline"}});
    if (!require(depth_pipeline, "material Vulkan actual depth PSO failed")) return 36;
    const renderer::RenderMaterialAttachmentContract depth_attachment{
        rhi::Format::kUnknown, depth_graphics.samples, depth_graphics.depth_stencil_format};
    const auto depth_epoch = shader::detail::ShaderPipelineLayoutAccess::cache_epoch(depth_layout.value);
    const renderer::RendererProfilePsoContract depth_profile{
        {renderer::kRendererProfileContractVersion, renderer::RendererProfile::kShadow,
         shader::detail::ShaderPipelineLayoutAccess::contract_hash(depth_layout.value), depth_epoch},
        depth_attachment, depth_epoch};

    const auto gbuffer_program = shader::load_shader_program(AEG_GBUFFER_MESH_SHADER_PACKAGE_PATH);
    if (!require(gbuffer_program, "material Vulkan GBuffer shader load failed")) return 24;
    const auto gbuffer_layout = shader_manager.build_pipeline_layout(gbuffer_program.value, {"Runtime.GBuffer.Layout"});
    if (!require(gbuffer_layout, "material Vulkan GBuffer layout failed")) return 24;
    const std::array gbuffer_formats{rhi::Format::kRgba16Float, rhi::Format::kRgba16Float, rhi::Format::kRgba8Unorm};
    auto gbuffer_graphics = graphics;
    gbuffer_graphics.color_formats = gbuffer_formats;
    gbuffer_graphics.depth_test_enable = gbuffer_graphics.depth_write_enable = true;
    gbuffer_graphics.depth_stencil_format = rhi::Format::kD32Float;
    const auto gbuffer_pipeline = shader_manager.create_pipeline(gbuffer_program.value, gbuffer_layout.value,
        {rhi::PipelineType::kGraphics, gbuffer_graphics, {"Runtime.GBuffer.Pipeline"}});
    auto replacement_graphics = gbuffer_graphics;
    replacement_graphics.depth_compare_operation = rhi::CompareOp::kGreaterOrEqual;
    const auto replacement_pipeline = shader_manager.create_pipeline(gbuffer_program.value, gbuffer_layout.value,
        {rhi::PipelineType::kGraphics, replacement_graphics, {"Runtime.GBuffer.Replacement"}});
    if (!require(gbuffer_pipeline && replacement_pipeline && gbuffer_pipeline.value != replacement_pipeline.value,
        "material Vulkan real replacement GBuffer pipeline failed")) return 24;
    const renderer::RenderMaterialAttachmentContract gbuffer_contract{gbuffer_graphics.color_formats[0],
        gbuffer_graphics.samples, gbuffer_graphics.depth_stencil_format,
        {gbuffer_graphics.color_formats[1], gbuffer_graphics.color_formats[2]}};
    const auto gbuffer_epoch = shader::detail::ShaderPipelineLayoutAccess::cache_epoch(gbuffer_layout.value);
    const renderer::RendererProfilePsoContract gbuffer_profile{
        {renderer::kRendererProfileContractVersion, renderer::RendererProfile::kGBuffer,
         shader::detail::ShaderPipelineLayoutAccess::contract_hash(gbuffer_layout.value), gbuffer_epoch},
        gbuffer_contract, gbuffer_epoch};

    const auto clustered_program = shader::load_shader_program(AEG_FORWARD_CLUSTERED_SHADER_PACKAGE_PATH);
    const auto unshadowed_program = shader::load_shader_program(AEG_FORWARD_CLUSTERED_UNSHADOWED_SHADER_PACKAGE_PATH);
    if (!require(clustered_program && unshadowed_program, "actual clustered shader packages unavailable")) return 28;
    const auto clustered_layout = shader_manager.build_pipeline_layout(clustered_program.value);
    const auto unshadowed_layout = shader_manager.build_pipeline_layout(unshadowed_program.value);
    if (!require(clustered_layout && unshadowed_layout, "actual clustered layouts unavailable")) return 28;
    const std::array clustered_formats{rhi::Format::kRgba16Float};
    auto clustered_graphics = gbuffer_graphics;
    clustered_graphics.color_formats = clustered_formats;
    const auto clustered = shader_manager.create_pipeline(clustered_program.value, clustered_layout.value,
        {rhi::PipelineType::kGraphics, clustered_graphics});
    const auto unshadowed = shader_manager.create_pipeline(unshadowed_program.value, unshadowed_layout.value,
        {rhi::PipelineType::kGraphics, clustered_graphics});
    const auto clustered_replacement = shader_manager.create_pipeline(clustered_program.value, clustered_layout.value,
        {rhi::PipelineType::kGraphics, clustered_graphics});
    const auto unshadowed_replacement = shader_manager.create_pipeline(unshadowed_program.value, unshadowed_layout.value,
        {rhi::PipelineType::kGraphics, clustered_graphics});
    if (!require(clustered && unshadowed && clustered_replacement && unshadowed_replacement,
        "actual HDR clustered PSOs unavailable")) return 28;
    const renderer::RenderMaterialAttachmentContract clustered_contract{clustered_graphics.color_formats[0],
        clustered_graphics.samples, clustered_graphics.depth_stencil_format};
    const renderer::RendererProfilePsoContract clustered_profile_contract{
        {renderer::kRendererProfileContractVersion, renderer::RendererProfile::kForwardClustered,
         shader::detail::ShaderPipelineLayoutAccess::contract_hash(clustered_layout.value)},
        clustered_contract};
    const renderer::RendererProfilePsoContract unshadowed_profile_contract{
        {renderer::kRendererProfileContractVersion, renderer::RendererProfile::kForwardClustered,
         shader::detail::ShaderPipelineLayoutAccess::contract_hash(unshadowed_layout.value)},
        clustered_contract};
    const auto wrong_mode = renderer::create_clustered_forward_pipelines(*device, shader_manager, clustered_graphics,
        AEG_FORWARD_CLUSTERED_SHADER_PACKAGE_PATH, AEG_FORWARD_CLUSTERED_SHADER_PACKAGE_PATH);
    if (wrong_mode) {
        (void)device->destroy_pipeline(wrong_mode.value.shadowed.pipeline);
        (void)device->destroy_pipeline(wrong_mode.value.unshadowed.pipeline);
    }
    if (!require(!wrong_mode && wrong_mode.result.code == foundation::ResultCode::kInvalidArgument,
        "clustered factory accepted a shadow descriptor in its unshadowed pipeline")) return 33;

    render_graph::FrameContext frame(*device, {.frames_in_flight = 2});
    render_graph::RenderGraph graph(*device);
    resource::ResourceUploader uploader(*device);
    resource::TextureUploader texture_uploader(*device);
    const rhi::SamplerDesc sampler{
        .min_filter = rhi::Filter::kNearest,
        .mag_filter = rhi::Filter::kNearest,
        .mip_filter = rhi::Filter::kNearest,
        .address_u = rhi::AddressMode::kClampToEdge,
        .address_v = rhi::AddressMode::kClampToEdge,
        .address_w = rhi::AddressMode::kClampToEdge,
        .name = rhi::DebugName{"Runtime.Material.Sampler"},
    };
    auto variant_context = application::runtime::RuntimeAssetResolver::UploadContext{.device = device.get(),
                                 .frame_context = &frame,
                                 .graph = &graph,
                                 .uploader = &uploader,
                                 .texture_uploader = &texture_uploader,
                                 .material_binding_layout = material_layout.value,
                                 .material_pipeline = pipeline.value,
                                 .material_shader_contract =
                                     {renderer::kRenderMaterialShaderContractVersion,
                                      shader::detail::ShaderPipelineLayoutAccess::contract_hash(shader_layout.value),
                                      shader::detail::ShaderPipelineLayoutAccess::cache_epoch(shader_layout.value)},
                                 .material_shader_layout = &shader_layout.value,
                                 .material_shader = shader_id.value,
                                 .material_shader_program = &shader_program,
                                 .material_sampler = sampler,
                                 .material_binding_group = 1,
                                 .shadow_pipeline = shadow_pipeline.value,
                                 .clustered_pipeline = clustered.value,
                                 .clustered_profile_contract = clustered_profile_contract};
    variant_context.material_attachment_contract = {graphics.color_formats[0], graphics.samples,
                                                   graphics.depth_stencil_format};
    variant_context.variant_cache_epoch = initial_variant_epoch;
    variant_context.clustered_pipeline_contract = clustered_contract;
    variant_context.unshadowed_clustered_variant = renderer::RenderMaterialPipelineVariant{
        unshadowed.value, clustered_contract, variant_context.material_shader_contract, unshadowed_profile_contract};
    variant_context.shadow_pipeline_contract = color_depth_attachment;
    variant_context.shadow_shader_contract = shadow_shader_contract;
    variant_context.shadow_pipeline_cache_epoch = initial_variant_epoch;
    variant_context.shadow_shader_layout = &shadow_layout.value;
    variant_context.gbuffer_pipeline = gbuffer_pipeline.value;
    variant_context.gbuffer_pipeline_contract = gbuffer_contract;
    variant_context.gbuffer_profile_contract = gbuffer_profile;
    variant_context.gbuffer_shader_layout = &gbuffer_layout.value;
    resolver.set_upload_context(variant_context);

    if (!require(frame.begin_frame(), "material Vulkan first frame begin failed"))
        return 12;
    const shader::ShaderPipelineLayout invalid_shadow_layout{};
    bool forward_contracts_rejected = true;
    const auto check_forward_contracts = [&](const char* phase) {
        for (unsigned bad = 0; bad < 11; ++bad) {
            variant_context.shadow_shader_contract = shadow_shader_contract;
            variant_context.shadow_pipeline_cache_epoch = initial_variant_epoch;
            variant_context.shadow_shader_layout = &shadow_layout.value;
            switch (bad) {
                case 0: variant_context.shadow_shader_contract = {}; break;
                case 1: ++variant_context.shadow_shader_contract.version; break;
                case 2: variant_context.shadow_shader_contract.layout_hash = 0; break;
                case 3: ++variant_context.shadow_shader_contract.layout_hash; break;
                case 4: variant_context.shadow_shader_contract.cache_epoch = 0; break;
                case 5: ++variant_context.shadow_shader_contract.cache_epoch; break;
                case 6: variant_context.shadow_pipeline_cache_epoch = 0; break;
                case 7: ++variant_context.shadow_pipeline_cache_epoch; break;
                case 8: variant_context.shadow_shader_layout = nullptr; break;
                case 9: variant_context.shadow_shader_layout = &invalid_shadow_layout; break;
                case 10: variant_context.shadow_shader_layout = &shader_layout.value; break;
            }
            if (resolver.resolve_material(material_handle.value)) {
                std::cerr << "Runtime accepted " << phase << " ForwardShadow metadata: " << bad << '\n';
                forward_contracts_rejected = false;
            }
        }
        variant_context.shadow_shader_contract = shadow_shader_contract;
        variant_context.shadow_pipeline_cache_epoch = initial_variant_epoch;
        variant_context.shadow_shader_layout = &shadow_layout.value;
        const renderer::RenderMaterialPipelineVariant valid_variant{
            color_depth_pipeline.value, color_depth_attachment, variant_context.material_shader_contract, {},
            initial_variant_epoch};
        for (unsigned bad = 0; bad < 9; ++bad) {
            auto variant = valid_variant;
            switch (bad) {
                case 0: variant = {}; break;
                case 1: variant.shader_contract = {}; break;
                case 2: ++variant.shader_contract.version; break;
                case 3: variant.shader_contract.layout_hash = 0; break;
                case 4: ++variant.shader_contract.layout_hash; break;
                case 5: variant.shader_contract.cache_epoch = 0; break;
                case 6: ++variant.shader_contract.cache_epoch; break;
                case 7: variant.cache_epoch = 0; break;
                case 8: ++variant.cache_epoch; break;
            }
            variant_context.depth_attachment_variant = variant;
            if (resolver.resolve_material(material_handle.value)) {
                std::cerr << "Runtime accepted " << phase << " color/depth variant metadata: " << bad << '\n';
                forward_contracts_rejected = false;
            }
        }
        variant_context.depth_attachment_variant.reset();
    };
    check_forward_contracts("cold");
    for (unsigned bad = 0; bad != 17; ++bad) {
        variant_context.gbuffer_profile_contract = gbuffer_profile;
        variant_context.gbuffer_shader_layout = &gbuffer_layout.value;
        switch (bad) {
            case 0: variant_context.gbuffer_profile_contract = {}; break;
            case 1: variant_context.gbuffer_profile_contract.shader.profile = renderer::RendererProfile::kDeferredLighting; break;
            case 2: ++variant_context.gbuffer_profile_contract.shader.version; break;
            case 3: variant_context.gbuffer_profile_contract.shader.layout_hash = 0; break;
            case 4: ++variant_context.gbuffer_profile_contract.shader.layout_hash; break;
            case 5: variant_context.gbuffer_profile_contract.shader.cache_epoch = 0; break;
            case 6: variant_context.gbuffer_profile_contract.cache_epoch = 0; break;
            case 7: ++variant_context.gbuffer_profile_contract.cache_epoch; break;
            case 8: ++variant_context.gbuffer_profile_contract.shader.cache_epoch; ++variant_context.gbuffer_profile_contract.cache_epoch; break;
            case 9: variant_context.gbuffer_profile_contract.attachment.color_format = rhi::Format::kRgba8Unorm; break;
            case 10: variant_context.gbuffer_profile_contract.attachment.additional_color_formats[0] = rhi::Format::kRgba8Unorm; break;
            case 11: variant_context.gbuffer_profile_contract.attachment.additional_color_formats[1] = rhi::Format::kRgba16Float; break;
            case 12: variant_context.gbuffer_profile_contract.attachment.depth_stencil_format = rhi::Format::kD24UnormS8Uint; break;
            case 13: variant_context.gbuffer_profile_contract.attachment.samples = rhi::SampleCount::kX4; break;
            case 14: variant_context.gbuffer_shader_layout = nullptr; break;
            case 15: variant_context.gbuffer_shader_layout = &shader_layout.value; break;
            case 16: variant_context.gbuffer_profile_contract.attachment.additional_color_formats[1] = rhi::Format::kUnknown; break;
        }
        if (resolver.resolve_material(material_handle.value)) {
            std::cerr << "Runtime accepted malformed/stale GBuffer profile before upload: " << bad << '\n';
            std::exit(38);
        }
    }
    variant_context.gbuffer_profile_contract = gbuffer_profile;
    variant_context.gbuffer_shader_layout = &gbuffer_layout.value;
    const auto first = resolver.resolve_material(material_handle.value);
    if (!require(first && first.value.clustered_pipeline == clustered.value &&
        first.value.clustered_pipeline_contract == clustered_contract &&
        first.value.clustered_profile_contract == clustered_profile_contract &&
        first.value.unshadowed_clustered_variant &&
        first.value.unshadowed_clustered_variant->pipeline == unshadowed.value &&
        first.value.unshadowed_clustered_variant->attachment_contract == clustered_contract &&
        first.value.unshadowed_clustered_variant->profile_contract == unshadowed_profile_contract,
        "Runtime material lost actual clustered HDR PSO metadata")) {
        (void)frame.abort_frame();
        (void)resolver.retire_gpu_resources();
        return 29;
    }
    if (!require(first && first.value.is_valid() && first.value.material_parameter_buffer.is_valid() &&
                     first.value.material_parameter_size == sizeof(resource::TextureMaterialParameters) &&
                     first.value.material_parameter_resource.is_valid() &&
                     first.value.shadow_pipeline == shadow_pipeline.value && first.value.clustered_pipeline == clustered.value &&
                     first.value.shadow_pipeline_contract == variant_context.shadow_pipeline_contract &&
                     first.value.gbuffer_pipeline == gbuffer_pipeline.value && first.value.gbuffer_pipeline_contract == gbuffer_contract &&
                     first.value.gbuffer_profile_contract == gbuffer_profile,
                 "material Vulkan first resolve failed")) {
        (void)frame.abort_frame();
        (void)resolver.retire_gpu_resources();
        return 13;
    }
    check_forward_contracts("warm");
    if (!forward_contracts_rejected)
        std::exit(39); // Retain cold/warm rejection evidence before the borrowed active-frame host unwinds.
    if (!require(first.value.shadow_shader_contract == shadow_shader_contract &&
                 first.value.shadow_pipeline_cache_epoch == initial_variant_epoch,
                 "material Vulkan did not publish the real ForwardShadow Shader/PSO metadata"))
        std::exit(39);
    shader::ShaderPipelineLayout expired_material_layout;
    {
        shader::ShaderManager temporary_shaders(*device);
        const auto temporary_layout = temporary_shaders.build_pipeline_layout(shader_program);
        if (!require(temporary_layout, "temporary material layout creation failed")) std::exit(40);
        expired_material_layout = temporary_layout.value;
    }
    if (!require(!expired_material_layout.is_valid() &&
                 shader::detail::ShaderPipelineLayoutAccess::contract_hash(expired_material_layout) ==
                     variant_context.material_shader_contract.layout_hash &&
                 shader::detail::ShaderPipelineLayoutAccess::cache_epoch(expired_material_layout) ==
                     variant_context.material_shader_contract.cache_epoch,
                 "expired material layout fixture did not retain matching metadata with a retired actual owner"))
        std::exit(40);
    bool live_and_shape_contracts_rejected = true;
    variant_context.material_shader_layout = &expired_material_layout;
    const auto expired_owner_result = resolver.resolve_material(material_handle.value);
    variant_context.material_shader_layout = &shader_layout.value;
    if (expired_owner_result) {
        std::cerr << "Runtime accepted material layout whose actual ShaderManager owner expired\n";
        live_and_shape_contracts_rejected = false;
    }
    const auto malformed_color_shape = [](renderer::RenderMaterialAttachmentContract attachment, unsigned bad) {
        switch (bad) {
            case 0: attachment.additional_color_formats[0] = rhi::Format::kRgba8Unorm; break;
            case 1: attachment.additional_color_formats[1] = rhi::Format::kRgba8Unorm; break;
            case 2: attachment.color_format = rhi::Format::kUnknown; break;
            case 3: attachment.color_format = rhi::Format::kD32Float; break;
            case 4: attachment.samples = static_cast<rhi::SampleCount>(0); break;
            case 5: attachment.depth_stencil_format = rhi::Format::kRgba8Unorm; break;
            case 6: attachment.color_format = rhi::Format::kBc1RgbaUnorm; break;
            case 7: attachment.color_format = static_cast<rhi::Format>(0xffff); break;
        }
        return attachment;
    };
    const auto valid_base_attachment = variant_context.material_attachment_contract;
    for (unsigned bad = 0; bad < 8; ++bad) {
        variant_context.material_attachment_contract = malformed_color_shape(valid_base_attachment, bad);
        if (resolver.resolve_material(material_handle.value)) {
            std::cerr << "Runtime accepted malformed single-color base attachment shape: " << bad << '\n';
            live_and_shape_contracts_rejected = false;
        }
    }
    variant_context.material_attachment_contract = valid_base_attachment;
    for (unsigned bad = 0; bad < 8; ++bad) {
        variant_context.shadow_pipeline_contract = malformed_color_shape(color_depth_attachment, bad);
        if (resolver.resolve_material(material_handle.value)) {
            std::cerr << "Runtime accepted malformed single-color ForwardShadow attachment shape: " << bad << '\n';
            live_and_shape_contracts_rejected = false;
        }
    }
    variant_context.shadow_pipeline_contract = color_depth_attachment;
    for (unsigned bad = 0; bad < 8; ++bad) {
        variant_context.depth_attachment_variant = renderer::RenderMaterialPipelineVariant{
            color_depth_pipeline.value, malformed_color_shape(color_depth_attachment, bad),
            variant_context.material_shader_contract, {}, initial_variant_epoch};
        if (resolver.resolve_material(material_handle.value)) {
            std::cerr << "Runtime accepted malformed single-color depth variant attachment shape: " << bad << '\n';
            live_and_shape_contracts_rejected = false;
        }
    }
    variant_context.depth_attachment_variant.reset();
    if (!live_and_shape_contracts_rejected)
        std::exit(40); // Record the independent live-owner/shape regression without active-frame unwinding.
    const auto valid_material_shader_contract = variant_context.material_shader_contract;
    variant_context.material_shader_contract.layout_hash ^= 1u;
    resolver.set_upload_context(variant_context);
    const auto mismatched_shader_layout = resolver.resolve_material(material_handle.value);
    if (!require(!mismatched_shader_layout &&
                     mismatched_shader_layout.result.code == foundation::ResultCode::kInvalidArgument &&
                     mismatched_shader_layout.result.message.find("shader layout") != std::string::npos,
                 "Runtime material accepted a mismatching shader layout contract")) {
        (void)frame.abort_frame();
        (void)resolver.retire_gpu_resources();
        return 29;
    }
    variant_context.material_shader_contract = valid_material_shader_contract;
    resolver.set_upload_context(variant_context);
    const auto active_retirement = resolver.retire_gpu_resources();
    if (!require(!active_retirement && active_retirement.code == foundation::ResultCode::kInvalidState,
                 "material Vulkan retirement unexpectedly succeeded during an active frame"))
        return 14;
    const auto active_again = resolver.resolve_material(material_handle.value);
    if (!require(active_again && active_again.value.binding_set == first.value.binding_set &&
                     active_again.value.material_parameter_buffer == first.value.material_parameter_buffer,
                 "material Vulkan resources were lost after active-frame retirement failure"))
        return 14;
    const auto again = resolver.resolve_material(material_handle.value);
    if (!require(
            again && again.value.binding_set == first.value.binding_set &&
                again.value.material_parameter_buffer == first.value.material_parameter_buffer &&
                again.value.material_parameter_resource.graph_id == first.value.material_parameter_resource.graph_id,
            "material Vulkan same-frame resolve did not reuse resources"))
        return 15;
    const auto first_graph_resource = first.value.material_parameter_resource;
    variant_context.clustered_pipeline = clustered_replacement.value;
    variant_context.unshadowed_clustered_variant = renderer::RenderMaterialPipelineVariant{
        unshadowed_replacement.value, clustered_contract, variant_context.material_shader_contract,
        unshadowed_profile_contract};
    resolver.set_upload_context(variant_context);
    const auto replaced_clustered = resolver.resolve_material(material_handle.value);
    if (!require(replaced_clustered && replaced_clustered.value.clustered_pipeline == clustered_replacement.value &&
        replaced_clustered.value.clustered_pipeline_contract == clustered_contract &&
        replaced_clustered.value.unshadowed_clustered_variant &&
        replaced_clustered.value.unshadowed_clustered_variant->pipeline == unshadowed_replacement.value &&
        replaced_clustered.value.unshadowed_clustered_variant->attachment_contract == clustered_contract &&
        replaced_clustered.value.binding_set == first.value.binding_set &&
        replaced_clustered.value.material_parameter_buffer == first.value.material_parameter_buffer,
        "warm material lost actual clustered replacement or duplicated resources")) return 30;
    variant_context.clustered_pipeline = {};
    variant_context.clustered_pipeline_contract = {};
    variant_context.unshadowed_clustered_variant.reset();
    resolver.set_upload_context(variant_context);
    const auto removed_clustered = resolver.resolve_material(material_handle.value);
    if (!require(removed_clustered && !removed_clustered.value.clustered_pipeline.is_valid() &&
        removed_clustered.value.clustered_pipeline_contract == renderer::RenderMaterialAttachmentContract{} &&
        !removed_clustered.value.unshadowed_clustered_variant &&
        removed_clustered.value.binding_set == first.value.binding_set &&
        removed_clustered.value.material_parameter_buffer == first.value.material_parameter_buffer,
        "warm material retained removed clustered metadata or duplicated resources")) return 31;
    variant_context.gbuffer_pipeline = {};
    const auto detached_gbuffer = resolver.resolve_material(material_handle.value);
    if (!detached_gbuffer || detached_gbuffer.value.gbuffer_pipeline.is_valid() ||
        detached_gbuffer.value.gbuffer_pipeline_contract != renderer::RenderMaterialAttachmentContract{} ||
        detached_gbuffer.value.gbuffer_profile_contract != renderer::RendererProfilePsoContract{}) {
        std::cerr << "Removed GBuffer PSO retained stale attachment/profile metadata\n";
        std::exit(38);
    }
    variant_context.gbuffer_pipeline_contract = {};
    variant_context.gbuffer_profile_contract = {};
    variant_context.gbuffer_shader_layout = nullptr;
    resolver.set_upload_context(variant_context);
    const auto removed_gbuffer = resolver.resolve_material(material_handle.value);
    if (!require(removed_gbuffer && !removed_gbuffer.value.gbuffer_pipeline.is_valid() &&
        removed_gbuffer.value.gbuffer_pipeline_contract == renderer::RenderMaterialAttachmentContract{} &&
        removed_gbuffer.value.gbuffer_profile_contract == renderer::RendererProfilePsoContract{} &&
        removed_gbuffer.value.binding_set == first.value.binding_set &&
        removed_gbuffer.value.material_parameter_buffer == first.value.material_parameter_buffer,
        "warm material retained removed GBuffer metadata or duplicated resources")) return 25;
    variant_context.gbuffer_pipeline = gbuffer_pipeline.value;
    variant_context.gbuffer_pipeline_contract = gbuffer_contract;
    variant_context.gbuffer_profile_contract = gbuffer_profile;
    variant_context.gbuffer_shader_layout = &gbuffer_layout.value;
    variant_context.depth_attachment_variant = renderer::RenderMaterialPipelineVariant{
        color_depth_pipeline.value, color_depth_attachment, variant_context.material_shader_contract, {},
        initial_variant_epoch};
    variant_context.depth_pipeline = depth_pipeline.value;
    variant_context.depth_pipeline_contract = depth_attachment;
    variant_context.depth_profile_contract = depth_profile;
    variant_context.depth_shader_layout = &depth_layout.value;
    // A warm material must validate the currently advertised host contract,
    // before creating uploads or returning cached descriptors.
    for (unsigned bad = 0; bad != 12; ++bad) {
        variant_context.depth_profile_contract = depth_profile;
        variant_context.depth_shader_layout = &depth_layout.value;
        switch (bad) {
            case 0: variant_context.depth_profile_contract = {}; break;
            case 1: variant_context.depth_profile_contract.shader.profile = renderer::RendererProfile::kPostProcess; break;
            case 2: ++variant_context.depth_profile_contract.shader.version; break;
            case 3: variant_context.depth_profile_contract.shader.layout_hash = 0; break;
            case 4: ++variant_context.depth_profile_contract.shader.layout_hash; break;
            case 5: variant_context.depth_profile_contract.shader.cache_epoch = 0; break;
            case 6: variant_context.depth_profile_contract.cache_epoch = 0; break;
            case 7: ++variant_context.depth_profile_contract.cache_epoch; break;
            case 8: ++variant_context.depth_profile_contract.shader.cache_epoch; ++variant_context.depth_profile_contract.cache_epoch; break;
            case 9: variant_context.depth_profile_contract.attachment.color_format = rhi::Format::kRgba8Unorm; break;
            case 10: variant_context.depth_profile_contract.attachment.depth_stencil_format = rhi::Format::kD24UnormS8Uint; break;
            case 11: variant_context.depth_shader_layout = nullptr; break;
        }
        const auto rejected = resolver.resolve_material(material_handle.value);
        if (rejected) {
            std::cerr << "Runtime accepted a malformed/stale advertised depth profile contract: " << bad << '\n';
            std::exit(36); // Preserve the rejection oracle before the borrowed active-frame host unwinds.
        }
    }
    variant_context.depth_profile_contract = depth_profile;
    variant_context.depth_shader_layout = &depth_layout.value;
    variant_context.shadow_pipeline = replacement_shadow_pipeline.value;
    variant_context.shadow_pipeline_contract = replacement_color_depth_attachment;
    resolver.set_upload_context(variant_context);
    const auto with_depth = resolver.resolve_material(material_handle.value);
    if (!require(with_depth && with_depth.value.depth_attachment_variant &&
                     with_depth.value.depth_pipeline == depth_pipeline.value &&
                     with_depth.value.depth_profile_contract == depth_profile &&
                     with_depth.value.depth_pipeline_contract == variant_context.depth_pipeline_contract &&
                     with_depth.value.shadow_pipeline == replacement_shadow_pipeline.value &&
                     with_depth.value.shadow_pipeline_contract == variant_context.shadow_pipeline_contract &&
                     with_depth.value.shadow_shader_contract == shadow_shader_contract &&
                     with_depth.value.shadow_pipeline_cache_epoch == initial_variant_epoch &&
                     with_depth.value.gbuffer_pipeline == gbuffer_pipeline.value && with_depth.value.gbuffer_pipeline_contract == gbuffer_contract &&
                     with_depth.value.depth_attachment_variant->pipeline == color_depth_pipeline.value &&
                     with_depth.value.depth_attachment_variant->shader_contract == variant_context.material_shader_contract &&
                     with_depth.value.depth_attachment_variant->cache_epoch == initial_variant_epoch &&
                     with_depth.value.depth_attachment_variant->attachment_contract ==
                         variant_context.depth_attachment_variant->attachment_contract &&
                     with_depth.value.binding_set == first.value.binding_set &&
                     with_depth.value.material_parameter_buffer == first.value.material_parameter_buffer,
                 "warm material resolve did not adopt depth variant without duplicating resources")) return 21;
    variant_context.gbuffer_pipeline = replacement_pipeline.value;
    resolver.set_upload_context(variant_context);
    const auto replaced_gbuffer = resolver.resolve_material(material_handle.value);
    if (!require(replaced_gbuffer && replaced_gbuffer.value.gbuffer_pipeline == replacement_pipeline.value &&
        replaced_gbuffer.value.gbuffer_pipeline_contract == gbuffer_contract &&
        replaced_gbuffer.value.binding_set == first.value.binding_set &&
        replaced_gbuffer.value.material_parameter_buffer == first.value.material_parameter_buffer,
        "warm material lost real GBuffer replacement or duplicated resources")) return 26;
    variant_context.gbuffer_pipeline = {};
    variant_context.gbuffer_pipeline_contract = {};
    variant_context.gbuffer_profile_contract = {};
    variant_context.gbuffer_shader_layout = nullptr;
    variant_context.depth_attachment_variant.reset();
    variant_context.depth_pipeline = {};
    const auto detached_depth = resolver.resolve_material(material_handle.value);
    if (!detached_depth || detached_depth.value.depth_pipeline.is_valid() ||
        detached_depth.value.depth_pipeline_contract != renderer::RenderMaterialAttachmentContract{} ||
        detached_depth.value.depth_profile_contract != renderer::RendererProfilePsoContract{}) {
        std::cerr << "Removed depth PSO retained stale attachment/profile metadata\n";
        std::exit(36);
    }
    variant_context.depth_pipeline_contract = {};
    variant_context.depth_profile_contract = {};
    variant_context.depth_shader_layout = nullptr;
    variant_context.shadow_pipeline = {};
    variant_context.shadow_shader_layout = &invalid_shadow_layout;
    const auto detached_shadow = resolver.resolve_material(material_handle.value);
    if (!detached_shadow || detached_shadow.value.shadow_pipeline.is_valid() ||
        detached_shadow.value.shadow_pipeline_contract != renderer::RenderMaterialAttachmentContract{} ||
        detached_shadow.value.shadow_shader_contract != renderer::RenderMaterialShaderContract{} ||
        detached_shadow.value.shadow_pipeline_cache_epoch != 0 ||
        detached_shadow.value.binding_set != first.value.binding_set ||
        detached_shadow.value.material_parameter_buffer != first.value.material_parameter_buffer) {
        std::cerr << "Removed ForwardShadow PSO retained metadata or consulted its inactive layout\n";
        std::exit(39);
    }
    variant_context.shadow_pipeline_contract = {};
    variant_context.shadow_shader_contract = {};
    variant_context.shadow_pipeline_cache_epoch = 0;
    variant_context.shadow_shader_layout = nullptr;
    resolver.set_upload_context(variant_context);
    const auto without_depth = resolver.resolve_material(material_handle.value);
    if (!require(without_depth && !without_depth.value.depth_attachment_variant &&
                     !without_depth.value.depth_pipeline.is_valid() &&
                     without_depth.value.depth_pipeline_contract == renderer::RenderMaterialAttachmentContract{} &&
                     without_depth.value.depth_profile_contract == renderer::RendererProfilePsoContract{} &&
                     !without_depth.value.gbuffer_pipeline.is_valid() &&
                     without_depth.value.gbuffer_pipeline_contract == renderer::RenderMaterialAttachmentContract{} &&
                     !without_depth.value.shadow_pipeline.is_valid() &&
                     without_depth.value.shadow_pipeline_contract == renderer::RenderMaterialAttachmentContract{} &&
                     without_depth.value.shadow_shader_contract == renderer::RenderMaterialShaderContract{} &&
                     without_depth.value.shadow_pipeline_cache_epoch == 0 &&
                     without_depth.value.binding_set == first.value.binding_set &&
                     without_depth.value.material_parameter_buffer == first.value.material_parameter_buffer,
                 "warm material resolve retained a removed depth variant")) return 22;
    if (!require(graph.compile(), "material Vulkan graph compilation failed"))
        return 16;
    const auto execution = graph.execute(frame);
    if (!require(execution && execution.value.executed_pass_count > 0 && execution.value.submitted_batch_count > 0,
                 "material Vulkan graph did not execute the texture upload pass"))
        return 16;
    if (execution.value.completion.is_valid() &&
        !require(device->wait(execution.value.completion, std::numeric_limits<uint64_t>::max()),
                 "material Vulkan texture upload completion wait failed"))
        return 16;
    if (!require(frame.end_frame(), "material Vulkan first frame end failed"))
        return 17;
    resource::TextureMaterialParameters parameters{};
    if (!require(device->read_buffer(first.value.material_parameter_buffer, first.value.material_parameter_offset,
                                     std::as_writable_bytes(std::span{&parameters, 1})),
                 "material Vulkan parameter readback failed") ||
        !require(parameters.tint == std::array<float, 4>{0.5f, 0.75f, 0.25f, 1.0f},
                 "material Vulkan authored Tint was not packed into the uniform buffer"))
        return 17;

    graph.reset();
    variant_context.clustered_pipeline = clustered_replacement.value;
    variant_context.clustered_pipeline_contract = clustered_contract;
    variant_context.unshadowed_clustered_variant = renderer::RenderMaterialPipelineVariant{
        unshadowed_replacement.value, clustered_contract, variant_context.material_shader_contract,
        unshadowed_profile_contract};
    variant_context.gbuffer_pipeline = gbuffer_pipeline.value;
    variant_context.gbuffer_pipeline_contract = gbuffer_contract;
    variant_context.gbuffer_profile_contract = gbuffer_profile;
    variant_context.gbuffer_shader_layout = &gbuffer_layout.value;
    variant_context.shadow_pipeline = replacement_shadow_pipeline.value;
    variant_context.shadow_pipeline_contract = replacement_color_depth_attachment;
    variant_context.shadow_shader_contract = shadow_shader_contract;
    variant_context.shadow_pipeline_cache_epoch = initial_variant_epoch;
    variant_context.shadow_shader_layout = &shadow_layout.value;
    variant_context.depth_pipeline = depth_pipeline.value;
    variant_context.depth_pipeline_contract = depth_attachment;
    variant_context.depth_profile_contract = depth_profile;
    variant_context.depth_shader_layout = &depth_layout.value;
    variant_context.depth_attachment_variant = renderer::RenderMaterialPipelineVariant{
        replacement_color_depth_pipeline.value, replacement_color_depth_attachment,
        variant_context.material_shader_contract, {}, initial_variant_epoch};
    resolver.set_upload_context(variant_context);
    if (!require(frame.begin_frame(), "material Vulkan graph-reset frame begin failed"))
        return 18;
    const auto next = resolver.resolve_material(material_handle.value);
    if (!require(next && next.value.clustered_pipeline == clustered_replacement.value &&
        next.value.clustered_pipeline_contract == clustered_contract && next.value.unshadowed_clustered_variant &&
        next.value.unshadowed_clustered_variant->pipeline == unshadowed_replacement.value &&
        next.value.unshadowed_clustered_variant->attachment_contract == clustered_contract &&
        next.value.binding_set == first.value.binding_set &&
        next.value.material_parameter_buffer == first.value.material_parameter_buffer,
        "graph-reset material lost restored clustered metadata or duplicated resources")) return 32;
    if (!require(next && next.value.depth_attachment_variant &&
                     next.value.depth_pipeline == depth_pipeline.value &&
                     next.value.depth_profile_contract == depth_profile &&
                     next.value.depth_pipeline_contract == variant_context.depth_pipeline_contract &&
                     next.value.gbuffer_pipeline == gbuffer_pipeline.value && next.value.gbuffer_pipeline_contract == gbuffer_contract &&
                     next.value.shadow_pipeline == replacement_shadow_pipeline.value &&
                     next.value.shadow_pipeline_contract == variant_context.shadow_pipeline_contract &&
                     next.value.shadow_shader_contract == shadow_shader_contract &&
                     next.value.shadow_pipeline_cache_epoch == initial_variant_epoch &&
                     next.value.depth_attachment_variant->pipeline == replacement_color_depth_pipeline.value &&
                     next.value.depth_attachment_variant->shader_contract == variant_context.material_shader_contract &&
                     next.value.depth_attachment_variant->attachment_contract ==
                         variant_context.depth_attachment_variant->attachment_contract &&
                     next.value.binding_set == first.value.binding_set &&
                     next.value.material_parameter_buffer == first.value.material_parameter_buffer,
                 "graph-reset material resolve lost replacement depth variant or reused resources")) return 23;
    if (!require(next && next.value.material_parameter_buffer == first.value.material_parameter_buffer &&
                     next.value.material_parameter_resource.is_valid() &&
                     next.value.material_parameter_resource != first_graph_resource,
                 "material Vulkan graph reset did not re-import a new generation"))
        return 19;
    // A new descriptor-compatible host contract must not reuse material
    // descriptors or parameter buffers cached under the prior epoch, even
    // when the shader/layout values happen to remain unchanged.
    const auto previous_epoch_binding_set = next.value.binding_set;
    const auto previous_epoch_parameter_buffer = next.value.material_parameter_buffer;
    ++variant_context.material_contract_epoch;
    resolver.set_upload_context(variant_context);
    const auto epoch_changed = resolver.resolve_material(material_handle.value);
    if (!require(epoch_changed && epoch_changed.value.binding_set != previous_epoch_binding_set &&
                     epoch_changed.value.material_parameter_buffer != previous_epoch_parameter_buffer,
                 "material contract epoch reused stale GPU descriptors"))
        return 33;
    const auto matching_shader_contract = variant_context.material_shader_contract;
    ++variant_context.material_shader_contract.cache_epoch;
    resolver.set_upload_context(variant_context);
    const auto retired_layout = resolver.resolve_material(material_handle.value);
    if (!require(!retired_layout && retired_layout.result.code == foundation::ResultCode::kInvalidArgument &&
                     retired_layout.result.message.find("retired cache epoch") != std::string::npos,
                 "Runtime accepted a material layout from a retired ShaderManager cache epoch"))
        return 35;
    variant_context.material_shader_contract = matching_shader_contract;
    const auto previous_variant_binding_set = epoch_changed.value.binding_set;
    const auto previous_variant_parameter_buffer = epoch_changed.value.material_parameter_buffer;
    // Recreate a real depth layout/PSO in the next cache generation; stamping
    // the old shader metadata with the new host epoch must never be accepted.
    shader::ShaderManager next_depth_shaders(*device);
    if (!require(next_depth_shaders.clear_layout_cache(), "next depth cache generation failed")) return 36;
    const auto next_depth_layout = next_depth_shaders.build_pipeline_layout(depth_program.value);
    if (!require(next_depth_layout, "next depth layout failed")) return 36;
    const auto next_depth_pipeline = next_depth_shaders.create_pipeline(depth_program.value, next_depth_layout.value,
        {rhi::PipelineType::kGraphics, depth_graphics});
    if (!require(next_depth_pipeline, "next depth PSO failed")) return 36;
    const auto next_gbuffer_layout = next_depth_shaders.build_pipeline_layout(gbuffer_program.value);
    if (!require(next_gbuffer_layout, "next GBuffer layout failed")) return 38;
    const auto next_gbuffer_pipeline = next_depth_shaders.create_pipeline(gbuffer_program.value, next_gbuffer_layout.value,
        {rhi::PipelineType::kGraphics, gbuffer_graphics});
    if (!require(next_gbuffer_pipeline, "next GBuffer PSO failed")) return 38;
    ++variant_context.variant_cache_epoch;
    const auto previous_depth_attachment_variant = variant_context.depth_attachment_variant;
    variant_context.depth_attachment_variant.reset();
    variant_context.depth_pipeline = {};
    variant_context.gbuffer_pipeline = {};
    if (resolver.resolve_material(material_handle.value)) {
        std::cerr << "Host concrete epoch change accepted the old ForwardShadow PSO\n";
        std::exit(39);
    }
    const auto next_shadow_pipeline = shader_manager.create_pipeline(shadow_program.value, shadow_layout.value,
        {rhi::PipelineType::kGraphics, replacement_color_depth_graphics, {"Runtime.ForwardShadow.NextPsoEpoch"}});
    if (!require(next_shadow_pipeline && next_shadow_pipeline.value != replacement_shadow_pipeline.value,
                 "next concrete-epoch ForwardShadow PSO was not recreated")) return 39;
    variant_context.shadow_pipeline = next_shadow_pipeline.value;
    variant_context.shadow_pipeline_cache_epoch = variant_context.variant_cache_epoch;
    variant_context.depth_pipeline = depth_pipeline.value;
    variant_context.gbuffer_pipeline = gbuffer_pipeline.value;
    if (!require(!resolver.resolve_material(material_handle.value), "Host epoch change accepted the old depth PSO")) return 36;
    variant_context.depth_pipeline = next_depth_pipeline.value;
    variant_context.depth_shader_layout = &next_depth_layout.value;
    variant_context.depth_profile_contract = {
        {renderer::kRendererProfileContractVersion, renderer::RendererProfile::kShadow,
         shader::detail::ShaderPipelineLayoutAccess::contract_hash(next_depth_layout.value),
         shader::detail::ShaderPipelineLayoutAccess::cache_epoch(next_depth_layout.value)},
        depth_attachment, shader::detail::ShaderPipelineLayoutAccess::cache_epoch(next_depth_layout.value)};
    if (resolver.resolve_material(material_handle.value)) {
        std::cerr << "Host epoch change accepted the old GBuffer PSO\n";
        std::exit(38);
    }
    variant_context.gbuffer_pipeline = next_gbuffer_pipeline.value;
    variant_context.gbuffer_shader_layout = &next_gbuffer_layout.value;
    variant_context.gbuffer_profile_contract = {
        {renderer::kRendererProfileContractVersion, renderer::RendererProfile::kGBuffer,
         shader::detail::ShaderPipelineLayoutAccess::contract_hash(next_gbuffer_layout.value),
         shader::detail::ShaderPipelineLayoutAccess::cache_epoch(next_gbuffer_layout.value)},
        gbuffer_contract, shader::detail::ShaderPipelineLayoutAccess::cache_epoch(next_gbuffer_layout.value)};
    variant_context.depth_attachment_variant = previous_depth_attachment_variant;
    if (resolver.resolve_material(material_handle.value)) {
        std::cerr << "Host concrete epoch change accepted the old color/depth variant PSO\n";
        std::exit(39);
    }
    const auto next_color_depth_pipeline = shader_manager.create_pipeline(shader_program, shader_layout.value,
        {rhi::PipelineType::kGraphics, replacement_color_depth_graphics, {"Runtime.Material.NextColorDepthPsoEpoch"}});
    if (!require(next_color_depth_pipeline && next_color_depth_pipeline.value != replacement_color_depth_pipeline.value,
                 "next concrete-epoch color/depth PSO was not recreated")) return 39;
    variant_context.depth_attachment_variant = renderer::RenderMaterialPipelineVariant{
        next_color_depth_pipeline.value, replacement_color_depth_attachment, variant_context.material_shader_contract, {},
        variant_context.variant_cache_epoch};
    resolver.set_upload_context(variant_context);
    const auto variant_epoch_changed = resolver.resolve_material(material_handle.value);
    if (!require(variant_epoch_changed && variant_epoch_changed.value.binding_set != previous_variant_binding_set &&
                     variant_epoch_changed.value.material_parameter_buffer != previous_variant_parameter_buffer &&
                     variant_epoch_changed.value.variant_cache_epoch == variant_context.variant_cache_epoch &&
                     variant_epoch_changed.value.depth_pipeline == next_depth_pipeline.value &&
                     variant_epoch_changed.value.depth_profile_contract == variant_context.depth_profile_contract &&
                     variant_epoch_changed.value.gbuffer_pipeline == next_gbuffer_pipeline.value &&
                     variant_epoch_changed.value.gbuffer_profile_contract == variant_context.gbuffer_profile_contract &&
                     variant_epoch_changed.value.shadow_pipeline == next_shadow_pipeline.value &&
                     variant_epoch_changed.value.shadow_shader_contract == shadow_shader_contract &&
                     variant_epoch_changed.value.shadow_shader_contract.cache_epoch == 1 &&
                     variant_epoch_changed.value.shadow_pipeline_cache_epoch == 2 &&
                     variant_epoch_changed.value.depth_attachment_variant &&
                     variant_epoch_changed.value.depth_attachment_variant->pipeline == next_color_depth_pipeline.value &&
                     variant_epoch_changed.value.depth_attachment_variant->shader_contract == variant_context.material_shader_contract &&
                     variant_epoch_changed.value.depth_attachment_variant->shader_contract.cache_epoch == 1 &&
                     variant_epoch_changed.value.depth_attachment_variant->cache_epoch == 2,
                 "Shader/PSO variant cache epoch reused stale GPU descriptors"))
        return 34;
    ++variant_context.resource_cache_epoch;
    const auto same_host_generation = resolver.set_upload_context(variant_context);
    if (!require(!same_host_generation && same_host_generation.code == foundation::ResultCode::kInvalidState,
                 "Runtime resolver accepted a mutated same-pointer host generation with live GPU resources"))
        return 37;
    if (!require(frame.end_frame(), "material Vulkan graph-reset frame end failed"))
        return 20;

    render_graph::FrameContext replacement_frame(*device, {.frames_in_flight = 2});
    render_graph::RenderGraph replacement_graph(*device);
    auto replacement_context = variant_context;
    replacement_context.frame_context = &replacement_frame;
    replacement_context.graph = &replacement_graph;
    const auto active_host_clear = resolver.set_upload_context({});
    if (!require(!active_host_clear && active_host_clear.code == foundation::ResultCode::kInvalidState,
                 "Runtime material resolver cleared an upload host with live GPU resources"))
        return 36;
    const auto active_host_rebind = resolver.set_upload_context(replacement_context);
    if (!require(!active_host_rebind && active_host_rebind.code == foundation::ResultCode::kInvalidState,
                 "Runtime material resolver replaced an upload host with live GPU resources"))
        return 36;
    if (!require(resolver.retire_gpu_resources(), "material Vulkan resource retirement failed"))
        return 20;
    if (!require(resolver.set_upload_context(replacement_context),
                 "Runtime material resolver did not accept a new host after retirement"))
        return 36;

    resolver.set_upload_context({});
    const auto missing_host = resolver.resolve_material(material_handle.value);
    if (!require(!missing_host && missing_host.result.code == foundation::ResultCode::kNotReady,
                 "material Vulkan missing host shader was not rejected"))
        return 21;
    auto mismatch_context = application::runtime::RuntimeAssetResolver::UploadContext{};
    mismatch_context.material_shader = wrong_shader_id.value;
    resolver.set_upload_context(mismatch_context);
    const auto mismatched_host = resolver.resolve_material(material_handle.value);
    if (!require(!mismatched_host && mismatched_host.result.code == foundation::ResultCode::kInvalidArgument,
                 "material Vulkan mismatched host shader was not rejected"))
        return 22;

    std::error_code error;
    if (!require(device->destroy_pipeline(next_shadow_pipeline.value), "next ForwardShadow PSO retirement failed") ||
        !require(device->destroy_pipeline(next_color_depth_pipeline.value), "next color/depth PSO retirement failed") ||
        !require(device->destroy_pipeline(replacement_shadow_pipeline.value), "replacement ForwardShadow retirement failed") ||
        !require(device->destroy_pipeline(shadow_pipeline.value), "ForwardShadow retirement failed") ||
        !require(device->destroy_pipeline(replacement_color_depth_pipeline.value), "replacement color/depth PSO retirement failed") ||
        !require(device->destroy_pipeline(color_depth_pipeline.value), "color/depth PSO retirement failed") ||
        !require(device->destroy_pipeline(next_gbuffer_pipeline.value), "next GBuffer PSO retirement failed") ||
        !require(device->destroy_pipeline(next_depth_pipeline.value), "next depth PSO retirement failed") ||
        !require(device->destroy_pipeline(depth_pipeline.value), "depth PSO retirement failed") ||
        !require(device->destroy_pipeline(clustered.value), "clustered pipeline retirement failed") ||
        !require(device->destroy_pipeline(unshadowed.value), "unshadowed pipeline retirement failed") ||
        !require(device->destroy_pipeline(clustered_replacement.value), "replacement clustered retirement failed") ||
        !require(device->destroy_pipeline(unshadowed_replacement.value), "replacement unshadowed retirement failed") ||
        !require(device->destroy_pipeline(replacement_pipeline.value), "replacement GBuffer pipeline retirement failed") ||
        !require(device->destroy_pipeline(gbuffer_pipeline.value), "GBuffer pipeline retirement failed") ||
        !require(device->destroy_pipeline(pipeline.value), "material pipeline retirement failed")) return 27;
    std::filesystem::remove(package_path, error);
    std::cout << "runtime material Vulkan resolution passed\n";
    return 0;
}
