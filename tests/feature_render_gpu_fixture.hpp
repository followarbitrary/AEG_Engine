#pragma once

#include "../Engine/RenderGraph/private/render_graph_execution.hpp"
#include "feature_render_test_fixture.hpp"
#include "render_feature_runtime.hpp"
#include "rhi.hpp"

namespace render_feature_test {
inline void execute_pixels_and_retire_owners(rhi::Device& device,
                                    foundation::ResultValue<std::vector<RenderFeatureOutput>> captured,
                                    bool attachment_only = false,
                                    bool reversed_dependency = false) {
    render_graph::FrameContext frame(device, {.frames_in_flight = 1});
    render_graph::RenderGraph graph(device);
    require(static_cast<bool>(frame.begin_frame()), "frame begin failed");
    rhi::TextureDesc desc{};
    desc.extent = {4, 4, 1};
    desc.format = rhi::Format::kRgba8Unorm;
    desc.capabilities = rhi::TextureCapability::kTransferSource | rhi::TextureCapability::kTransferDestination;
    if (attachment_only)
        desc.capabilities = rhi::TextureCapability::kTransferSource | rhi::TextureCapability::kColorAttachment;
    if (reversed_dependency) desc.capabilities |= rhi::TextureCapability::kTransferDestination;
    std::array<render_graph::RenderGraphTexture, 2> targets{};
    for (auto& target : targets) {
        const auto created = graph.create_texture(desc);
        require(static_cast<bool>(created), "target creation failed");
        target = created.value;
    }
    const std::array bindings{
        render_graph::RenderGraphFeatureTextureBinding{RenderResourceId("Target.Red"), targets[0]},
        render_graph::RenderGraphFeatureTextureBinding{RenderResourceId("Target.Blue"), targets[1]}};
    require(static_cast<bool>(captured) && captured.value.size() == 2, "committed Feature outputs were not captured");
    auto authored_outputs = std::move(captured.value);
    if (attachment_only) {
        for (auto& output : authored_outputs) {
            auto clear = std::get<RenderClearColor>(output.intent.operations.front());
            clear.target = *output.intent.final_output;
            output.intent.textures.clear();
            output.intent.operations = {clear};
        }
    }
    if (reversed_dependency) {
        auto& producer = authored_outputs[0];
        auto& consumer = authored_outputs[1];
        consumer.intent.operations = {RenderCopyTexture{*producer.intent.final_output, *consumer.intent.final_output, 4, 4}};
        PortMessage message{};
        message.source_port = PortId("Ready");
        message.target_port = PortId("Color");
        message.kind = EdgeKind::kData;
        message.delivery_id = MessageDeliveryId(1);
        message.source = {producer.instance_id, producer.invocation, producer.resource_owner.definition_digest,
                          producer.resource_owner.resource_epoch, producer.contract.clock.id};
        consumer.input_messages = {message};
        std::swap(authored_outputs[0], authored_outputs[1]);
    }
    auto outputs = std::make_shared<const std::vector<RenderFeatureOutput>>(std::move(authored_outputs));
    std::weak_ptr<const std::vector<RenderFeatureOutput>> weak_outputs = outputs;
    require(static_cast<bool>(render_graph::build_render_graph_features(graph, outputs, {.textures = bindings}, 42)),
            "Feature output graph build failed");
    const auto allocation = frame.allocate_readback(128, 256);
    require(static_cast<bool>(allocation), "readback allocation failed");
    rhi::BufferDesc buffer_desc{};
    buffer_desc.size = allocation.value.buffer_size;
    buffer_desc.memory = rhi::MemoryClass::kReadback;
    buffer_desc.capabilities = rhi::BufferCapability::kTransferDestination;
    const auto destination = graph.import_buffer(allocation.value.buffer, buffer_desc);
    require(static_cast<bool>(destination), "readback import failed");
    for (unsigned index = 0; index < targets.size(); ++index) {
        const auto offset = allocation.value.offset + index * 64;
        // FrameContext allocates readback arenas with initial Copy ownership.
        const auto pass = graph.add_pass("Feature.Readback", rhi::QueueClass::kCopy,
            [texture = targets[index], buffer = destination.value, offset](render_graph::PassBuilder& builder) {
                if (const auto result = builder.read(texture, rhi::ResourceUsage::kCopySource, rhi::PipelineStage::kCopy); !result)
                    return result;
                builder.set_side_effects();
                return builder.write_to(buffer, rhi::ResourceUsage::kCopyDestination, rhi::PipelineStage::kCopy,
                                        rhi::ShaderStage::kAll, rhi::ResourceUsage::kHostRead, {offset, 64}, true);
            },
            [texture = targets[index], buffer = destination.value, offset](render_graph::RenderPassCommandContext& command) {
                return command.copy_texture_to_buffer(texture, {rhi::Aspect::kColor, 0, 0, 1}, {}, {4, 4, 1}, buffer, offset, 0, 0);
            });
        require(static_cast<bool>(pass), "readback pass failed");
    }
    const auto native_compile = graph.compile();
    if (!native_compile) throw std::runtime_error(native_compile.message);
    const unsigned operations_per_instance = attachment_only ? 1 : 2;
    const unsigned expected_passes = 2 * operations_per_instance + 2;
    require(aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).pass_count == expected_passes && aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).culled_pass_count == 0 &&
            aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).resource_count == (attachment_only ? 3 : 5),
            "instances shared local resources or did not expand into real passes");
    for (unsigned index = 0; index < 2 * operations_per_instance; ++index) {
        const auto& owner = aeg::render_graph::detail::RenderGraphExecution::plan(graph).pass_plans[index].feature_work;
        const auto source_index = reversed_dependency ? 1 - index : index / operations_per_instance;
        require(owner && owner->frame_index == 42 && owner->invocation.invocation_id.is_valid() &&
                owner->feature_definition_id == outputs->at(source_index).contract.definition_id &&
                owner->instance_id == outputs->at(source_index).instance_id &&
                owner->node_id == outputs->at(source_index).node_id && owner->operation_index == index % operations_per_instance,
                "real pass lost original Feature/operation provenance after dependency ordering");
    }
    const auto executed = graph.execute(frame);
    require(static_cast<bool>(executed) && executed.value.executed_pass_count == expected_passes,
            "real Feature passes did not execute");
    require(static_cast<bool>(frame.end_frame()) && static_cast<bool>(device.wait_idle()), "frame completion failed");
    std::array<std::byte, 128> pixels{};
    require(static_cast<bool>(frame.read_readback(allocation.value, pixels)), "pixel readback failed");
    for (unsigned pixel = 0; pixel < 32; ++pixel) {
        const bool red = pixel < 16 || reversed_dependency;
        require(pixels[pixel * 4] == (red ? std::byte{255} : std::byte{0}) && pixels[pixel * 4 + 1] == std::byte{0} &&
                pixels[pixel * 4 + 2] == (red ? std::byte{0} : std::byte{255}) && pixels[pixel * 4 + 3] == std::byte{255},
                "Feature clear/copy pixels or per-instance ownership are wrong");
    }
    graph.reset();
    outputs.reset();
    require(!weak_outputs.expired(), "submitted Feature output owner retired before frame drain");
    require(static_cast<bool>(frame.drain()) && weak_outputs.expired(), "Feature output owner did not retire with its GPU frame");
}

}  // namespace render_feature_test
