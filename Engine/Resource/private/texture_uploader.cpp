/**
 * @file texture_uploader.cpp
 * @brief Implements format-aware frame-local texture uploads.
 */
#include "../public/texture_uploader.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace aeg::resource {
    namespace {
        constexpr uint64_t kBufferCopyAlignment = 4;

        struct TextureFormatInfo {
            uint32_t bytes_per_block{0};
            uint32_t block_width{1};
            uint32_t block_height{1};
            uint32_t block_depth{1};
        };

        struct TextureCopyOperation {
            uint64_t relative_offset{0};
            uint64_t data_size{0};
            uint32_t source_row_length{0};
            uint32_t source_image_height{0};
            rhi::TextureSubresourceLayers destination_subresources{};
            rhi::Extent3D extent{};
        };

        struct PreparedTextureUpload {
            std::vector<TextureCopyOperation> operations{};
            uint64_t transfer_size{0};
        };

        [[nodiscard]] std::optional<TextureFormatInfo> texture_format_info(rhi::Format format) noexcept {
            switch (format) {
                case rhi::Format::kR8Unorm:
                    return TextureFormatInfo{1, 1, 1, 1};
                case rhi::Format::kRg8Unorm:
                    return TextureFormatInfo{2, 1, 1, 1};
                case rhi::Format::kRgba8Unorm:
                case rhi::Format::kBgra8Unorm:
                case rhi::Format::kRgba8Srgb:
                case rhi::Format::kR32Uint:
                case rhi::Format::kR32Float:
                    return TextureFormatInfo{4, 1, 1, 1};
                case rhi::Format::kRgba16Float:
                case rhi::Format::kRg32Float:
                    return TextureFormatInfo{8, 1, 1, 1};
                case rhi::Format::kRgb32Float:
                    return TextureFormatInfo{12, 1, 1, 1};
                case rhi::Format::kRgba32Float:
                    return TextureFormatInfo{16, 1, 1, 1};
                case rhi::Format::kBc1RgbaUnorm:
                    return TextureFormatInfo{8, 4, 4, 1};
                case rhi::Format::kBc3RgbaUnorm:
                case rhi::Format::kBc5Unorm:
                case rhi::Format::kBc7RgbaUnorm:
                    return TextureFormatInfo{16, 4, 4, 1};
                case rhi::Format::kD24UnormS8Uint:
                case rhi::Format::kD32Float:
                case rhi::Format::kD32FloatS8Uint:
                case rhi::Format::kUnknown:
                    return std::nullopt;
            }
            return std::nullopt;
        }

        [[nodiscard]] uint32_t mip_dimension(uint32_t dimension, uint32_t mip) noexcept {
            return mip >= 32 ? 1u : std::max(1u, dimension >> mip);
        }

        [[nodiscard]] uint64_t ceil_div(uint64_t value, uint64_t divisor) noexcept {
            return value / divisor + (value % divisor == 0 ? 0 : 1);
        }

        [[nodiscard]] bool checked_add(uint64_t lhs, uint64_t rhs, uint64_t& result) noexcept {
            if (rhs > std::numeric_limits<uint64_t>::max() - lhs) {
                return false;
            }
            result = lhs + rhs;
            return true;
        }

        [[nodiscard]] bool checked_mul(uint64_t lhs, uint64_t rhs, uint64_t& result) noexcept {
            if (lhs != 0 && rhs > std::numeric_limits<uint64_t>::max() / lhs) {
                return false;
            }
            result = lhs * rhs;
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

        [[nodiscard]] foundation::Result prepare_subresource(
                const TextureSubresourceData& input, const TextureFormatInfo& format, uint32_t mip,
                uint32_t array_layer, const rhi::TextureDesc& desc, uint64_t& cursor,
                TextureCopyOperation& operation) {
            const uint64_t width = mip_dimension(desc.extent.width, mip);
            const uint64_t height = mip_dimension(desc.extent.height, mip);
            const uint64_t depth = desc.dimension == rhi::TextureDimension::kTex3D
                                           ? mip_dimension(desc.extent.depth, mip)
                                           : 1;
            const uint64_t blocks_x = ceil_div(width, format.block_width);
            const uint64_t blocks_y = ceil_div(height, format.block_height);
            const uint64_t blocks_z = ceil_div(depth, format.block_depth);

            uint64_t tight_row_pitch = 0;
            if (!checked_mul(blocks_x, format.bytes_per_block, tight_row_pitch)) {
                return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                    "Texture row pitch overflows");
            }
            const uint64_t row_pitch = input.row_pitch == 0 ? tight_row_pitch : input.row_pitch;
            if (row_pitch < tight_row_pitch || row_pitch % format.bytes_per_block != 0) {
                return foundation::Result::failure(
                        foundation::ResultCode::kInvalidArgument,
                        "Texture row pitch is smaller than the packed row or cannot represent whole blocks");
            }

            const uint64_t row_blocks = row_pitch / format.bytes_per_block;
            uint64_t row_length = 0;
            if (!checked_mul(row_blocks, format.block_width, row_length) || row_length < width) {
                return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                    "Texture row length is invalid");
            }

            uint64_t tight_slice_pitch = 0;
            if (!checked_mul(row_pitch, blocks_y, tight_slice_pitch)) {
                return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                    "Texture slice pitch overflows");
            }
            const uint64_t slice_pitch = input.slice_pitch == 0 ? tight_slice_pitch : input.slice_pitch;
            if (slice_pitch < tight_slice_pitch || slice_pitch % row_pitch != 0) {
                return foundation::Result::failure(
                        foundation::ResultCode::kInvalidArgument,
                        "Texture slice pitch is smaller than the packed slice or is not row aligned");
            }

            const uint64_t image_rows = slice_pitch / row_pitch;
            uint64_t image_height = 0;
            if (!checked_mul(image_rows, format.block_height, image_height) || image_height < height) {
                return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                    "Texture image height is invalid");
            }

            uint64_t required_data_size = 0;
            if (!checked_mul(slice_pitch, blocks_z, required_data_size) ||
                required_data_size > input.data.size()) {
                return foundation::Result::failure(
                        foundation::ResultCode::kInvalidArgument,
                        "Texture subresource data does not contain the declared footprint");
            }
            if (row_length > std::numeric_limits<uint32_t>::max() ||
                image_height > std::numeric_limits<uint32_t>::max()) {
                return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                    "Texture copy dimensions exceed the RHI limits");
            }

            uint64_t aligned_offset = 0;
            if (!align_up(cursor, kBufferCopyAlignment, aligned_offset)) {
                return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                                    "Texture staging layout overflows");
            }
            uint64_t next_cursor = 0;
            if (!checked_add(aligned_offset, required_data_size, next_cursor)) {
                return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                                    "Texture staging layout overflows");
            }

            operation.relative_offset = aligned_offset;
            operation.data_size = required_data_size;
            operation.source_row_length = row_pitch == tight_row_pitch ? 0 : static_cast<uint32_t>(row_length);
            operation.source_image_height = slice_pitch == tight_slice_pitch ? 0 : static_cast<uint32_t>(image_height);
            operation.destination_subresources = rhi::TextureSubresourceLayers{
                rhi::Aspect::kColor,
                mip,
                desc.dimension == rhi::TextureDimension::kTex3D ? 0 : array_layer,
                1
            };
            operation.extent = rhi::Extent3D{
                static_cast<uint32_t>(width),
                static_cast<uint32_t>(height),
                static_cast<uint32_t>(depth)
            };
            cursor = next_cursor;
            return foundation::Result::success();
        }

        [[nodiscard]] foundation::ResultValue<PreparedTextureUpload> prepare_texture_layout(
                const TextureData& texture) {
            const foundation::Result validation_result = validate_texture_data(texture);
            if (!validation_result) {
                return {validation_result, {}};
            }

            const std::optional<TextureFormatInfo> format = texture_format_info(texture.desc.format);
            if (!format.has_value()) {
                return {foundation::Result::failure(
                                foundation::ResultCode::kUnsupported,
                                "Texture uploader does not support this texture format"), {}};
            }

            PreparedTextureUpload prepared{};
            prepared.operations.reserve(texture.subresources.size());
            uint64_t cursor = 0;
            size_t subresource_index = 0;
            for (uint32_t mip = 0; mip < texture.desc.mip_levels; ++mip) {
                for (uint32_t array_layer = 0; array_layer < texture.desc.array_layers; ++array_layer) {
                    TextureCopyOperation operation{};
                    const foundation::Result result = prepare_subresource(
                            texture.subresources[subresource_index], format.value(), mip, array_layer,
                            texture.desc, cursor, operation);
                    if (!result) {
                        return {result, {}};
                    }
                    prepared.operations.push_back(operation);
                    ++subresource_index;
                }
            }
            prepared.transfer_size = cursor;
            return {foundation::Result::success(), std::move(prepared)};
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
            desc.name = rhi::DebugName{"TextureUploader.Staging"};
            return desc;
        }

        [[nodiscard]] foundation::Result add_texture_copy_pass(
                render_graph::RenderGraph& graph, std::string_view name,
                render_graph::RenderGraphBuffer source, render_graph::RenderGraphTexture target,
                uint64_t source_offset, uint64_t transfer_size, uint32_t mip_levels, uint32_t array_layers,
                std::vector<TextureCopyOperation> operations) {
            const render_graph::RenderGraphBufferRange source_range{source_offset, transfer_size};
            const rhi::SubresourceRange target_range{
                rhi::Aspect::kColor, 0, mip_levels, 0, array_layers
            };
            const auto pass_result = graph.add_pass(
                    name, rhi::QueueClass::kCopy,
                    [source, target, source_range, target_range](render_graph::PassBuilder& builder) {
                        if (const foundation::Result result = builder.read(
                                    source, rhi::ResourceUsage::kCopySource, rhi::PipelineStage::kCopy,
                                    rhi::ShaderStage::kAll, source_range);
                            !result) {
                            return result;
                        }
                        if (const foundation::Result result = builder.write(
                                    target, rhi::ResourceUsage::kCopyDestination, rhi::PipelineStage::kCopy,
                                    rhi::ShaderStage::kAll, target_range, true);
                            !result) {
                            return result;
                        }
                        builder.set_side_effects();
                        return foundation::Result::success();
                    },
                    [source, target, operations = std::move(operations)](
                            render_graph::PassExecutionContext& context) {
                        for (const TextureCopyOperation& operation: operations) {
                            if (const foundation::Result result = context.copy_buffer_to_texture(
                                        source, operation.relative_offset, operation.source_row_length,
                                        operation.source_image_height, target, operation.destination_subresources,
                                        rhi::Offset3D{}, operation.extent);
                                !result) {
                                return result;
                            }
                        }
                        return foundation::Result::success();
                    });
            return pass_result.result;
        }
    } // namespace

    foundation::ResultValue<TextureUpload> TextureUploader::upload_texture(
            render_graph::FrameContext& frame_context, render_graph::RenderGraph& graph,
            const TextureData& texture, std::string_view name) {
        try {
            if (&frame_context.device() != &device_ || &graph.device() != &device_) {
                return {foundation::Result::failure(
                                foundation::ResultCode::kInvalidArgument,
                                "Texture upload objects must use the same RHI device"), {}};
            }

            auto layout_result = prepare_texture_layout(texture);
            if (!layout_result) {
                return {layout_result.result, {}};
            }
            PreparedTextureUpload prepared = std::move(layout_result.value);

            rhi::TextureDesc destination_desc = texture.desc;
            destination_desc.capabilities |= rhi::TextureCapability::kTransferDestination;
            if (destination_desc.memory == rhi::MemoryClass::kAuto) {
                destination_desc.memory = rhi::MemoryClass::kDeviceLocal;
            }
            // A newly created image has undefined contents and layout. The
            // upload pass establishes the first meaningful resource state.
            destination_desc.initial_usage = rhi::ResourceUsage::kUndefined;
            destination_desc.initial_stages = rhi::PipelineStage::kAllCommands;
            destination_desc.initial_queue = rhi::QueueClass::kAuto;
            const std::string_view resource_name = name.empty()
                                                           ? (texture.desc.name.value.empty()
                                                                      ? std::string_view{"Resource.Texture"}
                                                                      : texture.desc.name.value)
                                                           : name;
            destination_desc.name = rhi::DebugName{resource_name};

            auto allocation_result = frame_context.allocate_upload(prepared.transfer_size, kBufferCopyAlignment);
            if (!allocation_result) {
                return {allocation_result.result, {}};
            }
            const render_graph::FrameAllocation allocation = allocation_result.value;
            for (size_t index = 0; index < prepared.operations.size(); ++index) {
                const TextureCopyOperation& operation = prepared.operations[index];
                const size_t data_size = static_cast<size_t>(operation.data_size);
                const foundation::Result write_result = frame_context.write_upload(
                        allocation, operation.relative_offset,
                        texture.subresources[index].data.first(data_size));
                if (!write_result) {
                    return {write_result, {}};
                }
            }

            auto texture_result = device_.create_texture(destination_desc);
            if (!texture_result) {
                return {texture_result.result, {}};
            }
            GpuTexture gpu_texture{};
            gpu_texture.texture = texture_result.value;
            gpu_texture.format = destination_desc.format;
            gpu_texture.extent = destination_desc.extent;
            gpu_texture.mip_levels = destination_desc.mip_levels;
            gpu_texture.array_layers = destination_desc.array_layers;

            const auto cleanup_texture = [this, &gpu_texture]() {
                if (gpu_texture.sampled_view.is_valid()) {
                    (void) device_.destroy_texture_view(gpu_texture.sampled_view);
                    gpu_texture.sampled_view = {};
                }
                if (gpu_texture.texture.is_valid()) {
                    (void) device_.destroy_texture(gpu_texture.texture);
                    gpu_texture.texture = {};
                }
            };

            if (rhi::has_flag(destination_desc.capabilities, rhi::TextureCapability::kSampled)) {
                rhi::TextureViewDesc view_desc{};
                view_desc.texture = gpu_texture.texture;
                view_desc.kind = rhi::TextureViewKind::kSampled;
                view_desc.subresources = rhi::SubresourceRange{
                    rhi::Aspect::kColor, 0, destination_desc.mip_levels, 0, destination_desc.array_layers
                };
                view_desc.name = rhi::DebugName{resource_name};
                auto view_result = device_.create_texture_view(view_desc);
                if (!view_result) {
                    cleanup_texture();
                    return {view_result.result, {}};
                }
                gpu_texture.sampled_view = view_result.value;
            }

            const rhi::ResourceState initial_state{
                rhi::ResourceUsage::kUndefined,
                rhi::ShaderStage::kAll,
                rhi::QueueClass::kAuto,
                false,
                rhi::PipelineStage::kAllCommands
            };
            auto target_result = graph.import_texture(
                    gpu_texture.texture, gpu_texture.sampled_view, destination_desc, initial_state);
            if (!target_result) {
                cleanup_texture();
                return {target_result.result, {}};
            }
            auto source_result = graph.import_buffer(
                    allocation.buffer, make_staging_desc(allocation),
                    rhi::ResourceState{
                        rhi::ResourceUsage::kHostWrite,
                        rhi::ShaderStage::kAll,
                        rhi::QueueClass::kCopy,
                        false,
                        rhi::PipelineStage::kHost
                    });
            if (!source_result) {
                cleanup_texture();
                return {source_result.result, {}};
            }

            for (TextureCopyOperation& operation: prepared.operations) {
                if (!checked_add(operation.relative_offset, allocation.offset, operation.relative_offset)) {
                    cleanup_texture();
                    return {foundation::Result::failure(
                                    foundation::ResultCode::kInvalidArgument,
                                    "Texture staging source offset overflows"), {}};
                }
            }
            const foundation::Result pass_result = add_texture_copy_pass(
                    graph, resource_name, source_result.value, target_result.value, allocation.offset,
                    prepared.transfer_size, destination_desc.mip_levels, destination_desc.array_layers,
                    prepared.operations);
            if (!pass_result) {
                cleanup_texture();
                return {pass_result, {}};
            }

            return {foundation::Result::success(),
                    TextureUpload{
                        gpu_texture,
                        allocation,
                        source_result.value,
                        target_result.value,
                        prepared.transfer_size,
                        static_cast<uint32_t>(prepared.operations.size())
                    }};
        } catch (const std::bad_alloc&) {
            return {foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                                 "Texture upload bookkeeping allocation failed"), {}};
        }
    }

    foundation::Result TextureUploader::defer_destroy_texture(
            render_graph::FrameContext& frame_context, GpuTexture& texture) const {
        if (texture.sampled_view.is_valid()) {
            const foundation::Result result = frame_context.defer_destroy_texture_view(texture.sampled_view);
            if (!result) {
                return result;
            }
            texture.sampled_view = {};
        }
        if (texture.texture.is_valid()) {
            const foundation::Result result = frame_context.defer_destroy_texture(texture.texture);
            if (!result) {
                return result;
            }
            texture.texture = {};
        }
        if (!texture.sampled_view.is_valid() && !texture.texture.is_valid()) {
            texture.reset();
        }
        return foundation::Result::success();
    }

    foundation::Result TextureUploader::destroy_texture(GpuTexture& texture) const {
        if (texture.sampled_view.is_valid()) {
            const foundation::Result result = device_.destroy_texture_view(texture.sampled_view);
            if (!result && result.code != foundation::ResultCode::kNotFound) {
                return result;
            }
            texture.sampled_view = {};
        }
        if (texture.texture.is_valid()) {
            const foundation::Result result = device_.destroy_texture(texture.texture);
            if (!result && result.code != foundation::ResultCode::kNotFound) {
                return result;
            }
            texture.texture = {};
        }
        if (!texture.sampled_view.is_valid() && !texture.texture.is_valid()) {
            texture.reset();
        }
        return foundation::Result::success();
    }
} // namespace aeg::resource
