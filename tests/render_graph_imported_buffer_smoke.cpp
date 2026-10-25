/** @brief Shared native buffers keep one graph identity, dependency and state history. */
#include <array>
#include <cassert>
#include <cstddef>
#include <iostream>
#include <limits>

#include "frame_context.hpp"
#include "render_graph.hpp"
#include "../Engine/RenderGraph/private/render_graph_execution.hpp"
#include "../Engine/RenderGraph/private/render_graph_resource_access.hpp"
#include "rhi.hpp"

namespace {
bool verify_resource_membership(aeg::rhi::Device& device) {
    using namespace aeg;
    render_graph::RenderGraph graph(device);
    render_graph::RenderGraph other(device);
    rhi::BufferDesc buffer_desc{};
    buffer_desc.size = 16;
    buffer_desc.capabilities = rhi::BufferCapability::kTransferDestination;
    rhi::TextureDesc texture_desc{};
    texture_desc.extent = {1, 1, 1};
    texture_desc.format = rhi::Format::kRgba8Unorm;
    texture_desc.capabilities = rhi::TextureCapability::kTransferDestination;
    const auto buffer = graph.create_buffer(buffer_desc);
    const auto texture = graph.create_texture(texture_desc);
    const auto other_buffer = other.create_buffer(buffer_desc);
    const auto other_texture = other.create_texture(texture_desc);
    if (!buffer || !texture || !other_buffer || !other_texture) return false;

    const auto verify = [&](auto handle, auto foreign, auto contains, auto export_resource) {
        if (!contains(graph, handle) || !export_resource(graph, handle) || contains(other, handle) ||
            contains(graph, foreign) || export_resource(graph, foreign)) return false;
        auto wrong_graph = handle;
        wrong_graph.graph_id = foreign.graph_id;
        auto wrong_generation = handle;
        ++wrong_generation.generation;
        auto out_of_range = handle;
        out_of_range.index = 99;
        for (const auto invalid : {decltype(handle){}, wrong_graph, wrong_generation, out_of_range}) {
            if (contains(graph, invalid) || export_resource(graph, invalid)) return false;
        }
        return true;
    };
    const auto contains_buffer = render_graph::detail::RenderGraphResourceAccess::contains_buffer;
    const auto contains_texture = render_graph::detail::RenderGraphResourceAccess::contains_texture;
    if (!verify(buffer.value, other_buffer.value, contains_buffer,
                [](auto& owner, auto handle) { return owner.export_buffer(handle); }) ||
        !verify(texture.value, other_texture.value, contains_texture,
                [](auto& owner, auto handle) { return owner.export_texture(handle); })) return false;

    graph.reset();
    const auto replacement_buffer = graph.create_buffer(buffer_desc);
    const auto replacement_texture = graph.create_texture(texture_desc);
    if (!replacement_buffer || !replacement_texture || replacement_buffer.value.index != buffer.value.index ||
        replacement_texture.value.index != texture.value.index || contains_buffer(graph, buffer.value) ||
        contains_texture(graph, texture.value) || graph.export_buffer(buffer.value) ||
        graph.export_texture(texture.value) || !contains_buffer(graph, replacement_buffer.value) ||
        !contains_texture(graph, replacement_texture.value) || !contains_buffer(other, other_buffer.value) ||
        !contains_texture(other, other_texture.value)) return false;
    return true;
}
}  // namespace

int main() {
    using namespace aeg;
    const auto check = [](const foundation::Result& result) {
        if (!result) std::cerr << result.message << '\n';
        return static_cast<bool>(result);
    };
    auto created = rhi::create_device({rhi::BackendType::kVulkan, "Shared imported buffer smoke",
        std::numeric_limits<uint32_t>::max(), true, true});
    if (!created) return 1;
    auto& device = *created.value;
    if (!verify_resource_membership(device)) {
        std::cerr << "Resource membership accepted an invalid, foreign or stale graph handle\n";
        return 31;
    }
    render_graph::FrameContext frame(device);
    render_graph::RenderGraph graph(device);
    const std::array<uint32_t, 4> values{17, 23, 41, 59};
    rhi::BufferDesc desc{};
    desc.size = sizeof(values);
    desc.capabilities = rhi::BufferCapability::kUniform | rhi::BufferCapability::kTransferSource |
        rhi::BufferCapability::kTransferDestination;
    desc.memory = rhi::MemoryClass::kDeviceLocal;
    desc.initial_usage = rhi::ResourceUsage::kUniformRead;
    desc.initial_queue = rhi::QueueClass::kGraphics;
    desc.name = {"Shared buffer"};
    const auto native = device.create_buffer(desc);
    const auto other = device.create_buffer(desc);
    if (!native || !other) return 2;
    const auto first = graph.import_buffer(native.value, desc);
    auto renamed = desc;
    renamed.name = {"Second Feature's same buffer"};
    const auto second = graph.import_buffer(native.value, renamed);
    if (!first || !second || first.value != second.value) {
        std::cerr << "Repeated native buffer import must preserve graph identity\n";
        return 3;
    }
    if (!check(graph.compile())) return 4;
    const auto count = aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).resource_count;
    for (unsigned case_index = 0; case_index < 5; ++case_index) {
        auto invalid = desc;
        rhi::ResourceState state{};
        switch (case_index) {
        case 0: invalid.size *= 2; break;
        case 1: invalid.capabilities = rhi::BufferCapability::kVertex; break;
        case 2: invalid.memory = rhi::MemoryClass::kUpload; break;
        case 3: state = {rhi::ResourceUsage::kCopySource, rhi::ShaderStage::kAll,
            rhi::QueueClass::kGraphics, false, rhi::PipelineStage::kCopy}; break;
        case 4: invalid.flags = rhi::ResourceFlags::kExternal; break;
        }
        if (graph.import_buffer(native.value, invalid, state) || !check(graph.compile()) ||
            aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).resource_count != count) return 5;
    }
    const auto other_resource = graph.import_buffer(other.value, desc);
    if (!other_resource || graph.update_imported_buffer(first.value, other.value)) return 6;

    auto upload_desc = desc;
    upload_desc.capabilities = rhi::BufferCapability::kTransferSource;
    upload_desc.memory = rhi::MemoryClass::kUpload;
    upload_desc.initial_usage = rhi::ResourceUsage::kHostWrite;
    upload_desc.initial_stages = rhi::PipelineStage::kHost;
    upload_desc.initial_queue = rhi::QueueClass::kCopy;
    const auto upload = device.create_buffer(upload_desc);
    if (!upload || !check(device.write_buffer(upload.value, 0, std::as_bytes(std::span(values))))) return 7;
    const auto source = graph.import_buffer(upload.value, upload_desc);
    if (!source || !check(frame.begin_frame())) return 8;
    const auto readback = frame.allocate_readback(sizeof(values), 16);
    if (!readback) return 9;
    rhi::BufferDesc readback_desc{};
    readback_desc.size = readback.value.buffer_size;
    readback_desc.capabilities = rhi::BufferCapability::kTransferDestination;
    readback_desc.memory = rhi::MemoryClass::kReadback;
    const auto destination = graph.import_buffer(readback.value.buffer, readback_desc);
    if (!destination) return 10;
    const auto producer = graph.add_pass("Shared buffer producer", rhi::QueueClass::kCopy,
        [&](render_graph::PassBuilder& pass) {
            if (const auto result = pass.read(source.value, rhi::ResourceUsage::kCopySource,
                    rhi::PipelineStage::kCopy); !result) return result;
            return pass.write(first.value, rhi::ResourceUsage::kCopyDestination, rhi::PipelineStage::kCopy);
        }, [&](render_graph::RenderPassCommandContext& command) {
            return command.copy_buffer(source.value, first.value, 0, 0, sizeof(values));
        });
    if (!producer) return 11;
    for (unsigned index = 0; index < 2; ++index) {
        const auto stage = index == 0 ? rhi::PipelineStage::kVertexShader : rhi::PipelineStage::kFragmentShader;
        const auto shader = index == 0 ? rhi::ShaderStage::kVertex : rhi::ShaderStage::kFragment;
        if (!graph.add_pass("Shared buffer consumer", rhi::QueueClass::kGraphics,
                [resource = second.value, stage, shader](render_graph::PassBuilder& pass) {
                    pass.set_side_effects();
                    return pass.read(resource, rhi::ResourceUsage::kUniformRead, stage, shader);
                }, [](render_graph::RenderPassCommandContext&) { return foundation::Result::success(); })) return 12;
    }
    const auto copy = graph.add_pass("Shared buffer readback", rhi::QueueClass::kGraphics,
        [&](render_graph::PassBuilder& pass) {
            pass.set_side_effects();
            if (const auto result = pass.read(second.value, rhi::ResourceUsage::kCopySource,
                    rhi::PipelineStage::kCopy); !result) return result;
            return pass.write(destination.value, rhi::ResourceUsage::kCopyDestination, rhi::PipelineStage::kCopy);
        }, [&](render_graph::RenderPassCommandContext& command) {
            return command.copy_buffer(second.value, destination.value, 0, readback.value.offset, sizeof(values));
        });
    if (!copy || !check(graph.compile())) return 13;
    const auto& plan = aeg::render_graph::detail::RenderGraphExecution::plan(graph);
    if (!plan.pass_plans.front().live || plan.topological_order.front() != producer.value.index) return 14;
    bool cross_queue_dependency = false;
    for (const auto& dependency : plan.queue_dependencies)
        cross_queue_dependency = cross_queue_dependency || dependency.resource_hazard_count != 0;
    if (!cross_queue_dependency) return 15;
    const auto execution = graph.execute(frame);
    if (!execution || !check(frame.end_frame()) || !check(device.wait_idle())) {
        if (!execution) check(execution.result);
        return 16;
    }
    std::array<uint32_t, 4> actual{};
    if (!check(frame.read_readback(readback.value, std::as_writable_bytes(std::span(actual)))) || actual != values)
        return 17;
    // Import compares the declared initial contract, not the state left by GPU execution.
    const auto repeated = graph.import_buffer(native.value, desc);
    if (!repeated || repeated.value != first.value) return 18;
    graph.reset();
    if (!check(frame.drain())) return 20;
    for (unsigned mode = 0; mode != 3; ++mode) {
        if (!check(frame.begin_frame())) return 20;
        const auto target = graph.import_buffer(native.value, desc);
        const auto input = graph.import_buffer(upload.value, upload_desc);
        const auto result_bytes = frame.allocate_readback(sizeof(values), 256);
        if (!target || !input || !result_bytes) return 20;
        rhi::BufferDesc result_desc{};
        result_desc.size = result_bytes.value.buffer_size;
        result_desc.memory = rhi::MemoryClass::kReadback;
        result_desc.capabilities = rhi::BufferCapability::kTransferDestination;
        const auto output = graph.import_buffer(result_bytes.value.buffer, result_desc);
        if (!output) return 20;
        unsigned observer_calls = 0;
        const auto observer = graph.add_pass("Unused read before a live writer", rhi::QueueClass::kCompute,
            [&](render_graph::PassBuilder& pass) {
                if (mode == 2) pass.set_side_effects();
                return pass.read(target.value, rhi::ResourceUsage::kUniformRead, rhi::PipelineStage::kCompute,
                    rhi::ShaderStage::kCompute);
            }, [&](render_graph::RenderPassCommandContext&) {
                ++observer_calls;
                return foundation::Result::success();
            });
        const auto writer = graph.add_pass("Live writer after the unused observer", rhi::QueueClass::kGraphics,
            [&](render_graph::PassBuilder& pass) {
                if (const auto result = pass.read(input.value, rhi::ResourceUsage::kCopySource, rhi::PipelineStage::kCopy); !result)
                    return result;
                return pass.write(target.value, rhi::ResourceUsage::kCopyDestination, rhi::PipelineStage::kCopy);
            }, [&](render_graph::RenderPassCommandContext& command) {
                return command.copy_buffer(input.value, target.value, 0, 0, sizeof(values));
            });
        const auto sink = graph.add_pass("Liveness readback", rhi::QueueClass::kGraphics,
            [&](render_graph::PassBuilder& pass) {
                pass.set_side_effects();
                if (const auto result = pass.read(target.value, rhi::ResourceUsage::kCopySource, rhi::PipelineStage::kCopy); !result)
                    return result;
                return pass.write(output.value, rhi::ResourceUsage::kCopyDestination, rhi::PipelineStage::kCopy);
            }, [&](render_graph::RenderPassCommandContext& command) {
                return command.copy_buffer(target.value, output.value, 0, result_bytes.value.offset, sizeof(values));
            });
        if (!observer || !writer || !sink || (mode == 1 && !check(graph.add_dependency(observer.value, writer.value))) ||
            !check(graph.compile())) return 20;
        const auto& liveness = aeg::render_graph::detail::RenderGraphExecution::plan(graph);
        if (liveness.pass_plans[observer.value.index].live != (mode != 0) ||
            !liveness.pass_plans[writer.value.index].live || !liveness.pass_plans[sink.value.index].live) {
            std::cerr << "read/write ordering kept unused work alive, mode=" << mode << '\n';
            return 21;
        }
        if (mode == 0) {
            for (const auto& dependency : liveness.queue_dependencies) {
                if (liveness.batches[dependency.source_batch].queue == rhi::QueueClass::kCompute ||
                    liveness.batches[dependency.destination_batch].queue == rhi::QueueClass::kCompute) return 22;
            }
            for (const auto& pass : liveness.pass_plans)
                if (pass.live) for (const auto& barrier : pass.barriers)
                    if (barrier.requires_queue_transfer && barrier.before.queue == barrier.after.queue) return 22;
        }
        const auto performed = graph.execute(frame);
        if (!performed || !check(frame.end_frame()) || !check(device.wait_idle()) || observer_calls != (mode == 0 ? 0u : 1u)) return 23;
        std::array<uint32_t, 4> liveness_values{};
        if (!check(frame.read_readback(result_bytes.value, std::as_writable_bytes(std::span{liveness_values}))) ||
            liveness_values != values) return 23;
        graph.reset();
        if (!check(frame.drain())) return 23;
    }
    // A host write does not acquire a GPU queue family. Exercise both the first
    // GPU use and partial CPU rewrites of a previously used buffer across queues.
    auto host_desc = upload_desc;
    host_desc.initial_queue = rhi::QueueClass::kCompute;
    const auto host_upload = device.create_buffer(host_desc);
    if (!host_upload) return 24;
    auto expected = values;
    const std::array host_queues{rhi::QueueClass::kGraphics, rhi::QueueClass::kCopy, rhi::QueueClass::kCompute};
    for (size_t index = 0; index < host_queues.size(); ++index) {
        if (!check(frame.begin_frame())) return 24;
        const size_t first_element = index == 0 ? 0 : 2;
        for (size_t element = first_element; element < expected.size(); ++element) expected[element] += uint32_t(index + 1);
        if (!check(device.write_buffer(host_upload.value, first_element * sizeof(uint32_t),
            std::as_bytes(std::span{expected}.subspan(first_element))))) return 24;
        const auto input = graph.import_buffer(host_upload.value, host_desc);
        const auto bytes = frame.allocate_readback(sizeof(expected), 16);
        if (!input || !bytes) return 24;
        auto result_desc = readback_desc;
        result_desc.size = bytes.value.buffer_size;
        const auto output = graph.import_buffer(bytes.value.buffer, result_desc);
        if (!output) return 24;
        const auto copied = graph.add_pass("Host upload to selected GPU queue", host_queues[index],
            [&](render_graph::PassBuilder& pass) {
                pass.set_side_effects();
                if (const auto result = pass.read(input.value, rhi::ResourceUsage::kCopySource, rhi::PipelineStage::kCopy); !result)
                    return result;
                return pass.write(output.value, rhi::ResourceUsage::kCopyDestination, rhi::PipelineStage::kCopy);
            }, [&](render_graph::RenderPassCommandContext& command) {
                return command.copy_buffer(input.value, output.value, 0, bytes.value.offset, sizeof(expected));
            });
        if (!copied || !check(graph.compile())) return 24;
        const auto performed = graph.execute(frame);
        if (!performed || !check(frame.end_frame()) || !check(device.wait_idle())) return 24;
        const auto state = device.query_buffer_state(host_upload.value);
        if (!state || (index == 0 && state.value.last_use.size() != 1)) return 24;
        for (const auto& segment : state.value.segments)
            if (segment.state.queue != host_queues[index] || segment.state.usage != rhi::ResourceUsage::kCopySource) return 24;
        std::array<uint32_t, 4> received{};
        if (!check(frame.read_readback(bytes.value, std::as_writable_bytes(std::span{received}))) || received != expected) return 24;
        graph.reset();
        if (!check(frame.drain())) return 24;
    }
    // A cached reader must see a GPU write by another graph even when no
    // declaration in the reader changes. The writer changes only half the
    // native buffer, so admission must retain both real state segments.
    {
        render_graph::RenderGraph reader(device), writer(device);
        auto shared_desc = desc;
        shared_desc.initial_usage = rhi::ResourceUsage::kCopySource;
        shared_desc.initial_stages = rhi::PipelineStage::kCopy;
        shared_desc.initial_queue = rhi::QueueClass::kGraphics;
        const auto shared = device.create_buffer(shared_desc);
        auto sink_desc = readback_desc;
        sink_desc.size = sizeof(uint32_t) * 2;
        const auto sink = device.create_buffer(sink_desc);
        if (!shared || !sink) return 25;
        const auto source_handle = reader.import_buffer(shared.value, shared_desc);
        const auto sink_handle = reader.import_buffer(sink.value, sink_desc);
        if (!source_handle || !sink_handle) return 25;
        const auto consumer = reader.add_pass("Cached reader after external partial GPU write", rhi::QueueClass::kGraphics,
            [&](render_graph::PassBuilder& pass) {
                pass.set_side_effects();
                if (const auto result = pass.read(source_handle.value, rhi::ResourceUsage::kCopySource,
                    rhi::PipelineStage::kCopy, rhi::ShaderStage::kAll, {0, sink_desc.size}); !result) return result;
                return pass.write(sink_handle.value, rhi::ResourceUsage::kCopyDestination,
                    rhi::PipelineStage::kCopy, rhi::ShaderStage::kAll, {0, sink_desc.size});
            }, [&](render_graph::RenderPassCommandContext& command) {
                return command.copy_buffer(source_handle.value, sink_handle.value, 0, 0, sink_desc.size);
            });
        if (!consumer || !check(reader.compile())) return 25;
        const auto original_revision = aeg::render_graph::detail::RenderGraphExecution::plan(reader).revision;
        for (const auto& barrier : aeg::render_graph::detail::RenderGraphExecution::plan(reader).pass_plans[consumer.value.index].barriers)
            if (barrier.resource_kind == render_graph::RenderGraphResourceKind::kBuffer &&
                barrier.resource_index == source_handle.value.index) return 25;
        if (!check(frame.begin_frame())) return 25;
        const auto from = writer.import_buffer(upload.value, upload_desc);
        const auto to = writer.import_buffer(shared.value, shared_desc);
        if (!from || !to) return 25;
        const auto produced = writer.add_pass("External partial GPU producer", rhi::QueueClass::kCopy,
            [&](render_graph::PassBuilder& pass) {
                pass.set_side_effects();
                if (const auto result = pass.read(from.value, rhi::ResourceUsage::kCopySource,
                    rhi::PipelineStage::kCopy, rhi::ShaderStage::kAll, {0, sink_desc.size}); !result) return result;
                return pass.write(to.value, rhi::ResourceUsage::kCopyDestination,
                    rhi::PipelineStage::kCopy, rhi::ShaderStage::kAll, {0, sink_desc.size});
            }, [&](render_graph::RenderPassCommandContext& command) {
                return command.copy_buffer(from.value, to.value, 0, 0, sink_desc.size);
            });
        if (!produced || !check(writer.compile())) return 25;
        const auto written = writer.execute(frame);
        if (!written || !check(frame.end_frame()) || !check(device.wait_idle())) return 25;
        const auto segments = device.query_buffer_state(shared.value);
        if (!segments || segments.value.segments.size() != 2 ||
            segments.value.segments[0].state.usage != rhi::ResourceUsage::kCopyDestination ||
            segments.value.segments[0].state.queue != rhi::QueueClass::kCopy ||
            segments.value.segments[1].state.usage != rhi::ResourceUsage::kCopySource ||
            segments.value.segments[1].state.queue != rhi::QueueClass::kGraphics) return 25;
        if (!check(reader.compile())) return 25;
        bool refreshed = false;
        for (const auto& barrier : aeg::render_graph::detail::RenderGraphExecution::plan(reader).pass_plans[consumer.value.index].barriers)
            if (barrier.resource_kind == render_graph::RenderGraphResourceKind::kBuffer &&
                barrier.resource_index == source_handle.value.index && barrier.buffer_range.offset == 0 &&
                barrier.buffer_range.size == sink_desc.size && barrier.before.usage == rhi::ResourceUsage::kCopyDestination &&
                barrier.before.queue == rhi::QueueClass::kCopy && barrier.after.usage == rhi::ResourceUsage::kCopySource &&
                barrier.after.queue == rhi::QueueClass::kGraphics) refreshed = true;
        if (!refreshed || aeg::render_graph::detail::RenderGraphExecution::plan(reader).revision <= original_revision) {
            std::cerr << "cached reader retained stale imported buffer state after external partial GPU write\n";
            return 25;
        }
        // Actual state refresh must preserve the repeated-import declaration.
        const auto repeated_source = reader.import_buffer(shared.value, shared_desc);
        if (!repeated_source || repeated_source.value != source_handle.value || !check(frame.begin_frame())) return 25;
        const auto consumed = reader.execute(frame);
        if (!consumed || !check(frame.end_frame()) || !check(device.wait_idle())) return 25;
        std::array<uint32_t, 2> received{};
        if (!check(device.read_buffer(sink.value, 0, std::as_writable_bytes(std::span{received}))) ||
            received != std::array{values[0], values[1]}) return 25;
        reader.reset();
        writer.reset();
        if (!check(frame.drain()) || !check(device.destroy_buffer(sink.value)) || !check(device.destroy_buffer(shared.value))) return 25;
    }
    if (!check(device.destroy_buffer(host_upload.value)) || !check(frame.drain()) || !check(device.destroy_buffer(upload.value)) ||
        !check(device.destroy_buffer(other.value)) || !check(device.destroy_buffer(native.value))) return 19;
    return 0;
}
