/** @brief Exercises shared texture retirement across two real ImGui renderer hosts. */
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include "frame_context.hpp"
#include "imgui_renderer.hpp"
#include "render_graph.hpp"
#include "rhi.hpp"
#include "texture_uploader.hpp"
#include "../Application/EditorFramework/public/imgui_render_execution.hpp"

namespace {
using aeg::foundation::Result;
using aeg::foundation::ResultCode;
using namespace aeg::rhi;
constexpr uint32_t kExtent = 64;
constexpr uint64_t kReadbackSize = uint64_t{kExtent} * kExtent * 4;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void require(const Result& result, const char* operation) {
    if (!result) throw std::runtime_error(std::string(operation) + ": " + result.message);
}

template <typename T>
void require(const aeg::foundation::ResultValue<T>& result, const char* operation) {
    require(result.result, operation);
}

ResourceState host_read_state() {
    return {ResourceUsage::kHostRead, ShaderStage::kAll, QueueClass::kCopy, false, PipelineStage::kHost};
}

BufferDesc readback_desc(uint64_t size) {
    BufferDesc desc{};
    desc.size = size;
    desc.capabilities = BufferCapability::kTransferSource | BufferCapability::kTransferDestination |
                        BufferCapability::kVertex | BufferCapability::kIndex | BufferCapability::kUniform;
    desc.memory = MemoryClass::kReadback;
    desc.initial_usage = ResourceUsage::kHostRead;
    desc.initial_stages = PipelineStage::kHost;
    desc.initial_queue = QueueClass::kCopy;
    return desc;
}

struct SharedTexture final {
    explicit SharedTexture(Device& source) : device(source) {}
    ~SharedTexture() {
        (void)device.wait_idle();
        (void)aeg::resource::TextureUploader(device).destroy_texture(texture);
        if (sampler.is_valid()) (void)device.destroy_sampler(sampler);
    }
    Device& device;
    aeg::resource::GpuTexture texture{};
    SamplerHandle sampler{};
    TextureDesc desc{};
};

void upload_shared_texture(SharedTexture& shared) {
    shared.desc.extent = {1, 1, 1};
    shared.desc.format = Format::kRgba8Unorm;
    shared.desc.capabilities = TextureCapability::kTransferDestination | TextureCapability::kSampled;
    const std::array<std::byte, 4> white{
        std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}};
    const aeg::resource::TextureSubresourceData subresource{white, 0, 0};
    const aeg::resource::TextureData data{shared.desc, std::span{&subresource, 1}};
    aeg::render_graph::FrameContext frame(shared.device);
    aeg::render_graph::RenderGraph graph(shared.device);
    require(frame.begin_frame(), "begin shared texture upload");
    const auto uploaded = aeg::resource::TextureUploader(shared.device).upload_texture(frame, graph, data,
                                                                                     "ImGui.Shared.Font");
    require(uploaded, "upload shared texture");
    shared.texture = uploaded.value.texture;
    const auto executed = graph.execute(frame);
    require(executed, "execute shared texture upload");
    if (executed.value.completion.is_valid())
        require(shared.device.wait(executed.value.completion, std::numeric_limits<uint64_t>::max()),
                "wait shared texture upload");
    require(frame.end_frame(), "end shared texture upload");
    graph.reset();
    require(frame.drain(), "drain shared texture upload");
    SamplerDesc sampler{};
    sampler.min_filter = sampler.mag_filter = sampler.mip_filter = Filter::kNearest;
    sampler.address_u = sampler.address_v = sampler.address_w = AddressMode::kClampToEdge;
    const auto created = shared.device.create_sampler(sampler);
    require(created, "create shared sampler");
    shared.sampler = created.value;
}

struct UiHost final {
    UiHost(Device& source, aeg::editor::ImGuiTextureRegistry& textures)
        : device(source), frame(source), graph(source), renderer(source, textures) {}
    ~UiHost() {
        if (frame.is_frame_active()) (void)frame.abort_frame();
        (void)device.wait_idle();
        graph.reset();
        (void)renderer.release_prepared_frame();
        (void)frame.drain();
        (void)renderer.shutdown();
        if (target_view.is_valid()) (void)device.destroy_texture_view(target_view);
        if (target_texture.is_valid()) (void)device.destroy_texture(target_texture);
    }
    Device& device;
    aeg::render_graph::FrameContext frame;
    aeg::render_graph::RenderGraph graph;
    aeg::editor::ImGuiRenderer renderer;
    TextureHandle target_texture{};
    TextureViewHandle target_view{};
};

void render_packet(UiHost& host, aeg::editor::ImGuiTextureHandle shared_texture, bool green) {
    require(host.renderer.initialize(), "initialize ImGui host");
    TextureDesc target_desc{};
    target_desc.extent = {kExtent, kExtent, 1};
    target_desc.format = Format::kRgba8Unorm;
    target_desc.capabilities = TextureCapability::kColorAttachment | TextureCapability::kTransferSource;
    const auto texture = host.device.create_texture(target_desc);
    require(texture, "create ImGui target");
    host.target_texture = texture.value;
    const auto view = host.device.create_texture_view({.texture = texture.value,
                                                       .kind = TextureViewKind::kColorAttachment});
    require(view, "create ImGui target view");
    host.target_view = view.value;
    require(host.frame.begin_frame(), "begin ImGui host frame");
    const auto target = host.graph.import_texture(texture.value, view.value, target_desc,
        {ResourceUsage::kUndefined, ShaderStage::kAll, QueueClass::kAuto, false, PipelineStage::kAllCommands});
    require(target, "import ImGui target");
    const auto allocation = host.frame.allocate_readback(kReadbackSize, 256);
    require(allocation, "allocate ImGui readback");
    const auto readback = host.graph.import_buffer(allocation.value.buffer,
        readback_desc(allocation.value.buffer_size), host_read_state());
    require(readback, "import ImGui readback");
    const auto cleared = host.graph.add_pass("ImGui.Shared.Clear", QueueClass::kGraphics,
        [target = target.value](aeg::render_graph::PassBuilder& builder) {
            return builder.use_color_attachment(target, LoadOp::kClear, StoreOp::kStore,
                ClearColorValue{0.f, 0.f, 0.f, 1.f}, ResourceUsage::kColorAttachmentWrite);
        }, [](aeg::render_graph::PassExecutionContext& context) {
            if (const auto result = context.begin_rendering(); !result) return result;
            return context.end_rendering();
        });
    require(cleared, "add ImGui target clear");
    aeg::editor::ImGuiDrawPacket packet{};
    packet.display_width = packet.display_height = static_cast<float>(kExtent);
    packet.frame_number = 1;
    const uint32_t color = green ? 0xff40ff40u : 0xff4040ffu;
    packet.vertices = {{8.f, 8.f, 0.f, 0.f, color}, {56.f, 8.f, 1.f, 0.f, color},
                       {32.f, 56.f, 0.5f, 1.f, color}};
    packet.indices = {0, 1, 2};
    packet.commands = {{shared_texture, 0.f, 0.f, float(kExtent), float(kExtent), 3, 0, 0}};
    const aeg::editor::ImGuiRenderTargetDesc target_contract{
        aeg::feature::RenderResourceId("SceneColor"), target_desc.format, {kExtent, kExtent}, ResourceUsage::kCopySource};
    require(host.renderer.prepare_frame(host.frame, host.graph, packet, target_contract), "prepare ImGui packet");
    require(aeg::editor::detail::ImGuiRenderExecution::build_render_graph(host.renderer, host.graph, target.value),
            "build fixed ImGui draw");
    const auto copied = host.graph.add_pass("ImGui.Shared.Readback", QueueClass::kCopy,
        [target = target.value, readback = readback.value, allocation = allocation.value](
            aeg::render_graph::PassBuilder& builder) {
            if (const auto result = builder.read(target, ResourceUsage::kCopySource, PipelineStage::kCopy,
                                                 ShaderStage::kAll); !result) return result;
            if (const auto result = builder.write(readback, ResourceUsage::kCopyDestination, PipelineStage::kCopy,
                ShaderStage::kAll, aeg::render_graph::RenderGraphBufferRange{allocation.offset, kReadbackSize}, true);
                !result) return result;
            builder.set_side_effects();
            return Result::success();
        }, [target = target.value, readback = readback.value, allocation = allocation.value](
            aeg::render_graph::PassExecutionContext& context) {
            return context.copy_texture_to_buffer(target, TextureSubresourceLayers{Aspect::kColor, 0, 0, 1},
                Offset3D{}, Extent3D{kExtent, kExtent, 1}, readback, allocation.offset, 0, 0);
        });
    require(copied, "add ImGui readback");
    const auto executed = host.graph.execute(host.frame);
    require(executed, "execute ImGui host");
    require(executed.value.draw_call_count == 1, "ImGui host did not execute its actual triangle");
    if (executed.value.completion.is_valid())
        require(host.device.wait(executed.value.completion, std::numeric_limits<uint64_t>::max()), "wait ImGui host");
    std::array<std::byte, kReadbackSize> pixels{};
    require(host.frame.read_readback(allocation.value, pixels), "read ImGui host pixels");
    constexpr size_t center = (size_t{kExtent / 2} * kExtent + kExtent / 2) * 4;
    const auto red = std::to_integer<uint32_t>(pixels[center]);
    const auto green_channel = std::to_integer<uint32_t>(pixels[center + 1]);
    require(green ? green_channel > 200 && red < 128 : red > 200 && green_channel < 128,
            "ImGui host center pixel did not come from its colored packet and shared sampled texture");
    require(pixels[0] == std::byte{0} && pixels[1] == std::byte{0} && pixels[2] == std::byte{0},
            "ImGui host did not preserve the clear outside its triangle");
    require(host.frame.end_frame(), "end ImGui host frame");
    host.graph.reset();
    require(host.frame.drain(), "drain completed ImGui host commands");
    require(host.renderer.has_prepared_frame(), "ImGui renderer dropped its current prepared frame at GPU completion");
}
} // namespace

int main() try {
    auto created = aeg::rhi::create_device({});
    require(created, "create shared ImGui RHI device");
    auto device = std::move(created.value);
    SharedTexture shared(*device);
    upload_shared_texture(shared);
    aeg::editor::ImGuiTextureRegistry textures;
    aeg::editor::ImGuiTextureBinding binding{shared.texture.sampled_view, shared.sampler, shared.texture.texture,
        shared.desc, {ResourceUsage::kCopyDestination, ShaderStage::kAll, QueueClass::kCopy, false, PipelineStage::kCopy}};
    const auto identity = textures.register_texture(binding);
    require(identity, "register shared ImGui texture");
    UiHost main_host(*device, textures);
    UiHost platform_host(*device, textures);
    render_packet(main_host, identity.value, false);
    binding.initial_state = {ResourceUsage::kSampledRead, ShaderStage::kFragment, QueueClass::kGraphics,
                             false, PipelineStage::kFragmentShader};
    require(textures.update_texture(identity.value, binding), "publish shared sampled texture state for second host");
    render_packet(platform_host, identity.value, true);

    require(main_host.renderer.release_prepared_frame(), "release main prepared frame");
    require(!main_host.renderer.has_prepared_frame() && platform_host.renderer.has_prepared_frame(),
            "main release unexpectedly released another renderer's prepared frame");
    const auto main_only_replacement = device->destroy_texture_view(shared.texture.sampled_view);
    require(!main_only_replacement && (main_only_replacement.code == ResultCode::kInvalidState ||
                                        main_only_replacement.code == ResultCode::kNotReady),
            "main-only release did not preserve the second host's actual shared texture reference");
    // The per-renderer API correctly releases only that renderer. Shared texture replacement
    // uses the same complete host group as the editor's main font/viewport replacement path.
    const std::array<aeg::editor::ImGuiRenderer*, 2> hosts{&main_host.renderer, &platform_host.renderer};
    require(aeg::editor::detail::ImGuiRenderExecution::release_prepared_frames(hosts),
            "release all ImGui hosts borrowing the shared texture");
    require(!main_host.renderer.has_prepared_frame() && !platform_host.renderer.has_prepared_frame(),
            "shared texture host group retained a prepared frame after release");
    if (shared.texture.sampled_view.is_valid()) {
        require(device->destroy_texture_view(shared.texture.sampled_view), "destroy shared view after both releases");
        shared.texture.sampled_view = {};
    }
    require(device->destroy_texture(shared.texture.texture), "destroy shared texture after both releases");
    shared.texture.texture = {};
    require(device->destroy_sampler(shared.sampler), "destroy shared sampler after both releases");
    shared.sampler = {};
    require(textures.unregister_texture(identity.value), "unregister retired shared texture identity");
    require(main_host.renderer.shutdown(), "shutdown main ImGui renderer");
    require(platform_host.renderer.shutdown(), "shutdown second ImGui renderer");
    std::cout << "both ImGui packets rendered; shared view/texture/sampler retired after both host releases\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "shared ImGui texture fixture: " << error.what() << '\n';
    return 1;
}
