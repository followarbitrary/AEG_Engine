#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "rhi_config.hpp"

namespace aeg::rhi {
    template<class Tag>
    struct Handle {
        uint64_t value{0};

        [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0; }
        constexpr explicit operator bool() const noexcept { return is_valid(); }

        friend constexpr bool operator==(Handle, Handle) = default;
    };

    struct BufferTag;
    struct TextureTag;
    struct BufferViewTag;
    struct TextureViewTag;
    struct SamplerTag;
    struct BindingLayoutTag;
    struct BindingSetTag;
    struct PipelineTag;
    struct CommandListTag;
    struct TimelineTag;
    struct AccelerationStructureTag;
    struct SurfaceTag;
    struct SwapchainTag;

    using BufferHandle = Handle<BufferTag>;
    using TextureHandle = Handle<TextureTag>;
    using BufferViewHandle = Handle<BufferViewTag>;
    using TextureViewHandle = Handle<TextureViewTag>;
    using SamplerHandle = Handle<SamplerTag>;
    using BindingLayoutHandle = Handle<BindingLayoutTag>;
    using BindingSetHandle = Handle<BindingSetTag>;
    using PipelineHandle = Handle<PipelineTag>;
    using CommandListHandle = Handle<CommandListTag>;
    using TimelineHandle = Handle<TimelineTag>;
    using AccelerationStructureHandle = Handle<AccelerationStructureTag>;
    using SurfaceHandle = Handle<SurfaceTag>;
    using SwapchainHandle = Handle<SwapchainTag>;

    template<typename E>
    struct EnableBitmaskOperators : std::false_type {
    };

    template<typename E>
    concept BitmaskEnum = std::is_enum_v<E> && EnableBitmaskOperators<E>::value;

    template<BitmaskEnum E>
    constexpr E operator|(E lhs, E rhs) noexcept {
        using U = std::underlying_type_t<E>;
        return static_cast<E>(static_cast<U>(lhs) | static_cast<U>(rhs));
    }

    template<BitmaskEnum E>
    constexpr E operator&(E lhs, E rhs) noexcept {
        using U = std::underlying_type_t<E>;
        return static_cast<E>(static_cast<U>(lhs) & static_cast<U>(rhs));
    }

    template<BitmaskEnum E>
    constexpr E operator^(E lhs, E rhs) noexcept {
        using U = std::underlying_type_t<E>;
        return static_cast<E>(static_cast<U>(lhs) ^ static_cast<U>(rhs));
    }

    template<BitmaskEnum E>
    constexpr E operator~(E value) noexcept {
        using U = std::underlying_type_t<E>;
        return static_cast<E>(~static_cast<U>(value));
    }

    template<BitmaskEnum E>
    constexpr E &operator|=(E &lhs, E rhs) noexcept {
        lhs = lhs | rhs;
        return lhs;
    }

    template<BitmaskEnum E>
    constexpr E &operator&=(E &lhs, E rhs) noexcept {
        lhs = lhs & rhs;
        return lhs;
    }

    template<BitmaskEnum E>
    [[nodiscard]] constexpr bool has_flag(E value, E flag) noexcept {
        using U = std::underlying_type_t<E>;
        return (static_cast<U>(value) & static_cast<U>(flag)) == static_cast<U>(flag);
    }

    enum class ResultCode : uint8_t {
        kSuccess,
        kInvalidArgument,
        kInvalidState,
        kUnsupported,
        kNotFound,
        kOutOfMemory,
        kTimeout,
        kDeviceLost,
        kBackendError,
        kNotReady,
        kOutOfDate,
        kSurfaceLost,
        kUnknown
    };

    struct Result {
        ResultCode code{ResultCode::kSuccess};
        std::string message{};

        [[nodiscard]] static Result success() { return {}; }

        [[nodiscard]] static Result failure(ResultCode result_code, std::string result_message = {}) {
            return Result{result_code, std::move(result_message)};
        }

        [[nodiscard]] explicit operator bool() const noexcept { return code == ResultCode::kSuccess; }
    };

    template<typename T>
    struct ResultValue {
        Result result{};
        T value{};

        [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(result); }
    };

    struct DebugName {
        std::string_view value{};
    };

    struct Extent2D {
        uint32_t width{0};
        uint32_t height{0};

        friend constexpr bool operator==(Extent2D, Extent2D) = default;
    };

    struct Extent3D {
        uint32_t width{1};
        uint32_t height{1};
        uint32_t depth{1};
    };

    enum class SurfaceType : uint8_t { kWindow, kHeadless };

    struct SurfaceDesc {
        SurfaceType type{SurfaceType::kWindow};

        // Opaque native handles keep the public RHI independent from platform
        // window headers. Their interpretation belongs to the backend.
        void *native_display_handle{nullptr};
        void *native_window_handle{nullptr};
        DebugName name{};
    };

    enum class Format : uint16_t {
        kUnknown,
        kR8Unorm,
        kRg8Unorm,
        kRgba8Unorm,
        kBgra8Unorm,
        kRgba8Srgb,
        kRgba16Float,
        kRgba32Float,
        kR32Uint,
        kR32Float,
        kRg32Float,
        kD24UnormS8Uint,
        kD32Float,
        kD32FloatS8Uint,
        kBc1RgbaUnorm,
        kBc3RgbaUnorm,
        kBc5Unorm,
        kBc7RgbaUnorm
    };

    enum class SampleCount : uint8_t { kX1 = 1, kX2 = 2, kX4 = 4, kX8 = 8, kX16 = 16, kX32 = 32, kX64 = 64 };

    enum class TextureDimension : uint8_t { kTex1D, kTex2D, kTex3D };

    enum class MemoryClass : uint8_t { kAuto, kDeviceLocal, kUpload, kReadback };

    enum class ResourceFlags : uint32_t {
        kNone = 0,
        kAllowAlias = 1u << 0,
        kMutableFormat = 1u << 1,
        kCubeCompatible = 1u << 2,
        kExternal = 1u << 3
    };

    template<>
    struct EnableBitmaskOperators<ResourceFlags> : std::true_type {
    };

    enum class BufferCapability : uint64_t {
        kNone = 0,
        kTransferSource = 1ull << 0,
        kTransferDestination = 1ull << 1,
        kVertex = 1ull << 2,
        kIndex = 1ull << 3,
        kUniform = 1ull << 4,
        kStorage = 1ull << 5,
        kIndirect = 1ull << 6,
        kTypedView = 1ull << 7,
        kDeviceAddress = 1ull << 8,
        kAccelerationInput = 1ull << 9
    };

    template<>
    struct EnableBitmaskOperators<BufferCapability> : std::true_type {
    };

    enum class TextureCapability : uint64_t {
        kNone = 0,
        kTransferSource = 1ull << 0,
        kTransferDestination = 1ull << 1,
        kSampled = 1ull << 2,
        kStorage = 1ull << 3,
        kColorAttachment = 1ull << 4,
        kDepthStencil = 1ull << 5
    };

    template<>
    struct EnableBitmaskOperators<TextureCapability> : std::true_type {
    };

    enum class Aspect : uint8_t { kColor = 1u << 0, kDepth = 1u << 1, kStencil = 1u << 2 };

    template<>
    struct EnableBitmaskOperators<Aspect> : std::true_type {
    };

    using AspectMask = Aspect;

    enum class ResourceUsage : uint8_t {
        kUndefined,
        kCopySource,
        kCopyDestination,
        kVertexInput,
        kIndexInput,
        kIndirectArguments,
        kUniformRead,
        kSampledRead,
        kTypedRead,
        kStorageRead,
        kStorageWrite,
        kStorageReadWrite,
        kColorAttachmentRead,
        kColorAttachmentWrite,
        kDepthStencilRead,
        kDepthStencilWrite,
        kResolveSource,
        kResolveDestination,
        kPresent,
        kHostRead,
        kHostWrite,
        kAccelerationStructureRead,
        kAccelerationStructureBuildRead,
        kAccelerationStructureBuildWrite
    };

    enum class QueueClass : uint8_t { kAuto, kGraphics, kCompute, kCopy, kPresent };

    enum class ShaderStage : uint32_t {
        kNone = 0,
        kVertex = 1u << 0,
        kTessellationControl = 1u << 1,
        kTessellationEvaluation = 1u << 2,
        kGeometry = 1u << 3,
        kFragment = 1u << 4,
        kCompute = 1u << 5,
        kRayTracing = 1u << 6,
        kMesh = 1u << 7,
        kAll = 0xffffffffu
    };

    template<>
    struct EnableBitmaskOperators<ShaderStage> : std::true_type {
    };

    using ShaderStageMask = ShaderStage;

    struct ShaderStageDesc {
        ShaderStage stage{ShaderStage::kNone};
        // The shader system supplies a compiled binary compatible with the active device.
        std::span<const std::byte> code{};
        std::string_view entry_point{"main"};
        DebugName name{};
    };

    using ShaderModuleDesc = ShaderStageDesc;

    enum class PipelineType : uint8_t { kGraphics, kCompute };

    enum class PrimitiveTopology : uint8_t {
        kPointList,
        kLineList,
        kLineStrip,
        kTriangleList,
        kTriangleStrip,
        kTriangleFan,
        kLineListWithAdjacency,
        kLineStripWithAdjacency,
        kTriangleListWithAdjacency,
        kTriangleStripWithAdjacency,
        kPatchList
    };

    enum class VertexInputRate : uint8_t { kVertex, kInstance };

    enum class PolygonMode : uint8_t { kFill, kLine, kPoint };

    enum class CullMode : uint8_t { kNone, kFront, kBack, kFrontAndBack };

    enum class FrontFace : uint8_t { kCounterClockwise, kClockwise };

    enum class CompareOp : uint8_t {
        kNever, kLess, kEqual, kLessOrEqual, kGreater, kNotEqual, kGreaterOrEqual, kAlways
    };

    enum class BlendFactor : uint8_t {
        kZero,
        kOne,
        kSourceColor,
        kOneMinusSourceColor,
        kDestinationColor,
        kOneMinusDestinationColor,
        kSourceAlpha,
        kOneMinusSourceAlpha,
        kDestinationAlpha,
        kOneMinusDestinationAlpha,
        kConstantColor,
        kOneMinusConstantColor,
        kConstantAlpha,
        kOneMinusConstantAlpha,
        kSourceAlphaSaturate,
        kSourceOneColor,
        kOneMinusSourceOneColor,
        kSourceOneAlpha,
        kOneMinusSourceOneAlpha
    };

    enum class BlendOp : uint8_t { kAdd, kSubtract, kReverseSubtract, kMin, kMax };

    enum class ColorComponent : uint8_t {
        kNone = 0, kR = 1u << 0, kG = 1u << 1, kB = 1u << 2, kA = 1u << 3, kAll = 0x0fu
    };

    template<>
    struct EnableBitmaskOperators<ColorComponent> : std::true_type {
    };

    enum class PipelineStage : uint32_t {
        kNone = 0,
        kCopy = 1u << 0,
        kVertexInput = 1u << 1,
        kVertexShader = 1u << 2,
        kFragmentShader = 1u << 3,
        kEarlyDepth = 1u << 4,
        kLateDepth = 1u << 5,
        kColorOutput = 1u << 6,
        kCompute = 1u << 7,
        kRayTracing = 1u << 8,
        kHost = 1u << 9,
        kAllCommands = 1u << 31
    };

    template<>
    struct EnableBitmaskOperators<PipelineStage> : std::true_type {
    };

    using PipelineStageMask = PipelineStage;

    enum class ComponentSwizzle : uint8_t { kIdentity, kZero, kOne, kR, kG, kB, kA };

    struct ComponentMapping {
        ComponentSwizzle r{ComponentSwizzle::kIdentity};
        ComponentSwizzle g{ComponentSwizzle::kIdentity};
        ComponentSwizzle b{ComponentSwizzle::kIdentity};
        ComponentSwizzle a{ComponentSwizzle::kIdentity};
    };

    constexpr uint32_t kRemaining = std::numeric_limits<uint32_t>::max();

    struct SubresourceRange {
        AspectMask aspects{Aspect::kColor};
        uint32_t base_mip{0};
        uint32_t mip_count{kRemaining};
        uint32_t base_layer{0};
        uint32_t layer_count{kRemaining};
    };

    struct BufferRange {
        BufferHandle buffer{};
        uint64_t offset{0};
        uint64_t size{std::numeric_limits<uint64_t>::max()};
    };

    struct BufferDesc {
        uint64_t size{0};
        BufferCapability capabilities{BufferCapability::kNone};
        MemoryClass memory{MemoryClass::kAuto};
        ResourceFlags flags{ResourceFlags::kNone};
        DebugName name{};
    };

    struct TextureDesc {
        TextureDimension dimension{TextureDimension::kTex2D};
        Extent3D extent{};
        Format format{Format::kUnknown};
        uint32_t mip_levels{1};
        uint32_t array_layers{1};
        SampleCount samples{SampleCount::kX1};
        TextureCapability capabilities{TextureCapability::kNone};
        MemoryClass memory{MemoryClass::kDeviceLocal};
        ResourceFlags flags{ResourceFlags::kNone};
        std::span<const Format> compatible_view_formats{};
        DebugName name{};
    };

    enum class PresentMode : uint8_t { kImmediate, kLowLatency, kVsync, kAdaptiveVsync };

    struct SwapchainDesc {
        SurfaceHandle surface{};
        Extent2D extent{};
        Format format{Format::kUnknown};
        PresentMode present_mode{PresentMode::kVsync};
        uint32_t min_image_count{3};
        uint32_t frames_in_flight{2};
        TextureCapability image_capabilities{TextureCapability::kColorAttachment};
        DebugName name{};
    };

    struct SwapchainImage {
        TextureHandle texture{};
        TextureViewHandle view{};
    };

    struct SwapchainInfo {
        Extent2D extent{};
        Format format{Format::kUnknown};
        PresentMode present_mode{PresentMode::kVsync};
        uint32_t image_count{0};
        uint32_t frames_in_flight{0};
        bool needs_recreate{false};
        std::span<const SwapchainImage> images{};
    };

    enum class BufferViewKind : uint8_t { kUniform, kStorage, kRaw, kStructured, kTyped };

    struct BufferViewDesc {
        BufferHandle buffer{};
        BufferViewKind kind{BufferViewKind::kStorage};
        uint64_t offset{0};
        uint64_t size{std::numeric_limits<uint64_t>::max()};
        Format format{Format::kUnknown};
        uint32_t structure_stride{0};
        DebugName name{};
    };

    enum class TextureViewKind : uint8_t {
        kSampled,
        kStorage,
        kColorAttachment,
        kDepthStencil
    };

    enum class TextureViewDimension : uint8_t {
        kAuto,
        kTex1D,
        kTex1DArray,
        kTex2D,
        kTex2DArray,
        kCube,
        kCubeArray,
        kTex3D
    };

    struct TextureViewDesc {
        TextureHandle texture{};
        TextureViewKind kind{TextureViewKind::kSampled};
        TextureViewDimension dimension{TextureViewDimension::kAuto};
        Format format{Format::kUnknown};
        SubresourceRange subresources{};
        ComponentMapping components{};
        DebugName name{};
    };

    enum class Filter : uint8_t { kNearest, kLinear };

    enum class AddressMode : uint8_t { kRepeat, kMirroredRepeat, kClampToEdge, kClampToBorder };

    struct SamplerDesc {
        Filter min_filter{Filter::kLinear};
        Filter mag_filter{Filter::kLinear};
        Filter mip_filter{Filter::kLinear};
        AddressMode address_u{AddressMode::kRepeat};
        AddressMode address_v{AddressMode::kRepeat};
        AddressMode address_w{AddressMode::kRepeat};
        float mip_lod_bias{0.0f};
        float min_lod{0.0f};
        float max_lod{1000.0f};
        bool anisotropy_enable{false};
        float max_anisotropy{1.0f};
        DebugName name{};
    };

    enum class BindingType : uint8_t {
        kSampler,
        kSampledTexture,
        kStorageTexture,
        kUniformBuffer,
        kStorageBuffer,
        kReadOnlyTypedBuffer,
        kReadWriteTypedBuffer,
        kAccelerationStructure
    };

    struct BindingLocation {
        uint16_t group{0};
        uint16_t binding{0};

        friend constexpr bool operator==(BindingLocation, BindingLocation) = default;
    };

    enum class BindingArrayMode : uint8_t { kSingle, kFixed, kVariable, kUnbounded };

    enum class BindingFlags : uint32_t {
        kNone = 0,
        kBindless = 1u << 0,
        kPartiallyPopulated = 1u << 1,
        kDynamicOffset = 1u << 2,
        kImmutable = 1u << 3
    };

    template<>
    struct EnableBitmaskOperators<BindingFlags> : std::true_type {
    };

    struct BindingLayoutEntry {
        BindingLocation location{};
        BindingType type{BindingType::kSampledTexture};
        BindingArrayMode array_mode{BindingArrayMode::kSingle};
        uint32_t array_count{1};
        ShaderStageMask shader_stages{ShaderStage::kAll};
        BindingFlags flags{BindingFlags::kNone};
    };

    struct BindingLayoutDesc {
        std::span<const BindingLayoutEntry> entries{};
        DebugName name{};
    };

    struct InlineConstantRange {
        ShaderStageMask shader_stages{ShaderStage::kNone};
        uint32_t offset{0};
        uint32_t size{0};
    };

    struct PipelineLayoutDesc {
        std::span<const BindingLayoutHandle> binding_layouts{};
        std::span<const InlineConstantRange> inline_constant_ranges{};
    };

    struct VertexBindingDesc {
        uint32_t binding{0};
        uint32_t stride{0};
        VertexInputRate input_rate{VertexInputRate::kVertex};
    };

    struct VertexAttributeDesc {
        uint32_t location{0};
        uint32_t binding{0};
        Format format{Format::kUnknown};
        uint32_t offset{0};
    };

    struct BlendAttachmentDesc {
        bool blend_enable{false};
        BlendFactor source_color_factor{BlendFactor::kOne};
        BlendFactor destination_color_factor{BlendFactor::kZero};
        BlendOp color_operation{BlendOp::kAdd};
        BlendFactor source_alpha_factor{BlendFactor::kOne};
        BlendFactor destination_alpha_factor{BlendFactor::kZero};
        BlendOp alpha_operation{BlendOp::kAdd};
        ColorComponent color_write_mask{ColorComponent::kAll};
    };

    struct GraphicsPipelineState {
        std::span<const VertexBindingDesc> vertex_bindings{};
        std::span<const VertexAttributeDesc> vertex_attributes{};
        PrimitiveTopology topology{PrimitiveTopology::kTriangleList};
        bool primitive_restart_enable{false};
        PolygonMode polygon_mode{PolygonMode::kFill};
        CullMode cull_mode{CullMode::kBack};
        FrontFace front_face{FrontFace::kCounterClockwise};
        bool depth_clamp_enable{false};
        bool rasterizer_discard_enable{false};
        bool depth_bias_enable{false};
        float depth_bias_constant_factor{0.0f};
        float depth_bias_clamp{0.0f};
        float depth_bias_slope_factor{0.0f};
        float line_width{1.0f};
        SampleCount samples{SampleCount::kX1};
        bool alpha_to_coverage_enable{false};
        bool depth_test_enable{false};
        bool depth_write_enable{false};
        CompareOp depth_compare_operation{CompareOp::kLessOrEqual};
        bool stencil_test_enable{false};
        std::span<const Format> color_formats{};
        Format depth_stencil_format{Format::kUnknown};
        std::span<const BlendAttachmentDesc> blend_attachments{};
    };

    struct PipelineDesc {
        PipelineType type{PipelineType::kGraphics};
        PipelineLayoutDesc layout{};
        std::span<const ShaderStageDesc> shader_stages{};
        GraphicsPipelineState graphics{};
        DebugName name{};
    };

    enum class LoadOp : uint8_t { kLoad, kClear, kDontCare };

    enum class StoreOp : uint8_t { kStore, kDontCare };

    struct ClearColorValue {
        float r{0.0f};
        float g{0.0f};
        float b{0.0f};
        float a{1.0f};
    };

    struct ColorAttachmentDesc {
        TextureViewHandle view{};
        LoadOp load_op{LoadOp::kClear};
        StoreOp store_op{StoreOp::kStore};
        ClearColorValue clear_color{};
        ResourceUsage final_usage{ResourceUsage::kColorAttachmentWrite};
    };

    struct DepthStencilAttachmentDesc {
        TextureViewHandle view{};
        LoadOp depth_load_op{LoadOp::kClear};
        StoreOp depth_store_op{StoreOp::kStore};
        float clear_depth{1.0f};
        uint32_t clear_stencil{0};
        ResourceUsage final_usage{ResourceUsage::kDepthStencilWrite};
    };

    struct RenderingDesc {
        std::span<const ColorAttachmentDesc> color_attachments{};
        DepthStencilAttachmentDesc depth_stencil{};
        DebugName name{};
    };

    struct Viewport {
        float x{0.0f};
        float y{0.0f};
        float width{0.0f};
        float height{0.0f};
        float min_depth{0.0f};
        float max_depth{1.0f};
    };

    struct ScissorRect {
        int32_t x{0};
        int32_t y{0};
        uint32_t width{0};
        uint32_t height{0};
    };

    struct DrawDesc {
        uint32_t vertex_count{0};
        uint32_t instance_count{1};
        uint32_t first_vertex{0};
        uint32_t first_instance{0};
    };

    struct BindingResource {
        BindingType type{BindingType::kSampledTexture};
        TextureViewHandle texture_view{};
        BufferViewHandle buffer_view{};
        BufferRange buffer_range{};
        SamplerHandle sampler{};
        AccelerationStructureHandle acceleration_structure{};
    };

    struct BindingWrite {
        BindingLocation location{};
        uint32_t array_index{0};
        BindingResource resource{};
    };

    struct ResourceState {
        ResourceUsage usage{ResourceUsage::kUndefined};
        ShaderStageMask shader_stages{ShaderStage::kAll};
        QueueClass queue{QueueClass::kAuto};
        bool discard_contents{false};
    };

    struct GpuSyncPoint {
        TimelineHandle timeline{};
        uint64_t value{0};

        [[nodiscard]] constexpr bool is_valid() const noexcept { return timeline.is_valid() && value != 0; }
    };

    struct GpuWait {
        GpuSyncPoint point{};
        PipelineStageMask destination_stages{PipelineStage::kAllCommands};
    };

    struct FrameToken {
        SwapchainHandle swapchain{};
        uint32_t frame_slot{kRemaining};
        uint32_t image_index{kRemaining};
        uint64_t generation{0};

        [[nodiscard]] constexpr bool is_valid() const noexcept {
            return swapchain.is_valid() && frame_slot != kRemaining && image_index != kRemaining && generation != 0;
        }
    };

    struct FrameAcquireResult {
        FrameToken frame{};
        SwapchainImage image{};
        bool suboptimal{false};
    };

    struct CommandListDesc {
        QueueClass queue{QueueClass::kGraphics};
        bool one_time_submit{true};
        DebugName name{};
    };

    struct SubmitDesc {
        QueueClass queue{QueueClass::kGraphics};
        std::span<const CommandListHandle> command_lists{};
        std::span<const GpuWait> waits{};
    };

    struct SubmitResult {
        GpuSyncPoint completion{};
    };

    struct FrameSubmitDesc {
        FrameToken frame{};
        QueueClass queue{QueueClass::kGraphics};
        std::span<const CommandListHandle> command_lists{};
        std::span<const GpuWait> waits{};
    };

    struct PresentDesc {
        FrameToken frame{};
    };

    struct PresentResult {
        bool suboptimal{false};
    };

    struct DeviceCreateDesc {
        std::string_view application_name{"AEG Engine"};
        // UINT32_MAX selects automatically using the adapter score. Any other
        // value explicitly selects that enumerated adapter after validation.
        uint32_t preferred_adapter_index{std::numeric_limits<uint32_t>::max()};
        bool enable_validation{config::kValidationEnabled};
        bool enable_debug{config::kDebugEnabled};
    };

    enum class AdapterType : uint8_t { kOther, kIntegrated, kDiscrete, kVirtual, kCpu };

    enum class DeviceFeature : uint64_t {
        kNone = 0,
        kTimelineSynchronization = 1ull << 0,
        kBindlessResources = 1ull << 1,
        kGpuVirtualAddress = 1ull << 2
    };

    template<>
    struct EnableBitmaskOperators<DeviceFeature> : std::true_type {
    };

    using DeviceFeatureMask = DeviceFeature;

    struct DeviceCapabilities {
        uint32_t vendor_id{0};
        uint32_t device_id{0};
        std::string adapter_name{};
        AdapterType adapter_type{AdapterType::kOther};

        uint64_t local_memory_bytes{0};
        uint64_t cpu_visible_memory_bytes{0};
        bool dedicated_compute_queue{false};
        bool dedicated_copy_queue{false};

        DeviceFeatureMask supported_features{DeviceFeature::kNone};
        bool supports_swapchain{false};
        uint64_t max_uniform_buffer_bytes{0};
        uint32_t max_inline_constant_bytes{0};
    };
} // namespace aeg::rhi
