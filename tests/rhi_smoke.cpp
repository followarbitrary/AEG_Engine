#include <array>
#include <cstddef>
#include <limits>
#include <logging.hpp>
#include <rhi.hpp>
#include <span>
#include <string_view>

namespace {
bool check(const aeg::rhi::Result& result, std::string_view operation) {
    if (result) {
        return true;
    }
    AEG_LOG_ERROR(aeg::logging::Category::kTest, "{} failed [{}]: {}", operation,
                  aeg::rhi::result_code_name(result.code), result.message);
    return false;
}

template <typename T>
bool check(const aeg::rhi::ResultValue<T>& result, std::string_view operation) {
    return check(result.result, operation);
}

bool expect_code(const aeg::rhi::Result& result, aeg::rhi::ResultCode expected, std::string_view operation) {
    if (result.code == expected) {
        return true;
    }
    AEG_LOG_ERROR(aeg::logging::Category::kTest, "{} returned [{}] instead of [{}]: {}", operation,
                  aeg::rhi::result_code_name(result.code), aeg::rhi::result_code_name(expected), result.message);
    return false;
}

struct LoggingGuard {
    ~LoggingGuard() { aeg::logging::shutdown(); }
};
}  // namespace

int main() {
    const aeg::logging::Status logging_status = aeg::logging::initialize();
    if (!logging_status) {
        return 2;
    }
    const LoggingGuard logging_guard{};

    using namespace aeg::rhi;

    DeviceCreateDesc device_desc{};
    device_desc.application_name = "AEG RHI Smoke";

    auto device_result = create_vulkan_device(device_desc);
    if (!check(device_result, "create_vulkan_device")) {
        return 1;
    }
    std::unique_ptr<Device> device = std::move(device_result.value);

    AEG_LOG_INFO(aeg::logging::Category::kTest, "GPU: {}", device->capabilities().device_name);
    const uint32_t api_version = device->capabilities().api_version;
    AEG_LOG_INFO(aeg::logging::Category::kTest, "Vulkan API: {}.{}.{}", api_version >> 22u,
                 (api_version >> 12u) & 0x3ffu, api_version & 0xfffu);

    BufferDesc buffer_desc{};
    buffer_desc.size = 256;
    buffer_desc.capabilities =
        BufferCapability::kUniform | BufferCapability::kTexel | BufferCapability::kTransferDestination;
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
    layout_entries[3].type = BindingType::kUniformTexelBuffer;
    layout_entries[3].shader_stages = ShaderStage::kFragment;

    BindingLayoutDesc layout_desc{};
    layout_desc.entries = layout_entries;
    layout_desc.name = DebugName{"Smoke.BindingLayout"};
    auto layout_result = device->create_binding_layout(layout_desc);
    if (!check(layout_result, "create_binding_layout")) {
        return 1;
    }
    const BindingLayoutHandle layout = layout_result.value;

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
    writes[3].resource.type = BindingType::kUniformTexelBuffer;
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
        !check(device->end_command_list(command_list), "end_command_list")) {
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
        !check(device->destroy_binding_set(set), "destroy_binding_set") ||
        !check(device->destroy_binding_layout(layout), "destroy_binding_layout") ||
        !check(device->destroy_sampler(sampler), "destroy_sampler") ||
        !check(device->destroy_texture_view(texture_view), "destroy_texture_view") ||
        !check(device->destroy_buffer_view(buffer_view), "destroy_buffer_view") ||
        !check(device->destroy_texture(texture), "destroy_texture") ||
        !check(device->destroy_buffer(buffer), "destroy_buffer") || !check(device->wait_idle(), "wait_idle")) {
        return 1;
    }

    if (!expect_code(device->destroy_buffer(buffer), ResultCode::kNotFound, "destroy stale buffer handle")) {
        return 1;
    }

    AEG_LOG_INFO(aeg::logging::Category::kTest, "RHI smoke test passed");
    return 0;
}
