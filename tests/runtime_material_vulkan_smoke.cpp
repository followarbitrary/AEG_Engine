/** @file runtime_material_vulkan_smoke.cpp
 *  @brief Validates the cooked material to Vulkan TextureMaterial ABI path.
 */
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
#include "shader_contract.hpp"
#include "texture_uploader.hpp"

namespace {
using namespace aeg;

template <typename T>
void append(std::vector<std::byte>& bytes, T value) {
    const auto* first = reinterpret_cast<const std::byte*>(&value);
    bytes.insert(bytes.end(), first, first + sizeof(value));
}

void append_uuid(std::vector<std::byte>& bytes, asset::AssetId id) {
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

std::vector<std::byte> material_payload(asset::AssetId shader, asset::AssetId texture) {
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
    const auto material_id = asset::AssetId::parse("d0112233-4455-6677-8899-aabbccddeeff");
    const auto texture_id = asset::AssetId::parse("d1112233-4455-6677-8899-aabbccddeeff");
    const auto shader_id = asset::AssetId::parse("d2112233-4455-6677-8899-aabbccddeeff");
    const auto wrong_shader_id = asset::AssetId::parse("d3112233-4455-6677-8899-aabbccddeeff");
    if (!require(material_id && texture_id && shader_id && wrong_shader_id, "material Vulkan fixture IDs invalid"))
        return 1;

    const auto cooked_material = asset::encode_cooked_asset(
        {material_id.value, asset::AssetKind::kMaterial, 1, {}, material_payload(shader_id.value, texture_id.value)});
    const auto cooked_texture =
        asset::encode_cooked_asset({texture_id.value, asset::AssetKind::kTexture, 1, {}, texture_payload()});
    if (!require(cooked_material && cooked_texture, "material Vulkan fixture cook failed"))
        return 2;

    const auto package_path = std::filesystem::temp_directory_path() / "aeg_runtime_material_vulkan.aegbin";
    const std::vector<tooling::AssetPackageEntry> entries{
        {material_id.value, asset::AssetKind::kMaterial, 1, cooked_material.value},
        {texture_id.value, asset::AssetKind::kTexture, 1, cooked_texture.value},
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
    resolver.set_upload_context({.device = device.get(),
                                 .frame_context = &frame,
                                 .graph = &graph,
                                 .uploader = &uploader,
                                 .texture_uploader = &texture_uploader,
                                 .material_binding_layout = material_layout.value,
                                 .material_pipeline = pipeline.value,
                                 .material_shader = shader_id.value,
                                 .material_shader_program = &shader_program,
                                 .material_sampler = sampler,
                                 .material_binding_group = 1,
                                 .shadow_pipeline = pipeline.value,
                                 .clustered_pipeline = pipeline.value});

    if (!require(frame.begin_frame(), "material Vulkan first frame begin failed"))
        return 12;
    const auto first = resolver.resolve_material(material_handle.value);
    if (!require(first && first.value.is_valid() && first.value.material_parameter_buffer.is_valid() &&
                     first.value.material_parameter_size == sizeof(resource::TextureMaterialParameters) &&
                     first.value.material_parameter_resource.is_valid() &&
                     first.value.shadow_pipeline == pipeline.value && first.value.clustered_pipeline == pipeline.value,
                 "material Vulkan first resolve failed"))
        return 13;
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
    if (!require(frame.begin_frame(), "material Vulkan graph-reset frame begin failed"))
        return 18;
    const auto next = resolver.resolve_material(material_handle.value);
    if (!require(next && next.value.material_parameter_buffer == first.value.material_parameter_buffer &&
                     next.value.material_parameter_resource.is_valid() &&
                     next.value.material_parameter_resource != first_graph_resource,
                 "material Vulkan graph reset did not re-import a new generation"))
        return 19;
    if (!require(frame.end_frame(), "material Vulkan graph-reset frame end failed") ||
        !require(resolver.retire_gpu_resources(), "material Vulkan resource retirement failed"))
        return 20;

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
    std::filesystem::remove(package_path, error);
    std::cout << "runtime material Vulkan resolution passed\n";
    return 0;
}
