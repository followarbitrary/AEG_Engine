/**
 * @file resource_uploader.cpp
 * @brief Implements frame-local buffer and indexed-mesh uploads.
 */
#include "../public/resource_uploader.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace aeg::resource {
    namespace {
        constexpr uint64_t kBufferCopyAlignment = 4;

        struct PreparedUpload {
            render_graph::FrameAllocation staging{};
            rhi::BufferHandle destination{};
            render_graph::RenderGraphBuffer source{};
            render_graph::RenderGraphBuffer target{};
            uint64_t destination_offset{0};
            uint64_t size{0};
            uint64_t transfer_size{0};
        };

        struct CopyOperation {
            render_graph::RenderGraphBuffer source{};
            render_graph::RenderGraphBuffer target{};
            uint64_t source_offset{0};
            uint64_t destination_offset{0};
            uint64_t copy_size{0};
        };

        [[nodiscard]] bool checked_add(uint64_t lhs, uint64_t rhs, uint64_t& result) noexcept {
            if (rhs > std::numeric_limits<uint64_t>::max() - lhs) {
                return false;
            }
            result = lhs + rhs;
            return true;
        }

        [[nodiscard]] bool align_up(uint64_t value, uint64_t alignment, uint64_t& result) noexcept {
            if (alignment == 0) {
                return false;
            }
            const uint64_t remainder = value % alignment;
            const uint64_t padding = remainder == 0 ? 0 : alignment - remainder;
            return checked_add(value, padding, result);
        }

        [[nodiscard]] bool normalize_upload_alignment(uint64_t requested, uint64_t& alignment) noexcept {
            alignment = requested == 0 ? kBufferCopyAlignment : requested;
            if (alignment < kBufferCopyAlignment) {
                alignment = kBufferCopyAlignment;
            }
            const uint64_t remainder = alignment % kBufferCopyAlignment;
            if (remainder == 0) {
                return true;
            }
            return checked_add(alignment, kBufferCopyAlignment - remainder, alignment);
        }

        [[nodiscard]] rhi::BufferDesc make_staging_desc(const render_graph::FrameAllocation& allocation) noexcept {
            rhi::BufferDesc desc{};
            desc.size = allocation.buffer_size;
            desc.capabilities = rhi::BufferCapability::kTransferSource |
                                 rhi::BufferCapability::kTransferDestination;
            desc.memory = rhi::MemoryClass::kUpload;
            desc.initial_usage = rhi::ResourceUsage::kHostWrite;
            desc.initial_stages = rhi::PipelineStage::kHost;
            desc.initial_queue = rhi::QueueClass::kCopy;
            desc.name = rhi::DebugName{"ResourceUploader.Staging"};
            return desc;
        }

        [[nodiscard]] rhi::ResourceState make_initial_state(const rhi::BufferDesc& desc) noexcept {
            return rhi::ResourceState{desc.initial_usage, rhi::ShaderStage::kAll, desc.initial_queue, false,
                                       desc.initial_stages};
        }

        [[nodiscard]] foundation::Result validate_upload_description(const BufferUploadDesc& input,
                                                                      rhi::BufferDesc& destination,
                                                                      uint64_t& size, uint64_t& transfer_size,
                                                                      uint64_t& allocation_alignment) noexcept {
            if (input.data.empty()) {
                return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                    "Buffer upload data must not be empty");
            }
            if (input.data.size() > std::numeric_limits<uint64_t>::max()) {
                return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                    "Buffer upload data size overflows");
            }
            size = static_cast<uint64_t>(input.data.size());
            if ((input.destination_offset % kBufferCopyAlignment) != 0) {
                return foundation::Result::failure(
                        foundation::ResultCode::kInvalidArgument,
                        "Buffer upload destination offset must be four-byte aligned");
            }
            if (!align_up(size, kBufferCopyAlignment, transfer_size)) {
                return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                    "Buffer upload transfer size overflows");
            }
            if (!normalize_upload_alignment(input.alignment, allocation_alignment)) {
                return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                    "Buffer upload alignment overflows");
            }

            uint64_t required_size = 0;
            if (!checked_add(input.destination_offset, transfer_size, required_size)) {
                return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                    "Buffer upload destination range overflows");
            }

            destination = input.destination;
            if (destination.size == 0) {
                destination.size = required_size;
            }
            if (destination.size == 0 || required_size > destination.size) {
                return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                    "Buffer upload destination is smaller than the uploaded data");
            }
            if (destination.memory == rhi::MemoryClass::kUpload ||
                destination.memory == rhi::MemoryClass::kReadback) {
                return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                    "Buffer upload destination must use device-local memory");
            }
            if (destination.memory == rhi::MemoryClass::kAuto) {
                destination.memory = rhi::MemoryClass::kDeviceLocal;
            }
            destination.capabilities |= rhi::BufferCapability::kTransferDestination;
            if (destination.name.value.empty()) {
                destination.name = rhi::DebugName{"ResourceUploader.Buffer"};
            }
            return foundation::Result::success();
        }

        [[nodiscard]] foundation::Result write_upload_data(
                rhi::Device& device, render_graph::FrameContext& frame_context,
                const render_graph::FrameAllocation& allocation, std::span<const std::byte> data,
                uint64_t transfer_size) {
            const foundation::Result write_result = frame_context.write_upload(allocation, data);
            if (!write_result) {
                return write_result;
            }
            const uint64_t padding_size = transfer_size - static_cast<uint64_t>(data.size());
            if (padding_size == 0) {
                return foundation::Result::success();
            }

            std::array<std::byte, kBufferCopyAlignment> zero_padding{};
            return device.write_buffer(
                    allocation.buffer, allocation.offset + static_cast<uint64_t>(data.size()),
                    std::span<const std::byte>{zero_padding}.first(static_cast<size_t>(padding_size)));
        }

        [[nodiscard]] foundation::ResultValue<PreparedUpload> prepare_upload(
                rhi::Device& device, render_graph::FrameContext& frame_context, render_graph::RenderGraph& graph,
                const BufferUploadDesc& input) {
            if (&frame_context.device() != &device || &graph.device() != &device) {
                return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                     "Resource upload objects must use the same RHI device"), {}};
            }

            rhi::BufferDesc destination_desc{};
            uint64_t size = 0;
            uint64_t transfer_size = 0;
            uint64_t allocation_alignment = 0;
            const foundation::Result validation_result =
                    validate_upload_description(input, destination_desc, size, transfer_size,
                                                 allocation_alignment);
            if (!validation_result) {
                return {validation_result, {}};
            }

            auto allocation_result = frame_context.allocate_upload(transfer_size, allocation_alignment);
            if (!allocation_result) {
                return {allocation_result.result, {}};
            }
            const render_graph::FrameAllocation allocation = allocation_result.value;
            if (allocation.buffer_size == 0) {
                return {foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                                     "Frame upload allocation does not expose its buffer size"), {}};
            }
            const foundation::Result write_result = write_upload_data(
                    device, frame_context, allocation, input.data, transfer_size);
            if (!write_result) {
                return {write_result, {}};
            }

            auto destination_result = device.create_buffer(destination_desc);
            if (!destination_result) {
                return {destination_result.result, {}};
            }

            auto target_resource_result = graph.import_buffer(destination_result.value, destination_desc,
                                                               make_initial_state(destination_desc));
            if (!target_resource_result) {
                (void) device.destroy_buffer(destination_result.value);
                return {target_resource_result.result, {}};
            }

            return {foundation::Result::success(),
                    PreparedUpload{allocation, destination_result.value, {}, target_resource_result.value,
                                   input.destination_offset, size, transfer_size}};
        }

        [[nodiscard]] foundation::ResultValue<render_graph::RenderGraphBuffer> import_staging_buffer(
                render_graph::RenderGraph& graph, const render_graph::FrameAllocation& allocation) {
            const rhi::BufferDesc staging_desc = make_staging_desc(allocation);
            return graph.import_buffer(
                    allocation.buffer, staging_desc,
                    rhi::ResourceState{rhi::ResourceUsage::kHostWrite, rhi::ShaderStage::kAll,
                                       rhi::QueueClass::kCopy, false, rhi::PipelineStage::kHost});
        }

        template<size_t Count>
        [[nodiscard]] foundation::Result add_copy_pass(render_graph::RenderGraph& graph, std::string_view name,
                                                        const std::array<CopyOperation, Count>& operations) {
            return graph.add_pass(
                           name, rhi::QueueClass::kCopy,
                           [operations](render_graph::PassBuilder& builder) {
                               for (const CopyOperation& operation: operations) {
                                   if (const foundation::Result result = builder.read(
                                               operation.source, rhi::ResourceUsage::kCopySource,
                                               rhi::PipelineStage::kCopy, rhi::ShaderStage::kAll,
                                               render_graph::RenderGraphBufferRange{operation.source_offset,
                                                                                    operation.copy_size});
                                       !result) {
                                       return result;
                                   }
                                   if (const foundation::Result result = builder.write(
                                               operation.target, rhi::ResourceUsage::kCopyDestination,
                                               rhi::PipelineStage::kCopy, rhi::ShaderStage::kAll,
                                               render_graph::RenderGraphBufferRange{operation.destination_offset,
                                                                                    operation.copy_size},
                                               true);
                                       !result) {
                                       return result;
                                   }
                               }
                               builder.set_side_effects();
                               return foundation::Result::success();
                           },
                           [operations](render_graph::PassExecutionContext& context) {
                               for (const CopyOperation& operation: operations) {
                                   if (const foundation::Result result = context.copy_buffer(
                                           operation.source, operation.target, operation.source_offset,
                                           operation.destination_offset, operation.copy_size);
                                       !result) {
                                       return result;
                                   }
                               }
                               return foundation::Result::success();
                           })
                    .result;
        }

        [[nodiscard]] foundation::Result first_failure(const foundation::Result& first,
                                                        const foundation::Result& current) noexcept {
            if (!first || current.code == foundation::ResultCode::kNotFound) {
                return first;
            }
            return current;
        }
    } // namespace

    foundation::ResultValue<BufferUpload> ResourceUploader::upload_buffer(
            render_graph::FrameContext& frame_context, render_graph::RenderGraph& graph,
            const BufferUploadDesc& desc) {
        auto prepared_result = prepare_upload(device_, frame_context, graph, desc);
        if (!prepared_result) {
            return {prepared_result.result, {}};
        }
        PreparedUpload prepared = prepared_result.value;
        auto source_resource_result = import_staging_buffer(graph, prepared.staging);
        if (!source_resource_result) {
            (void) device_.destroy_buffer(prepared.destination);
            return {source_resource_result.result, {}};
        }
        prepared.source = source_resource_result.value;
        const std::array<CopyOperation, 1> operations{
                CopyOperation{prepared.source, prepared.target, prepared.staging.offset, prepared.destination_offset,
                              prepared.transfer_size}
        };
        const std::string_view pass_name = desc.destination.name.value.empty()
                                                    ? std::string_view{"ResourceUploader.BufferUpload"}
                                                    : desc.destination.name.value;
        const foundation::Result pass_result = add_copy_pass(graph, pass_name, operations);
        if (!pass_result) {
            (void) device_.destroy_buffer(prepared.destination);
            return {pass_result, {}};
        }

        return {foundation::Result::success(),
                BufferUpload{prepared.staging, prepared.destination, prepared.source, prepared.target,
                             prepared.destination_offset, prepared.size, prepared.transfer_size}};
    }

    foundation::ResultValue<MeshUpload> ResourceUploader::upload_mesh(
            render_graph::FrameContext& frame_context, render_graph::RenderGraph& graph, const MeshData& mesh,
            std::string_view name) {
        const foundation::Result mesh_result = validate_mesh_data(mesh);
        if (!mesh_result) {
            return {mesh_result, {}};
        }
        if (&frame_context.device() != &device_ || &graph.device() != &device_) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                 "Resource upload objects must use the same RHI device"), {}};
        }

        const std::string_view resource_name = name.empty() ? std::string_view{"Resource.Mesh"} : name;
        rhi::BufferDesc vertex_destination{};
        vertex_destination.capabilities = rhi::BufferCapability::kTransferDestination |
                                           rhi::BufferCapability::kVertex;
        vertex_destination.memory = rhi::MemoryClass::kDeviceLocal;
        vertex_destination.name = rhi::DebugName{resource_name};

        rhi::BufferDesc index_destination{};
        index_destination.capabilities = rhi::BufferCapability::kTransferDestination |
                                          rhi::BufferCapability::kIndex;
        index_destination.memory = rhi::MemoryClass::kDeviceLocal;
        index_destination.name = rhi::DebugName{resource_name};

        const BufferUploadDesc vertex_upload_desc{mesh.vertex_data, vertex_destination, 0, 4};
        const BufferUploadDesc index_upload_desc{mesh.index_data, index_destination, 0,
                                                 4};

        auto vertex_result = prepare_upload(device_, frame_context, graph, vertex_upload_desc);
        if (!vertex_result) {
            return {vertex_result.result, {}};
        }
        auto index_result = prepare_upload(device_, frame_context, graph, index_upload_desc);
        if (!index_result) {
            (void) device_.destroy_buffer(vertex_result.value.destination);
            return {index_result.result, {}};
        }

        PreparedUpload vertex = vertex_result.value;
        PreparedUpload index = index_result.value;
        auto vertex_source_result = import_staging_buffer(graph, vertex.staging);
        if (!vertex_source_result) {
            (void) device_.destroy_buffer(vertex.destination);
            (void) device_.destroy_buffer(index.destination);
            return {vertex_source_result.result, {}};
        }
        vertex.source = vertex_source_result.value;

        if (index.staging.buffer == vertex.staging.buffer) {
            index.source = vertex.source;
        } else {
            auto index_source_result = import_staging_buffer(graph, index.staging);
            if (!index_source_result) {
                (void) device_.destroy_buffer(vertex.destination);
                (void) device_.destroy_buffer(index.destination);
                return {index_source_result.result, {}};
            }
            index.source = index_source_result.value;
        }
        const std::array<CopyOperation, 2> operations{
                CopyOperation{vertex.source, vertex.target, vertex.staging.offset, vertex.destination_offset,
                              vertex.transfer_size},
                CopyOperation{index.source, index.target, index.staging.offset, index.destination_offset,
                              index.transfer_size}
        };
        const foundation::Result pass_result = add_copy_pass(graph, resource_name, operations);
        if (!pass_result) {
            (void) device_.destroy_buffer(vertex.destination);
            (void) device_.destroy_buffer(index.destination);
            return {pass_result, {}};
        }

        GpuMesh gpu_mesh{};
        gpu_mesh.vertex_buffer = vertex.destination;
        gpu_mesh.index_buffer = index.destination;
        gpu_mesh.vertex_buffer_size = vertex.transfer_size;
        gpu_mesh.index_buffer_size = index.transfer_size;
        gpu_mesh.vertex_stride = mesh.vertex_stride;
        gpu_mesh.vertex_count = mesh.vertex_count;
        gpu_mesh.index_count = mesh.index_count;
        gpu_mesh.index_format = mesh.index_format;
        return {foundation::Result::success(), MeshUpload{gpu_mesh, vertex.target, index.target}};
    }

    foundation::Result ResourceUploader::defer_destroy_mesh(render_graph::FrameContext& frame_context,
                                                             GpuMesh& mesh) const {
        foundation::Result first_result = foundation::Result::success();
        if (mesh.vertex_buffer.is_valid()) {
            const foundation::Result result = frame_context.defer_destroy_buffer(mesh.vertex_buffer);
            if (!result) {
                first_result = first_failure(first_result, result);
            } else {
                mesh.vertex_buffer = {};
            }
        }
        if (mesh.index_buffer.is_valid()) {
            const foundation::Result result = frame_context.defer_destroy_buffer(mesh.index_buffer);
            if (!result) {
                first_result = first_failure(first_result, result);
            } else {
                mesh.index_buffer = {};
            }
        }
        if (!mesh.vertex_buffer.is_valid() && !mesh.index_buffer.is_valid()) {
            mesh.reset();
        }
        return first_result;
    }

    foundation::Result ResourceUploader::destroy_mesh(GpuMesh& mesh) const {
        foundation::Result first_result = foundation::Result::success();
        if (mesh.vertex_buffer.is_valid()) {
            const foundation::Result result = device_.destroy_buffer(mesh.vertex_buffer);
            if (result || result.code == foundation::ResultCode::kNotFound) {
                mesh.vertex_buffer = {};
            } else {
                first_result = first_failure(first_result, result);
            }
        }
        if (mesh.index_buffer.is_valid()) {
            const foundation::Result result = device_.destroy_buffer(mesh.index_buffer);
            if (result || result.code == foundation::ResultCode::kNotFound) {
                mesh.index_buffer = {};
            } else {
                first_result = first_failure(first_result, result);
            }
        }
        if (!mesh.vertex_buffer.is_valid() && !mesh.index_buffer.is_valid()) {
            mesh.reset();
        }
        return first_result;
    }
} // namespace aeg::resource
