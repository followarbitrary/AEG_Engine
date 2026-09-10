/**
 * @file render_graph.cpp
 * @brief Implements render-graph compilation, transient allocation, and execution.
 */
#include "../public/render_graph.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aeg::render_graph {
    namespace {
        std::atomic<uint64_t> g_next_graph_id{1};

        [[nodiscard]] uint64_t allocate_graph_id() noexcept {
            uint64_t graph_id = g_next_graph_id.fetch_add(1, std::memory_order_relaxed);
            while (graph_id == 0) {
                graph_id = g_next_graph_id.fetch_add(1, std::memory_order_relaxed);
            }
            return graph_id;
        }

        constexpr uint32_t kAspectPlaneCount = 3;

        enum class ResourceKind : uint8_t { kBuffer, kTexture };

        struct OwnedBufferDesc {
            rhi::BufferDesc desc{};
            std::string name{};

            [[nodiscard]] rhi::BufferDesc materialize() const noexcept {
                rhi::BufferDesc result = desc;
                result.name = rhi::DebugName{name};
                return result;
            }
        };

        struct OwnedTextureDesc {
            rhi::TextureDesc desc{};
            std::vector<rhi::Format> compatible_formats{};
            std::string name{};

            [[nodiscard]] rhi::TextureDesc materialize() const noexcept {
                rhi::TextureDesc result = desc;
                result.compatible_view_formats = compatible_formats;
                result.name = rhi::DebugName{name};
                return result;
            }
        };

        struct BufferViewCacheEntry {
            rhi::BufferViewDesc desc{};
            rhi::BufferViewHandle handle{};
        };

        struct BufferStateSegment {
            uint64_t offset{0};
            uint64_t size{0};
            rhi::ResourceState state{};
        };

        struct PhysicalBuffer {
            rhi::BufferHandle handle{};
            OwnedBufferDesc desc{};
            rhi::ResourceState state{};
            std::vector<BufferStateSegment> states{};
            std::vector<BufferViewCacheEntry> views{};
            bool owns_handle{false};
        };

        struct TextureViewCacheEntry {
            rhi::TextureViewDesc desc{};
            rhi::TextureViewHandle handle{};
            bool owns_handle{false};
        };

        struct PhysicalTexture {
            rhi::TextureHandle handle{};
            OwnedTextureDesc desc{};
            std::vector<rhi::ResourceState> states{};
            std::vector<TextureViewCacheEntry> views{};
            bool owns_handle{false};
        };

        struct ResourceUse {
            ResourceKind kind{ResourceKind::kBuffer};
            uint32_t resource_index{kInvalidHandleIndex};
            uint32_t resource_generation{0};
            ResourceAccess access{ResourceAccess::kRead};
            rhi::ResourceState before_state{};
            rhi::ResourceState after_state{};
            RenderGraphBufferRange buffer_range{};
            rhi::SubresourceRange subresources{};
            bool discard_on_first_use{false};
            bool requires_memory_dependency{false};
            bool requires_queue_transfer{false};
        };

        struct ColorAttachment {
            RenderGraphTexture texture{};
            rhi::LoadOp load_op{rhi::LoadOp::kClear};
            rhi::StoreOp store_op{rhi::StoreOp::kStore};
            rhi::ClearColorValue clear_color{};
            rhi::ResourceUsage final_usage{rhi::ResourceUsage::kColorAttachmentWrite};
            rhi::SubresourceRange subresources{};
        };

        struct DepthStencilAttachment {
            RenderGraphTexture texture{};
            rhi::LoadOp load_op{rhi::LoadOp::kClear};
            rhi::StoreOp store_op{rhi::StoreOp::kStore};
            float clear_depth{1.0f};
            uint32_t clear_stencil{0};
            rhi::ResourceUsage final_usage{rhi::ResourceUsage::kDepthStencilWrite};
            rhi::SubresourceRange subresources{};
        };

        struct BufferRecord {
            uint32_t generation{0};
            ResourceLifetime lifetime{ResourceLifetime::kTransient};
            OwnedBufferDesc desc{};
            rhi::BufferHandle imported{};
            PhysicalBuffer physical{};
            rhi::GpuCompletionSet last_completions{};
            bool exported{false};
            bool has_use{false};
            uint32_t first_use{kInvalidHandleIndex};
            uint32_t last_use{kInvalidHandleIndex};
            uint32_t physical_slot{kInvalidHandleIndex};
        };

        struct TextureRecord {
            uint32_t generation{0};
            ResourceLifetime lifetime{ResourceLifetime::kTransient};
            OwnedTextureDesc desc{};
            rhi::TextureHandle imported{};
            rhi::TextureViewHandle imported_default_view{};
            PhysicalTexture physical{};
            rhi::ResourceState imported_state{};
            rhi::GpuCompletionSet last_completions{};
            bool is_swapchain_image{false};
            bool exported{false};
            bool has_use{false};
            uint32_t first_use{kInvalidHandleIndex};
            uint32_t last_use{kInvalidHandleIndex};
            uint32_t physical_slot{kInvalidHandleIndex};
        };

        struct PassRecord {
            uint32_t generation{0};
            std::string name{};
            rhi::QueueClass queue{rhi::QueueClass::kGraphics};
            std::vector<ResourceUse> uses{};
            std::vector<ColorAttachment> color_attachments{};
            std::optional<DepthStencilAttachment> depth_stencil{};
            PassSetupCallback setup{};
            PassExecuteCallback execute{};
            bool side_effects{false};
        };

        struct TransientBufferSlotPlan {
            OwnedBufferDesc desc{};
            uint32_t last_resource{kInvalidHandleIndex};
            uint32_t last_use{kInvalidHandleIndex};
        };

        struct TransientTextureSlotPlan {
            OwnedTextureDesc desc{};
            uint32_t last_resource{kInvalidHandleIndex};
            uint32_t last_use{kInvalidHandleIndex};
        };

        struct TransientPool {
            std::vector<PhysicalBuffer> buffers{};
            std::vector<PhysicalTexture> textures{};
        };

        struct BatchDependency {
            uint32_t source_batch{kInvalidHandleIndex};
            rhi::PipelineStageMask destination_stages{rhi::PipelineStage::kNone};
            uint32_t resource_hazard_count{0};
        };

        struct Batch {
            rhi::QueueClass queue{rhi::QueueClass::kGraphics};
            std::vector<uint32_t> passes{};
            std::vector<BatchDependency> dependencies{};
            rhi::GpuSyncPoint completion{};
            bool waits_for_acquire{false};
            bool submits_frame{false};
        };

        struct UseReference {
            uint32_t pass_index{kInvalidHandleIndex};
            const ResourceUse* use{nullptr};
        };

        struct Edge {
            uint32_t source{kInvalidHandleIndex};
            uint32_t destination{kInvalidHandleIndex};

            friend bool operator<(const Edge& lhs, const Edge& rhs) noexcept {
                if (lhs.source != rhs.source) {
                    return lhs.source < rhs.source;
                }
                return lhs.destination < rhs.destination;
            }

            friend bool operator==(const Edge&, const Edge&) = default;
        };

        struct RuntimeBufferState {
            PhysicalBuffer* physical{nullptr};
            std::vector<BufferStateSegment> states{};
            RuntimeBufferState* shared{nullptr};
        };

        struct RuntimeTextureState {
            PhysicalTexture* physical{nullptr};
            std::vector<rhi::ResourceState> states{};
            RuntimeTextureState* shared{nullptr};
        };

        [[nodiscard]] bool is_depth_format(rhi::Format format) noexcept {
            return format == rhi::Format::kD24UnormS8Uint || format == rhi::Format::kD32Float ||
                   format == rhi::Format::kD32FloatS8Uint;
        }

        [[nodiscard]] bool has_stencil(rhi::Format format) noexcept {
            return format == rhi::Format::kD24UnormS8Uint || format == rhi::Format::kD32FloatS8Uint;
        }

        [[nodiscard]] rhi::AspectMask default_aspect(rhi::Format format) noexcept {
            if (has_stencil(format)) {
                return rhi::Aspect::kDepth | rhi::Aspect::kStencil;
            }
            return is_depth_format(format) ? rhi::Aspect::kDepth : rhi::Aspect::kColor;
        }

        [[nodiscard]] bool is_write_access(ResourceAccess access) noexcept {
            return access == ResourceAccess::kWrite || access == ResourceAccess::kReadWrite;
        }

        [[nodiscard]] bool state_matches(const rhi::ResourceState& lhs,
                                         const rhi::ResourceState& rhs) noexcept {
            return lhs.usage == rhs.usage && lhs.shader_stages == rhs.shader_stages && lhs.queue == rhs.queue &&
                   lhs.pipeline_stages == rhs.pipeline_stages;
        }

        [[nodiscard]] rhi::ResourceState normalize_state(rhi::ResourceState state,
                                                          rhi::QueueClass queue) noexcept {
            if (state.queue == rhi::QueueClass::kAuto) {
                state.queue = queue;
            }
            state.discard_contents = false;
            return state;
        }

        [[nodiscard]] bool resolve_buffer_range(uint64_t buffer_size, RenderGraphBufferRange range,
                                                 uint64_t& end) noexcept {
            if (range.offset > buffer_size) {
                return false;
            }
            if (range.size == std::numeric_limits<uint64_t>::max()) {
                end = buffer_size;
                return true;
            }
            if (range.size > buffer_size - range.offset) {
                return false;
            }
            end = range.offset + range.size;
            return true;
        }

        void split_buffer_state_segment(std::vector<BufferStateSegment>& states, uint64_t position) {
            for (size_t index = 0; index < states.size(); ++index) {
                BufferStateSegment& segment = states[index];
                const uint64_t segment_end = segment.offset + segment.size;
                if (position <= segment.offset || position >= segment_end) {
                    continue;
                }
                const BufferStateSegment right{position, segment_end - position, segment.state};
                segment.size = position - segment.offset;
                states.insert(states.begin() + static_cast<std::ptrdiff_t>(index + 1), right);
                return;
            }
        }

        void merge_buffer_state_segments(std::vector<BufferStateSegment>& states) noexcept {
            for (size_t index = 1; index < states.size();) {
                BufferStateSegment& previous = states[index - 1];
                BufferStateSegment& current = states[index];
                if (previous.offset + previous.size == current.offset && previous.state == current.state) {
                    previous.size += current.size;
                    states.erase(states.begin() + static_cast<std::ptrdiff_t>(index));
                    continue;
                }
                ++index;
            }
        }

        void initialize_buffer_state_segments(std::vector<BufferStateSegment>& states, uint64_t buffer_size,
                                              const rhi::ResourceState& initial_state) {
            if (states.empty() && buffer_size != 0) {
                states.push_back(BufferStateSegment{0, buffer_size, initial_state});
            }
        }

        void refresh_physical_buffer_state(PhysicalBuffer& physical) noexcept {
            if (physical.states.empty()) {
                return;
            }
            const rhi::ResourceState state = physical.states.front().state;
            const bool uniform = std::all_of(
                    physical.states.begin(), physical.states.end(),
                    [state](const BufferStateSegment& segment) { return segment.state == state; });
            physical.state = uniform ? state : rhi::ResourceState{};
        }

        [[nodiscard]] bool buffer_desc_equal(const OwnedBufferDesc& lhs, const OwnedBufferDesc& rhs) noexcept {
            return lhs.desc.size == rhs.desc.size && lhs.desc.capabilities == rhs.desc.capabilities &&
                   lhs.desc.memory == rhs.desc.memory && lhs.desc.flags == rhs.desc.flags &&
                   lhs.desc.initial_usage == rhs.desc.initial_usage &&
                   lhs.desc.initial_stages == rhs.desc.initial_stages && lhs.desc.initial_queue == rhs.desc.initial_queue;
        }

        [[nodiscard]] bool texture_desc_equal(const OwnedTextureDesc& lhs, const OwnedTextureDesc& rhs) noexcept {
            if (lhs.desc.dimension != rhs.desc.dimension || lhs.desc.extent.width != rhs.desc.extent.width ||
                lhs.desc.extent.height != rhs.desc.extent.height || lhs.desc.extent.depth != rhs.desc.extent.depth ||
                lhs.desc.format != rhs.desc.format || lhs.desc.mip_levels != rhs.desc.mip_levels ||
                lhs.desc.array_layers != rhs.desc.array_layers || lhs.desc.samples != rhs.desc.samples ||
                lhs.desc.capabilities != rhs.desc.capabilities || lhs.desc.memory != rhs.desc.memory ||
                lhs.desc.flags != rhs.desc.flags || lhs.desc.initial_usage != rhs.desc.initial_usage ||
                lhs.desc.initial_stages != rhs.desc.initial_stages || lhs.desc.initial_queue != rhs.desc.initial_queue ||
                lhs.compatible_formats != rhs.compatible_formats) {
                return false;
            }
            return true;
        }

        [[nodiscard]] bool view_desc_equal(const rhi::TextureViewDesc& lhs,
                                           const rhi::TextureViewDesc& rhs) noexcept {
            return lhs.texture == rhs.texture && lhs.kind == rhs.kind && lhs.dimension == rhs.dimension &&
                   lhs.format == rhs.format && lhs.subresources.aspects == rhs.subresources.aspects &&
                   lhs.subresources.base_mip == rhs.subresources.base_mip &&
                   lhs.subresources.mip_count == rhs.subresources.mip_count &&
                   lhs.subresources.base_layer == rhs.subresources.base_layer &&
                   lhs.subresources.layer_count == rhs.subresources.layer_count &&
                   lhs.components.r == rhs.components.r && lhs.components.g == rhs.components.g &&
                   lhs.components.b == rhs.components.b && lhs.components.a == rhs.components.a;
        }

        [[nodiscard]] bool buffer_view_desc_equal(const rhi::BufferViewDesc& lhs,
                                                  const rhi::BufferViewDesc& rhs) noexcept {
            return lhs.buffer == rhs.buffer && lhs.kind == rhs.kind && lhs.offset == rhs.offset &&
                   lhs.size == rhs.size && lhs.format == rhs.format && lhs.structure_stride == rhs.structure_stride;
        }

        [[nodiscard]] bool ranges_overlap(const RenderGraphBufferRange& lhs,
                                          const RenderGraphBufferRange& rhs) noexcept {
            const auto range_end = [](const RenderGraphBufferRange& range) noexcept {
                if (range.size == std::numeric_limits<uint64_t>::max() ||
                    range.size > std::numeric_limits<uint64_t>::max() - range.offset) {
                    return std::numeric_limits<uint64_t>::max();
                }
                return range.offset + range.size;
            };
            const uint64_t lhs_end = range_end(lhs);
            const uint64_t rhs_end = range_end(rhs);
            return lhs.offset < rhs_end && rhs.offset < lhs_end;
        }

        [[nodiscard]] bool ranges_overlap(const rhi::SubresourceRange& lhs, const rhi::SubresourceRange& rhs,
                                          const rhi::TextureDesc& desc) noexcept {
            const uint32_t lhs_mips = lhs.mip_count == rhi::kRemaining ? desc.mip_levels - lhs.base_mip : lhs.mip_count;
            const uint32_t rhs_mips = rhs.mip_count == rhi::kRemaining ? desc.mip_levels - rhs.base_mip : rhs.mip_count;
            const uint32_t lhs_layers = lhs.layer_count == rhi::kRemaining
                                                ? desc.array_layers - lhs.base_layer
                                                : lhs.layer_count;
            const uint32_t rhs_layers = rhs.layer_count == rhi::kRemaining
                                                ? desc.array_layers - rhs.base_layer
                                                : rhs.layer_count;
            const bool mip_overlap = lhs.base_mip < rhs.base_mip + rhs_mips && rhs.base_mip < lhs.base_mip + lhs_mips;
            const bool layer_overlap = lhs.base_layer < rhs.base_layer + rhs_layers &&
                                       rhs.base_layer < lhs.base_layer + lhs_layers;
            return mip_overlap && layer_overlap &&
                   (static_cast<uint32_t>(lhs.aspects) & static_cast<uint32_t>(rhs.aspects)) != 0;
        }

        [[nodiscard]] uint32_t texture_state_index(rhi::Aspect aspect, const rhi::TextureDesc& desc,
                                                    uint32_t mip, uint32_t layer) noexcept {
            uint32_t aspect_index = 0;
            if (aspect == rhi::Aspect::kDepth) {
                aspect_index = 1;
            } else if (aspect == rhi::Aspect::kStencil) {
                aspect_index = 2;
            }
            return (aspect_index * desc.mip_levels + mip) * desc.array_layers + layer;
        }

        [[nodiscard]] bool checked_range_end(uint64_t offset, uint64_t size, uint64_t& end) noexcept {
            if (size == std::numeric_limits<uint64_t>::max()) {
                end = std::numeric_limits<uint64_t>::max();
                return true;
            }
            if (size > std::numeric_limits<uint64_t>::max() - offset) {
                return false;
            }
            end = offset + size;
            return true;
        }

        [[nodiscard]] foundation::Result normalize_texture_range(const rhi::TextureDesc& desc,
                                                           rhi::SubresourceRange& range) {
            if (range.aspects == static_cast<rhi::AspectMask>(0)) {
                return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                             "Render graph texture aspects must not be empty");
            }
            if (range.aspects == rhi::Aspect::kColor && is_depth_format(desc.format)) {
                range.aspects = default_aspect(desc.format);
            }
            const rhi::AspectMask valid_aspects = default_aspect(desc.format);
            if ((static_cast<uint32_t>(range.aspects) & ~static_cast<uint32_t>(valid_aspects)) != 0) {
                return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                             "Render graph texture range contains an invalid aspect");
            }
            const uint32_t mip_count = range.mip_count == rhi::kRemaining ? desc.mip_levels - range.base_mip
                                                                            : range.mip_count;
            const uint32_t layer_count = range.layer_count == rhi::kRemaining
                                                 ? desc.array_layers - range.base_layer
                                                 : range.layer_count;
            if (range.base_mip >= desc.mip_levels || mip_count == 0 || mip_count > desc.mip_levels - range.base_mip ||
                range.base_layer >= desc.array_layers || layer_count == 0 ||
                layer_count > desc.array_layers - range.base_layer) {
                return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                             "Render graph texture subresource range is out of bounds");
            }
            range.mip_count = mip_count;
            range.layer_count = layer_count;
            return foundation::Result::success();
        }

        [[nodiscard]] foundation::Result copy_buffer_desc(const rhi::BufferDesc& source, OwnedBufferDesc& destination) {
            destination.desc = source;
            destination.desc.name = {};
            destination.name.assign(source.name.value.begin(), source.name.value.end());
            return foundation::Result::success();
        }

        [[nodiscard]] foundation::Result copy_texture_desc(const rhi::TextureDesc& source, OwnedTextureDesc& destination) {
            try {
                destination.desc = source;
                destination.desc.compatible_view_formats = {};
                destination.desc.name = {};
                destination.compatible_formats.assign(source.compatible_view_formats.begin(),
                                                       source.compatible_view_formats.end());
                destination.name.assign(source.name.value.begin(), source.name.value.end());
            } catch (const std::bad_alloc&) {
                return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                             "Render graph texture description allocation failed");
            }
            return foundation::Result::success();
        }

        [[nodiscard]] rhi::ResourceState make_state(rhi::ResourceUsage usage, rhi::QueueClass queue,
                                                     rhi::PipelineStageMask stages,
                                                     rhi::ShaderStageMask shader_stages) noexcept {
            rhi::ResourceState state{};
            state.usage = usage;
            state.queue = queue;
            state.pipeline_stages = stages;
            state.shader_stages = shader_stages;
            return state;
        }
    } // namespace

    /**
     * @brief Private render-graph storage and compiler implementation.
     *
     * The implementation owns logical declarations, compiled dependency data,
     * transient pools, and execution-time state while the public RenderGraph
     * keeps the device-facing API small.
     */

    struct RenderGraph::Impl {
        explicit Impl(rhi::Device& device_reference) : device(device_reference) {
        }

        rhi::Device& device;
        uint64_t graph_id{allocate_graph_id()};
        uint32_t next_generation{1};
        uint64_t revision{1};
        uint64_t compiled_revision{0};
        bool compiled{false};

        std::vector<BufferRecord> buffers{};
        std::vector<TextureRecord> textures{};
        std::vector<PassRecord> passes{};
        std::vector<TransientPool> transient_pools{};

        std::vector<uint8_t> live_passes{};
        std::vector<uint32_t> topological_order{};
        std::vector<Edge> edges{};
        std::vector<Batch> batches{};
        std::vector<TransientBufferSlotPlan> transient_buffer_slots{};
        std::vector<TransientTextureSlotPlan> transient_texture_slots{};
        std::vector<uint32_t> pass_to_batch{};
        RenderGraphPlan plan{};
        std::vector<std::shared_ptr<const void>> execution_owners{};

        struct FeatureBuildTransaction {
            bool active{false};
            size_t buffer_count{0};
            size_t texture_count{0};
            size_t pass_count{0};
            size_t execution_owner_count{0};
            uint32_t next_generation{1};
            uint64_t revision{0};
            uint64_t compiled_revision{0};
            bool compiled{false};
            RenderGraphPlan plan{};
            RenderGraphCompileInfo compile_info{};
            std::vector<uint8_t> buffer_exports{};
            std::vector<uint8_t> texture_exports{};
        } feature_build_transaction{};

        [[nodiscard]] foundation::Result begin_feature_build_transaction(
                const RenderGraphCompileInfo& compile_info);
        void commit_feature_build_transaction() noexcept;
        void rollback_feature_build_transaction(RenderGraphCompileInfo& compile_info) noexcept;

        [[nodiscard]] uint32_t allocate_generation() noexcept {
            const uint32_t generation = next_generation++;
            if (next_generation == 0) {
                next_generation = 1;
            }
            return generation == 0 ? allocate_generation() : generation;
        }

        void mark_dirty() noexcept {
            compiled = false;
            plan = {};
            ++revision;
            if (revision == 0) {
                revision = 1;
            }
        }

        [[nodiscard]] BufferRecord* get_buffer(RenderGraphBuffer handle) noexcept {
            if (!handle.is_valid() || handle.index >= buffers.size()) {
                return nullptr;
            }
            BufferRecord& record = buffers[handle.index];
            return record.generation == handle.generation && handle.graph_id == graph_id ? &record : nullptr;
        }

        [[nodiscard]] const BufferRecord* get_buffer(RenderGraphBuffer handle) const noexcept {
            if (!handle.is_valid() || handle.index >= buffers.size()) {
                return nullptr;
            }
            const BufferRecord& record = buffers[handle.index];
            return record.generation == handle.generation && handle.graph_id == graph_id ? &record : nullptr;
        }

        [[nodiscard]] TextureRecord* get_texture(RenderGraphTexture handle) noexcept {
            if (!handle.is_valid() || handle.index >= textures.size()) {
                return nullptr;
            }
            TextureRecord& record = textures[handle.index];
            return record.generation == handle.generation && handle.graph_id == graph_id ? &record : nullptr;
        }

        [[nodiscard]] const TextureRecord* get_texture(RenderGraphTexture handle) const noexcept {
            if (!handle.is_valid() || handle.index >= textures.size()) {
                return nullptr;
            }
            const TextureRecord& record = textures[handle.index];
            return record.generation == handle.generation && handle.graph_id == graph_id ? &record : nullptr;
        }

        [[nodiscard]] PassRecord* get_pass(RenderGraphPass handle) noexcept {
            if (!handle.is_valid() || handle.index >= passes.size()) {
                return nullptr;
            }
            PassRecord& record = passes[handle.index];
            return record.generation == handle.generation && handle.graph_id == graph_id ? &record : nullptr;
        }

        [[nodiscard]] const PassRecord* get_pass(RenderGraphPass handle) const noexcept {
            if (!handle.is_valid() || handle.index >= passes.size()) {
                return nullptr;
            }
            const PassRecord& record = passes[handle.index];
            return record.generation == handle.generation && handle.graph_id == graph_id ? &record : nullptr;
        }

        [[nodiscard]] foundation::Result add_use(uint32_t pass_index, ResourceUse use);
        [[nodiscard]] foundation::Result add_color_attachment(uint32_t pass_index, ColorAttachment attachment);
        [[nodiscard]] foundation::Result add_depth_stencil_attachment(uint32_t pass_index,
                                                               DepthStencilAttachment attachment);
        [[nodiscard]] foundation::Result compile_graph(RenderGraphCompileInfo& info);
        [[nodiscard]] foundation::Result ensure_transient_pool(uint32_t frame_count);
        [[nodiscard]] foundation::Result ensure_transient_buffer(uint32_t frame_slot, uint32_t slot_index);
        [[nodiscard]] foundation::Result ensure_transient_texture(uint32_t frame_slot, uint32_t slot_index);
        [[nodiscard]] foundation::Result ensure_persistent_buffer(BufferRecord& record);
        [[nodiscard]] foundation::Result ensure_persistent_texture(TextureRecord& record);
        [[nodiscard]] foundation::Result release_physical_buffer(PhysicalBuffer& physical);
        [[nodiscard]] foundation::Result release_physical_texture(PhysicalTexture& physical);
        [[nodiscard]] foundation::Result release_all();
        [[nodiscard]] foundation::Result build_plan(RenderGraphCompileInfo& info);
        [[nodiscard]] foundation::Result audit_cross_queue_dependencies() const;
        [[nodiscard]] bool resource_uses_overlap(const ResourceUse& lhs,
                                                  const ResourceUse& rhs) const noexcept;
        [[nodiscard]] uint32_t resource_hazard_count(const Edge& edge) const noexcept;
        [[nodiscard]] foundation::ResultValue<rhi::BufferHandle> resolve_buffer(RenderGraphBuffer handle,
                                                                          uint32_t frame_slot) const;
        [[nodiscard]] foundation::ResultValue<rhi::TextureHandle> resolve_texture(RenderGraphTexture handle,
                                                                           uint32_t frame_slot) const;
        [[nodiscard]] foundation::ResultValue<rhi::BufferViewHandle> resolve_buffer_view(
                RenderGraphBuffer handle, uint32_t frame_slot, rhi::BufferViewKind kind, uint64_t offset,
                uint64_t size, rhi::Format format, uint32_t structure_stride) const;
        [[nodiscard]] foundation::ResultValue<rhi::TextureViewHandle> resolve_texture_view(
                RenderGraphTexture handle, uint32_t frame_slot, rhi::TextureViewKind kind,
                rhi::SubresourceRange subresources, rhi::Format format, rhi::TextureViewDimension dimension) const;
        [[nodiscard]] foundation::ResultValue<rhi::BufferViewHandle> ensure_buffer_view(
                RenderGraphBuffer handle, uint32_t frame_slot, rhi::BufferViewKind kind, uint64_t offset,
                uint64_t size, rhi::Format format, uint32_t structure_stride);
        [[nodiscard]] foundation::ResultValue<rhi::TextureViewHandle> ensure_texture_view(
                RenderGraphTexture handle, uint32_t frame_slot, rhi::TextureViewKind kind,
                rhi::SubresourceRange subresources, rhi::Format format, rhi::TextureViewDimension dimension);
        [[nodiscard]] foundation::Result prepare_execution_views(const RenderGraphPlan& execution_plan,
                                                                 uint32_t frame_slot);
        [[nodiscard]] foundation::Result record_barrier_batch(
                rhi::CommandListHandle command_list, uint32_t frame_slot,
                const RenderGraphPassPlan& pass_plan, RenderGraphBarrierPhase phase,
                std::vector<RuntimeBufferState>& buffer_states,
                std::vector<RuntimeTextureState>& texture_states);
        void commit_runtime_states(const std::vector<RuntimeBufferState>& buffer_states,
                                   const std::vector<RuntimeTextureState>& texture_states);
        [[nodiscard]] PhysicalBuffer* physical_buffer(BufferRecord& record, uint32_t frame_slot) noexcept;
        [[nodiscard]] const PhysicalBuffer* physical_buffer(const BufferRecord& record,
                                                             uint32_t frame_slot) const noexcept;
        [[nodiscard]] PhysicalTexture* physical_texture(TextureRecord& record, uint32_t frame_slot) noexcept;
        [[nodiscard]] const PhysicalTexture* physical_texture(const TextureRecord& record,
                                                              uint32_t frame_slot) const noexcept;
        [[nodiscard]] foundation::Result initialize_runtime_states(uint32_t frame_slot,
                                                             std::vector<RuntimeBufferState>& buffer_states,
                                                             std::vector<RuntimeTextureState>& texture_states);
        [[nodiscard]] foundation::Result validate_pass_attachments(const PassRecord& pass) const;
        [[nodiscard]] foundation::Result validate_use(const ResourceUse& use, const PassRecord& pass) const;
    };

    RenderGraph::RenderGraph(rhi::Device& device)
        : impl_(std::make_unique<Impl>(device)), device_(device) {
    }

    RenderGraph::~RenderGraph() {
        if (impl_ != nullptr) {
            (void) impl_->release_all();
        }
    }

    foundation::Result RenderGraph::Impl::begin_feature_build_transaction(
            const RenderGraphCompileInfo& compile_info) {
        if (feature_build_transaction.active) {
            return foundation::Result::failure(
                    foundation::ResultCode::kInvalidState,
                    "Render graph feature build transaction is already active");
        }

        FeatureBuildTransaction checkpoint{};
        checkpoint.buffer_count = buffers.size();
        checkpoint.texture_count = textures.size();
        checkpoint.pass_count = passes.size();
        checkpoint.execution_owner_count = execution_owners.size();
        checkpoint.next_generation = next_generation;
        checkpoint.revision = revision;
        checkpoint.compiled_revision = compiled_revision;
        checkpoint.compiled = compiled;
        checkpoint.compile_info = compile_info;
        checkpoint.plan = std::move(plan);
        try {
            checkpoint.buffer_exports.resize(checkpoint.buffer_count);
            checkpoint.texture_exports.resize(checkpoint.texture_count);
            for (size_t index = 0; index < checkpoint.buffer_count; ++index) {
                checkpoint.buffer_exports[index] = buffers[index].exported ? 1 : 0;
            }
            for (size_t index = 0; index < checkpoint.texture_count; ++index) {
                checkpoint.texture_exports[index] = textures[index].exported ? 1 : 0;
            }
        } catch (const std::bad_alloc&) {
            plan = std::move(checkpoint.plan);
            return foundation::Result::failure(
                    foundation::ResultCode::kOutOfMemory,
                    "Render graph feature transaction allocation failed");
        }

        checkpoint.active = true;
        feature_build_transaction = std::move(checkpoint);
        return foundation::Result::success();
    }

    void RenderGraph::Impl::commit_feature_build_transaction() noexcept {
        if (feature_build_transaction.active &&
            revision == feature_build_transaction.revision &&
            compiled == feature_build_transaction.compiled) {
            plan = std::move(feature_build_transaction.plan);
        }
        feature_build_transaction = {};
    }

    void RenderGraph::Impl::rollback_feature_build_transaction(
            RenderGraphCompileInfo& compile_info) noexcept {
        if (!feature_build_transaction.active) {
            return;
        }

        for (size_t index = feature_build_transaction.buffer_count; index < buffers.size(); ++index) {
            (void) release_physical_buffer(buffers[index].physical);
        }
        for (size_t index = feature_build_transaction.texture_count; index < textures.size(); ++index) {
            (void) release_physical_texture(textures[index].physical);
        }

        buffers.resize(feature_build_transaction.buffer_count);
        textures.resize(feature_build_transaction.texture_count);
        passes.resize(feature_build_transaction.pass_count);
        execution_owners.resize(feature_build_transaction.execution_owner_count);
        for (size_t index = 0;
             index < feature_build_transaction.buffer_exports.size() && index < buffers.size(); ++index) {
            buffers[index].exported = feature_build_transaction.buffer_exports[index] != 0;
        }
        for (size_t index = 0;
             index < feature_build_transaction.texture_exports.size() && index < textures.size(); ++index) {
            textures[index].exported = feature_build_transaction.texture_exports[index] != 0;
        }

        next_generation = feature_build_transaction.next_generation;
        revision = feature_build_transaction.revision;
        compiled_revision = feature_build_transaction.compiled_revision;
        compiled = feature_build_transaction.compiled;
        plan = std::move(feature_build_transaction.plan);
        compile_info = feature_build_transaction.compile_info;
        feature_build_transaction = {};
    }

    foundation::Result RenderGraph::begin_feature_build_transaction() {
        if (impl_ == nullptr) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                                "Render graph implementation is not initialized");
        }
        return impl_->begin_feature_build_transaction(compile_info_);
    }

    foundation::Result RenderGraph::retain_execution_owner(std::shared_ptr<const void> owner) {
        if (impl_ == nullptr) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                                "Render graph implementation is not initialized");
        }
        if (!impl_->feature_build_transaction.active) {
            return foundation::Result::failure(
                    foundation::ResultCode::kInvalidState,
                    "Render graph execution ownership requires an active feature build transaction");
        }
        if (owner.use_count() == 0) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                "Render graph execution owner is empty");
        }
        try {
            impl_->execution_owners.push_back(std::move(owner));
        } catch (const std::bad_alloc&) {
            return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                                "Render graph execution ownership allocation failed");
        }
        return foundation::Result::success();
    }

    void RenderGraph::commit_feature_build_transaction() noexcept {
        if (impl_ != nullptr) {
            impl_->commit_feature_build_transaction();
        }
    }

    void RenderGraph::rollback_feature_build_transaction() noexcept {
        if (impl_ != nullptr) {
            impl_->rollback_feature_build_transaction(compile_info_);
        }
    }

    const RenderGraphPlan& RenderGraph::plan() const noexcept {
        return impl_->plan;
    }

    void RenderGraph::clear_passes() noexcept {
        if (impl_ == nullptr) {
            return;
        }
        for (TransientPool& pool: impl_->transient_pools) {
            for (PhysicalBuffer& buffer: pool.buffers) {
                (void) impl_->release_physical_buffer(buffer);
            }
            for (PhysicalTexture& texture: pool.textures) {
                (void) impl_->release_physical_texture(texture);
            }
        }
        impl_->transient_pools.clear();
        impl_->passes.clear();
        impl_->execution_owners.clear();
        impl_->live_passes.clear();
        impl_->topological_order.clear();
        impl_->edges.clear();
        impl_->batches.clear();
        impl_->pass_to_batch.clear();
        impl_->transient_buffer_slots.clear();
        impl_->transient_texture_slots.clear();
        impl_->plan = {};
        for (BufferRecord& record: impl_->buffers) {
            record.has_use = false;
            record.first_use = kInvalidHandleIndex;
            record.last_use = kInvalidHandleIndex;
            record.physical_slot = kInvalidHandleIndex;
        }
        for (TextureRecord& record: impl_->textures) {
            record.has_use = false;
            record.first_use = kInvalidHandleIndex;
            record.last_use = kInvalidHandleIndex;
            record.physical_slot = kInvalidHandleIndex;
        }
        impl_->mark_dirty();
        compile_info_ = {};
    }

    void RenderGraph::reset() noexcept {
        if (impl_ == nullptr) {
            return;
        }
        (void) impl_->release_all();
        impl_->buffers.clear();
        impl_->textures.clear();
        impl_->passes.clear();
        impl_->execution_owners.clear();
        impl_->transient_pools.clear();
        impl_->live_passes.clear();
        impl_->topological_order.clear();
        impl_->edges.clear();
        impl_->batches.clear();
        impl_->pass_to_batch.clear();
        impl_->transient_buffer_slots.clear();
        impl_->transient_texture_slots.clear();
        impl_->plan = {};
        impl_->mark_dirty();
        compile_info_ = {};
    }

    foundation::ResultValue<RenderGraphBuffer> RenderGraph::create_buffer(const rhi::BufferDesc& desc,
                                                                    ResourceLifetime lifetime) {
        if (impl_ == nullptr) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                         "Render graph implementation is not initialized"), {}};
        }
        if (lifetime == ResourceLifetime::kImported) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                         "Imported buffers must be created with import_buffer"), {}};
        }
        if (desc.size == 0 || desc.capabilities == rhi::BufferCapability::kNone) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                         "Render graph buffer size and capabilities must be non-zero"), {}};
        }
        if (lifetime == ResourceLifetime::kTransient && rhi::has_flag(desc.flags, rhi::ResourceFlags::kExternal)) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                         "Transient buffers cannot be external resources"), {}};
        }

        BufferRecord record{};
        record.generation = impl_->allocate_generation();
        record.lifetime = lifetime;
        const foundation::Result copy_result = copy_buffer_desc(desc, record.desc);
        if (!copy_result) {
            return {copy_result, {}};
        }
        if (lifetime == ResourceLifetime::kTransient) {
            record.desc.desc.initial_usage = rhi::ResourceUsage::kUndefined;
            record.desc.desc.initial_stages = rhi::PipelineStage::kAllCommands;
            record.desc.desc.initial_queue = rhi::QueueClass::kAuto;
        }

        try {
            const uint32_t index = static_cast<uint32_t>(impl_->buffers.size());
            impl_->buffers.push_back(std::move(record));
            impl_->mark_dirty();
            return {foundation::Result::success(),
                    RenderGraphBuffer{index, impl_->buffers.back().generation, impl_->graph_id}};
        } catch (const std::bad_alloc&) {
            return {foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                         "Render graph buffer resource allocation failed"), {}};
        }
    }

    foundation::ResultValue<RenderGraphTexture> RenderGraph::create_texture(const rhi::TextureDesc& desc,
                                                                      ResourceLifetime lifetime) {
        if (impl_ == nullptr) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                         "Render graph implementation is not initialized"), {}};
        }
        if (lifetime == ResourceLifetime::kImported) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                         "Imported textures must be created with import_texture"), {}};
        }
        if (desc.format == rhi::Format::kUnknown || desc.extent.width == 0 || desc.extent.height == 0 ||
            desc.extent.depth == 0 || desc.mip_levels == 0 || desc.array_layers == 0 ||
            desc.capabilities == rhi::TextureCapability::kNone) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                         "Render graph texture description is incomplete"), {}};
        }
        if (lifetime == ResourceLifetime::kTransient && rhi::has_flag(desc.flags, rhi::ResourceFlags::kExternal)) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                         "Transient textures cannot be external resources"), {}};
        }

        TextureRecord record{};
        record.generation = impl_->allocate_generation();
        record.lifetime = lifetime;
        const foundation::Result copy_result = copy_texture_desc(desc, record.desc);
        if (!copy_result) {
            return {copy_result, {}};
        }
        if (lifetime == ResourceLifetime::kTransient) {
            record.desc.desc.initial_usage = rhi::ResourceUsage::kUndefined;
            record.desc.desc.initial_stages = rhi::PipelineStage::kAllCommands;
            record.desc.desc.initial_queue = rhi::QueueClass::kAuto;
        }

        try {
            const uint32_t index = static_cast<uint32_t>(impl_->textures.size());
            impl_->textures.push_back(std::move(record));
            impl_->mark_dirty();
            return {foundation::Result::success(),
                    RenderGraphTexture{index, impl_->textures.back().generation, impl_->graph_id}};
        } catch (const std::bad_alloc&) {
            return {foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                         "Render graph texture resource allocation failed"), {}};
        }
    }

    bool RenderGraph::contains_buffer(RenderGraphBuffer resource) const noexcept {
        return impl_ != nullptr && impl_->get_buffer(resource) != nullptr;
    }

    bool RenderGraph::contains_texture(RenderGraphTexture resource) const noexcept {
        return impl_ != nullptr && impl_->get_texture(resource) != nullptr;
    }

    foundation::ResultValue<RenderGraphBuffer> RenderGraph::import_buffer(rhi::BufferHandle buffer,
                                                                    const rhi::BufferDesc& desc,
                                                                    const rhi::ResourceState& initial_state) {
        if (!buffer.is_valid()) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                         "Cannot import an invalid buffer handle"), {}};
        }
        auto resource_result = create_buffer(desc, ResourceLifetime::kPersistent);
        if (!resource_result) {
            return resource_result;
        }
        BufferRecord* record = impl_->get_buffer(resource_result.value);
        record->lifetime = ResourceLifetime::kImported;
        record->imported = buffer;
        record->physical.handle = buffer;
        record->physical.owns_handle = false;
        record->physical.desc = record->desc;
        record->physical.state = initial_state.usage == rhi::ResourceUsage::kUndefined &&
                                         desc.initial_usage != rhi::ResourceUsage::kUndefined
                                         ? rhi::ResourceState{desc.initial_usage, rhi::ShaderStage::kAll,
                                                              desc.initial_queue, false, desc.initial_stages}
                                         : initial_state;
        record->physical.states.clear();
        initialize_buffer_state_segments(record->physical.states, desc.size, record->physical.state);
        impl_->mark_dirty();
        return resource_result;
    }

    foundation::ResultValue<RenderGraphTexture> RenderGraph::import_texture(rhi::TextureHandle texture,
                                                                       rhi::TextureViewHandle default_view,
                                                                       const rhi::TextureDesc& desc,
                                                                       const rhi::ResourceState& initial_state) {
        if (!texture.is_valid()) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                         "Cannot import an invalid texture handle"), {}};
        }
        auto resource_result = create_texture(desc, ResourceLifetime::kPersistent);
        if (!resource_result) {
            return resource_result;
        }
        TextureRecord* record = impl_->get_texture(resource_result.value);
        record->lifetime = ResourceLifetime::kImported;
        record->imported = texture;
        record->imported_default_view = default_view;
        record->physical.handle = texture;
        record->physical.owns_handle = false;
        record->physical.desc = record->desc;
        record->imported_state = initial_state.usage == rhi::ResourceUsage::kUndefined &&
                                         desc.initial_usage != rhi::ResourceUsage::kUndefined
                                         ? rhi::ResourceState{desc.initial_usage, rhi::ShaderStage::kAll,
                                                              desc.initial_queue, false, desc.initial_stages}
                                         : initial_state;
        try {
            record->physical.states.assign(static_cast<size_t>(kAspectPlaneCount) * desc.mip_levels * desc.array_layers,
                                           record->imported_state);
        } catch (const std::bad_alloc&) {
            impl_->textures.pop_back();
            return {foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                         "Render graph imported texture state allocation failed"), {}};
        }
        impl_->mark_dirty();
        return resource_result;
    }

    foundation::Result RenderGraph::update_imported_buffer(RenderGraphBuffer resource, rhi::BufferHandle buffer,
                                                    const rhi::ResourceState& initial_state) {
        BufferRecord* record = impl_->get_buffer(resource);
        if (record == nullptr || record->lifetime != ResourceLifetime::kImported) {
            return foundation::Result::failure(foundation::ResultCode::kNotFound,
                                         "Invalid imported buffer resource");
        }
        if (!buffer.is_valid()) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                        "Imported buffer handle must be valid");
        }
        for (const BufferViewCacheEntry& view: record->physical.views) {
            if (view.handle.is_valid()) {
                (void) impl_->device.destroy_buffer_view(view.handle);
            }
        }
        record->physical.views.clear();
        record->imported = buffer;
        record->physical.handle = buffer;
        record->physical.owns_handle = false;
        record->physical.state = initial_state.usage == rhi::ResourceUsage::kUndefined &&
                                         record->desc.desc.initial_usage != rhi::ResourceUsage::kUndefined
                                     ? rhi::ResourceState{record->desc.desc.initial_usage,
                                                          rhi::ShaderStage::kAll,
                                                          record->desc.desc.initial_queue,
                                                          false,
                                                           record->desc.desc.initial_stages}
                                      : initial_state;
        record->physical.states.clear();
        initialize_buffer_state_segments(record->physical.states, record->desc.desc.size, record->physical.state);
        record->last_completions.clear();
        impl_->mark_dirty();
        return foundation::Result::success();
    }

    foundation::Result RenderGraph::update_imported_texture(RenderGraphTexture resource, rhi::TextureHandle texture,
                                                     rhi::TextureViewHandle default_view,
                                                     const rhi::ResourceState& initial_state) {
        TextureRecord* record = impl_->get_texture(resource);
        if (record == nullptr || record->lifetime != ResourceLifetime::kImported) {
            return foundation::Result::failure(foundation::ResultCode::kNotFound,
                                        "Invalid imported texture resource");
        }
        const rhi::TextureDesc desc = record->desc.materialize();
        return update_imported_texture(resource, texture, default_view, desc, initial_state);
    }

    foundation::Result RenderGraph::update_imported_texture(RenderGraphTexture resource, rhi::TextureHandle texture,
                                                     rhi::TextureViewHandle default_view,
                                                     const rhi::TextureDesc& desc,
                                                     const rhi::ResourceState& initial_state) {
        TextureRecord* record = impl_->get_texture(resource);
        if (record == nullptr || record->lifetime != ResourceLifetime::kImported) {
            return foundation::Result::failure(foundation::ResultCode::kNotFound,
                                        "Invalid imported texture resource");
        }
        if (!texture.is_valid()) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                        "Imported texture handle must be valid");
        }
        if (desc.format == rhi::Format::kUnknown || desc.extent.width == 0 || desc.extent.height == 0 ||
            desc.extent.depth == 0 || desc.mip_levels == 0 || desc.array_layers == 0 ||
            desc.capabilities == rhi::TextureCapability::kNone) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                        "Imported texture description is incomplete");
        }

        OwnedTextureDesc next_desc{};
        if (const foundation::Result copy_result = copy_texture_desc(desc, next_desc); !copy_result) {
            return copy_result;
        }
        const rhi::ResourceState resolved_state =
                initial_state.usage == rhi::ResourceUsage::kUndefined &&
                        next_desc.desc.initial_usage != rhi::ResourceUsage::kUndefined
                    ? rhi::ResourceState{next_desc.desc.initial_usage,
                                         rhi::ShaderStage::kAll,
                                         next_desc.desc.initial_queue,
                                         false,
                                         next_desc.desc.initial_stages}
                    : initial_state;
        std::vector<rhi::ResourceState> new_states;
        try {
            new_states.assign(static_cast<size_t>(kAspectPlaneCount) * next_desc.desc.mip_levels *
                                      next_desc.desc.array_layers,
                              resolved_state);
        } catch (const std::bad_alloc&) {
            return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                        "Render graph imported texture state allocation failed");
        }
        for (TextureViewCacheEntry& view: record->physical.views) {
            if (view.owns_handle && view.handle.is_valid()) {
                (void) impl_->device.destroy_texture_view(view.handle);
            }
        }
        record->physical.views.clear();
        record->imported = texture;
        record->imported_default_view = default_view;
        record->physical.handle = texture;
        record->physical.owns_handle = false;
        record->desc = std::move(next_desc);
        record->physical.desc = record->desc;
        record->physical.states.swap(new_states);
        record->imported_state = resolved_state;
        record->last_completions.clear();
        impl_->mark_dirty();
        return foundation::Result::success();
    }

    foundation::ResultValue<RenderGraphTexture> RenderGraph::import_swapchain_image(
            const rhi::FrameAcquireResult& acquired, const rhi::TextureDesc& desc,
            const rhi::ResourceState& initial_state) {
        if (!acquired.frame.is_valid() || !acquired.image.texture.is_valid() || !acquired.image.view.is_valid()) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                         "Swapchain image import requires a valid acquired frame"), {}};
        }
        const rhi::ResourceState resolved_initial_state =
                initial_state.usage == rhi::ResourceUsage::kUndefined ? acquired.initial_state : initial_state;
        rhi::TextureDesc import_desc = desc;
        if (resolved_initial_state.usage == rhi::ResourceUsage::kUndefined) {
            import_desc.initial_usage = rhi::ResourceUsage::kUndefined;
        }
        const foundation::ResultValue<RenderGraphTexture> result =
                import_texture(acquired.image.texture, acquired.image.view, import_desc, resolved_initial_state);
        if (!result) {
            return result;
        }
        TextureRecord* record = impl_->get_texture(result.value);
        if (record == nullptr) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                                "Imported swapchain texture record is unavailable"), {}};
        }
        record->is_swapchain_image = true;
        return result;
    }

    foundation::Result RenderGraph::export_buffer(RenderGraphBuffer resource) {
        BufferRecord* record = impl_->get_buffer(resource);
        if (record == nullptr) {
            return foundation::Result::failure(foundation::ResultCode::kNotFound,
                                        "Invalid render graph buffer export handle");
        }
        record->exported = true;
        impl_->mark_dirty();
        return foundation::Result::success();
    }

    foundation::Result RenderGraph::export_texture(RenderGraphTexture resource) {
        TextureRecord* record = impl_->get_texture(resource);
        if (record == nullptr) {
            return foundation::Result::failure(foundation::ResultCode::kNotFound,
                                        "Invalid render graph texture export handle");
        }
        record->exported = true;
        impl_->mark_dirty();
        return foundation::Result::success();
    }

    foundation::ResultValue<RenderGraphPass> RenderGraph::add_pass(std::string_view name, rhi::QueueClass queue,
                                                            PassSetupCallback setup,
                                                            PassExecuteCallback execute) {
        if (impl_ == nullptr) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                         "Render graph implementation is not initialized"), {}};
        }
        queue = normalize_queue(queue);
        if (queue == rhi::QueueClass::kPresent) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                         "Render graph passes cannot use the present queue"), {}};
        }

        PassRecord pass{};
        pass.generation = impl_->allocate_generation();
        pass.queue = queue;
        pass.setup = std::move(setup);
        pass.execute = std::move(execute);
        const uint32_t generation = pass.generation;
        try {
            pass.name.assign(name.begin(), name.end());
            const uint32_t index = static_cast<uint32_t>(impl_->passes.size());
            impl_->passes.push_back(std::move(pass));
            PassRecord& stored_pass = impl_->passes.back();
            if (stored_pass.setup) {
                PassBuilder builder(*this, index);
                const foundation::Result setup_result = stored_pass.setup(builder);
                if (!setup_result) {
                    impl_->passes.pop_back();
                    return {setup_result, {}};
                }
            }
            impl_->mark_dirty();
            return {foundation::Result::success(),
                    RenderGraphPass{index, stored_pass.generation, impl_->graph_id}};
        } catch (const std::bad_alloc&) {
            if (!impl_->passes.empty() && impl_->passes.back().generation == generation) {
                impl_->passes.pop_back();
            }
            return {foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                         "Render graph pass allocation failed"), {}};
        } catch (...) {
            if (!impl_->passes.empty() && impl_->passes.back().generation == generation) {
                impl_->passes.pop_back();
            }
            return {foundation::Result::failure(foundation::ResultCode::kBackendError,
                                         "Render graph pass setup threw an exception"), {}};
        }
    }

    foundation::Result RenderGraph::compile() {
        if (impl_ == nullptr) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Render graph implementation is not initialized");
        }
        const foundation::Result result = impl_->compile_graph(compile_info_);
        if (!result) {
            compile_info_ = {};
        }
        return result;
    }

    foundation::Result PassBuilder::read(RenderGraphBuffer buffer, rhi::ResourceUsage usage,
                                  rhi::PipelineStageMask pipeline_stages,
                                  rhi::ShaderStageMask shader_stages,
                                  RenderGraphBufferRange range) {
        if (pass_index_ >= graph_.impl_->passes.size()) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Pass builder is no longer attached to a pass");
        }
        const PassRecord* pass = graph_.impl_->get_pass(RenderGraphPass{
                pass_index_, graph_.impl_->passes[pass_index_].generation, graph_.impl_->graph_id});
        if (pass == nullptr) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Pass builder is no longer attached to a pass");
        }
        ResourceUse use{};
        use.kind = ResourceKind::kBuffer;
        use.resource_index = buffer.index;
        use.resource_generation = buffer.generation;
        use.access = ResourceAccess::kRead;
        use.before_state = make_state(usage, pass->queue, pipeline_stages, shader_stages);
        use.after_state = use.before_state;
        use.buffer_range = range;
        return graph_.impl_->add_use(pass_index_, std::move(use));
    }

    foundation::Result PassBuilder::write(RenderGraphBuffer buffer, rhi::ResourceUsage usage,
                                          rhi::PipelineStageMask pipeline_stages,
                                          rhi::ShaderStageMask shader_stages,
                                          RenderGraphBufferRange range, bool discard_contents) {
        return write_to(buffer, usage, pipeline_stages, shader_stages, usage, range, discard_contents);
    }

    foundation::Result PassBuilder::write_to(RenderGraphBuffer buffer, rhi::ResourceUsage usage,
                                             rhi::PipelineStageMask pipeline_stages,
                                             rhi::ShaderStageMask shader_stages, rhi::ResourceUsage final_usage,
                                             RenderGraphBufferRange range, bool discard_contents) {
        if (pass_index_ >= graph_.impl_->passes.size()) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Pass builder is no longer attached to a pass");
        }
        const PassRecord* pass = graph_.impl_->get_pass(RenderGraphPass{
                pass_index_, graph_.impl_->passes[pass_index_].generation, graph_.impl_->graph_id});
        if (pass == nullptr) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Pass builder is no longer attached to a pass");
        }
        ResourceUse use{};
        use.kind = ResourceKind::kBuffer;
        use.resource_index = buffer.index;
        use.resource_generation = buffer.generation;
        use.access = ResourceAccess::kWrite;
        use.before_state = make_state(usage, pass->queue, pipeline_stages, shader_stages);
        use.after_state = final_usage == usage
                                  ? use.before_state
                                  : make_state(final_usage, pass->queue,
                                               final_usage == rhi::ResourceUsage::kHostRead ||
                                                               final_usage == rhi::ResourceUsage::kHostWrite
                                                       ? rhi::PipelineStage::kHost
                                                       : rhi::PipelineStage::kAllCommands,
                                               rhi::ShaderStage::kAll);
        use.buffer_range = range;
        use.discard_on_first_use = discard_contents;
        return graph_.impl_->add_use(pass_index_, std::move(use));
    }

    foundation::Result PassBuilder::read_write(RenderGraphBuffer buffer, rhi::ResourceUsage usage,
                                        rhi::PipelineStageMask pipeline_stages,
                                        rhi::ShaderStageMask shader_stages,
                                        RenderGraphBufferRange range) {
        if (pass_index_ >= graph_.impl_->passes.size()) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Pass builder is no longer attached to a pass");
        }
        const PassRecord* pass = graph_.impl_->get_pass(RenderGraphPass{
                pass_index_, graph_.impl_->passes[pass_index_].generation, graph_.impl_->graph_id});
        if (pass == nullptr) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Pass builder is no longer attached to a pass");
        }
        ResourceUse use{};
        use.kind = ResourceKind::kBuffer;
        use.resource_index = buffer.index;
        use.resource_generation = buffer.generation;
        use.access = ResourceAccess::kReadWrite;
        use.before_state = make_state(usage, pass->queue, pipeline_stages, shader_stages);
        use.after_state = use.before_state;
        use.buffer_range = range;
        return graph_.impl_->add_use(pass_index_, std::move(use));
    }

    foundation::Result PassBuilder::read(RenderGraphTexture texture, rhi::ResourceUsage usage,
                                         rhi::PipelineStageMask pipeline_stages,
                                         rhi::ShaderStageMask shader_stages,
                                         rhi::SubresourceRange subresources) {
        return read_to(texture, usage, pipeline_stages, shader_stages, usage, subresources);
    }

    foundation::Result PassBuilder::read_to(RenderGraphTexture texture, rhi::ResourceUsage usage,
                                            rhi::PipelineStageMask pipeline_stages,
                                            rhi::ShaderStageMask shader_stages, rhi::ResourceUsage final_usage,
                                            rhi::SubresourceRange subresources) {
        if (pass_index_ >= graph_.impl_->passes.size()) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Pass builder is no longer attached to a pass");
        }
        const PassRecord* pass = graph_.impl_->get_pass(RenderGraphPass{
                pass_index_, graph_.impl_->passes[pass_index_].generation, graph_.impl_->graph_id});
        if (pass == nullptr) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Pass builder is no longer attached to a pass");
        }
        const TextureRecord* record = graph_.impl_->get_texture(texture);
        if (record == nullptr) {
            return foundation::Result::failure(foundation::ResultCode::kNotFound,
                                        "Pass references an invalid render graph texture");
        }
        const foundation::Result range_result = normalize_texture_range(record->desc.desc, subresources);
        if (!range_result) {
            return range_result;
        }
        ResourceUse use{};
        use.kind = ResourceKind::kTexture;
        use.resource_index = texture.index;
        use.resource_generation = texture.generation;
        use.access = ResourceAccess::kRead;
        use.before_state = make_state(usage, pass->queue, pipeline_stages, shader_stages);
        use.after_state = final_usage == usage
                                  ? use.before_state
                                  : make_state(final_usage, pass->queue, rhi::PipelineStage::kAllCommands,
                                               rhi::ShaderStage::kAll);
        use.subresources = subresources;
        return graph_.impl_->add_use(pass_index_, std::move(use));
    }

    foundation::Result PassBuilder::write(RenderGraphTexture texture, rhi::ResourceUsage usage,
                                   rhi::PipelineStageMask pipeline_stages,
                                   rhi::ShaderStageMask shader_stages,
                                   rhi::SubresourceRange subresources, bool discard_contents) {
        if (pass_index_ >= graph_.impl_->passes.size()) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Pass builder is no longer attached to a pass");
        }
        const PassRecord* pass = graph_.impl_->get_pass(RenderGraphPass{
                pass_index_, graph_.impl_->passes[pass_index_].generation, graph_.impl_->graph_id});
        if (pass == nullptr) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Pass builder is no longer attached to a pass");
        }
        const TextureRecord* record = graph_.impl_->get_texture(texture);
        if (record == nullptr) {
            return foundation::Result::failure(foundation::ResultCode::kNotFound,
                                        "Pass references an invalid render graph texture");
        }
        const foundation::Result range_result = normalize_texture_range(record->desc.desc, subresources);
        if (!range_result) {
            return range_result;
        }
        ResourceUse use{};
        use.kind = ResourceKind::kTexture;
        use.resource_index = texture.index;
        use.resource_generation = texture.generation;
        use.access = ResourceAccess::kWrite;
        use.before_state = make_state(usage, pass->queue, pipeline_stages, shader_stages);
        use.after_state = use.before_state;
        use.subresources = subresources;
        use.discard_on_first_use = discard_contents;
        return graph_.impl_->add_use(pass_index_, std::move(use));
    }

    foundation::Result PassBuilder::read_write(RenderGraphTexture texture, rhi::ResourceUsage usage,
                                        rhi::PipelineStageMask pipeline_stages,
                                        rhi::ShaderStageMask shader_stages,
                                        rhi::SubresourceRange subresources) {
        if (pass_index_ >= graph_.impl_->passes.size()) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Pass builder is no longer attached to a pass");
        }
        const PassRecord* pass = graph_.impl_->get_pass(RenderGraphPass{
                pass_index_, graph_.impl_->passes[pass_index_].generation, graph_.impl_->graph_id});
        if (pass == nullptr) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Pass builder is no longer attached to a pass");
        }
        const TextureRecord* record = graph_.impl_->get_texture(texture);
        if (record == nullptr) {
            return foundation::Result::failure(foundation::ResultCode::kNotFound,
                                        "Pass references an invalid render graph texture");
        }
        const foundation::Result range_result = normalize_texture_range(record->desc.desc, subresources);
        if (!range_result) {
            return range_result;
        }
        ResourceUse use{};
        use.kind = ResourceKind::kTexture;
        use.resource_index = texture.index;
        use.resource_generation = texture.generation;
        use.access = ResourceAccess::kReadWrite;
        use.before_state = make_state(usage, pass->queue, pipeline_stages, shader_stages);
        use.after_state = use.before_state;
        use.subresources = subresources;
        return graph_.impl_->add_use(pass_index_, std::move(use));
    }

    foundation::Result PassBuilder::use_color_attachment(RenderGraphTexture texture, rhi::LoadOp load_op,
                                                  rhi::StoreOp store_op, rhi::ClearColorValue clear_color,
                                                  rhi::ResourceUsage final_usage,
                                                  rhi::SubresourceRange subresources) {
        if (pass_index_ >= graph_.impl_->passes.size()) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Pass builder is no longer attached to a pass");
        }
        const TextureRecord* record = graph_.impl_->get_texture(texture);
        if (record == nullptr) {
            return foundation::Result::failure(foundation::ResultCode::kNotFound,
                                        "Color attachment references an invalid render graph texture");
        }
        const foundation::Result range_result = normalize_texture_range(record->desc.desc, subresources);
        if (!range_result) {
            return range_result;
        }
        return graph_.impl_->add_color_attachment(
                pass_index_, ColorAttachment{texture, load_op, store_op, clear_color, final_usage, subresources});
    }

    foundation::Result PassBuilder::use_depth_stencil_attachment(RenderGraphTexture texture, rhi::LoadOp depth_load_op,
                                                          rhi::StoreOp depth_store_op, float clear_depth,
                                                          uint32_t clear_stencil, rhi::ResourceUsage final_usage,
                                                          rhi::SubresourceRange subresources) {
        if (pass_index_ >= graph_.impl_->passes.size()) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Pass builder is no longer attached to a pass");
        }
        const TextureRecord* record = graph_.impl_->get_texture(texture);
        if (record == nullptr) {
            return foundation::Result::failure(foundation::ResultCode::kNotFound,
                                        "Depth attachment references an invalid render graph texture");
        }
        const foundation::Result range_result = normalize_texture_range(record->desc.desc, subresources);
        if (!range_result) {
            return range_result;
        }
        return graph_.impl_->add_depth_stencil_attachment(
                pass_index_, DepthStencilAttachment{texture, depth_load_op, depth_store_op, clear_depth,
                                                    clear_stencil, final_usage, subresources});
    }

    void PassBuilder::set_side_effects(bool enabled) noexcept {
        if (pass_index_ < graph_.impl_->passes.size()) {
            graph_.impl_->passes[pass_index_].side_effects = enabled;
        }
    }

    foundation::Result RenderGraph::Impl::add_use(uint32_t pass_index, ResourceUse use) {
        if (pass_index >= passes.size()) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Render graph pass index is invalid");
        }
        PassRecord& pass = passes[pass_index];
        if (use.kind == ResourceKind::kBuffer) {
            if (use.resource_index >= buffers.size() ||
                buffers[use.resource_index].generation != use.resource_generation) {
                return foundation::Result::failure(foundation::ResultCode::kNotFound,
                                            "Render graph pass references an invalid buffer");
            }
            const BufferRecord& record = buffers[use.resource_index];
            uint64_t end = 0;
            if (!checked_range_end(use.buffer_range.offset, use.buffer_range.size, end) ||
                use.buffer_range.offset > record.desc.desc.size ||
                (end != std::numeric_limits<uint64_t>::max() && end > record.desc.desc.size)) {
                return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                            "Render graph buffer range is out of bounds");
            }
        } else {
            if (use.resource_index >= textures.size() ||
                textures[use.resource_index].generation != use.resource_generation) {
                return foundation::Result::failure(foundation::ResultCode::kNotFound,
                                            "Render graph pass references an invalid texture");
            }
            rhi::SubresourceRange normalized = use.subresources;
            const foundation::Result range_result = normalize_texture_range(
                    textures[use.resource_index].desc.desc, normalized);
            if (!range_result) {
                return range_result;
            }
            use.subresources = normalized;
        }

        if (use.before_state.usage == rhi::ResourceUsage::kUndefined ||
            use.before_state.pipeline_stages == rhi::PipelineStage::kNone ||
            use.after_state.usage == rhi::ResourceUsage::kUndefined ||
            use.after_state.pipeline_stages == rhi::PipelineStage::kNone) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                        "Render graph resource usage must specify a valid state");
        }
        use.before_state = normalize_state(use.before_state, pass.queue);
        use.after_state = normalize_state(use.after_state, pass.queue);
        try {
            pass.uses.push_back(std::move(use));
        } catch (const std::bad_alloc&) {
            return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                        "Render graph resource-use allocation failed");
        }
        return foundation::Result::success();
    }

    foundation::Result RenderGraph::Impl::add_color_attachment(uint32_t pass_index, ColorAttachment attachment) {
        if (pass_index >= passes.size()) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Render graph pass index is invalid");
        }
        PassRecord& pass = passes[pass_index];
        if (pass.queue != rhi::QueueClass::kGraphics) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                        "Color attachments require a graphics pass");
        }
        const TextureRecord* texture = get_texture(attachment.texture);
        if (texture == nullptr) {
            return foundation::Result::failure(foundation::ResultCode::kNotFound,
                                        "Color attachment references an invalid texture");
        }
        const foundation::Result range_result = normalize_texture_range(texture->desc.desc, attachment.subresources);
        if (!range_result) {
            return range_result;
        }
        if (is_depth_format(texture->desc.desc.format)) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                        "A depth texture cannot be used as a color attachment");
        }
        if (attachment.subresources.mip_count != 1 || attachment.subresources.layer_count != 1 ||
            attachment.subresources.aspects != rhi::Aspect::kColor) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                        "Color attachments must select one color subresource");
        }
        if (attachment.final_usage == rhi::ResourceUsage::kUndefined) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                        "Color attachment final usage must be specified");
        }

        ResourceUse use{};
        use.kind = ResourceKind::kTexture;
        use.resource_index = attachment.texture.index;
        use.resource_generation = attachment.texture.generation;
        use.access = attachment.load_op == rhi::LoadOp::kLoad ? ResourceAccess::kReadWrite : ResourceAccess::kWrite;
        use.before_state = make_state(rhi::ResourceUsage::kColorAttachmentWrite, pass.queue,
                                       rhi::PipelineStage::kColorOutput, rhi::ShaderStage::kAll);
        use.after_state = make_state(attachment.final_usage, pass.queue,
                                      rhi::PipelineStage::kAllCommands, rhi::ShaderStage::kAll);
        use.subresources = attachment.subresources;
        use.discard_on_first_use = attachment.load_op != rhi::LoadOp::kLoad;

        const size_t previous_count = pass.color_attachments.size();
        try {
            pass.color_attachments.push_back(std::move(attachment));
        } catch (const std::bad_alloc&) {
            return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                        "Render graph color attachment allocation failed");
        }
        const foundation::Result use_result = add_use(pass_index, std::move(use));
        if (!use_result) {
            pass.color_attachments.resize(previous_count);
        }
        return use_result;
    }

    foundation::Result RenderGraph::Impl::add_depth_stencil_attachment(uint32_t pass_index,
                                                                DepthStencilAttachment attachment) {
        if (pass_index >= passes.size()) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Render graph pass index is invalid");
        }
        PassRecord& pass = passes[pass_index];
        if (pass.queue != rhi::QueueClass::kGraphics) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                        "Depth attachments require a graphics pass");
        }
        if (pass.depth_stencil.has_value()) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                        "A pass cannot have multiple depth attachments");
        }
        const TextureRecord* texture = get_texture(attachment.texture);
        if (texture == nullptr) {
            return foundation::Result::failure(foundation::ResultCode::kNotFound,
                                        "Depth attachment references an invalid texture");
        }
        const foundation::Result range_result = normalize_texture_range(texture->desc.desc, attachment.subresources);
        if (!range_result) {
            return range_result;
        }
        if (!is_depth_format(texture->desc.desc.format)) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                        "A color texture cannot be used as a depth attachment");
        }
        const rhi::AspectMask expected_aspects = default_aspect(texture->desc.desc.format);
        if (attachment.subresources.mip_count != 1 || attachment.subresources.layer_count != 1 ||
            attachment.subresources.aspects != expected_aspects) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                        "Depth attachments must select one depth subresource");
        }
        if (attachment.final_usage == rhi::ResourceUsage::kUndefined) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                        "Depth attachment final usage must be specified");
        }

        ResourceUse use{};
        use.kind = ResourceKind::kTexture;
        use.resource_index = attachment.texture.index;
        use.resource_generation = attachment.texture.generation;
        use.access = attachment.load_op == rhi::LoadOp::kLoad ? ResourceAccess::kReadWrite : ResourceAccess::kWrite;
        use.before_state = make_state(rhi::ResourceUsage::kDepthStencilWrite, pass.queue,
                                       rhi::PipelineStage::kEarlyDepth | rhi::PipelineStage::kLateDepth,
                                       rhi::ShaderStage::kAll);
        use.after_state = make_state(attachment.final_usage, pass.queue,
                                      rhi::PipelineStage::kAllCommands, rhi::ShaderStage::kAll);
        use.subresources = attachment.subresources;
        use.discard_on_first_use = attachment.load_op != rhi::LoadOp::kLoad;

        try {
            pass.depth_stencil = std::move(attachment);
        } catch (const std::bad_alloc&) {
            return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                        "Render graph depth attachment allocation failed");
        }
        const foundation::Result use_result = add_use(pass_index, std::move(use));
        if (!use_result) {
            pass.depth_stencil.reset();
        }
        return use_result;
    }

    foundation::Result RenderGraph::Impl::validate_use(const ResourceUse& use, const PassRecord& pass) const {
        if (use.before_state.usage == rhi::ResourceUsage::kUndefined ||
            use.after_state.usage == rhi::ResourceUsage::kUndefined ||
            use.before_state.pipeline_stages == rhi::PipelineStage::kNone ||
            use.after_state.pipeline_stages == rhi::PipelineStage::kNone) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                        "Render graph pass contains an undefined resource state");
        }
        if (use.before_state.queue != pass.queue || use.after_state.queue != pass.queue) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                        "Render graph resource state queue does not match its pass queue");
        }
        if (use.kind == ResourceKind::kBuffer) {
            if (use.resource_index >= buffers.size() ||
                buffers[use.resource_index].generation != use.resource_generation) {
                return foundation::Result::failure(foundation::ResultCode::kNotFound,
                                            "Render graph pass contains an invalid buffer use");
            }
            const BufferRecord& buffer = buffers[use.resource_index];
            uint64_t end = 0;
            if (!checked_range_end(use.buffer_range.offset, use.buffer_range.size, end) ||
                use.buffer_range.offset > buffer.desc.desc.size ||
                (end != std::numeric_limits<uint64_t>::max() && end > buffer.desc.desc.size)) {
                return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                            "Render graph buffer use range is out of bounds");
            }
        } else {
            if (use.resource_index >= textures.size() ||
                textures[use.resource_index].generation != use.resource_generation) {
                return foundation::Result::failure(foundation::ResultCode::kNotFound,
                                            "Render graph pass contains an invalid texture use");
            }
            rhi::SubresourceRange range = use.subresources;
            const foundation::Result range_result = normalize_texture_range(textures[use.resource_index].desc.desc, range);
            if (!range_result) {
                return range_result;
            }
        }
        return foundation::Result::success();
    }

    foundation::Result RenderGraph::Impl::validate_pass_attachments(const PassRecord& pass) const {
        if ((!pass.color_attachments.empty() || pass.depth_stencil.has_value()) &&
            pass.queue != rhi::QueueClass::kGraphics) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                        "Render graph attachments require a graphics pass");
        }
        if ((!pass.color_attachments.empty() || pass.depth_stencil.has_value()) && !pass.execute) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                        "Attachment passes require an execute callback");
        }

        for (size_t index = 0; index < pass.color_attachments.size(); ++index) {
            const ColorAttachment& attachment = pass.color_attachments[index];
            const TextureRecord* texture = get_texture(attachment.texture);
            if (texture == nullptr) {
                return foundation::Result::failure(foundation::ResultCode::kNotFound,
                                            "Render graph color attachment texture is invalid");
            }
            if (is_depth_format(texture->desc.desc.format) ||
                !rhi::has_flag(texture->desc.desc.capabilities, rhi::TextureCapability::kColorAttachment)) {
                return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                            "Render graph color attachment texture is not color-renderable");
            }
            if (attachment.subresources.aspects != rhi::Aspect::kColor ||
                attachment.subresources.mip_count != 1 || attachment.subresources.layer_count != 1) {
                return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                            "Render graph color attachment must select one subresource");
            }
            for (size_t previous = 0; previous < index; ++previous) {
                if (pass.color_attachments[previous].texture == attachment.texture &&
                    ranges_overlap(pass.color_attachments[previous].subresources, attachment.subresources,
                                   texture->desc.desc)) {
                    return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                "Render graph pass contains duplicate color attachments");
                }
            }
        }

        if (pass.depth_stencil.has_value()) {
            const DepthStencilAttachment& attachment = pass.depth_stencil.value();
            const TextureRecord* texture = get_texture(attachment.texture);
            if (texture == nullptr) {
                return foundation::Result::failure(foundation::ResultCode::kNotFound,
                                            "Render graph depth attachment texture is invalid");
            }
            if (!is_depth_format(texture->desc.desc.format) ||
                !rhi::has_flag(texture->desc.desc.capabilities, rhi::TextureCapability::kDepthStencil)) {
                return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                            "Render graph depth attachment texture is not depth-renderable");
            }
            if (attachment.subresources.mip_count != 1 || attachment.subresources.layer_count != 1 ||
                attachment.subresources.aspects != default_aspect(texture->desc.desc.format)) {
                return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                            "Render graph depth attachment must select one subresource");
            }
        }
        return foundation::Result::success();
    }

    foundation::Result RenderGraph::Impl::compile_graph(RenderGraphCompileInfo& info) {
        if (compiled && compiled_revision == revision) {
            info.resource_count = static_cast<uint32_t>(buffers.size() + textures.size());
            info.pass_count = static_cast<uint32_t>(passes.size());
            info.culled_pass_count = static_cast<uint32_t>(std::count(live_passes.begin(), live_passes.end(), 0));
            info.batch_count = static_cast<uint32_t>(batches.size());
            info.transient_buffer_slot_count = static_cast<uint32_t>(transient_buffer_slots.size());
            info.transient_texture_slot_count = static_cast<uint32_t>(transient_texture_slots.size());
            info.barrier_count = 0;
            info.barrier_point_count = 0;
            for (const RenderGraphPassPlan& pass_plan: plan.pass_plans) {
                info.barrier_count += static_cast<uint32_t>(pass_plan.barriers.size());
                const bool has_before = std::any_of(
                        pass_plan.barriers.begin(), pass_plan.barriers.end(),
                        [](const RenderGraphBarrierPlan& barrier) {
                            return barrier.phase == RenderGraphBarrierPhase::kBeforePass;
                        });
                const bool has_after = std::any_of(
                        pass_plan.barriers.begin(), pass_plan.barriers.end(),
                        [](const RenderGraphBarrierPlan& barrier) {
                            return barrier.phase == RenderGraphBarrierPhase::kAfterPass;
                        });
                info.barrier_point_count += has_before ? 1u : 0u;
                info.barrier_point_count += has_after ? 1u : 0u;
            }
            info.queue_dependency_count = static_cast<uint32_t>(plan.queue_dependencies.size());
            return foundation::Result::success();
        }

        try {
            edges.clear();
            live_passes.assign(passes.size(), 0);
            topological_order.clear();
            batches.clear();
            pass_to_batch.assign(passes.size(), kInvalidHandleIndex);
            transient_buffer_slots.clear();
            transient_texture_slots.clear();
            plan = {};

            for (BufferRecord& record: buffers) {
                record.has_use = false;
                record.first_use = kInvalidHandleIndex;
                record.last_use = kInvalidHandleIndex;
                record.physical_slot = kInvalidHandleIndex;
            }
            for (TextureRecord& record: textures) {
                record.has_use = false;
                record.first_use = kInvalidHandleIndex;
                record.last_use = kInvalidHandleIndex;
                record.physical_slot = kInvalidHandleIndex;
            }

            for (const PassRecord& pass: passes) {
                const foundation::Result attachment_result = validate_pass_attachments(pass);
                if (!attachment_result) {
                    return attachment_result;
                }
                for (const ResourceUse& use: pass.uses) {
                    const foundation::Result use_result = validate_use(use, pass);
                    if (!use_result) {
                        return use_result;
                    }
                }
                for (size_t use_index = 0; use_index < pass.uses.size(); ++use_index) {
                    const ResourceUse& use = pass.uses[use_index];
                    for (size_t previous_index = 0; previous_index < use_index; ++previous_index) {
                        const ResourceUse& previous = pass.uses[previous_index];
                        if (use.kind != previous.kind || use.resource_index != previous.resource_index ||
                            use.resource_generation != previous.resource_generation) {
                            continue;
                        }
                        const bool overlap = use.kind == ResourceKind::kBuffer
                                                     ? ranges_overlap(previous.buffer_range, use.buffer_range)
                                                     : ranges_overlap(previous.subresources, use.subresources,
                                                                       textures[use.resource_index].desc.desc);
                        if (overlap && (!state_matches(previous.before_state, use.before_state) ||
                                        !state_matches(previous.after_state, use.after_state))) {
                            return foundation::Result::failure(
                                    foundation::ResultCode::kInvalidArgument,
                                    "A render graph pass contains overlapping resource uses with different states");
                        }
                    }
                }
            }

            const auto add_edge = [this](uint32_t source, uint32_t destination) {
                if (source != destination) {
                    edges.push_back(Edge{source, destination});
                }
            };

            std::vector<std::vector<UseReference>> buffer_history(buffers.size());
            std::vector<std::vector<UseReference>> texture_history(textures.size());
            for (uint32_t pass_index = 0; pass_index < passes.size(); ++pass_index) {
                PassRecord& pass = passes[pass_index];
                for (ResourceUse& use: pass.uses) {
                    use.requires_memory_dependency = false;
                    use.requires_queue_transfer = false;
                    std::vector<UseReference>& history = use.kind == ResourceKind::kBuffer
                                                               ? buffer_history[use.resource_index]
                                                               : texture_history[use.resource_index];
                    for (const UseReference& previous: history) {
                        bool overlap = false;
                        if (use.kind == ResourceKind::kBuffer) {
                            overlap = ranges_overlap(previous.use->buffer_range, use.buffer_range);
                        } else {
                            overlap = ranges_overlap(previous.use->subresources, use.subresources,
                                                     textures[use.resource_index].desc.desc);
                        }
                        if (!overlap) {
                            continue;
                        }
                        const bool hazard = is_write_access(previous.use->access) || is_write_access(use.access);
                        const bool state_change = !state_matches(previous.use->after_state, use.before_state);
                        if (hazard) {
                            use.requires_memory_dependency = true;
                        }
                        if (previous.use->after_state.queue != use.before_state.queue) {
                            use.requires_queue_transfer = true;
                        }
                        if (hazard || state_change) {
                            add_edge(previous.pass_index, pass_index);
                        }
                    }
                    history.push_back(UseReference{pass_index, &use});
                }
            }

            std::sort(edges.begin(), edges.end());
            edges.erase(std::unique(edges.begin(), edges.end()), edges.end());

            std::vector<uint32_t> roots;
            roots.reserve(passes.size());
            for (uint32_t pass_index = 0; pass_index < passes.size(); ++pass_index) {
                const PassRecord& pass = passes[pass_index];
                bool root = pass.side_effects;
                for (const ResourceUse& use: pass.uses) {
                    if (is_write_access(use.access)) {
                        const bool exported = use.kind == ResourceKind::kBuffer
                                                      ? buffers[use.resource_index].exported
                                                      : textures[use.resource_index].exported;
                        root = root || exported;
                    }
                    if (use.after_state.usage == rhi::ResourceUsage::kPresent) {
                        root = true;
                    }
                }
                if (root) {
                    roots.push_back(pass_index);
                }
                for (const ColorAttachment& attachment: pass.color_attachments) {
                    if (attachment.final_usage == rhi::ResourceUsage::kPresent) {
                        root = true;
                    }
                }
                if (pass.depth_stencil.has_value() &&
                    pass.depth_stencil->final_usage == rhi::ResourceUsage::kPresent) {
                    root = true;
                }
                if (root && std::find(roots.begin(), roots.end(), pass_index) == roots.end()) {
                    roots.push_back(pass_index);
                }
            }

            if (roots.empty()) {
                std::fill(live_passes.begin(), live_passes.end(), static_cast<uint8_t>(1));
            } else {
                std::vector<std::vector<uint32_t>> reverse_edges(passes.size());
                for (const Edge& edge: edges) {
                    reverse_edges[edge.destination].push_back(edge.source);
                }
                std::vector<uint32_t> pending = roots;
                while (!pending.empty()) {
                    const uint32_t pass_index = pending.back();
                    pending.pop_back();
                    if (live_passes[pass_index] != 0) {
                        continue;
                    }
                    live_passes[pass_index] = 1;
                    pending.insert(pending.end(), reverse_edges[pass_index].begin(), reverse_edges[pass_index].end());
                }
            }

            std::vector<uint32_t> indegree(passes.size(), 0);
            for (const Edge& edge: edges) {
                if (live_passes[edge.source] != 0 && live_passes[edge.destination] != 0) {
                    ++indegree[edge.destination];
                }
            }
            std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<uint32_t>> ready;
            uint32_t live_count = 0;
            for (uint32_t pass_index = 0; pass_index < passes.size(); ++pass_index) {
                if (live_passes[pass_index] == 0) {
                    continue;
                }
                ++live_count;
                if (indegree[pass_index] == 0) {
                    ready.push(pass_index);
                }
            }
            while (!ready.empty()) {
                const uint32_t pass_index = ready.top();
                ready.pop();
                topological_order.push_back(pass_index);
                for (const Edge& edge: edges) {
                    if (edge.source != pass_index || live_passes[edge.destination] == 0) {
                        continue;
                    }
                    if (--indegree[edge.destination] == 0) {
                        ready.push(edge.destination);
                    }
                }
            }
            if (topological_order.size() != live_count) {
                return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                            "Render graph contains a dependency cycle");
            }

            for (uint32_t topo_index = 0; topo_index < topological_order.size(); ++topo_index) {
                const PassRecord& pass = passes[topological_order[topo_index]];
                for (const ResourceUse& use: pass.uses) {
                    if (use.kind == ResourceKind::kBuffer) {
                        BufferRecord& record = buffers[use.resource_index];
                        if (!record.has_use) {
                            record.first_use = topo_index;
                            record.last_use = topo_index;
                            record.has_use = true;
                        } else {
                            record.first_use = std::min(record.first_use, topo_index);
                            record.last_use = std::max(record.last_use, topo_index);
                        }
                    } else {
                        TextureRecord& record = textures[use.resource_index];
                        if (!record.has_use) {
                            record.first_use = topo_index;
                            record.last_use = topo_index;
                            record.has_use = true;
                        } else {
                            record.first_use = std::min(record.first_use, topo_index);
                            record.last_use = std::max(record.last_use, topo_index);
                        }
                    }
                }
            }

            struct Interval {
                ResourceKind kind{ResourceKind::kBuffer};
                uint32_t index{kInvalidHandleIndex};
                uint32_t first{kInvalidHandleIndex};
                uint32_t last{kInvalidHandleIndex};
            };
            std::vector<Interval> intervals;
            for (uint32_t index = 0; index < buffers.size(); ++index) {
                BufferRecord& record = buffers[index];
                if (record.lifetime == ResourceLifetime::kTransient && record.has_use) {
                    intervals.push_back(Interval{ResourceKind::kBuffer, index, record.first_use, record.last_use});
                }
            }
            for (uint32_t index = 0; index < textures.size(); ++index) {
                TextureRecord& record = textures[index];
                if (record.lifetime == ResourceLifetime::kTransient && record.has_use) {
                    intervals.push_back(Interval{ResourceKind::kTexture, index, record.first_use, record.last_use});
                }
            }
            std::sort(intervals.begin(), intervals.end(), [](const Interval& lhs, const Interval& rhs) {
                if (lhs.first != rhs.first) {
                    return lhs.first < rhs.first;
                }
                if (lhs.last != rhs.last) {
                    return lhs.last < rhs.last;
                }
                if (lhs.kind != rhs.kind) {
                    return lhs.kind < rhs.kind;
                }
                return lhs.index < rhs.index;
            });

            for (const Interval& interval: intervals) {
                const PassRecord& first_pass = passes[topological_order[interval.first]];
                const ResourceUse* first_resource_use = nullptr;
                for (const ResourceUse& use: first_pass.uses) {
                    if (((interval.kind == ResourceKind::kBuffer && use.kind == ResourceKind::kBuffer) ||
                         (interval.kind == ResourceKind::kTexture && use.kind == ResourceKind::kTexture)) &&
                        use.resource_index == interval.index) {
                        first_resource_use = &use;
                        break;
                    }
                }
                if (first_resource_use == nullptr || first_resource_use->access != ResourceAccess::kWrite ||
                    !first_resource_use->discard_on_first_use) {
                    return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                "A transient resource must be first written with discarded contents");
                }

                if (interval.kind == ResourceKind::kBuffer) {
                    BufferRecord& record = buffers[interval.index];
                    uint32_t selected_slot = kInvalidHandleIndex;
                    for (uint32_t slot_index = 0; slot_index < transient_buffer_slots.size(); ++slot_index) {
                        TransientBufferSlotPlan& slot = transient_buffer_slots[slot_index];
                        if (slot.last_use < interval.first && buffer_desc_equal(slot.desc, record.desc)) {
                            selected_slot = slot_index;
                            if (slot.last_resource != kInvalidHandleIndex) {
                                edges.push_back(Edge{buffers[slot.last_resource].last_use == kInvalidHandleIndex
                                                             ? topological_order[interval.first]
                                                             : topological_order[buffers[slot.last_resource].last_use],
                                                     topological_order[interval.first]});
                            }
                            slot.last_resource = interval.index;
                            slot.last_use = interval.last;
                            break;
                        }
                    }
                    if (selected_slot == kInvalidHandleIndex) {
                        selected_slot = static_cast<uint32_t>(transient_buffer_slots.size());
                        transient_buffer_slots.push_back(TransientBufferSlotPlan{record.desc, interval.index,
                                                                                  interval.last});
                    }
                    record.physical_slot = selected_slot;
                } else {
                    TextureRecord& record = textures[interval.index];
                    uint32_t selected_slot = kInvalidHandleIndex;
                    for (uint32_t slot_index = 0; slot_index < transient_texture_slots.size(); ++slot_index) {
                        TransientTextureSlotPlan& slot = transient_texture_slots[slot_index];
                        if (slot.last_use < interval.first && texture_desc_equal(slot.desc, record.desc)) {
                            selected_slot = slot_index;
                            if (slot.last_resource != kInvalidHandleIndex) {
                                edges.push_back(Edge{textures[slot.last_resource].last_use == kInvalidHandleIndex
                                                             ? topological_order[interval.first]
                                                             : topological_order[textures[slot.last_resource].last_use],
                                                     topological_order[interval.first]});
                            }
                            slot.last_resource = interval.index;
                            slot.last_use = interval.last;
                            break;
                        }
                    }
                    if (selected_slot == kInvalidHandleIndex) {
                        selected_slot = static_cast<uint32_t>(transient_texture_slots.size());
                        transient_texture_slots.push_back(TransientTextureSlotPlan{record.desc, interval.index,
                                                                                   interval.last});
                    }
                    record.physical_slot = selected_slot;
                }
            }

            std::sort(edges.begin(), edges.end());
            edges.erase(std::unique(edges.begin(), edges.end()), edges.end());

            for (const uint32_t pass_index: topological_order) {
                const rhi::QueueClass queue = passes[pass_index].queue;
                if (batches.empty() || batches.back().queue != queue) {
                    batches.push_back(Batch{queue});
                }
                const uint32_t batch_index = static_cast<uint32_t>(batches.size() - 1);
                batches.back().passes.push_back(pass_index);
                pass_to_batch[pass_index] = batch_index;
            }

            const auto dependency_stages = [&](const Edge& edge) noexcept {
                rhi::PipelineStageMask stages = rhi::PipelineStage::kNone;
                const PassRecord& source_pass = passes[edge.source];
                const PassRecord& destination_pass = passes[edge.destination];
                for (const ResourceUse& destination_use: destination_pass.uses) {
                    bool related = false;
                    for (const ResourceUse& source_use: source_pass.uses) {
                        if (resource_uses_overlap(source_use, destination_use)) {
                            related = true;
                            break;
                        }
                    }
                    if (related) {
                        stages |= destination_use.before_state.pipeline_stages;
                    }
                }
                if (stages == rhi::PipelineStage::kNone) {
                    for (const ResourceUse& use: destination_pass.uses) {
                        stages |= use.before_state.pipeline_stages;
                    }
                }
                return stages == rhi::PipelineStage::kNone ? rhi::PipelineStage::kAllCommands : stages;
            };

            const auto add_batch_dependency = [](Batch& destination, uint32_t source_batch,
                                                 rhi::PipelineStageMask stages, uint32_t hazard_count) {
                for (BatchDependency& dependency: destination.dependencies) {
                    if (dependency.source_batch != source_batch) {
                        continue;
                    }
                    dependency.destination_stages |= stages;
                    const uint32_t remaining = std::numeric_limits<uint32_t>::max() -
                                               dependency.resource_hazard_count;
                    dependency.resource_hazard_count += std::min(remaining, hazard_count);
                    return;
                }
                destination.dependencies.push_back(BatchDependency{source_batch, stages, hazard_count});
            };

            for (const Edge& edge: edges) {
                if (live_passes[edge.source] == 0 || live_passes[edge.destination] == 0) {
                    continue;
                }
                const uint32_t source_batch = pass_to_batch[edge.source];
                const uint32_t destination_batch = pass_to_batch[edge.destination];
                if (source_batch == destination_batch || batches[source_batch].queue == batches[destination_batch].queue) {
                    continue;
                }
                const uint32_t hazard_count = std::max(1u, resource_hazard_count(edge));
                add_batch_dependency(batches[destination_batch], source_batch, dependency_stages(edge), hazard_count);
            }

            bool acquire_batch_found = false;
            for (Batch& batch: batches) {
                bool touches_swapchain = false;
                for (const uint32_t pass_index: batch.passes) {
                    for (const ResourceUse& use: passes[pass_index].uses) {
                        if (use.kind == ResourceKind::kTexture && use.resource_index < textures.size() &&
                            textures[use.resource_index].generation == use.resource_generation &&
                            textures[use.resource_index].is_swapchain_image) {
                            touches_swapchain = true;
                            break;
                        }
                    }
                    if (touches_swapchain) {
                        break;
                    }
                }
                if (touches_swapchain && !acquire_batch_found) {
                    batch.waits_for_acquire = true;
                    acquire_batch_found = true;
                }
            }

            const foundation::Result audit_result = audit_cross_queue_dependencies();
            if (!audit_result) {
                return audit_result;
            }

            uint32_t frame_output_batches = 0;
            for (Batch& batch: batches) {
                for (const uint32_t pass_index: batch.passes) {
                    const PassRecord& pass = passes[pass_index];
                    for (const ResourceUse& use: pass.uses) {
                        if (use.after_state.usage == rhi::ResourceUsage::kPresent && is_write_access(use.access)) {
                            batch.submits_frame = true;
                        }
                    }
                    for (const ColorAttachment& attachment: pass.color_attachments) {
                        batch.submits_frame = batch.submits_frame ||
                                              attachment.final_usage == rhi::ResourceUsage::kPresent;
                    }
                    if (pass.depth_stencil.has_value()) {
                        batch.submits_frame = batch.submits_frame ||
                                              pass.depth_stencil->final_usage == rhi::ResourceUsage::kPresent;
                    }
                }
                frame_output_batches += batch.submits_frame ? 1u : 0u;
            }
            if (frame_output_batches > 1) {
                return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                            "A render graph can have only one present output batch");
            }

            const foundation::Result plan_result = build_plan(info);
            if (!plan_result) {
                return plan_result;
            }
            compiled_revision = revision;
            compiled = true;
            return foundation::Result::success();
        } catch (const std::bad_alloc&) {
            compiled = false;
            return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                        "Render graph compilation allocation failed");
        }
    }

    bool RenderGraph::Impl::resource_uses_overlap(const ResourceUse& lhs,
                                                   const ResourceUse& rhs) const noexcept {
        if (lhs.kind != rhs.kind || lhs.resource_index != rhs.resource_index ||
            lhs.resource_generation != rhs.resource_generation) {
            return false;
        }
        if (lhs.kind == ResourceKind::kBuffer) {
            return lhs.resource_index < buffers.size() && ranges_overlap(lhs.buffer_range, rhs.buffer_range);
        }
        return lhs.resource_index < textures.size() &&
               ranges_overlap(lhs.subresources, rhs.subresources, textures[lhs.resource_index].desc.desc);
    }

    uint32_t RenderGraph::Impl::resource_hazard_count(const Edge& edge) const noexcept {
        if (edge.source >= passes.size() || edge.destination >= passes.size()) {
            return 0;
        }

        uint32_t count = 0;
        const PassRecord& source_pass = passes[edge.source];
        const PassRecord& destination_pass = passes[edge.destination];
        for (const ResourceUse& source_use: source_pass.uses) {
            for (const ResourceUse& destination_use: destination_pass.uses) {
                if (!resource_uses_overlap(source_use, destination_use)) {
                    continue;
                }
                const bool hazard = is_write_access(source_use.access) || is_write_access(destination_use.access);
                const bool state_change = !state_matches(source_use.after_state, destination_use.before_state);
                if (hazard || state_change) {
                    if (count != std::numeric_limits<uint32_t>::max()) {
                        ++count;
                    }
                }
            }
        }
        return count;
    }

    foundation::Result RenderGraph::Impl::audit_cross_queue_dependencies() const {
        const auto dependency_for_batches = [&](uint32_t source_batch,
                                                  uint32_t destination_batch) noexcept {
            if (destination_batch >= batches.size()) {
                return static_cast<const BatchDependency*>(nullptr);
            }
            const Batch& destination = batches[destination_batch];
            const auto iterator = std::find_if(
                    destination.dependencies.begin(), destination.dependencies.end(),
                    [source_batch](const BatchDependency& dependency) {
                        return dependency.source_batch == source_batch;
                    });
            return iterator == destination.dependencies.end() ? nullptr : &*iterator;
        };

        for (uint32_t destination_batch_index = 0; destination_batch_index < batches.size();
             ++destination_batch_index) {
            const Batch& destination = batches[destination_batch_index];
            for (const BatchDependency& dependency: destination.dependencies) {
                if (dependency.source_batch >= batches.size() || dependency.source_batch >= destination_batch_index) {
                    return foundation::Result::failure(
                            foundation::ResultCode::kInvalidState,
                            "Render graph queue dependency does not follow batch topology");
                }
                const Batch& source = batches[dependency.source_batch];
                if (source.queue == destination.queue) {
                    return foundation::Result::failure(
                            foundation::ResultCode::kInvalidState,
                            "Render graph queue dependency references the same queue");
                }
                if (dependency.destination_stages == rhi::PipelineStage::kNone ||
                    dependency.resource_hazard_count == 0) {
                    return foundation::Result::failure(
                            foundation::ResultCode::kInvalidState,
                            "Render graph queue dependency has no destination stage or resource hazard");
                }

                uint32_t hazard_count = 0;
                for (const Edge& edge: edges) {
                    if (edge.source >= live_passes.size() || edge.destination >= live_passes.size() ||
                        live_passes[edge.source] == 0 || live_passes[edge.destination] == 0 ||
                        pass_to_batch[edge.source] != dependency.source_batch ||
                        pass_to_batch[edge.destination] != destination_batch_index) {
                        continue;
                    }
                    const uint32_t contribution = std::max(1u, resource_hazard_count(edge));
                    const uint32_t remaining = std::numeric_limits<uint32_t>::max() - hazard_count;
                    hazard_count += std::min(remaining, contribution);
                }
                if (hazard_count == 0 || hazard_count != dependency.resource_hazard_count) {
                    return foundation::Result::failure(
                            foundation::ResultCode::kInvalidState,
                            "Render graph queue dependency is not backed by the expected resource hazards");
                }
            }
        }

        for (const Edge& edge: edges) {
            if (edge.source >= live_passes.size() || edge.destination >= live_passes.size() ||
                live_passes[edge.source] == 0 || live_passes[edge.destination] == 0) {
                continue;
            }
            const uint32_t source_batch = pass_to_batch[edge.source];
            const uint32_t destination_batch = pass_to_batch[edge.destination];
            if (source_batch == kInvalidHandleIndex || destination_batch == kInvalidHandleIndex ||
                source_batch >= batches.size() || destination_batch >= batches.size()) {
                return foundation::Result::failure(
                        foundation::ResultCode::kInvalidState,
                        "Render graph edge is not assigned to a queue batch");
            }
            if (batches[source_batch].queue == batches[destination_batch].queue) {
                continue;
            }
            if (source_batch >= destination_batch || dependency_for_batches(source_batch, destination_batch) == nullptr) {
                return foundation::Result::failure(
                        foundation::ResultCode::kInvalidState,
                        "Render graph cross-queue edge has no audited dependency");
            }
        }

        return foundation::Result::success();
    }

    foundation::Result RenderGraph::Impl::build_plan(RenderGraphCompileInfo& info) {
        try {
            RenderGraphPlan built_plan{};
            built_plan.revision = revision;
            built_plan.topological_order = topological_order;
            built_plan.pass_plans.resize(passes.size());
            built_plan.batches.reserve(batches.size());
            built_plan.queue_dependencies.reserve(batches.size());

            std::vector<uint32_t> topological_positions(passes.size(), kInvalidHandleIndex);
            for (uint32_t topological_index = 0; topological_index < topological_order.size();
                 ++topological_index) {
                topological_positions[topological_order[topological_index]] = topological_index;
            }

            for (uint32_t pass_index = 0; pass_index < passes.size(); ++pass_index) {
                RenderGraphPassPlan& pass_plan = built_plan.pass_plans[pass_index];
                const PassRecord& pass = passes[pass_index];
                pass_plan.pass = RenderGraphPass{pass_index, pass.generation, graph_id};
                pass_plan.live = live_passes[pass_index] != 0;
                pass_plan.topological_index = topological_positions[pass_index];
                pass_plan.batch_index = pass_plan.live ? pass_to_batch[pass_index] : kInvalidHandleIndex;
            }

            for (uint32_t batch_index = 0; batch_index < batches.size(); ++batch_index) {
                const Batch& batch = batches[batch_index];
                RenderGraphBatchPlan batch_plan{};
                batch_plan.index = batch_index;
                batch_plan.queue = batch.queue;
                batch_plan.pass_indices = batch.passes;
                batch_plan.submits_frame = batch.submits_frame;
                batch_plan.waits_for_acquire = batch.waits_for_acquire;
                batch_plan.dependencies.reserve(batch.dependencies.size());
                for (const BatchDependency& dependency: batch.dependencies) {
                    if (dependency.source_batch >= batches.size()) {
                        return foundation::Result::failure(
                                foundation::ResultCode::kInvalidState,
                                "Render graph plan contains an invalid source batch");
                    }
                    batch_plan.dependencies.push_back(RenderGraphQueueDependency{
                            dependency.source_batch,
                            batch_index,
                            batches[dependency.source_batch].queue,
                            batch.queue,
                            dependency.destination_stages,
                            dependency.resource_hazard_count
                    });
                    built_plan.queue_dependencies.push_back(batch_plan.dependencies.back());
                }
                built_plan.batches.push_back(std::move(batch_plan));
            }

            std::vector<std::vector<BufferStateSegment>> buffer_states(buffers.size());
            std::vector<std::vector<rhi::ResourceState>> texture_states(textures.size());
            for (uint32_t index = 0; index < buffers.size(); ++index) {
                const BufferRecord& record = buffers[index];
                if (record.lifetime != ResourceLifetime::kTransient && record.physical.handle.is_valid() &&
                    !record.physical.states.empty()) {
                    buffer_states[index] = record.physical.states;
                } else {
                    initialize_buffer_state_segments(
                            buffer_states[index], record.desc.desc.size,
                            rhi::ResourceState{record.desc.desc.initial_usage,
                                               rhi::ShaderStage::kAll,
                                               record.desc.desc.initial_queue,
                                               false,
                                               record.desc.desc.initial_stages});
                }
            }
            for (uint32_t index = 0; index < textures.size(); ++index) {
                const TextureRecord& record = textures[index];
                if (record.lifetime != ResourceLifetime::kTransient && !record.physical.states.empty()) {
                    texture_states[index] = record.physical.states;
                } else {
                    texture_states[index].assign(
                            static_cast<size_t>(kAspectPlaneCount) * record.desc.desc.mip_levels *
                                    record.desc.desc.array_layers,
                            rhi::ResourceState{
                                    record.desc.desc.initial_usage,
                                    rhi::ShaderStage::kAll,
                                    record.desc.desc.initial_queue,
                                    false,
                                    record.desc.desc.initial_stages
                            });
                }
            }

            std::vector<std::vector<BufferStateSegment>> transient_buffer_states(transient_buffer_slots.size());
            for (uint32_t index = 0; index < transient_buffer_slots.size(); ++index) {
                const OwnedBufferDesc& desc = transient_buffer_slots[index].desc;
                initialize_buffer_state_segments(
                        transient_buffer_states[index], desc.desc.size,
                        rhi::ResourceState{desc.desc.initial_usage,
                                           rhi::ShaderStage::kAll,
                                           desc.desc.initial_queue,
                                           false,
                                           desc.desc.initial_stages});
            }
            std::vector<std::vector<rhi::ResourceState>> transient_texture_states(transient_texture_slots.size());
            for (uint32_t index = 0; index < transient_texture_slots.size(); ++index) {
                const OwnedTextureDesc& desc = transient_texture_slots[index].desc;
                transient_texture_states[index].assign(
                        static_cast<size_t>(kAspectPlaneCount) * desc.desc.mip_levels * desc.desc.array_layers,
                        rhi::ResourceState{
                                desc.desc.initial_usage,
                                rhi::ShaderStage::kAll,
                                desc.desc.initial_queue,
                                false,
                                desc.desc.initial_stages
                        });
            }

            const auto append_barrier = [](std::vector<RenderGraphBarrierPlan>& barriers,
                                           RenderGraphResourceKind resource_kind, uint32_t resource_index,
                                           uint32_t resource_generation, RenderGraphBarrierPhase phase,
                                           RenderGraphBarrierKind kind, ResourceAccess access,
                                           const rhi::ResourceState& before, const rhi::ResourceState& after,
                                           RenderGraphBufferRange buffer_range,
                                           rhi::SubresourceRange subresources, bool discard_contents,
                                           bool requires_memory_dependency, bool requires_queue_transfer) {
                barriers.push_back(RenderGraphBarrierPlan{
                        resource_kind,
                        resource_index,
                        resource_generation,
                        phase,
                        kind,
                        access,
                        before,
                        after,
                        buffer_range,
                        subresources,
                        discard_contents,
                        requires_memory_dependency,
                        requires_queue_transfer
                });
            };

            for (const uint32_t pass_index: topological_order) {
                const PassRecord& pass = passes[pass_index];
                RenderGraphPassPlan& pass_plan = built_plan.pass_plans[pass_index];
                std::vector<RenderGraphBarrierPlan> before_barriers;
                std::vector<RenderGraphBarrierPlan> after_barriers;
                before_barriers.reserve(pass.uses.size());
                after_barriers.reserve(pass.uses.size());

                for (const ResourceUse& use: pass.uses) {
                    const bool first_use = use.kind == ResourceKind::kBuffer
                                                   ? buffers[use.resource_index].first_use == pass_plan.topological_index
                                                   : textures[use.resource_index].first_use == pass_plan.topological_index;
                    if (use.kind == ResourceKind::kBuffer) {
                        BufferRecord& record = buffers[use.resource_index];
                        if (first_use && record.lifetime == ResourceLifetime::kTransient) {
                            if (record.physical_slot >= transient_buffer_states.size()) {
                                return foundation::Result::failure(
                                        foundation::ResultCode::kInvalidState,
                                        "Render graph transient buffer slot is unavailable while building its plan");
                            }
                            buffer_states[use.resource_index] = transient_buffer_states[record.physical_slot];
                        }

                        uint64_t range_end = 0;
                        if (!resolve_buffer_range(record.desc.desc.size, use.buffer_range, range_end)) {
                            return foundation::Result::failure(
                                    foundation::ResultCode::kInvalidArgument,
                                    "Render graph buffer barrier range is out of bounds");
                        }
                        initialize_buffer_state_segments(
                                buffer_states[use.resource_index], record.desc.desc.size,
                                rhi::ResourceState{record.desc.desc.initial_usage,
                                                   rhi::ShaderStage::kAll,
                                                   record.desc.desc.initial_queue,
                                                   false,
                                                   record.desc.desc.initial_stages});
                        std::vector<BufferStateSegment>& states = buffer_states[use.resource_index];
                        split_buffer_state_segment(states, use.buffer_range.offset);
                        split_buffer_state_segment(states, range_end);
                        const rhi::ResourceState target_before = normalize_state(use.before_state, pass.queue);
                        const bool discard = first_use && use.discard_on_first_use && is_write_access(use.access);
                        for (BufferStateSegment& segment: states) {
                            const uint64_t segment_end = segment.offset + segment.size;
                            if (segment.offset < use.buffer_range.offset || segment_end > range_end) {
                                continue;
                            }
                            const rhi::ResourceState before = segment.state;
                            const bool queue_transfer = use.requires_queue_transfer ||
                                                        before.queue != target_before.queue;
                            const bool transition = !state_matches(before, target_before) || discard;
                            const bool memory_dependency = use.requires_memory_dependency;
                            if (transition || memory_dependency || queue_transfer) {
                                const RenderGraphBarrierKind kind = queue_transfer
                                                                             ? RenderGraphBarrierKind::kQueueTransfer
                                                                             : transition
                                                                                       ? RenderGraphBarrierKind::kTransition
                                                                                       : RenderGraphBarrierKind::kMemoryDependency;
                                append_barrier(before_barriers,
                                               RenderGraphResourceKind::kBuffer,
                                               use.resource_index,
                                               use.resource_generation,
                                               RenderGraphBarrierPhase::kBeforePass,
                                               kind,
                                               use.access,
                                               before,
                                               target_before,
                                               RenderGraphBufferRange{segment.offset, segment.size},
                                               {},
                                               discard,
                                               memory_dependency,
                                               queue_transfer);
                            }
                            segment.state = target_before;
                        }

                        const rhi::ResourceState target_after = normalize_state(use.after_state, pass.queue);
                        for (BufferStateSegment& segment: states) {
                            const uint64_t segment_end = segment.offset + segment.size;
                            if (segment.offset < use.buffer_range.offset || segment_end > range_end) {
                                continue;
                            }
                            if (!state_matches(target_before, target_after)) {
                                append_barrier(after_barriers,
                                               RenderGraphResourceKind::kBuffer,
                                               use.resource_index,
                                               use.resource_generation,
                                               RenderGraphBarrierPhase::kAfterPass,
                                               RenderGraphBarrierKind::kTransition,
                                               use.access,
                                               target_before,
                                               target_after,
                                               RenderGraphBufferRange{segment.offset, segment.size},
                                               {},
                                               false,
                                               false,
                                               target_before.queue != target_after.queue);
                            }
                            segment.state = target_after;
                        }
                        merge_buffer_state_segments(states);
                        if (record.lifetime == ResourceLifetime::kTransient &&
                            record.physical_slot < transient_buffer_states.size()) {
                            transient_buffer_states[record.physical_slot] = states;
                        }
                        continue;
                    }

                    TextureRecord& record = textures[use.resource_index];
                    if (first_use && record.lifetime == ResourceLifetime::kTransient) {
                        if (record.physical_slot >= transient_texture_states.size()) {
                            return foundation::Result::failure(
                                    foundation::ResultCode::kInvalidState,
                                    "Render graph transient texture slot is unavailable while building its plan");
                        }
                        texture_states[use.resource_index] = transient_texture_states[record.physical_slot];
                    }

                    const rhi::ResourceState target_before = normalize_state(use.before_state, pass.queue);
                    const rhi::ResourceState target_after = normalize_state(use.after_state, pass.queue);
                    const rhi::Aspect aspects[] = {
                            rhi::Aspect::kColor,
                            rhi::Aspect::kDepth,
                            rhi::Aspect::kStencil
                    };
                    for (const rhi::Aspect aspect: aspects) {
                        if (!rhi::has_flag(use.subresources.aspects, aspect)) {
                            continue;
                        }
                        for (uint32_t mip = use.subresources.base_mip;
                             mip < use.subresources.base_mip + use.subresources.mip_count; ++mip) {
                            for (uint32_t layer = use.subresources.base_layer;
                                 layer < use.subresources.base_layer + use.subresources.layer_count; ++layer) {
                                const uint32_t state_index = texture_state_index(
                                        aspect, record.desc.desc, mip, layer);
                                if (state_index >= texture_states[use.resource_index].size()) {
                                    return foundation::Result::failure(
                                            foundation::ResultCode::kInvalidState,
                                            "Render graph texture state is unavailable while building its plan");
                                }
                                const rhi::ResourceState before = texture_states[use.resource_index][state_index];
                                const bool discard = first_use && use.discard_on_first_use && is_write_access(use.access);
                                const bool queue_transfer = use.requires_queue_transfer ||
                                                            before.queue != target_before.queue;
                                const bool transition = !state_matches(before, target_before) || discard;
                                const bool memory_dependency = use.requires_memory_dependency;
                                if (transition || memory_dependency || queue_transfer) {
                                    const RenderGraphBarrierKind kind = queue_transfer
                                                                                 ? RenderGraphBarrierKind::kQueueTransfer
                                                                                 : transition
                                                                                           ? RenderGraphBarrierKind::kTransition
                                                                                           : RenderGraphBarrierKind::kMemoryDependency;
                                    append_barrier(before_barriers,
                                                   RenderGraphResourceKind::kTexture,
                                                   use.resource_index,
                                                   use.resource_generation,
                                                   RenderGraphBarrierPhase::kBeforePass,
                                                   kind,
                                                   use.access,
                                                   before,
                                                   target_before,
                                                   {},
                                                   rhi::SubresourceRange{aspect, mip, 1, layer, 1},
                                                   discard,
                                                   memory_dependency,
                                                   queue_transfer);
                                }
                                if (!state_matches(target_before, target_after)) {
                                    append_barrier(after_barriers,
                                                   RenderGraphResourceKind::kTexture,
                                                   use.resource_index,
                                                   use.resource_generation,
                                                   RenderGraphBarrierPhase::kAfterPass,
                                                   RenderGraphBarrierKind::kTransition,
                                                   use.access,
                                                   target_before,
                                                   target_after,
                                                   {},
                                                   rhi::SubresourceRange{aspect, mip, 1, layer, 1},
                                                   false,
                                                   false,
                                                   target_before.queue != target_after.queue);
                                }
                                texture_states[use.resource_index][state_index] = target_after;
                                if (record.lifetime == ResourceLifetime::kTransient &&
                                    record.physical_slot < transient_texture_states.size()) {
                                    transient_texture_states[record.physical_slot][state_index] = target_after;
                                }
                            }
                        }
                    }
                }

                pass_plan.barriers.reserve(before_barriers.size() + after_barriers.size());
                pass_plan.barriers.insert(pass_plan.barriers.end(), before_barriers.begin(), before_barriers.end());
                pass_plan.barriers.insert(pass_plan.barriers.end(), after_barriers.begin(), after_barriers.end());
            }

            uint32_t barrier_count = 0;
            uint32_t barrier_point_count = 0;
            for (const RenderGraphPassPlan& pass_plan: built_plan.pass_plans) {
                barrier_count += static_cast<uint32_t>(pass_plan.barriers.size());
                const bool has_before = std::any_of(
                        pass_plan.barriers.begin(), pass_plan.barriers.end(), [](const RenderGraphBarrierPlan& barrier) {
                            return barrier.phase == RenderGraphBarrierPhase::kBeforePass;
                        });
                const bool has_after = std::any_of(
                        pass_plan.barriers.begin(), pass_plan.barriers.end(), [](const RenderGraphBarrierPlan& barrier) {
                            return barrier.phase == RenderGraphBarrierPhase::kAfterPass;
                        });
                barrier_point_count += has_before ? 1u : 0u;
                barrier_point_count += has_after ? 1u : 0u;
            }

            info.resource_count = static_cast<uint32_t>(buffers.size() + textures.size());
            info.pass_count = static_cast<uint32_t>(passes.size());
            info.culled_pass_count = static_cast<uint32_t>(
                    std::count(live_passes.begin(), live_passes.end(), static_cast<uint8_t>(0)));
            info.batch_count = static_cast<uint32_t>(batches.size());
            info.transient_buffer_slot_count = static_cast<uint32_t>(transient_buffer_slots.size());
            info.transient_texture_slot_count = static_cast<uint32_t>(transient_texture_slots.size());
            info.barrier_count = barrier_count;
            info.barrier_point_count = barrier_point_count;
            info.queue_dependency_count = static_cast<uint32_t>(built_plan.queue_dependencies.size());
            plan = std::move(built_plan);
            return foundation::Result::success();
        } catch (const std::bad_alloc&) {
            return foundation::Result::failure(
                    foundation::ResultCode::kOutOfMemory,
                    "Render graph plan allocation failed");
        }
    }

    foundation::Result RenderGraph::Impl::release_physical_buffer(PhysicalBuffer& physical) {
        foundation::Result first_error = foundation::Result::success();
        for (const BufferViewCacheEntry& view: physical.views) {
            if (!view.handle.is_valid()) {
                continue;
            }
            const foundation::Result result = device.destroy_buffer_view(view.handle);
            if (!result && result.code != foundation::ResultCode::kNotFound && first_error) {
                first_error = result;
            }
        }
        physical.views.clear();
        if (physical.owns_handle && physical.handle.is_valid()) {
            const foundation::Result result = device.destroy_buffer(physical.handle);
            if (!result && result.code != foundation::ResultCode::kNotFound && first_error) {
                first_error = result;
            }
        }
        physical = {};
        return first_error;
    }

    foundation::Result RenderGraph::Impl::release_physical_texture(PhysicalTexture& physical) {
        foundation::Result first_error = foundation::Result::success();
        for (const TextureViewCacheEntry& view: physical.views) {
            if (!view.owns_handle || !view.handle.is_valid()) {
                continue;
            }
            const foundation::Result result = device.destroy_texture_view(view.handle);
            if (!result && result.code != foundation::ResultCode::kNotFound && first_error) {
                first_error = result;
            }
        }
        physical.views.clear();
        if (physical.owns_handle && physical.handle.is_valid()) {
            const foundation::Result result = device.destroy_texture(physical.handle);
            if (!result && result.code != foundation::ResultCode::kNotFound && first_error) {
                first_error = result;
            }
        }
        physical = {};
        return first_error;
    }

    foundation::Result RenderGraph::Impl::ensure_transient_pool(uint32_t frame_count) {
        if (frame_count == 0) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                        "Render graph requires at least one frame slot");
        }
        try {
            if (transient_pools.size() > frame_count) {
                for (size_t frame_slot = frame_count; frame_slot < transient_pools.size(); ++frame_slot) {
                    for (PhysicalBuffer& buffer: transient_pools[frame_slot].buffers) {
                        (void) release_physical_buffer(buffer);
                    }
                    for (PhysicalTexture& texture: transient_pools[frame_slot].textures) {
                        (void) release_physical_texture(texture);
                    }
                }
                transient_pools.resize(frame_count);
            } else {
                transient_pools.resize(frame_count);
            }
            for (TransientPool& pool: transient_pools) {
                if (pool.buffers.size() > transient_buffer_slots.size()) {
                    for (size_t index = transient_buffer_slots.size(); index < pool.buffers.size(); ++index) {
                        (void) release_physical_buffer(pool.buffers[index]);
                    }
                }
                if (pool.textures.size() > transient_texture_slots.size()) {
                    for (size_t index = transient_texture_slots.size(); index < pool.textures.size(); ++index) {
                        (void) release_physical_texture(pool.textures[index]);
                    }
                }
                pool.buffers.resize(transient_buffer_slots.size());
                pool.textures.resize(transient_texture_slots.size());
            }
        } catch (const std::bad_alloc&) {
            return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                        "Render graph transient pool allocation failed");
        }
        return foundation::Result::success();
    }

    foundation::Result RenderGraph::Impl::ensure_transient_buffer(uint32_t frame_slot, uint32_t slot_index) {
        if (frame_slot >= transient_pools.size() || slot_index >= transient_buffer_slots.size()) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                        "Render graph transient buffer slot is out of bounds");
        }
        PhysicalBuffer& physical = transient_pools[frame_slot].buffers[slot_index];
        const TransientBufferSlotPlan& plan = transient_buffer_slots[slot_index];
        if (physical.handle.is_valid() && buffer_desc_equal(physical.desc, plan.desc)) {
            return foundation::Result::success();
        }
        const foundation::Result release_result = release_physical_buffer(physical);
        if (!release_result) {
            return release_result;
        }
        const rhi::BufferDesc desc = plan.desc.materialize();
        auto create_result = device.create_buffer(desc);
        if (!create_result) {
            return create_result.result;
        }
        try {
            physical.handle = create_result.value;
            physical.desc = plan.desc;
            physical.state = rhi::ResourceState{desc.initial_usage, rhi::ShaderStage::kAll,
                                                desc.initial_queue, false, desc.initial_stages};
            physical.states.clear();
            initialize_buffer_state_segments(physical.states, desc.size, physical.state);
            physical.owns_handle = true;
        } catch (const std::bad_alloc&) {
            (void) device.destroy_buffer(create_result.value);
            physical = {};
            return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                        "Render graph transient buffer bookkeeping failed");
        }
        return foundation::Result::success();
    }

    foundation::Result RenderGraph::Impl::ensure_transient_texture(uint32_t frame_slot, uint32_t slot_index) {
        if (frame_slot >= transient_pools.size() || slot_index >= transient_texture_slots.size()) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                        "Render graph transient texture slot is out of bounds");
        }
        PhysicalTexture& physical = transient_pools[frame_slot].textures[slot_index];
        const TransientTextureSlotPlan& plan = transient_texture_slots[slot_index];
        if (physical.handle.is_valid() && texture_desc_equal(physical.desc, plan.desc)) {
            return foundation::Result::success();
        }
        const foundation::Result release_result = release_physical_texture(physical);
        if (!release_result) {
            return release_result;
        }
        const rhi::TextureDesc desc = plan.desc.materialize();
        auto create_result = device.create_texture(desc);
        if (!create_result) {
            return create_result.result;
        }
        try {
            physical.handle = create_result.value;
            physical.desc = plan.desc;
            physical.states.assign(static_cast<size_t>(kAspectPlaneCount) * desc.mip_levels * desc.array_layers,
                                   rhi::ResourceState{desc.initial_usage, rhi::ShaderStage::kAll,
                                                      desc.initial_queue, false, desc.initial_stages});
            physical.owns_handle = true;
        } catch (const std::bad_alloc&) {
            (void) device.destroy_texture(create_result.value);
            physical = {};
            return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                        "Render graph transient texture bookkeeping failed");
        }
        return foundation::Result::success();
    }

    foundation::Result RenderGraph::Impl::ensure_persistent_buffer(BufferRecord& record) {
        if (record.physical.handle.is_valid()) {
            if (buffer_desc_equal(record.physical.desc, record.desc)) {
                return foundation::Result::success();
            }
            const foundation::Result release_result = release_physical_buffer(record.physical);
            if (!release_result) {
                return release_result;
            }
        }
        const rhi::BufferDesc desc = record.desc.materialize();
        auto create_result = device.create_buffer(desc);
        if (!create_result) {
            return create_result.result;
        }
        try {
            record.physical.handle = create_result.value;
            record.physical.desc = record.desc;
            record.physical.state = rhi::ResourceState{desc.initial_usage, rhi::ShaderStage::kAll,
                                                       desc.initial_queue, false, desc.initial_stages};
            record.physical.states.clear();
            initialize_buffer_state_segments(record.physical.states, desc.size, record.physical.state);
            record.physical.owns_handle = true;
        } catch (const std::bad_alloc&) {
            (void) device.destroy_buffer(create_result.value);
            record.physical = {};
            return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                        "Render graph persistent buffer bookkeeping failed");
        }
        return foundation::Result::success();
    }

    foundation::Result RenderGraph::Impl::ensure_persistent_texture(TextureRecord& record) {
        if (record.physical.handle.is_valid()) {
            if (texture_desc_equal(record.physical.desc, record.desc)) {
                return foundation::Result::success();
            }
            const foundation::Result release_result = release_physical_texture(record.physical);
            if (!release_result) {
                return release_result;
            }
        }
        const rhi::TextureDesc desc = record.desc.materialize();
        auto create_result = device.create_texture(desc);
        if (!create_result) {
            return create_result.result;
        }
        try {
            record.physical.handle = create_result.value;
            record.physical.desc = record.desc;
            record.physical.states.assign(static_cast<size_t>(kAspectPlaneCount) * desc.mip_levels * desc.array_layers,
                                          rhi::ResourceState{desc.initial_usage, rhi::ShaderStage::kAll,
                                                             desc.initial_queue, false, desc.initial_stages});
            record.physical.owns_handle = true;
        } catch (const std::bad_alloc&) {
            (void) device.destroy_texture(create_result.value);
            record.physical = {};
            return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                        "Render graph persistent texture bookkeeping failed");
        }
        return foundation::Result::success();
    }

    foundation::Result RenderGraph::Impl::release_all() {
        foundation::Result first_error = foundation::Result::success();
        for (TransientPool& pool: transient_pools) {
            for (PhysicalBuffer& buffer: pool.buffers) {
                const foundation::Result result = release_physical_buffer(buffer);
                if (!result && first_error) {
                    first_error = result;
                }
            }
            for (PhysicalTexture& texture: pool.textures) {
                const foundation::Result result = release_physical_texture(texture);
                if (!result && first_error) {
                    first_error = result;
                }
            }
        }
        for (BufferRecord& record: buffers) {
            if (record.lifetime != ResourceLifetime::kTransient) {
                const foundation::Result result = release_physical_buffer(record.physical);
                if (!result && first_error) {
                    first_error = result;
                }
            }
        }
        for (TextureRecord& record: textures) {
            if (record.lifetime != ResourceLifetime::kTransient) {
                const foundation::Result result = release_physical_texture(record.physical);
                if (!result && first_error) {
                    first_error = result;
                }
            }
        }
        return first_error;
    }

    PhysicalBuffer* RenderGraph::Impl::physical_buffer(BufferRecord& record, uint32_t frame_slot) noexcept {
        if (record.lifetime == ResourceLifetime::kTransient) {
            if (frame_slot >= transient_pools.size() || record.physical_slot >= transient_buffer_slots.size() ||
                record.physical_slot >= transient_pools[frame_slot].buffers.size()) {
                return nullptr;
            }
            return &transient_pools[frame_slot].buffers[record.physical_slot];
        }
        return &record.physical;
    }

    const PhysicalBuffer* RenderGraph::Impl::physical_buffer(const BufferRecord& record,
                                                              uint32_t frame_slot) const noexcept {
        if (record.lifetime == ResourceLifetime::kTransient) {
            if (frame_slot >= transient_pools.size() || record.physical_slot >= transient_buffer_slots.size() ||
                record.physical_slot >= transient_pools[frame_slot].buffers.size()) {
                return nullptr;
            }
            return &transient_pools[frame_slot].buffers[record.physical_slot];
        }
        return &record.physical;
    }

    PhysicalTexture* RenderGraph::Impl::physical_texture(TextureRecord& record, uint32_t frame_slot) noexcept {
        if (record.lifetime == ResourceLifetime::kTransient) {
            if (frame_slot >= transient_pools.size() || record.physical_slot >= transient_texture_slots.size() ||
                record.physical_slot >= transient_pools[frame_slot].textures.size()) {
                return nullptr;
            }
            return &transient_pools[frame_slot].textures[record.physical_slot];
        }
        return &record.physical;
    }

    const PhysicalTexture* RenderGraph::Impl::physical_texture(const TextureRecord& record,
                                                                uint32_t frame_slot) const noexcept {
        if (record.lifetime == ResourceLifetime::kTransient) {
            if (frame_slot >= transient_pools.size() || record.physical_slot >= transient_texture_slots.size() ||
                record.physical_slot >= transient_pools[frame_slot].textures.size()) {
                return nullptr;
            }
            return &transient_pools[frame_slot].textures[record.physical_slot];
        }
        return &record.physical;
    }

    foundation::ResultValue<rhi::BufferHandle> RenderGraph::Impl::resolve_buffer(RenderGraphBuffer handle,
                                                                            uint32_t frame_slot) const {
        const BufferRecord* record = get_buffer(handle);
        if (record == nullptr) {
            return {foundation::Result::failure(foundation::ResultCode::kNotFound,
                                         "Invalid render graph buffer handle"), {}};
        }
        const PhysicalBuffer* physical = physical_buffer(*record, frame_slot);
        if (physical == nullptr || !physical->handle.is_valid()) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                         "Render graph buffer has no physical allocation"), {}};
        }
        return {foundation::Result::success(), physical->handle};
    }

    foundation::ResultValue<rhi::TextureHandle> RenderGraph::Impl::resolve_texture(RenderGraphTexture handle,
                                                                             uint32_t frame_slot) const {
        const TextureRecord* record = get_texture(handle);
        if (record == nullptr) {
            return {foundation::Result::failure(foundation::ResultCode::kNotFound,
                                         "Invalid render graph texture handle"), {}};
        }
        const PhysicalTexture* physical = physical_texture(*record, frame_slot);
        if (physical == nullptr || !physical->handle.is_valid()) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                         "Render graph texture has no physical allocation"), {}};
        }
        return {foundation::Result::success(), physical->handle};
    }

    foundation::ResultValue<rhi::BufferViewHandle> RenderGraph::Impl::resolve_buffer_view(
            RenderGraphBuffer handle, uint32_t frame_slot, rhi::BufferViewKind kind, uint64_t offset, uint64_t size,
            rhi::Format format, uint32_t structure_stride) const {
        const BufferRecord* record = get_buffer(handle);
        if (record == nullptr) {
            return {foundation::Result::failure(foundation::ResultCode::kNotFound,
                                         "Invalid render graph buffer handle"), {}};
        }
        const PhysicalBuffer* physical = physical_buffer(*record, frame_slot);
        if (physical == nullptr || !physical->handle.is_valid()) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                         "Render graph buffer has no physical allocation"), {}};
        }
        rhi::BufferViewDesc desc{};
        desc.buffer = physical->handle;
        desc.kind = kind;
        desc.offset = offset;
        desc.size = size;
        desc.format = format;
        desc.structure_stride = structure_stride;
        for (const BufferViewCacheEntry& view: physical->views) {
            if (buffer_view_desc_equal(view.desc, desc)) {
                return {foundation::Result::success(), view.handle};
            }
        }
        return {foundation::Result::failure(foundation::ResultCode::kNotFound,
                                            "Render graph buffer view was not prepared before pass execution"), {}};
    }

    foundation::ResultValue<rhi::BufferViewHandle> RenderGraph::Impl::ensure_buffer_view(
            RenderGraphBuffer handle, uint32_t frame_slot, rhi::BufferViewKind kind, uint64_t offset, uint64_t size,
            rhi::Format format, uint32_t structure_stride) {
        const foundation::ResultValue<rhi::BufferViewHandle> resolved =
                resolve_buffer_view(handle, frame_slot, kind, offset, size, format, structure_stride);
        if (resolved || resolved.result.code != foundation::ResultCode::kNotFound) {
            return resolved;
        }

        BufferRecord* record = get_buffer(handle);
        if (record == nullptr) {
            return {foundation::Result::failure(foundation::ResultCode::kNotFound,
                                         "Invalid render graph buffer handle"), {}};
        }
        PhysicalBuffer* physical = physical_buffer(*record, frame_slot);
        if (physical == nullptr || !physical->handle.is_valid()) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                         "Render graph buffer has no physical allocation"), {}};
        }
        rhi::BufferViewDesc desc{};
        desc.buffer = physical->handle;
        desc.kind = kind;
        desc.offset = offset;
        desc.size = size;
        desc.format = format;
        desc.structure_stride = structure_stride;

        auto create_result = device.create_buffer_view(desc);
        if (!create_result) {
            return {create_result.result, {}};
        }
        try {
            physical->views.push_back(BufferViewCacheEntry{desc, create_result.value});
        } catch (const std::bad_alloc&) {
            (void) device.destroy_buffer_view(create_result.value);
            return {foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                         "Render graph buffer-view cache allocation failed"), {}};
        }
        return create_result;
    }

    foundation::ResultValue<rhi::TextureViewHandle> RenderGraph::Impl::resolve_texture_view(
            RenderGraphTexture handle, uint32_t frame_slot, rhi::TextureViewKind kind,
            rhi::SubresourceRange subresources, rhi::Format format, rhi::TextureViewDimension dimension) const {
        const TextureRecord* record = get_texture(handle);
        if (record == nullptr) {
            return {foundation::Result::failure(foundation::ResultCode::kNotFound,
                                         "Invalid render graph texture handle"), {}};
        }
        const foundation::Result range_result = normalize_texture_range(record->desc.desc, subresources);
        if (!range_result) {
            return {range_result, {}};
        }
        const PhysicalTexture* physical = physical_texture(*record, frame_slot);
        if (physical == nullptr || !physical->handle.is_valid()) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                         "Render graph texture has no physical allocation"), {}};
        }

        const rhi::Format view_format = format == rhi::Format::kUnknown ? record->desc.desc.format : format;
        const rhi::AspectMask default_range_aspect = default_aspect(record->desc.desc.format);
        const bool canonical_color_range = subresources.aspects == rhi::Aspect::kColor &&
                                           subresources.base_mip == 0 && subresources.mip_count == 1 &&
                                           subresources.base_layer == 0 && subresources.layer_count == 1;
        if (record->lifetime == ResourceLifetime::kImported && record->imported_default_view.is_valid() &&
            kind == rhi::TextureViewKind::kColorAttachment && canonical_color_range &&
            format == rhi::Format::kUnknown && dimension == rhi::TextureViewDimension::kAuto &&
            !is_depth_format(record->desc.desc.format)) {
            return {foundation::Result::success(), record->imported_default_view};
        }
        if (kind == rhi::TextureViewKind::kDepthStencil && subresources.aspects == rhi::Aspect::kColor) {
            subresources.aspects = default_range_aspect;
        }

        rhi::TextureViewDesc desc{};
        desc.texture = physical->handle;
        desc.kind = kind;
        desc.dimension = dimension;
        desc.format = view_format;
        desc.subresources = subresources;
        for (const TextureViewCacheEntry& view: physical->views) {
            if (view_desc_equal(view.desc, desc)) {
                return {foundation::Result::success(), view.handle};
            }
        }
        return {foundation::Result::failure(foundation::ResultCode::kNotFound,
                                            "Render graph texture view was not prepared before pass execution"), {}};
    }

    foundation::ResultValue<rhi::TextureViewHandle> RenderGraph::Impl::ensure_texture_view(
            RenderGraphTexture handle, uint32_t frame_slot, rhi::TextureViewKind kind,
            rhi::SubresourceRange subresources, rhi::Format format, rhi::TextureViewDimension dimension) {
        const foundation::ResultValue<rhi::TextureViewHandle> resolved =
                resolve_texture_view(handle, frame_slot, kind, subresources, format, dimension);
        if (resolved || resolved.result.code != foundation::ResultCode::kNotFound) {
            return resolved;
        }

        TextureRecord* record = get_texture(handle);
        if (record == nullptr) {
            return {foundation::Result::failure(foundation::ResultCode::kNotFound,
                                         "Invalid render graph texture handle"), {}};
        }
        const foundation::Result range_result = normalize_texture_range(record->desc.desc, subresources);
        if (!range_result) {
            return {range_result, {}};
        }
        PhysicalTexture* physical = physical_texture(*record, frame_slot);
        if (physical == nullptr || !physical->handle.is_valid()) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                         "Render graph texture has no physical allocation"), {}};
        }

        const rhi::Format view_format = format == rhi::Format::kUnknown ? record->desc.desc.format : format;
        if (kind == rhi::TextureViewKind::kDepthStencil && subresources.aspects == rhi::Aspect::kColor) {
            subresources.aspects = default_aspect(record->desc.desc.format);
        }

        rhi::TextureViewDesc desc{};
        desc.texture = physical->handle;
        desc.kind = kind;
        desc.dimension = dimension;
        desc.format = view_format;
        desc.subresources = subresources;

        auto create_result = device.create_texture_view(desc);
        if (!create_result) {
            return {create_result.result, {}};
        }
        try {
            physical->views.push_back(TextureViewCacheEntry{desc, create_result.value, true});
        } catch (const std::bad_alloc&) {
            (void) device.destroy_texture_view(create_result.value);
            return {foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                         "Render graph texture-view cache allocation failed"), {}};
        }
        return create_result;
    }

    foundation::Result RenderGraph::Impl::prepare_execution_views(const RenderGraphPlan& execution_plan,
                                                                    uint32_t frame_slot) {
        for (const RenderGraphPassPlan& pass_plan: execution_plan.pass_plans) {
            if (!pass_plan.live) {
                continue;
            }
            const PassRecord* pass = get_pass(pass_plan.pass);
            if (pass == nullptr) {
                return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                                   "Render graph execution plan references an invalid pass");
            }
            for (const ColorAttachment& attachment: pass->color_attachments) {
                const foundation::ResultValue<rhi::TextureViewHandle> view = ensure_texture_view(
                        attachment.texture, frame_slot, rhi::TextureViewKind::kColorAttachment,
                        attachment.subresources, rhi::Format::kUnknown, rhi::TextureViewDimension::kAuto);
                if (!view) {
                    return view.result;
                }
            }
            if (pass->depth_stencil.has_value()) {
                const DepthStencilAttachment& attachment = pass->depth_stencil.value();
                const foundation::ResultValue<rhi::TextureViewHandle> view = ensure_texture_view(
                        attachment.texture, frame_slot, rhi::TextureViewKind::kDepthStencil,
                        attachment.subresources, rhi::Format::kUnknown, rhi::TextureViewDimension::kAuto);
                if (!view) {
                    return view.result;
                }
            }
        }
        return foundation::Result::success();
    }

    foundation::Result RenderGraph::Impl::initialize_runtime_states(
            uint32_t frame_slot, std::vector<RuntimeBufferState>& buffer_states,
            std::vector<RuntimeTextureState>& texture_states) {
        try {
            buffer_states.resize(buffers.size());
            texture_states.resize(textures.size());
        } catch (const std::bad_alloc&) {
            return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                        "Render graph runtime-state allocation failed");
        }

        try {
            for (uint32_t index = 0; index < buffers.size(); ++index) {
                BufferRecord& record = buffers[index];
                PhysicalBuffer* physical = physical_buffer(record, frame_slot);
                if (record.has_use && (physical == nullptr || !physical->handle.is_valid())) {
                    return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                                "Render graph buffer was not physically allocated");
                }
                RuntimeBufferState& runtime = buffer_states[index];
                runtime = {};
                runtime.physical = physical;
                runtime.shared = &runtime;
                if (physical == nullptr) {
                    continue;
                }
                for (uint32_t previous = 0; previous < index; ++previous) {
                    if (buffer_states[previous].physical == physical && buffer_states[previous].shared != nullptr) {
                        runtime.shared = buffer_states[previous].shared;
                        break;
                    }
                }
                if (runtime.shared == &runtime) {
                    runtime.states = physical->states;
                    initialize_buffer_state_segments(runtime.states, physical->desc.desc.size, physical->state);
                }
            }
            for (uint32_t index = 0; index < textures.size(); ++index) {
                TextureRecord& record = textures[index];
                PhysicalTexture* physical = physical_texture(record, frame_slot);
                if (record.has_use &&
                    (physical == nullptr || !physical->handle.is_valid() || physical->states.empty())) {
                    return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                                "Render graph texture was not physically allocated");
                }
                RuntimeTextureState& runtime = texture_states[index];
                runtime = {};
                runtime.physical = physical;
                runtime.shared = &runtime;
                if (physical == nullptr) {
                    continue;
                }
                for (uint32_t previous = 0; previous < index; ++previous) {
                    if (texture_states[previous].physical == physical && texture_states[previous].shared != nullptr) {
                        runtime.shared = texture_states[previous].shared;
                        break;
                    }
                }
                if (runtime.shared == &runtime) {
                    runtime.states = physical->states;
                }
            }
        } catch (const std::bad_alloc&) {
            return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                        "Render graph runtime-state allocation failed");
        }
        return foundation::Result::success();
    }

    foundation::Result RenderGraph::Impl::record_barrier_batch(
            rhi::CommandListHandle command_list, uint32_t frame_slot,
            const RenderGraphPassPlan& pass_plan, RenderGraphBarrierPhase phase,
            std::vector<RuntimeBufferState>& buffer_states,
            std::vector<RuntimeTextureState>& texture_states) {
        (void) frame_slot;
        const PassRecord* pass = get_pass(pass_plan.pass);
        if (pass == nullptr || !pass_plan.live) {
            return foundation::Result::failure(
                    foundation::ResultCode::kInvalidState,
                    "Render graph barrier plan references an inactive pass");
        }

        std::vector<rhi::BufferBarrier> buffer_barriers;
        std::vector<rhi::TextureBarrier> texture_barriers;
        try {
            buffer_barriers.reserve(pass_plan.barriers.size());
            texture_barriers.reserve(pass_plan.barriers.size());

            const auto append_buffer_barrier = [&](const rhi::BufferBarrier& candidate) {
                for (rhi::BufferBarrier& existing: buffer_barriers) {
                    if (existing.buffer != candidate.buffer || existing.before != candidate.before ||
                        existing.after != candidate.after ||
                        existing.requires_memory_dependency != candidate.requires_memory_dependency) {
                        continue;
                    }
                    if (existing.offset == candidate.offset && existing.size == candidate.size) {
                        existing.after.discard_contents =
                                existing.after.discard_contents || candidate.after.discard_contents;
                        return;
                    }
                }
                buffer_barriers.push_back(candidate);
            };

            const auto append_texture_barrier = [&](const rhi::TextureBarrier& candidate) {
                for (rhi::TextureBarrier& existing: texture_barriers) {
                    if (existing.texture != candidate.texture || existing.before != candidate.before ||
                        existing.after != candidate.after ||
                        existing.requires_memory_dependency != candidate.requires_memory_dependency ||
                        existing.subresources.aspects != candidate.subresources.aspects) {
                        continue;
                    }
                    const bool same_mip = existing.subresources.base_mip == candidate.subresources.base_mip &&
                                          existing.subresources.mip_count == candidate.subresources.mip_count;
                    const bool adjacent_layers =
                            same_mip &&
                            (existing.subresources.base_layer + existing.subresources.layer_count ==
                             candidate.subresources.base_layer ||
                             candidate.subresources.base_layer + candidate.subresources.layer_count ==
                             existing.subresources.base_layer);
                    if (adjacent_layers) {
                        existing.subresources.base_layer =
                                std::min(existing.subresources.base_layer, candidate.subresources.base_layer);
                        const uint32_t end = std::max(
                                existing.subresources.base_layer + existing.subresources.layer_count,
                                candidate.subresources.base_layer + candidate.subresources.layer_count);
                        existing.subresources.layer_count = end - existing.subresources.base_layer;
                        existing.after.discard_contents =
                                existing.after.discard_contents || candidate.after.discard_contents;
                        return;
                    }
                    const bool same_layers = existing.subresources.base_layer == candidate.subresources.base_layer &&
                                             existing.subresources.layer_count == candidate.subresources.layer_count;
                    const bool adjacent_mips =
                            same_layers &&
                            (existing.subresources.base_mip + existing.subresources.mip_count ==
                             candidate.subresources.base_mip ||
                             candidate.subresources.base_mip + candidate.subresources.mip_count ==
                             existing.subresources.base_mip);
                    if (adjacent_mips) {
                        existing.subresources.base_mip =
                                std::min(existing.subresources.base_mip, candidate.subresources.base_mip);
                        const uint32_t end = std::max(
                                existing.subresources.base_mip + existing.subresources.mip_count,
                                candidate.subresources.base_mip + candidate.subresources.mip_count);
                        existing.subresources.mip_count = end - existing.subresources.base_mip;
                        existing.after.discard_contents =
                                existing.after.discard_contents || candidate.after.discard_contents;
                        return;
                    }
                }
                texture_barriers.push_back(candidate);
            };

            for (const RenderGraphBarrierPlan& barrier_plan: pass_plan.barriers) {
                if (barrier_plan.phase != phase) {
                    continue;
                }
                if (barrier_plan.resource_kind == RenderGraphResourceKind::kBuffer) {
                    if (barrier_plan.resource_index >= buffer_states.size()) {
                        return foundation::Result::failure(
                                foundation::ResultCode::kInvalidState,
                                "Render graph buffer barrier resource index is out of bounds");
                    }
                    if (barrier_plan.resource_index >= buffers.size() ||
                        buffers[barrier_plan.resource_index].generation != barrier_plan.resource_generation) {
                        return foundation::Result::failure(
                                foundation::ResultCode::kInvalidState,
                                "Render graph buffer barrier resource generation is stale");
                    }
                    RuntimeBufferState& state = buffer_states[barrier_plan.resource_index];
                    if (state.physical == nullptr || state.shared == nullptr ||
                        state.physical->handle.is_valid() == false) {
                        return foundation::Result::failure(
                                foundation::ResultCode::kInvalidState,
                                "Render graph buffer barrier has no physical resource");
                    }
                    uint64_t range_end = 0;
                    if (!resolve_buffer_range(state.physical->desc.desc.size, barrier_plan.buffer_range, range_end)) {
                        return foundation::Result::failure(
                                foundation::ResultCode::kInvalidArgument,
                                "Render graph buffer barrier range is out of bounds");
                    }
                    initialize_buffer_state_segments(
                            state.shared->states, state.physical->desc.desc.size, state.physical->state);
                    split_buffer_state_segment(state.shared->states, barrier_plan.buffer_range.offset);
                    split_buffer_state_segment(state.shared->states, range_end);
                    const rhi::ResourceState after = normalize_state(barrier_plan.after, pass->queue);
                    for (BufferStateSegment& segment: state.shared->states) {
                        const uint64_t segment_end = segment.offset + segment.size;
                        if (segment.offset < barrier_plan.buffer_range.offset || segment_end > range_end) {
                            continue;
                        }
                        const rhi::ResourceState before = segment.state;
                        const bool emit = !state_matches(before, after) || barrier_plan.discard_contents ||
                                          barrier_plan.requires_memory_dependency ||
                                          barrier_plan.requires_queue_transfer;
                        if (emit) {
                            rhi::ResourceState barrier_after = after;
                            barrier_after.discard_contents = barrier_plan.discard_contents;
                            append_buffer_barrier(rhi::BufferBarrier{
                                    state.physical->handle,
                                    segment.offset,
                                    segment.size,
                                    before,
                                    barrier_after,
                                    barrier_plan.requires_memory_dependency
                            });
                        }
                        segment.state = after;
                    }
                    merge_buffer_state_segments(state.shared->states);
                    continue;
                }

                if (barrier_plan.resource_index >= texture_states.size()) {
                    return foundation::Result::failure(
                            foundation::ResultCode::kInvalidState,
                            "Render graph texture barrier resource index is out of bounds");
                }
                if (barrier_plan.resource_index >= textures.size() ||
                    textures[barrier_plan.resource_index].generation != barrier_plan.resource_generation) {
                    return foundation::Result::failure(
                            foundation::ResultCode::kInvalidState,
                            "Render graph texture barrier resource generation is stale");
                }
                RuntimeTextureState& state = texture_states[barrier_plan.resource_index];
                if (state.physical == nullptr || state.shared == nullptr ||
                    state.physical->handle.is_valid() == false) {
                    return foundation::Result::failure(
                            foundation::ResultCode::kInvalidState,
                            "Render graph texture barrier has no physical resource");
                }
                const rhi::TextureDesc& desc = state.physical->desc.desc;
                const rhi::ResourceState after = normalize_state(barrier_plan.after, pass->queue);
                const rhi::Aspect aspects[] = {
                        rhi::Aspect::kColor,
                        rhi::Aspect::kDepth,
                        rhi::Aspect::kStencil
                };
                for (const rhi::Aspect aspect: aspects) {
                    if (!rhi::has_flag(barrier_plan.subresources.aspects, aspect)) {
                        continue;
                    }
                    for (uint32_t mip = barrier_plan.subresources.base_mip;
                         mip < barrier_plan.subresources.base_mip + barrier_plan.subresources.mip_count; ++mip) {
                        for (uint32_t layer = barrier_plan.subresources.base_layer;
                             layer < barrier_plan.subresources.base_layer + barrier_plan.subresources.layer_count;
                             ++layer) {
                            const uint32_t state_index = texture_state_index(aspect, desc, mip, layer);
                            if (state_index >= state.shared->states.size()) {
                                return foundation::Result::failure(
                                        foundation::ResultCode::kInvalidState,
                                        "Render graph texture barrier state index is out of bounds");
                            }
                            const rhi::ResourceState before = state.shared->states[state_index];
                            const bool emit = !state_matches(before, after) || barrier_plan.discard_contents ||
                                              barrier_plan.requires_memory_dependency ||
                                              barrier_plan.requires_queue_transfer;
                            if (emit) {
                                rhi::ResourceState barrier_after = after;
                                barrier_after.discard_contents = barrier_plan.discard_contents;
                                append_texture_barrier(rhi::TextureBarrier{
                                        state.physical->handle,
                                        rhi::SubresourceRange{aspect, mip, 1, layer, 1},
                                        before,
                                        barrier_after,
                                        barrier_plan.requires_memory_dependency
                                });
                            }
                            state.shared->states[state_index] = after;
                        }
                    }
                }
            }
        } catch (const std::bad_alloc&) {
            return foundation::Result::failure(
                    foundation::ResultCode::kOutOfMemory,
                    "Render graph barrier batch allocation failed");
        }

        if (buffer_barriers.empty() && texture_barriers.empty()) {
            return foundation::Result::success();
        }
        return device.barrier(command_list, rhi::BarrierDesc{buffer_barriers, texture_barriers});
    }

    void RenderGraph::Impl::commit_runtime_states(
            const std::vector<RuntimeBufferState>& buffer_states,
            const std::vector<RuntimeTextureState>& texture_states) {
        for (const RuntimeBufferState& runtime: buffer_states) {
            if (runtime.shared != &runtime || runtime.physical == nullptr) {
                continue;
            }
            runtime.physical->states = runtime.states;
            refresh_physical_buffer_state(*runtime.physical);
        }
        for (const RuntimeTextureState& runtime: texture_states) {
            if (runtime.shared != &runtime || runtime.physical == nullptr ||
                runtime.states.size() != runtime.physical->states.size()) {
                continue;
            }
            std::copy(runtime.states.begin(), runtime.states.end(), runtime.physical->states.begin());
        }
    }

    RenderPassCommandContext::RenderPassCommandContext(RenderGraph& graph, rhi::Device& device, RenderGraphPass pass,
                                                       rhi::CommandListHandle command_list, uint32_t pass_index,
                                                       uint32_t frame_slot) noexcept
        : graph_(graph), device_(device), pass_(pass), command_list_(command_list), pass_index_(pass_index),
          frame_slot_(frame_slot) {
    }

    foundation::ResultValue<rhi::BufferHandle> RenderPassCommandContext::resolve_buffer(
            RenderGraphBuffer buffer) const {
        return graph_.impl_->resolve_buffer(buffer, frame_slot_);
    }

    foundation::ResultValue<rhi::TextureHandle> RenderPassCommandContext::resolve_texture(
            RenderGraphTexture texture) const {
        return graph_.impl_->resolve_texture(texture, frame_slot_);
    }

    foundation::ResultValue<rhi::BufferViewHandle> RenderPassCommandContext::resolve_buffer_view(
            RenderGraphBuffer buffer, rhi::BufferViewKind kind, uint64_t offset, uint64_t size,
            rhi::Format format, uint32_t structure_stride) const {
        return graph_.impl_->resolve_buffer_view(buffer, frame_slot_, kind, offset, size, format, structure_stride);
    }

    foundation::ResultValue<rhi::TextureViewHandle> RenderPassCommandContext::resolve_texture_view(
            RenderGraphTexture texture, rhi::TextureViewKind kind, rhi::SubresourceRange subresources,
            rhi::Format format, rhi::TextureViewDimension dimension) const {
        return graph_.impl_->resolve_texture_view(texture, frame_slot_, kind, subresources, format, dimension);
    }

    foundation::Result RenderPassCommandContext::begin_rendering() {
        if (rendering_active_) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Render graph rendering scope is already active");
        }
        const PassRecord* pass_record = graph_.impl_->get_pass(pass_);
        if (pass_record == nullptr || pass_record->queue != rhi::QueueClass::kGraphics) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Pass execution context is not attached to a graphics pass");
        }
        if (pass_record->color_attachments.empty() && !pass_record->depth_stencil.has_value()) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                        "Pass has no graphics attachments");
        }

        std::vector<rhi::ColorAttachmentDesc> colors;
        std::optional<rhi::DepthStencilAttachmentDesc> depth;
        try {
            colors.reserve(pass_record->color_attachments.size());
            for (const ColorAttachment& attachment: pass_record->color_attachments) {
                auto view_result = graph_.impl_->resolve_texture_view(
                        attachment.texture, frame_slot_, rhi::TextureViewKind::kColorAttachment,
                        attachment.subresources, rhi::Format::kUnknown, rhi::TextureViewDimension::kAuto);
                if (!view_result) {
                    return view_result.result;
                }
                colors.push_back(rhi::ColorAttachmentDesc{view_result.value, attachment.load_op, attachment.store_op,
                                                          attachment.clear_color, attachment.final_usage});
            }
            if (pass_record->depth_stencil.has_value()) {
                const DepthStencilAttachment& attachment = pass_record->depth_stencil.value();
                auto view_result = graph_.impl_->resolve_texture_view(
                        attachment.texture, frame_slot_, rhi::TextureViewKind::kDepthStencil,
                        attachment.subresources, rhi::Format::kUnknown, rhi::TextureViewDimension::kAuto);
                if (!view_result) {
                    return view_result.result;
                }
                depth = rhi::DepthStencilAttachmentDesc{view_result.value, attachment.load_op, attachment.store_op,
                                                        attachment.clear_depth, attachment.clear_stencil,
                                                        attachment.final_usage};
            }
        } catch (const std::bad_alloc&) {
            return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                        "Render graph rendering attachment allocation failed");
        }

        rhi::RenderingDesc rendering_desc{};
        rendering_desc.color_attachments = colors;
        if (depth.has_value()) {
            rendering_desc.depth_stencil = depth.value();
        }
        rendering_desc.name = rhi::DebugName{pass_record->name};
        const foundation::Result result = device_.begin_rendering(command_list_, rendering_desc);
        if (result) {
            rendering_started_ = true;
            rendering_active_ = true;
        }
        return result;
    }

    foundation::Result RenderPassCommandContext::end_rendering() {
        if (!rendering_active_) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                        "Render graph rendering scope is not active");
        }
        const foundation::Result result = device_.end_rendering(command_list_);
        if (result) {
            rendering_active_ = false;
        }
        return result;
    }

    foundation::Result RenderPassCommandContext::set_viewport(const rhi::Viewport& viewport) {
        return device_.set_viewport(command_list_, viewport);
    }

    foundation::Result RenderPassCommandContext::set_scissor(const rhi::ScissorRect& scissor) {
        return device_.set_scissor(command_list_, scissor);
    }

    foundation::Result RenderPassCommandContext::bind_pipeline(rhi::PipelineHandle pipeline) {
        return device_.bind_pipeline(command_list_, pipeline);
    }

    foundation::Result RenderPassCommandContext::bind_binding_set(uint32_t group, rhi::BindingSetHandle set,
                                                                  std::span<const uint32_t> dynamic_offsets) {
        return device_.bind_binding_set(command_list_, group, set, dynamic_offsets);
    }

    foundation::Result RenderPassCommandContext::set_inline_constants(uint32_t offset,
                                                                       rhi::ShaderStageMask shader_stages,
                                                                       std::span<const std::byte> data) {
        return device_.set_inline_constants(command_list_, offset, shader_stages, data);
    }

    foundation::Result RenderPassCommandContext::bind_vertex_buffer(uint32_t binding, rhi::BufferHandle buffer,
                                                                    uint64_t offset) {
        return device_.bind_vertex_buffer(command_list_, binding, buffer, offset);
    }

    foundation::Result RenderPassCommandContext::bind_index_buffer(rhi::BufferHandle buffer, uint64_t offset,
                                                                    rhi::IndexFormat format) {
        return device_.bind_index_buffer(command_list_, buffer, offset, format);
    }

    foundation::Result RenderPassCommandContext::draw(const rhi::DrawDesc& desc) {
        return device_.draw(command_list_, desc);
    }

    foundation::Result RenderPassCommandContext::draw_indexed(const rhi::DrawIndexedDesc& desc) {
        return device_.draw_indexed(command_list_, desc);
    }

    foundation::Result RenderPassCommandContext::dispatch(const rhi::DispatchDesc& desc) {
        return device_.dispatch(command_list_, desc);
    }

    foundation::Result RenderPassCommandContext::dispatch_indirect(const rhi::DispatchIndirectDesc& desc) {
        return device_.dispatch_indirect(command_list_, desc);
    }

    foundation::Result RenderPassCommandContext::copy_buffer(RenderGraphBuffer source, RenderGraphBuffer destination,
                                                             uint64_t source_offset, uint64_t destination_offset,
                                                             uint64_t size) {
        auto source_result = resolve_buffer(source);
        if (!source_result) {
            return source_result.result;
        }
        auto destination_result = resolve_buffer(destination);
        if (!destination_result) {
            return destination_result.result;
        }
        return device_.copy_buffer(command_list_, rhi::BufferCopyDesc{source_result.value, destination_result.value,
                                                                       source_offset, destination_offset, size});
    }

    foundation::Result RenderPassCommandContext::copy_buffer_to_texture(
            RenderGraphBuffer source, uint64_t source_offset, uint32_t source_row_length,
            uint32_t source_image_height, RenderGraphTexture destination,
            rhi::TextureSubresourceLayers destination_subresources, rhi::Offset3D destination_offset,
            rhi::Extent3D extent) {
        auto source_result = resolve_buffer(source);
        if (!source_result) {
            return source_result.result;
        }
        auto destination_result = resolve_texture(destination);
        if (!destination_result) {
            return destination_result.result;
        }
        return device_.copy_buffer_to_texture(
                command_list_, rhi::BufferTextureCopyDesc{source_result.value, source_offset, source_row_length,
                                                          source_image_height, destination_result.value,
                                                          destination_subresources, destination_offset, extent});
    }

    foundation::Result RenderPassCommandContext::copy_texture_to_buffer(
            RenderGraphTexture source, rhi::TextureSubresourceLayers source_subresources,
            rhi::Offset3D source_offset, rhi::Extent3D extent, RenderGraphBuffer destination,
            uint64_t destination_offset, uint32_t destination_row_length, uint32_t destination_image_height) {
        auto source_result = resolve_texture(source);
        if (!source_result) {
            return source_result.result;
        }
        auto destination_result = resolve_buffer(destination);
        if (!destination_result) {
            return destination_result.result;
        }
        return device_.copy_texture_to_buffer(
                command_list_, rhi::TextureBufferCopyDesc{source_result.value, source_subresources, source_offset,
                                                          extent, destination_result.value, destination_offset,
                                                          destination_row_length, destination_image_height});
    }

    foundation::Result RenderPassCommandContext::copy_texture(
            RenderGraphTexture source, rhi::TextureSubresourceLayers source_subresources,
            rhi::Offset3D source_offset, RenderGraphTexture destination,
            rhi::TextureSubresourceLayers destination_subresources, rhi::Offset3D destination_offset,
            rhi::Extent3D extent) {
        auto source_result = resolve_texture(source);
        if (!source_result) {
            return source_result.result;
        }
        auto destination_result = resolve_texture(destination);
        if (!destination_result) {
            return destination_result.result;
        }
        return device_.copy_texture(
                command_list_, rhi::TextureCopyDesc{source_result.value, source_subresources, source_offset,
                                                    destination_result.value, destination_subresources,
                                                    destination_offset, extent});
    }

    foundation::Result RenderPassCommandContext::clear_texture(RenderGraphTexture texture,
                                                               rhi::SubresourceRange subresources,
                                                               rhi::ClearColorValue color, float depth,
                                                               uint32_t stencil) {
        auto texture_result = resolve_texture(texture);
        if (!texture_result) {
            return texture_result.result;
        }
        return device_.clear_texture(command_list_,
                                     rhi::ClearTextureDesc{texture_result.value, subresources, color, depth, stencil});
    }

    foundation::ResultValue<RenderGraphExecutionResult> RenderGraph::execute(
            FrameContext& frame_context, const RenderGraphExecuteDesc& desc) {
        if (impl_ == nullptr) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                         "Render graph implementation is not initialized"), {}};
        }
        if (!frame_context.is_frame_active()) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidState,
                                         "Render graph execution requires an active FrameContext frame"), {}};
        }
        if (&frame_context.device() != &device_) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                         "Render graph and FrameContext must use the same RHI device"), {}};
        }

        const auto fail_execution = [&frame_context](foundation::Result result)
                -> foundation::ResultValue<RenderGraphExecutionResult> {
            (void) frame_context.abort_frame();
            return {std::move(result), {}};
        };

        const foundation::Result compile_result = compile();
        if (!compile_result) {
            return fail_execution(compile_result);
        }

        for (const RenderGraphExecutionLease& lease: desc.execution_leases) {
            if (!lease.is_valid()) {
                return fail_execution(foundation::Result::failure(
                        foundation::ResultCode::kInvalidArgument,
                        "Render graph execution lease is empty"));
            }
        }
        for (const RenderGraphExecutionLease& lease: desc.execution_leases) {
            if (const foundation::Result result = frame_context.retain_until_complete(lease.owner());
                !result) {
                return fail_execution(result);
            }
        }
        for (const std::shared_ptr<const void>& owner: impl_->execution_owners) {
            if (const foundation::Result result = frame_context.retain_until_complete(owner); !result) {
                return fail_execution(result);
            }
        }
        impl_->execution_owners.clear();

        const uint32_t frame_slot = frame_context.frame_slot_index();
        const foundation::Result pool_result = impl_->ensure_transient_pool(frame_context.frames_in_flight());
        if (!pool_result) {
            return fail_execution(pool_result);
        }

        try {
            for (uint32_t slot_index = 0; slot_index < impl_->transient_buffer_slots.size(); ++slot_index) {
                const foundation::Result result = impl_->ensure_transient_buffer(frame_slot, slot_index);
                if (!result) {
                    return fail_execution(result);
                }
            }
            for (uint32_t slot_index = 0; slot_index < impl_->transient_texture_slots.size(); ++slot_index) {
                const foundation::Result result = impl_->ensure_transient_texture(frame_slot, slot_index);
                if (!result) {
                    return fail_execution(result);
                }
            }
            for (BufferRecord& record: impl_->buffers) {
                if (record.lifetime == ResourceLifetime::kPersistent && record.has_use) {
                    const foundation::Result result = impl_->ensure_persistent_buffer(record);
                    if (!result) {
                        return fail_execution(result);
                    }
                }
            }
            for (TextureRecord& record: impl_->textures) {
                if (record.lifetime == ResourceLifetime::kPersistent && record.has_use) {
                    const foundation::Result result = impl_->ensure_persistent_texture(record);
                    if (!result) {
                        return fail_execution(result);
                    }
                }
                if (record.lifetime == ResourceLifetime::kImported && record.has_use &&
                    !record.physical.handle.is_valid()) {
                    return fail_execution(foundation::Result::failure(
                            foundation::ResultCode::kInvalidState,
                        "Render graph imported texture has no current physical handle"));
                }
            }

            const foundation::Result view_result = impl_->prepare_execution_views(
                    impl_->plan, frame_slot);
            if (!view_result) {
                return fail_execution(view_result);
            }

            const rhi::FrameAcquireResult* acquired = frame_context.acquired_frame();
            const bool signal_acquired_frame = desc.present_acquired_frame || desc.signal_present;
            uint32_t acquire_batch = kInvalidHandleIndex;
            uint32_t swapchain_texture_index = kInvalidHandleIndex;
            uint32_t final_swapchain_batch = kInvalidHandleIndex;
            for (uint32_t batch_index = 0; batch_index < impl_->batches.size(); ++batch_index) {
                if (impl_->batches[batch_index].waits_for_acquire) {
                    if (acquire_batch != kInvalidHandleIndex) {
                        return fail_execution(foundation::Result::failure(
                                foundation::ResultCode::kInvalidState,
                                "Render graph contains multiple acquire synchronization batches"));
                    }
                    acquire_batch = batch_index;
                    if (impl_->batches[batch_index].queue != rhi::QueueClass::kGraphics) {
                        return fail_execution(foundation::Result::failure(
                                foundation::ResultCode::kInvalidArgument,
                        "Swapchain acquire synchronization must start on the graphics queue"));
                    }
                }
                for (const uint32_t pass_index: impl_->batches[batch_index].passes) {
                    for (const ResourceUse& use: impl_->passes[pass_index].uses) {
                        if (use.kind != ResourceKind::kTexture || use.resource_index >= impl_->textures.size() ||
                            !impl_->textures[use.resource_index].is_swapchain_image ||
                            impl_->textures[use.resource_index].generation != use.resource_generation) {
                            continue;
                        }
                        if (swapchain_texture_index == kInvalidHandleIndex) {
                            swapchain_texture_index = use.resource_index;
                        } else if (swapchain_texture_index != use.resource_index) {
                            return fail_execution(foundation::Result::failure(
                                    foundation::ResultCode::kInvalidState,
                                    "Render graph contains multiple imported swapchain images"));
                        }
                        final_swapchain_batch = batch_index;
                    }
                }
            }
            if (signal_acquired_frame && acquired == nullptr) {
                return fail_execution(foundation::Result::failure(
                        foundation::ResultCode::kInvalidState,
                        "Present was requested without an acquired swapchain frame"));
            }
            if (signal_acquired_frame && final_swapchain_batch == kInvalidHandleIndex) {
                return fail_execution(foundation::Result::failure(
                        foundation::ResultCode::kInvalidArgument,
                        "Present was requested but the graph does not access a swapchain image"));
            }
            if (acquire_batch != kInvalidHandleIndex && acquired == nullptr) {
                return fail_execution(foundation::Result::failure(
                        foundation::ResultCode::kInvalidState,
                        "Render graph accesses a swapchain image without an acquired frame"));
            }
            if (acquired != nullptr && final_swapchain_batch != kInvalidHandleIndex && acquire_batch == kInvalidHandleIndex) {
                return fail_execution(foundation::Result::failure(
                        foundation::ResultCode::kInvalidState,
                        "Render graph present output has no acquire synchronization batch"));
            }
            if (signal_acquired_frame && final_swapchain_batch != kInvalidHandleIndex &&
                impl_->batches[final_swapchain_batch].queue != rhi::QueueClass::kGraphics) {
                return fail_execution(foundation::Result::failure(
                        foundation::ResultCode::kInvalidArgument,
                        "The final swapchain output batch must run on the graphics queue"));
            }

            std::vector<RuntimeBufferState> buffer_states;
            std::vector<RuntimeTextureState> texture_states;
            const foundation::Result state_result = impl_->initialize_runtime_states(frame_slot, buffer_states,
                                                                                texture_states);
            if (!state_result) {
                return fail_execution(state_result);
            }

            RenderGraphExecutionResult execution_result{};
            execution_result.executed_pass_count = static_cast<uint32_t>(impl_->topological_order.size());
            std::vector<rhi::GpuWait> persistent_waits;
            const RenderGraphPlan& execution_plan = impl_->plan;
            if (execution_plan.revision != impl_->revision ||
                execution_plan.batches.size() != impl_->batches.size() ||
                execution_plan.pass_plans.size() != impl_->passes.size()) {
                return fail_execution(foundation::Result::failure(
                        foundation::ResultCode::kInvalidState,
                        "Render graph execution plan is stale or incomplete"));
            }

            const auto append_wait = [](std::vector<rhi::GpuWait>& waits, rhi::GpuSyncPoint point,
                                        rhi::PipelineStageMask stages) {
                if (!point.is_valid()) {
                    return;
                }
                for (rhi::GpuWait& wait: waits) {
                    if (wait.point.timeline == point.timeline && wait.point.value == point.value) {
                        wait.destination_stages |= stages;
                        return;
                    }
                }
                waits.push_back(rhi::GpuWait{point, stages});
            };

            const auto texture_requires_queue_wait = [](const PhysicalTexture& physical,
                                                         const rhi::SubresourceRange& range,
                                                         rhi::QueueClass queue) noexcept {
                const rhi::Aspect aspects[] = {rhi::Aspect::kColor, rhi::Aspect::kDepth, rhi::Aspect::kStencil};
                for (const rhi::Aspect aspect: aspects) {
                    if (!rhi::has_flag(range.aspects, aspect)) {
                        continue;
                    }
                    for (uint32_t mip = range.base_mip; mip < range.base_mip + range.mip_count; ++mip) {
                        for (uint32_t layer = range.base_layer;
                             layer < range.base_layer + range.layer_count; ++layer) {
                            const uint32_t state_index = texture_state_index(aspect, physical.desc.desc, mip, layer);
                            if (state_index >= physical.states.size() || physical.states[state_index].queue != queue) {
                                return true;
                            }
                        }
                    }
                }
                return false;
            };

            const auto buffer_requires_queue_wait = [](const PhysicalBuffer& physical,
                                                        RenderGraphBufferRange range,
                                                        rhi::QueueClass queue) noexcept {
                uint64_t range_end = 0;
                if (!resolve_buffer_range(physical.desc.desc.size, range, range_end)) {
                    return true;
                }
                if (physical.states.empty()) {
                    return physical.state.queue != queue;
                }
                for (const BufferStateSegment& segment: physical.states) {
                    const uint64_t segment_end = segment.offset + segment.size;
                    if (segment_end <= range.offset || segment.offset >= range_end) {
                        continue;
                    }
                    if (segment.state.queue != queue) {
                        return true;
                    }
                }
                return false;
            };

            for (uint32_t batch_index = 0; batch_index < execution_plan.batches.size(); ++batch_index) {
                const RenderGraphBatchPlan& batch_plan = execution_plan.batches[batch_index];
                if (batch_plan.index != batch_index || batch_index >= impl_->batches.size()) {
                    return fail_execution(foundation::Result::failure(
                            foundation::ResultCode::kInvalidState,
                            "Render graph execution plan contains an invalid batch index"));
                }
                Batch& batch = impl_->batches[batch_index];
                auto command_result = frame_context.begin_command_list(
                        batch_plan.queue, batch_index, rhi::BarrierMode::kExplicit);
                if (!command_result) {
                    return fail_execution(command_result.result);
                }
                const rhi::CommandListHandle command_list = command_result.value;

                persistent_waits.clear();
                for (const RenderGraphQueueDependency& dependency: batch_plan.dependencies) {
                    const uint32_t source_batch_index = dependency.source_batch;
                    if (source_batch_index >= impl_->batches.size() ||
                        !impl_->batches[source_batch_index].completion.is_valid() ||
                        dependency.destination_batch != batch_index ||
                        dependency.destination_queue != batch_plan.queue) {
                        return fail_execution(foundation::Result::failure(
                                foundation::ResultCode::kInvalidState,
                                "Render graph batch dependency has no submitted completion point"));
                    }
                    append_wait(persistent_waits, impl_->batches[source_batch_index].completion,
                                dependency.destination_stages);
                }

                for (const uint32_t pass_index: batch_plan.pass_indices) {
                    if (pass_index >= execution_plan.pass_plans.size()) {
                        return fail_execution(foundation::Result::failure(
                                foundation::ResultCode::kInvalidState,
                                "Render graph batch contains an invalid pass index"));
                    }
                    PassRecord& pass = impl_->passes[pass_index];
                    for (const ResourceUse& use: pass.uses) {
                        if (use.kind == ResourceKind::kBuffer) {
                            BufferRecord& record = impl_->buffers[use.resource_index];
                            if (record.lifetime != ResourceLifetime::kTransient &&
                                !record.last_completions.empty()) {
                                const PhysicalBuffer* physical = impl_->physical_buffer(record, frame_slot);
                                if (physical != nullptr &&
                                    buffer_requires_queue_wait(*physical, use.buffer_range, batch_plan.queue)) {
                                    for (const rhi::GpuSyncPoint& completion: record.last_completions.points()) {
                                        append_wait(persistent_waits, completion, use.before_state.pipeline_stages);
                                    }
                                }
                            }
                        } else {
                            TextureRecord& record = impl_->textures[use.resource_index];
                            if (record.lifetime != ResourceLifetime::kTransient &&
                                !record.last_completions.empty()) {
                                const PhysicalTexture* physical = impl_->physical_texture(record, frame_slot);
                                if (physical != nullptr &&
                                    texture_requires_queue_wait(*physical, use.subresources, batch_plan.queue)) {
                                    for (const rhi::GpuSyncPoint& completion: record.last_completions.points()) {
                                        append_wait(persistent_waits, completion, use.before_state.pipeline_stages);
                                    }
                                }
                            }
                        }
                    }
                }

                for (const uint32_t pass_index: batch_plan.pass_indices) {
                    const RenderGraphPassPlan& pass_plan = execution_plan.pass_plans[pass_index];
                    if (!pass_plan.live || pass_plan.batch_index != batch_index) {
                        return fail_execution(foundation::Result::failure(
                                foundation::ResultCode::kInvalidState,
                                "Render graph pass plan does not match its batch"));
                    }
                    PassRecord& pass = impl_->passes[pass_index];
                    const foundation::Result before_barrier_result = impl_->record_barrier_batch(
                            command_list, frame_slot, pass_plan, RenderGraphBarrierPhase::kBeforePass,
                            buffer_states, texture_states);
                    if (!before_barrier_result) {
                        return fail_execution(before_barrier_result);
                    }

                    PassExecutionContext context(
                            *this, device_, RenderGraphPass{pass_index, pass.generation, impl_->graph_id},
                            command_list, pass_index, frame_slot);
                    try {
                        if (pass.execute) {
                            const foundation::Result execute_result = pass.execute(context);
                            if (!execute_result) {
                                return fail_execution(execute_result);
                            }
                        }
                    } catch (const std::bad_alloc&) {
                        return fail_execution(foundation::Result::failure(
                                foundation::ResultCode::kOutOfMemory,
                                "Render graph pass execution allocation failed"));
                    } catch (...) {
                        return fail_execution(foundation::Result::failure(
                                foundation::ResultCode::kBackendError,
                                "Render graph pass execution threw an exception"));
                    }

                    if (!pass.color_attachments.empty() || pass.depth_stencil.has_value()) {
                        if (!context.rendering_started() || context.rendering_active()) {
                            return fail_execution(foundation::Result::failure(
                                    foundation::ResultCode::kInvalidState,
                                    "Attachment pass must begin and end its rendering scope"));
                        }
                    } else if (context.rendering_active()) {
                        return fail_execution(foundation::Result::failure(
                                foundation::ResultCode::kInvalidState,
                                "Pass ended with an active rendering scope"));
                    }

                    const foundation::Result after_barrier_result = impl_->record_barrier_batch(
                            command_list, frame_slot, pass_plan, RenderGraphBarrierPhase::kAfterPass,
                            buffer_states, texture_states);
                    if (!after_barrier_result) {
                        return fail_execution(after_barrier_result);
                    }
                }

                const bool signal_present = signal_acquired_frame && batch_index == final_swapchain_batch;
                if (signal_present) {
                    if (swapchain_texture_index >= texture_states.size()) {
                        return fail_execution(foundation::Result::failure(
                                foundation::ResultCode::kInvalidState,
                                "Final swapchain output has no runtime texture state"));
                    }
                    RuntimeTextureState& swapchain_state = texture_states[swapchain_texture_index];
                    if (swapchain_state.physical == nullptr || swapchain_state.shared == nullptr ||
                        swapchain_state.shared->states.empty()) {
                        return fail_execution(foundation::Result::failure(
                                foundation::ResultCode::kInvalidState,
                                "Final swapchain output has no color subresource state"));
                    }
                    const rhi::ResourceState before = swapchain_state.shared->states.front();
                    const rhi::ResourceState after{
                            rhi::ResourceUsage::kPresent,
                            rhi::ShaderStage::kAll,
                            rhi::QueueClass::kGraphics,
                            false,
                            rhi::PipelineStage::kAllCommands
                    };
                    if (!state_matches(before, after)) {
                        const rhi::TextureBarrier present_barrier{
                                swapchain_state.physical->handle,
                                rhi::SubresourceRange{rhi::Aspect::kColor, 0, 1, 0, 1},
                                before,
                                after,
                                false
                        };
                        const std::array<rhi::TextureBarrier, 1> present_barriers{present_barrier};
                        const foundation::Result present_barrier_result =
                                device_.barrier(command_list, rhi::BarrierDesc{{}, present_barriers});
                        if (!present_barrier_result) {
                            return fail_execution(present_barrier_result);
                        }
                        swapchain_state.shared->states.front() = after;
                    }
                }

                const foundation::Result end_result = frame_context.end_command_list(command_list);
                if (!end_result) {
                    return fail_execution(end_result);
                }
                const std::array<rhi::CommandListHandle, 1> command_lists{command_list};
                foundation::ResultValue<rhi::SubmitResult> submit_result{};
                if (batch_plan.waits_for_acquire || signal_present) {
                    if (acquired == nullptr) {
                        return fail_execution(foundation::Result::failure(
                                foundation::ResultCode::kInvalidState,
                                "Swapchain batch has no acquired frame token"));
                    }
                    submit_result = frame_context.submit_frame(rhi::FrameSubmitDesc{
                            acquired->frame, batch_plan.queue, command_lists, persistent_waits,
                            batch_plan.waits_for_acquire, signal_present
                    });
                } else {
                    submit_result = frame_context.submit(rhi::SubmitDesc{
                            batch_plan.queue, command_lists, persistent_waits
                    });
                }
                if (!submit_result) {
                    return fail_execution(submit_result.result);
                }
                batch.completion = submit_result.value.completion;
                impl_->commit_runtime_states(buffer_states, texture_states);
                (void) execution_result.queue_completions.add(batch.completion);
                execution_result.completion = batch.completion;
                ++execution_result.submitted_batch_count;

                for (const uint32_t pass_index: batch_plan.pass_indices) {
                    for (const ResourceUse& use: impl_->passes[pass_index].uses) {
                        if (use.kind == ResourceKind::kBuffer) {
                            BufferRecord& record = impl_->buffers[use.resource_index];
                            if (record.lifetime != ResourceLifetime::kTransient) {
                                (void) record.last_completions.add(batch.completion);
                            }
                        } else {
                            TextureRecord& record = impl_->textures[use.resource_index];
                            if (record.lifetime != ResourceLifetime::kTransient) {
                                (void) record.last_completions.add(batch.completion);
                            }
                        }
                    }
                }
            }

            if (desc.present_acquired_frame) {
                auto present_result = frame_context.present();
                if (!present_result) {
                    return {present_result.result, {}};
                }
                execution_result.presented = true;
                execution_result.suboptimal = present_result.value.suboptimal;
            }
            return {foundation::Result::success(), execution_result};
        } catch (const std::bad_alloc&) {
            return fail_execution(foundation::Result::failure(
                    foundation::ResultCode::kOutOfMemory,
                    "Render graph execution allocation failed"));
        }
    }
} // namespace aeg::render_graph
