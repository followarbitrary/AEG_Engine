/**
 * @file rhi_smoke.cpp
 * @brief Exercises backend-neutral resource, binding, command, and sync APIs.
 */
#include <array>
#include <cstddef>
#include <limits>
#include <logging.hpp>
#include <rhi.hpp>
#include <span>
#include <string_view>

namespace {
    using aeg::foundation::ResultCode;

    bool check(const aeg::foundation::Result &result, std::string_view operation) {
        if (result) {
            return true;
        }
        AEG_LOG_ERROR(aeg::logging::Category::kTest, "{} failed [{}]: {}", operation,
                      aeg::foundation::result_code_name(result.code), result.message);
        return false;
    }

    template<typename T>
    bool check(const aeg::foundation::ResultValue<T> &result, std::string_view operation) {
        return check(result.result, operation);
    }

    bool expect_code(const aeg::foundation::Result &result, aeg::foundation::ResultCode expected,
                     std::string_view operation) {
        if (result.code == expected) {
            return true;
        }
        AEG_LOG_ERROR(aeg::logging::Category::kTest, "{} returned [{}] instead of [{}]: {}", operation,
                      aeg::foundation::result_code_name(result.code),
                      aeg::foundation::result_code_name(expected), result.message);
        return false;
    }

    struct LoggingGuard {
        ~LoggingGuard() { aeg::logging::shutdown(); }
    };

    constexpr std::array<uint32_t, 88> kComputeShader{
        0x07230203u, 0x00010000u, 0x000d000bu, 0x0000000au, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
        0x00000001u,
        0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u, 0x0005000fu,
        0x00000005u,
        0x00000004u, 0x6e69616du, 0x00000000u, 0x00060010u, 0x00000004u, 0x00000011u, 0x00000001u, 0x00000001u,
        0x00000001u,
        0x00030003u, 0x00000002u, 0x000001c2u, 0x000a0004u, 0x475f4c47u, 0x4c474f4fu, 0x70635f45u, 0x74735f70u,
        0x5f656c79u,
        0x656e696cu, 0x7269645fu, 0x69746365u, 0x00006576u, 0x00080004u, 0x475f4c47u, 0x4c474f4fu, 0x6e695f45u,
        0x64756c63u,
        0x69645f65u, 0x74636572u, 0x006576u, 0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00040047u,
        0x00000009u,
        0x0000000bu, 0x00000019u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00040015u,
        0x00000006u,
        0x00000020u, 0x00000000u, 0x00040017u, 0x00000007u, 0x00000006u, 0x00000003u, 0x0004002bu, 0x00000006u,
        0x00000008u,
        0x00000001u, 0x0006002cu, 0x00000007u, 0x00000009u, 0x00000008u, 0x00000008u, 0x00000008u, 0x00050036u,
        0x00000002u,
        0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u, 0x000100fdu, 0x00010038u,
    };
} // namespace

int main() {
    const aeg::logging::Status logging_status = aeg::logging::initialize();
    if (!logging_status) {
        return 2;
    }
    const LoggingGuard logging_guard{};

    using namespace aeg::rhi;

    DeviceCreateDesc device_desc{};
    device_desc.application_name = "AEG RHI Smoke";

    auto device_result = create_device(device_desc);
    if (!check(device_result, "create_device")) {
        return 1;
    }
    std::unique_ptr<Device> device = std::move(device_result.value);

    AEG_LOG_INFO(aeg::logging::Category::kTest, "GPU: {}", device->capabilities().adapter_name);

    BufferDesc buffer_desc{};
    buffer_desc.size = 256;
    buffer_desc.capabilities =
            BufferCapability::kUniform | BufferCapability::kTypedView | BufferCapability::kTransferDestination;
    buffer_desc.memory = MemoryClass::kUpload;
    buffer_desc.name = DebugName{"Smoke.Buffer"};
    auto buffer_result = device->create_buffer(buffer_desc);
    if (!check(buffer_result, "create_buffer")) {
        return 1;
    }
    const BufferHandle buffer = buffer_result.value;

    const std::array<std::byte, 16> initial_data{};
    if (!check(device->write_buffer(buffer, 0, initial_data), "write_buffer")) {
        return 1;
    }

    BufferViewDesc buffer_view_desc{};
    buffer_view_desc.buffer = buffer;
    buffer_view_desc.kind = BufferViewKind::kTyped;
    buffer_view_desc.size = 256;
    buffer_view_desc.format = Format::kR32Float;
    buffer_view_desc.name = DebugName{"Smoke.BufferView"};
    auto buffer_view_result = device->create_buffer_view(buffer_view_desc);
    if (!check(buffer_view_result, "create_buffer_view")) {
        return 1;
    }
    const BufferViewHandle buffer_view = buffer_view_result.value;
    if (!expect_code(device->destroy_buffer(buffer), ResultCode::kInvalidState, "destroy referenced buffer")) {
        return 1;
    }

    TextureDesc texture_desc{};
    texture_desc.extent = {4, 4, 1};
    texture_desc.format = Format::kRgba8Unorm;
    texture_desc.capabilities = TextureCapability::kSampled | TextureCapability::kTransferDestination;
    texture_desc.name = DebugName{"Smoke.Texture"};
    auto texture_result = device->create_texture(texture_desc);
    if (!check(texture_result, "create_texture")) {
        return 1;
    }
    const TextureHandle texture = texture_result.value;

    TextureViewDesc texture_view_desc{};
    texture_view_desc.texture = texture;
    texture_view_desc.kind = TextureViewKind::kSampled;
    texture_view_desc.name = DebugName{"Smoke.TextureView"};
    auto texture_view_result = device->create_texture_view(texture_view_desc);
    if (!check(texture_view_result, "create_texture_view")) {
        return 1;
    }
    const TextureViewHandle texture_view = texture_view_result.value;
    if (!expect_code(device->destroy_texture(texture), ResultCode::kInvalidState, "destroy referenced texture")) {
        return 1;
    }

    SamplerDesc sampler_desc{};
    sampler_desc.name = DebugName{"Smoke.Sampler"};
    auto sampler_result = device->create_sampler(sampler_desc);
    if (!check(sampler_result, "create_sampler")) {
        return 1;
    }
    const SamplerHandle sampler = sampler_result.value;

    std::array<BindingLayoutEntry, 4> layout_entries{};
    layout_entries[0].location = {0, 0};
    layout_entries[0].type = BindingType::kSampledTexture;
    layout_entries[0].shader_stages = ShaderStage::kFragment;
    layout_entries[1].location = {0, 1};
    layout_entries[1].type = BindingType::kSampler;
    layout_entries[1].shader_stages = ShaderStage::kFragment;
    layout_entries[2].location = {0, 2};
    layout_entries[2].type = BindingType::kUniformBuffer;
    layout_entries[2].shader_stages = ShaderStage::kVertex | ShaderStage::kFragment;
    layout_entries[3].location = {0, 3};
    layout_entries[3].type = BindingType::kReadOnlyTypedBuffer;
    layout_entries[3].shader_stages = ShaderStage::kFragment;

    BindingLayoutDesc layout_desc{};
    layout_desc.entries = layout_entries;
    layout_desc.name = DebugName{"Smoke.BindingLayout"};
    auto layout_result = device->create_binding_layout(layout_desc);
    if (!check(layout_result, "create_binding_layout")) {
        return 1;
    }
    const BindingLayoutHandle layout = layout_result.value;

    ShaderStageDesc compute_stage{};
    compute_stage.stage = ShaderStage::kCompute;
    compute_stage.code = std::as_bytes(std::span{kComputeShader});
    compute_stage.name = DebugName{"Smoke.ComputeShader"};
    const std::array<ShaderStageDesc, 1> pipeline_stages{compute_stage};
    const std::array<BindingLayoutHandle, 1> pipeline_layouts{layout};
    PipelineDesc pipeline_desc{};
    pipeline_desc.type = PipelineType::kCompute;
    pipeline_desc.layout.binding_layouts = pipeline_layouts;
    pipeline_desc.shader_stages = pipeline_stages;
    pipeline_desc.name = DebugName{"Smoke.ComputePipeline"};
    auto pipeline_result = device->create_pipeline(pipeline_desc);
    if (!check(pipeline_result, "create_compute_pipeline")) {
        return 1;
    }
    const PipelineHandle pipeline = pipeline_result.value;

    auto set_result = device->create_binding_set(layout);
    if (!check(set_result, "create_binding_set")) {
        return 1;
    }
    const BindingSetHandle set = set_result.value;

    std::array<BindingWrite, 4> writes{};
    writes[0].location = {0, 0};
    writes[0].resource.type = BindingType::kSampledTexture;
    writes[0].resource.texture_view = texture_view;
    writes[1].location = {0, 1};
    writes[1].resource.type = BindingType::kSampler;
    writes[1].resource.sampler = sampler;
    writes[2].location = {0, 2};
    writes[2].resource.type = BindingType::kUniformBuffer;
    writes[2].resource.buffer_range = {buffer, 0, 64};
    writes[3].location = {0, 3};
    writes[3].resource.type = BindingType::kReadOnlyTypedBuffer;
    writes[3].resource.buffer_view = buffer_view;
    if (!check(device->update_binding_set(set, writes), "update_binding_set")) {
        return 1;
    }
    if (!expect_code(device->destroy_texture_view(texture_view), ResultCode::kInvalidState,
                     "destroy bound texture view") ||
        !expect_code(device->destroy_buffer_view(buffer_view), ResultCode::kInvalidState,
                     "destroy bound buffer view") ||
        !expect_code(device->destroy_sampler(sampler), ResultCode::kInvalidState, "destroy bound sampler")) {
        return 1;
    }

    CommandListDesc command_desc{};
    command_desc.queue = QueueClass::kGraphics;
    command_desc.name = DebugName{"Smoke.GraphicsCommandList"};
    auto command_result = device->create_command_list(command_desc);
    if (!check(command_result, "create_command_list")) {
        return 1;
    }
    const CommandListHandle command_list = command_result.value;
    if (!check(device->begin_command_list(command_list), "begin_command_list") ||
        !check(device->bind_pipeline(command_list, pipeline), "bind_pipeline") ||
        !check(device->end_command_list(command_list), "end_command_list")) {
        return 1;
    }
    if (!expect_code(device->destroy_pipeline(pipeline), ResultCode::kInvalidState, "destroy bound pipeline")) {
        return 1;
    }

    const std::array<CommandListHandle, 1> command_lists{command_list};
    SubmitDesc submit_desc{};
    submit_desc.queue = QueueClass::kGraphics;
    submit_desc.command_lists = command_lists;
    auto submit_result = device->submit(submit_desc);
    if (!check(submit_result, "submit")) {
        return 1;
    }
    if (!check(device->wait(submit_result.value.completion, std::numeric_limits<uint64_t>::max()), "wait")) {
        return 1;
    }

    CommandListDesc copy_command_desc{};
    copy_command_desc.queue = QueueClass::kCopy;
    copy_command_desc.name = DebugName{"Smoke.CopyCommandList"};
    auto copy_command_result = device->create_command_list(copy_command_desc);
    if (!check(copy_command_result, "create_copy_command_list")) {
        return 1;
    }
    const CommandListHandle copy_command_list = copy_command_result.value;
    if (!check(device->begin_command_list(copy_command_list), "begin_copy_command_list") ||
        !check(device->end_command_list(copy_command_list), "end_copy_command_list")) {
        return 1;
    }
    const std::array<CommandListHandle, 1> copy_command_lists{copy_command_list};
    SubmitDesc copy_submit_desc{};
    copy_submit_desc.queue = QueueClass::kCopy;
    copy_submit_desc.command_lists = copy_command_lists;
    auto copy_submit_result = device->submit(copy_submit_desc);
    if (!check(copy_submit_result, "submit_copy")) {
        return 1;
    }

    BufferDesc cross_source_desc{};
    cross_source_desc.size = 256;
    cross_source_desc.capabilities = BufferCapability::kTransferSource | BufferCapability::kTransferDestination;
    cross_source_desc.memory = MemoryClass::kUpload;
    cross_source_desc.name = DebugName{"Smoke.CrossQueueSource"};
    auto cross_source_result = device->create_buffer(cross_source_desc);
    if (!check(cross_source_result, "create_cross_queue_source")) {
        return 1;
    }
    const BufferHandle cross_source = cross_source_result.value;

    BufferDesc cross_destination_desc = cross_source_desc;
    cross_destination_desc.memory = MemoryClass::kReadback;
    cross_destination_desc.name = DebugName{"Smoke.CrossQueueDestination"};
    auto cross_destination_result = device->create_buffer(cross_destination_desc);
    if (!check(cross_destination_result, "create_cross_queue_destination")) {
        return 1;
    }
    const BufferHandle cross_destination = cross_destination_result.value;
    const std::array<std::byte, 16> cross_queue_data{
        std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04},
        std::byte{0x05}, std::byte{0x06}, std::byte{0x07}, std::byte{0x08},
        std::byte{0x09}, std::byte{0x0a}, std::byte{0x0b}, std::byte{0x0c},
        std::byte{0x0d}, std::byte{0x0e}, std::byte{0x0f}, std::byte{0x10}
    };
    if (!check(device->write_buffer(cross_source, 0, cross_queue_data), "write_cross_queue_source")) {
        return 1;
    }

    CommandListDesc cross_copy_desc{};
    cross_copy_desc.queue = QueueClass::kCopy;
    cross_copy_desc.name = DebugName{"Smoke.CrossQueueCopyCommand"};
    auto cross_copy_command_result = device->create_command_list(cross_copy_desc);
    if (!check(cross_copy_command_result, "create_cross_queue_copy_command")) {
        return 1;
    }
    const CommandListHandle cross_copy_command = cross_copy_command_result.value;
    if (!check(device->begin_command_list(cross_copy_command), "begin_cross_queue_copy_command") ||
        !check(device->copy_buffer(cross_copy_command, BufferCopyDesc{cross_source, cross_destination, 0, 0, 16}),
               "copy_cross_queue_source") ||
        !check(device->end_command_list(cross_copy_command), "end_cross_queue_copy_command")) {
        return 1;
    }
    const std::array<CommandListHandle, 1> cross_copy_commands{cross_copy_command};
    auto cross_copy_submit_result = device->submit(
        SubmitDesc{QueueClass::kCopy, cross_copy_commands, {}});
    if (!check(cross_copy_submit_result, "submit_cross_queue_copy")) {
        return 1;
    }

    CommandListDesc cross_graphics_desc{};
    cross_graphics_desc.queue = QueueClass::kGraphics;
    cross_graphics_desc.name = DebugName{"Smoke.CrossQueueGraphicsCommand"};
    auto cross_graphics_command_result = device->create_command_list(cross_graphics_desc);
    if (!check(cross_graphics_command_result, "create_cross_queue_graphics_command")) {
        return 1;
    }
    const CommandListHandle cross_graphics_command = cross_graphics_command_result.value;
    if (!check(device->begin_command_list(cross_graphics_command), "begin_cross_queue_graphics_command") ||
        !check(device->copy_buffer(cross_graphics_command,
                                    BufferCopyDesc{cross_source, cross_destination, 0, 0, 16}),
               "copy_cross_queue_graphics") ||
        !check(device->end_command_list(cross_graphics_command), "end_cross_queue_graphics_command")) {
        return 1;
    }
    const std::array<CommandListHandle, 1> cross_graphics_commands{cross_graphics_command};
    const std::array<GpuWait, 1> cross_graphics_waits{
        GpuWait{cross_copy_submit_result.value.completion, PipelineStage::kCopy}
    };
    auto cross_graphics_submit_result = device->submit(
        SubmitDesc{QueueClass::kGraphics, cross_graphics_commands, cross_graphics_waits});
    if (!check(cross_graphics_submit_result, "submit_cross_queue_graphics") ||
        !check(device->wait(cross_graphics_submit_result.value.completion, std::numeric_limits<uint64_t>::max()),
               "wait_cross_queue_graphics")) {
        return 1;
    }
    std::array<std::byte, 16> cross_queue_readback{};
    if (!check(device->read_buffer(cross_destination, 0, cross_queue_readback),
               "read_cross_queue_destination") || cross_queue_readback != cross_queue_data) {
        AEG_LOG_ERROR(aeg::logging::Category::kTest, "Cross-queue buffer data did not round-trip");
        return 1;
    }

    TextureDesc cross_texture_desc{};
    cross_texture_desc.extent = {4, 4, 1};
    cross_texture_desc.format = Format::kRgba8Unorm;
    cross_texture_desc.capabilities =
            TextureCapability::kTransferSource | TextureCapability::kTransferDestination;
    cross_texture_desc.initial_queue = QueueClass::kCopy;
    cross_texture_desc.name = DebugName{"Smoke.CrossQueueTexture"};
    auto cross_texture_result = device->create_texture(cross_texture_desc);
    if (!check(cross_texture_result, "create_cross_queue_texture")) {
        return 1;
    }
    const TextureHandle cross_texture = cross_texture_result.value;

    constexpr uint64_t kCrossTextureDataSize = 4 * 4 * 4;
    BufferDesc cross_texture_upload_desc{};
    cross_texture_upload_desc.size = kCrossTextureDataSize;
    cross_texture_upload_desc.capabilities = BufferCapability::kTransferSource;
    cross_texture_upload_desc.memory = MemoryClass::kUpload;
    cross_texture_upload_desc.initial_usage = ResourceUsage::kHostWrite;
    cross_texture_upload_desc.initial_stages = PipelineStage::kHost;
    cross_texture_upload_desc.initial_queue = QueueClass::kCopy;
    cross_texture_upload_desc.name = DebugName{"Smoke.CrossQueueTextureUpload"};
    auto cross_texture_upload_result = device->create_buffer(cross_texture_upload_desc);
    if (!check(cross_texture_upload_result, "create_cross_queue_texture_upload")) {
        return 1;
    }
    const BufferHandle cross_texture_upload = cross_texture_upload_result.value;

    std::array<std::byte, kCrossTextureDataSize> cross_texture_data{};
    for (size_t index = 0; index < cross_texture_data.size(); index += 4) {
        cross_texture_data[index + 0] = std::byte{0xff};
        cross_texture_data[index + 1] = std::byte{0x00};
        cross_texture_data[index + 2] = std::byte{0x00};
        cross_texture_data[index + 3] = std::byte{0xff};
    }
    if (!check(device->write_buffer(cross_texture_upload, 0, cross_texture_data),
               "write_cross_queue_texture_upload")) {
        return 1;
    }

    BufferDesc cross_texture_readback_desc{};
    cross_texture_readback_desc.size = kCrossTextureDataSize;
    cross_texture_readback_desc.capabilities = BufferCapability::kTransferDestination;
    cross_texture_readback_desc.memory = MemoryClass::kReadback;
    cross_texture_readback_desc.initial_queue = QueueClass::kGraphics;
    cross_texture_readback_desc.name = DebugName{"Smoke.CrossQueueTextureReadback"};
    auto cross_texture_readback_result = device->create_buffer(cross_texture_readback_desc);
    if (!check(cross_texture_readback_result, "create_cross_queue_texture_readback")) {
        return 1;
    }
    const BufferHandle cross_texture_readback = cross_texture_readback_result.value;

    CommandListDesc cross_texture_upload_desc_list{};
    cross_texture_upload_desc_list.queue = QueueClass::kCopy;
    cross_texture_upload_desc_list.name = DebugName{"Smoke.CrossQueueTextureUploadCommand"};
    auto cross_texture_upload_command_result = device->create_command_list(cross_texture_upload_desc_list);
    if (!check(cross_texture_upload_command_result, "create_cross_queue_texture_upload_command")) {
        return 1;
    }
    const CommandListHandle cross_texture_upload_command = cross_texture_upload_command_result.value;
    const TextureSubresourceLayers cross_texture_layers{Aspect::kColor, 0, 0, 1};
    if (!check(device->begin_command_list(cross_texture_upload_command),
               "begin_cross_queue_texture_upload_command") ||
        !check(device->copy_buffer_to_texture(
                       cross_texture_upload_command,
                       BufferTextureCopyDesc{cross_texture_upload, 0, 0, 0, cross_texture,
                                             cross_texture_layers, Offset3D{0, 0, 0}, Extent3D{4, 4, 1}}),
               "copy_cross_queue_buffer_to_texture") ||
        !check(device->end_command_list(cross_texture_upload_command),
               "end_cross_queue_texture_upload_command")) {
        return 1;
    }
    const std::array<CommandListHandle, 1> cross_texture_upload_commands{cross_texture_upload_command};
    auto cross_texture_upload_submit_result = device->submit(
            SubmitDesc{QueueClass::kCopy, cross_texture_upload_commands, {}});
    if (!check(cross_texture_upload_submit_result, "submit_cross_queue_texture_upload")) {
        return 1;
    }

    CommandListDesc cross_texture_read_desc{};
    cross_texture_read_desc.queue = QueueClass::kGraphics;
    cross_texture_read_desc.name = DebugName{"Smoke.CrossQueueTextureReadCommand"};
    auto cross_texture_read_command_result = device->create_command_list(cross_texture_read_desc);
    if (!check(cross_texture_read_command_result, "create_cross_queue_texture_read_command")) {
        return 1;
    }
    const CommandListHandle cross_texture_read_command = cross_texture_read_command_result.value;
    if (!check(device->begin_command_list(cross_texture_read_command),
               "begin_cross_queue_texture_read_command") ||
        !check(device->copy_texture_to_buffer(
                       cross_texture_read_command,
                       TextureBufferCopyDesc{cross_texture, cross_texture_layers, Offset3D{0, 0, 0},
                                             Extent3D{4, 4, 1}, cross_texture_readback, 0, 0, 0}),
               "copy_cross_queue_texture_to_buffer") ||
        !check(device->end_command_list(cross_texture_read_command),
               "end_cross_queue_texture_read_command")) {
        return 1;
    }
    const std::array<CommandListHandle, 1> cross_texture_read_commands{cross_texture_read_command};
    const std::array<GpuWait, 1> cross_texture_read_waits{
        GpuWait{cross_texture_upload_submit_result.value.completion, PipelineStage::kCopy}
    };
    auto cross_texture_read_submit_result = device->submit(
            SubmitDesc{QueueClass::kGraphics, cross_texture_read_commands, cross_texture_read_waits});
    if (!check(cross_texture_read_submit_result, "submit_cross_queue_texture_read") ||
        !check(device->wait(cross_texture_read_submit_result.value.completion,
                             std::numeric_limits<uint64_t>::max()),
               "wait_cross_queue_texture_read")) {
        return 1;
    }
    std::array<std::byte, kCrossTextureDataSize> cross_texture_readback_data{};
    if (!check(device->read_buffer(cross_texture_readback, 0, cross_texture_readback_data),
               "read_cross_queue_texture") || cross_texture_readback_data != cross_texture_data) {
        AEG_LOG_ERROR(aeg::logging::Category::kTest, "Cross-queue texture data did not round-trip");
        return 1;
    }
    if (!check(device->destroy_command_list(cross_texture_upload_command),
               "destroy_cross_queue_texture_upload_command") ||
        !check(device->destroy_command_list(cross_texture_read_command),
               "destroy_cross_queue_texture_read_command") ||
        !check(device->destroy_texture(cross_texture), "destroy_cross_queue_texture") ||
        !check(device->destroy_buffer(cross_texture_upload),
               "destroy_cross_queue_texture_upload") ||
        !check(device->destroy_buffer(cross_texture_readback),
               "destroy_cross_queue_texture_readback")) {
        return 1;
    }

    if (!check(device->reset_command_list(command_list), "reset_graphics_command_list") ||
        !check(device->begin_command_list(command_list), "rebegin_graphics_command_list") ||
        !check(device->end_command_list(command_list), "reend_graphics_command_list")) {
        return 1;
    }
    const std::array<GpuWait, 1> waits{{GpuWait{copy_submit_result.value.completion, PipelineStage::kAllCommands}}};
    SubmitDesc dependent_submit_desc{};
    dependent_submit_desc.queue = QueueClass::kGraphics;
    dependent_submit_desc.command_lists = command_lists;
    dependent_submit_desc.waits = waits;
    auto dependent_submit_result = device->submit(dependent_submit_desc);
    if (!check(dependent_submit_result, "submit_graphics_waiting_for_copy") ||
        !check(device->wait(dependent_submit_result.value.completion, std::numeric_limits<uint64_t>::max()),
               "wait_graphics_waiting_for_copy")) {
        return 1;
    }

    if (!check(device->destroy_command_list(command_list), "destroy_command_list") ||
        !check(device->destroy_command_list(copy_command_list), "destroy_copy_command_list") ||
        !check(device->destroy_command_list(cross_copy_command), "destroy_cross_queue_copy_command") ||
        !check(device->destroy_command_list(cross_graphics_command), "destroy_cross_queue_graphics_command") ||
        !check(device->destroy_pipeline(pipeline), "destroy_pipeline") ||
        !check(device->destroy_binding_set(set), "destroy_binding_set") ||
        !check(device->destroy_binding_layout(layout), "destroy_binding_layout") ||
        !check(device->destroy_sampler(sampler), "destroy_sampler") ||
        !check(device->destroy_texture_view(texture_view), "destroy_texture_view") ||
        !check(device->destroy_buffer_view(buffer_view), "destroy_buffer_view") ||
        !check(device->destroy_texture(texture), "destroy_texture") ||
        !check(device->destroy_buffer(cross_destination), "destroy_cross_queue_destination") ||
        !check(device->destroy_buffer(cross_source), "destroy_cross_queue_source") ||
        !check(device->destroy_buffer(buffer), "destroy_buffer") || !check(device->wait_idle(), "wait_idle")) {
        return 1;
    }

    if (!expect_code(device->destroy_buffer(buffer), ResultCode::kNotFound, "destroy stale buffer handle")) {
        return 1;
    }

    AEG_LOG_INFO(aeg::logging::Category::kTest, "RHI smoke test passed");
    return 0;
}
