/**
 * @file rhi_struct.hpp
 * @brief Backend-neutral RHI handles, enums, resource descriptions, commands, and synchronization data.
 *
 * All spans and string views in this file are non-owning. Unless a function
 * explicitly says otherwise, callers must keep referenced storage alive until
 * the receiving RHI call returns.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include "rhi_config.hpp"

namespace aeg::rhi {
    /**
     * @brief Strongly typed opaque handle used to identify an RHI object.
     *
     * Zero is reserved for the invalid handle. The numeric value has no
     * backend-visible meaning and must not be serialized as a native handle.
     */
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

    /**
     * @brief Opt-in trait enabling bitwise operators for an enum.
     *
     * The primary template disables operators. Specialize it with
     * std::true_type only for flag-style enums.
     */
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

    /**
     * @brief Optional non-owning debug name for an RHI object.
     *
     * The referenced characters must remain alive until the consuming create
     * call returns; the backend may copy them when debug names are enabled.
     */
    struct DebugName {
        std::string_view value{};
    };

    /** @brief Two-dimensional unsigned extent, normally measured in pixels. */
    struct Extent2D {
        uint32_t width{0};
        uint32_t height{0};

        friend constexpr bool operator==(Extent2D, Extent2D) = default;
    };

    /** @brief Three-dimensional unsigned extent, normally measured in texels. */
    struct Extent3D {
        uint32_t width{1};
        uint32_t height{1};
        uint32_t depth{1};
    };

    /** @brief Kind of presentation surface supplied to create_surface(). */
    enum class SurfaceType : uint8_t { kWindow, kHeadless };

    /** @brief Selects an RHI implementation without exposing native API types. */
    enum class BackendType : uint8_t { kVulkan };

    /**
     * @brief Backend-neutral description of a presentation surface.
     *
     * Native handles are opaque and interpreted only by the selected backend.
     * For headless surfaces both native handles should be null.
     */
    struct SurfaceDesc {
        SurfaceType type{SurfaceType::kWindow};

        // Opaque native handles keep the public RHI independent from platform
        // window headers. Their interpretation belongs to the backend.
        void *native_display_handle{nullptr};
        void *native_window_handle{nullptr};
        DebugName name{};
    };

    /** @brief Texture and buffer format identifiers understood by the RHI. */
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

    /** @brief Supported multisample sample counts. */
    enum class SampleCount : uint8_t { kX1 = 1, kX2 = 2, kX4 = 4, kX8 = 8, kX16 = 16, kX32 = 32, kX64 = 64 };

    /** @brief Dimensionality of a texture resource. */
    enum class TextureDimension : uint8_t { kTex1D, kTex2D, kTex3D };

    /** @brief Preferred memory residency and CPU visibility class. */
    enum class MemoryClass : uint8_t { kAuto, kDeviceLocal, kUpload, kReadback };

    /** @brief Resource creation flags that may be combined with bitwise operators. */
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

    /** @brief Operations and views a buffer is allowed to support. */
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

    /** @brief Operations and views a texture is allowed to support. */
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

    /** @brief Texture aspect bits used for subresource selection. */
    enum class Aspect : uint8_t { kColor = 1u << 0, kDepth = 1u << 1, kStencil = 1u << 2 };

    template<>
    struct EnableBitmaskOperators<Aspect> : std::true_type {
    };

    using AspectMask = Aspect;

    /**
     * @brief Abstract resource usage requested by a command or graph pass.
     *
     * Usage is a semantic access category; the backend derives native layout,
     * access masks, and synchronization from it together with stage and queue.
     */
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

    /** @brief Logical queue class used for scheduling and synchronization. */
    enum class QueueClass : uint8_t { kAuto, kGraphics, kCompute, kCopy, kPresent };

    /** @brief Shader stage bits used in binding visibility and synchronization. */
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

    /**
     * @brief Compiled shader stage input for pipeline creation.
     *
     * The code span is consumed synchronously by create_pipeline(); the RHI
     * does not retain caller-owned bytecode storage after that call returns.
     */
    struct ShaderStageDesc {
        ShaderStage stage{ShaderStage::kNone};
        std::span<const std::byte> code{};
        std::string_view entry_point{"main"};
        DebugName name{};
    };

    using ShaderModuleDesc = ShaderStageDesc;

    /** @brief Pipeline category selected by PipelineDesc. */
    enum class PipelineType : uint8_t { kGraphics, kCompute };

    /** @brief Primitive assembly topology for graphics pipelines. */
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

    /** @brief Frequency at which a vertex binding advances. */
    enum class VertexInputRate : uint8_t { kVertex, kInstance };

    /** @brief Rasterizer polygon representation. */
    enum class PolygonMode : uint8_t { kFill, kLine, kPoint };

    /** @brief Faces removed by rasterizer culling. */
    enum class CullMode : uint8_t { kNone, kFront, kBack, kFrontAndBack };

    /** @brief Winding order considered front-facing. */
    enum class FrontFace : uint8_t { kCounterClockwise, kClockwise };

    /** @brief Comparison operation used by depth and stencil tests. */
    enum class CompareOp : uint8_t {
        kNever, kLess, kEqual, kLessOrEqual, kGreater, kNotEqual, kGreaterOrEqual, kAlways
    };

    /** @brief Source or destination factor used by color blending. */
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

    /** @brief Arithmetic operation used by color blending. */
    enum class BlendOp : uint8_t { kAdd, kSubtract, kReverseSubtract, kMin, kMax };

    /** @brief Color channels enabled for a blend attachment write. */
    enum class ColorComponent : uint8_t {
        kNone = 0, kR = 1u << 0, kG = 1u << 1, kB = 1u << 2, kA = 1u << 3, kAll = 0x0fu
    };

    template<>
    struct EnableBitmaskOperators<ColorComponent> : std::true_type {
    };

    /**
     * @brief Pipeline execution stage mask used for barriers and waits.
     *
     * Values are combinable flags; kAllCommands requests backend-wide ordering
     * when a more precise stage cannot be expressed.
     */
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

    /** @brief Source selector for one texture-view component. */
    enum class ComponentSwizzle : uint8_t { kIdentity, kZero, kOne, kR, kG, kB, kA };

    /** @brief Per-component swizzle applied by a texture view. */
    struct ComponentMapping {
        ComponentSwizzle r{ComponentSwizzle::kIdentity};
        ComponentSwizzle g{ComponentSwizzle::kIdentity};
        ComponentSwizzle b{ComponentSwizzle::kIdentity};
        ComponentSwizzle a{ComponentSwizzle::kIdentity};
    };

    /** @brief Sentinel requesting all remaining mip levels or array layers. */
    constexpr uint32_t kRemaining = std::numeric_limits<uint32_t>::max();

    /** @brief Mip, array-layer, and aspect range of a texture. */
    struct SubresourceRange {
        AspectMask aspects{Aspect::kColor};
        uint32_t base_mip{0};
        uint32_t mip_count{kRemaining};
        uint32_t base_layer{0};
        uint32_t layer_count{kRemaining};
    };

    /** @brief Byte range referencing a buffer handle. */
    struct BufferRange {
        BufferHandle buffer{};
        uint64_t offset{0};
        uint64_t size{std::numeric_limits<uint64_t>::max()};
    };

    /**
     * @brief Creation and initial-state description for a buffer.
     *
     * The capabilities must include every operation and view that will be used
     * for the buffer during its lifetime.
     */
    struct BufferDesc {
        uint64_t size{0};
        BufferCapability capabilities{BufferCapability::kNone};
        MemoryClass memory{MemoryClass::kAuto};
        ResourceFlags flags{ResourceFlags::kNone};
        ResourceUsage initial_usage{ResourceUsage::kUndefined};
        PipelineStageMask initial_stages{PipelineStage::kAllCommands};
        QueueClass initial_queue{QueueClass::kAuto};
        DebugName name{};
    };

    /**
     * @brief Creation and initial-state description for a texture.
     *
     * compatible_view_formats is borrowed for the duration of the create call.
     * Dimensions and mip/layer counts are expressed in texels and subresources.
     */
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
        ResourceUsage initial_usage{ResourceUsage::kUndefined};
        PipelineStageMask initial_stages{PipelineStage::kAllCommands};
        QueueClass initial_queue{QueueClass::kAuto};
        std::span<const Format> compatible_view_formats{};
        DebugName name{};
    };

    /** @brief Presentation pacing policy requested for a swapchain. */
    enum class PresentMode : uint8_t { kImmediate, kLowLatency, kVsync, kAdaptiveVsync };

    /**
     * @brief Creation and pacing description for a presentation swapchain.
     *
     * The backend may choose a nearby supported format, image count, or present
     * mode and reports the effective values through SwapchainInfo.
     */
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

    /** @brief Native texture and default view for one swapchain image. */
    struct SwapchainImage {
        TextureHandle texture{};
        TextureViewHandle view{};
    };

    /**
     * @brief Effective swapchain configuration and image list.
     *
     * The images span is owned by the swapchain and remains valid until the
     * swapchain is recreated or destroyed.
     */
    struct SwapchainInfo {
        Extent2D extent{};
        Format format{Format::kUnknown};
        PresentMode present_mode{PresentMode::kVsync};
        uint32_t image_count{0};
        uint32_t frames_in_flight{0};
        bool needs_recreate{false};
        std::span<const SwapchainImage> images{};
    };

    /** @brief Interpretation of a buffer view when bound to a shader. */
    enum class BufferViewKind : uint8_t { kUniform, kStorage, kRaw, kStructured, kTyped };

    /** @brief Description of a view into a buffer. */
    struct BufferViewDesc {
        BufferHandle buffer{};
        BufferViewKind kind{BufferViewKind::kStorage};
        uint64_t offset{0};
        uint64_t size{std::numeric_limits<uint64_t>::max()};
        Format format{Format::kUnknown};
        uint32_t structure_stride{0};
        DebugName name{};
    };

    /** @brief Intended use of a texture view. */
    enum class TextureViewKind : uint8_t {
        kSampled,
        kStorage,
        kColorAttachment,
        kDepthStencil
    };

    /** @brief View dimensionality, including array and cube interpretations. */
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

    /** @brief Description of a view into a texture subresource range. */
    struct TextureViewDesc {
        TextureHandle texture{};
        TextureViewKind kind{TextureViewKind::kSampled};
        TextureViewDimension dimension{TextureViewDimension::kAuto};
        Format format{Format::kUnknown};
        SubresourceRange subresources{};
        ComponentMapping components{};
        DebugName name{};
    };

    /** @brief Minification, magnification, or mip filtering mode. */
    enum class Filter : uint8_t { kNearest, kLinear };

    /** @brief Addressing behavior outside the normalized texture coordinate range. */
    enum class AddressMode : uint8_t { kRepeat, kMirroredRepeat, kClampToEdge, kClampToBorder };

    /** @brief Sampling parameters used to create a sampler object. */
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

    /** @brief Resource category stored at one binding location. */
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

    /** @brief Shader-visible binding coordinates, expressed as group and binding. */
    struct BindingLocation {
        uint16_t group{0};
        uint16_t binding{0};

        /** @brief Compares both binding coordinates. */
        friend constexpr bool operator==(BindingLocation, BindingLocation) = default;
    };

    /** @brief Number and growth policy of resources at a binding. */
    enum class BindingArrayMode : uint8_t { kSingle, kFixed, kVariable, kUnbounded };

    /** @brief Optional binding behavior flags that may be combined. */
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

    /** @brief One binding declaration within a binding layout. */
    struct BindingLayoutEntry {
        BindingLocation location{};
        BindingType type{BindingType::kSampledTexture};
        BindingArrayMode array_mode{BindingArrayMode::kSingle};
        uint32_t array_count{1};
        ShaderStageMask shader_stages{ShaderStage::kAll};
        BindingFlags flags{BindingFlags::kNone};
    };

    /** @brief Non-owning list of declarations used to create a binding layout. */
    struct BindingLayoutDesc {
        std::span<const BindingLayoutEntry> entries{};
        DebugName name{};
    };

    /**
     * @brief Controls the allocation class used for a binding set.
     *
     * Frame-local sets should be destroyed when their owning frame slot is
     * recycled. Device still owns the native allocation until destruction.
     */
    enum class BindingSetLifetime : uint8_t { kPersistent, kFrameLocal };

    /** @brief Backend-neutral description used to allocate a binding set. */
    struct BindingSetDesc {
        BindingLayoutHandle layout{};
        BindingSetLifetime lifetime{BindingSetLifetime::kPersistent};
        DebugName name{};
    };

    /** @brief Inline constant range exposed by a pipeline layout. */
    struct InlineConstantRange {
        ShaderStageMask shader_stages{ShaderStage::kNone};
        uint32_t offset{0};
        uint32_t size{0};
    };

    /** @brief Non-owning pipeline-layout binding groups and inline constants. */
    struct PipelineLayoutDesc {
        std::span<const BindingLayoutHandle> binding_layouts{};
        std::span<const InlineConstantRange> inline_constant_ranges{};
    };

    /** @brief Stride and stepping policy for one vertex-buffer binding. */
    struct VertexBindingDesc {
        uint32_t binding{0};
        uint32_t stride{0};
        VertexInputRate input_rate{VertexInputRate::kVertex};
    };

    /** @brief Format and byte offset of one vertex shader input attribute. */
    struct VertexAttributeDesc {
        uint32_t location{0};
        uint32_t binding{0};
        Format format{Format::kUnknown};
        uint32_t offset{0};
    };

    /** @brief Blend and write-mask state for one color attachment. */
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

    /**
     * @brief Fixed-function state for a graphics pipeline.
     *
     * The color_formats and blend_attachments spans are ordered by color
     * attachment index and are borrowed for the duration of pipeline creation.
     */
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

    /**
     * @brief Complete graphics or compute pipeline creation description.
     *
     * Shader stage bytecode and layout handles are borrowed for the duration of
     * create_pipeline(). The graphics member is ignored for compute pipelines.
     */
    struct PipelineDesc {
        PipelineType type{PipelineType::kGraphics};
        PipelineLayoutDesc layout{};
        std::span<const ShaderStageDesc> shader_stages{};
        GraphicsPipelineState graphics{};
        DebugName name{};
    };

    /** @brief Operation used to initialize an attachment at rendering begin. */
    enum class LoadOp : uint8_t { kLoad, kClear, kDontCare };

    /** @brief Operation used to preserve or discard an attachment at rendering end. */
    enum class StoreOp : uint8_t { kStore, kDontCare };

    /** @brief Four-component floating-point clear value. */
    struct ClearColorValue {
        float r{0.0f};
        float g{0.0f};
        float b{0.0f};
        float a{1.0f};
    };

    /** @brief Color attachment view and load/store state for dynamic rendering. */
    struct ColorAttachmentDesc {
        TextureViewHandle view{};
        LoadOp load_op{LoadOp::kClear};
        StoreOp store_op{StoreOp::kStore};
        ClearColorValue clear_color{};
        ResourceUsage final_usage{ResourceUsage::kColorAttachmentWrite};
    };

    /** @brief Depth/stencil attachment view and load/store state for dynamic rendering. */
    struct DepthStencilAttachmentDesc {
        TextureViewHandle view{};
        LoadOp depth_load_op{LoadOp::kClear};
        StoreOp depth_store_op{StoreOp::kStore};
        float clear_depth{1.0f};
        uint32_t clear_stencil{0};
        ResourceUsage final_usage{ResourceUsage::kDepthStencilWrite};
    };

    /** @brief Dynamic-rendering attachments and optional debug name. */
    struct RenderingDesc {
        std::span<const ColorAttachmentDesc> color_attachments{};
        DepthStencilAttachmentDesc depth_stencil{};
        DebugName name{};
    };

    /** @brief Viewport rectangle and normalized depth interval. */
    struct Viewport {
        float x{0.0f};
        float y{0.0f};
        float width{0.0f};
        float height{0.0f};
        float min_depth{0.0f};
        float max_depth{1.0f};
    };

    /** @brief Integer scissor rectangle in framebuffer coordinates. */
    struct ScissorRect {
        int32_t x{0};
        int32_t y{0};
        uint32_t width{0};
        uint32_t height{0};
    };

    /** @brief Direct draw parameters for non-indexed geometry. */
    struct DrawDesc {
        uint32_t vertex_count{0};
        uint32_t instance_count{1};
        uint32_t first_vertex{0};
        uint32_t first_instance{0};
    };

    /** @brief Element width used by an indexed draw. */
    enum class IndexFormat : uint8_t { kUint16, kUint32 };

    /** @brief Direct draw parameters for indexed geometry. */
    struct DrawIndexedDesc {
        uint32_t index_count{0};
        uint32_t instance_count{1};
        uint32_t first_index{0};
        int32_t vertex_offset{0};
        uint32_t first_instance{0};
    };

    /** @brief Compute workgroup counts for a direct dispatch. */
    struct DispatchDesc {
        uint32_t group_count_x{0};
        uint32_t group_count_y{0};
        uint32_t group_count_z{0};
    };

    /** @brief Indirect draw arguments stored in a GPU buffer. */
    struct DrawIndirectDesc {
        BufferHandle buffer{};
        uint64_t offset{0};
        uint32_t draw_count{1};
        uint32_t stride{sizeof(uint32_t) * 4};
    };

    /** @brief Indirect dispatch arguments stored in a GPU buffer. */
    struct DispatchIndirectDesc {
        BufferHandle buffer{};
        uint64_t offset{0};
    };

    /** @brief Signed texel or voxel offset used by copy operations. */
    struct Offset3D {
        int32_t x{0};
        int32_t y{0};
        int32_t z{0};
    };

    /** @brief One mip level and array-layer range used by a copy operation. */
    struct TextureSubresourceLayers {
        AspectMask aspects{Aspect::kColor};
        uint32_t mip_level{0};
        uint32_t base_array_layer{0};
        uint32_t layer_count{1};
    };

    /** @brief Buffer-to-buffer copy parameters. */
    struct BufferCopyDesc {
        BufferHandle source{};
        BufferHandle destination{};
        uint64_t source_offset{0};
        uint64_t destination_offset{0};
        uint64_t size{0};
    };

    /** @brief Buffer-to-texture copy parameters. */
    struct BufferTextureCopyDesc {
        BufferHandle source{};
        uint64_t source_offset{0};
        uint32_t source_row_length{0};
        uint32_t source_image_height{0};
        TextureHandle destination{};
        TextureSubresourceLayers destination_subresources{};
        Offset3D destination_offset{};
        Extent3D extent{};
    };

    /** @brief Texture-to-buffer copy parameters. */
    struct TextureBufferCopyDesc {
        TextureHandle source{};
        TextureSubresourceLayers source_subresources{};
        Offset3D source_offset{};
        Extent3D extent{};
        BufferHandle destination{};
        uint64_t destination_offset{0};
        uint32_t destination_row_length{0};
        uint32_t destination_image_height{0};
    };

    /** @brief Texture-to-texture copy parameters. */
    struct TextureCopyDesc {
        TextureHandle source{};
        TextureSubresourceLayers source_subresources{};
        Offset3D source_offset{};
        TextureHandle destination{};
        TextureSubresourceLayers destination_subresources{};
        Offset3D destination_offset{};
        Extent3D extent{};
    };

    /** @brief Texture clear parameters for color, depth, or stencil aspects. */
    struct ClearTextureDesc {
        TextureHandle texture{};
        SubresourceRange subresources{};
        ClearColorValue color{};
        float depth{1.0f};
        uint32_t stencil{0};
    };

    /**
     * @brief One resource payload written into a binding set.
     *
     * Only the member matching type is consumed by the backend. Handles are
     * borrowed and must remain valid until the binding set is no longer used.
     */
    struct BindingResource {
        BindingType type{BindingType::kSampledTexture};
        TextureViewHandle texture_view{};
        BufferViewHandle buffer_view{};
        BufferRange buffer_range{};
        SamplerHandle sampler{};
        AccelerationStructureHandle acceleration_structure{};
    };

    /** @brief One array element update applied to a binding set. */
    struct BindingWrite {
        BindingLocation location{};
        uint32_t array_index{0};
        BindingResource resource{};
    };

    /**
     * @brief Backend-neutral resource state used by barriers and graph imports.
     *
     * discard_contents permits a transition to treat prior contents as
     * undefined. queue identifies logical ownership; kAuto lets the backend or
     * scheduler select the queue.
     */
    struct ResourceState {
        ResourceUsage usage{ResourceUsage::kUndefined};
        ShaderStageMask shader_stages{ShaderStage::kAll};
        QueueClass queue{QueueClass::kAuto};
        bool discard_contents{false};
        PipelineStageMask pipeline_stages{PipelineStage::kAllCommands};

        friend constexpr bool operator==(const ResourceState &, const ResourceState &) = default;
    };

    /** @brief State transition for a buffer range. */
    struct BufferBarrier {
        BufferHandle buffer{};
        uint64_t offset{0};
        uint64_t size{std::numeric_limits<uint64_t>::max()};
        ResourceState before{};
        ResourceState after{};
        bool requires_memory_dependency{false};
    };

    /** @brief State transition for a texture subresource range. */
    struct TextureBarrier {
        TextureHandle texture{};
        SubresourceRange subresources{};
        ResourceState before{};
        ResourceState after{};
        bool requires_memory_dependency{false};
    };

    /** @brief Batch of buffer and texture barriers recorded on a command list. */
    struct BarrierDesc {
        std::span<const BufferBarrier> buffers{};
        std::span<const TextureBarrier> textures{};
    };

    /**
     * @brief Monotonic GPU completion point on a timeline.
     *
     * A sync point is valid only when both timeline and value are non-zero. It
     * can be waited on or queried through Device and is immutable after submit.
     */
    struct GpuSyncPoint {
        TimelineHandle timeline{};
        uint64_t value{0};

        [[nodiscard]] constexpr bool is_valid() const noexcept { return timeline.is_valid() && value != 0; }

        friend constexpr bool operator==(GpuSyncPoint, GpuSyncPoint) = default;
    };

    /**
     * @brief Fixed-capacity collection of completion points across queues.
     *
     * Points using the same timeline are merged by retaining the greatest
     * value. The collection stores opaque points by value and is suitable for
     * frame retirement.
     */
    struct GpuCompletionSet {
        static constexpr uint32_t kCapacity = 8;

        [[nodiscard]] constexpr bool empty() const noexcept { return count_ == 0; }
        [[nodiscard]] constexpr uint32_t size() const noexcept { return count_; }
        [[nodiscard]] constexpr std::span<const GpuSyncPoint> points() const noexcept {
            return std::span<const GpuSyncPoint>{values_.data(), count_};
        }

        /** @brief Adds a point, merging it with an existing timeline when possible. */
        [[nodiscard]] constexpr bool add(GpuSyncPoint point) noexcept {
            if (!point.is_valid()) {
                return true;
            }

            for (uint32_t index = 0; index < count_; ++index) {
                GpuSyncPoint& existing = values_[index];
                if (existing.timeline != point.timeline) {
                    continue;
                }
                existing.value = existing.value < point.value ? point.value : existing.value;
                return true;
            }

            if (count_ >= kCapacity) {
                return false;
            }
            values_[count_++] = point;
            return true;
        }

        /** @brief Removes all stored completion points. */
        constexpr void clear() noexcept { count_ = 0; }

    private:
        std::array<GpuSyncPoint, kCapacity> values_{};
        uint32_t count_{0};
    };

    /** @brief One GPU wait and the first pipeline stages that depend on it. */
    struct GpuWait {
        GpuSyncPoint point{};
        PipelineStageMask destination_stages{PipelineStage::kAllCommands};
    };

    /**
     * @brief Opaque token tying acquire, submit, and present to one swapchain frame.
     *
     * The token is single-use and must be passed back to the same swapchain
     * lifecycle sequence that produced it.
     */
    struct FrameToken {
        SwapchainHandle swapchain{};
        uint32_t frame_slot{kRemaining};
        uint32_t image_index{kRemaining};
        uint64_t generation{0};

        [[nodiscard]] constexpr bool is_valid() const noexcept {
            return swapchain.is_valid() && frame_slot != kRemaining && image_index != kRemaining && generation != 0;
        }
    };

    /** @brief Result of acquiring a swapchain image. */
    struct FrameAcquireResult {
        FrameToken frame{};
        SwapchainImage image{};
        /** @brief Resource state the acquired image is in before this frame's commands. */
        ResourceState initial_state{};
        bool suboptimal{false};
    };

    /** @brief Selects whether resource commands may insert implicit transitions. */
    enum class BarrierMode : uint8_t { kImplicit, kExplicit };

    /** @brief Command-list creation options. */
    struct CommandListDesc {
        QueueClass queue{QueueClass::kGraphics};
        bool one_time_submit{true};
        DebugName name{};
        BarrierMode barrier_mode{BarrierMode::kImplicit};
    };

    /** @brief Submission description for ordinary command lists. */
    struct SubmitDesc {
        QueueClass queue{QueueClass::kGraphics};
        std::span<const CommandListHandle> command_lists{};
        std::span<const GpuWait> waits{};
    };

    /** @brief Completion point returned by a queue submission. */
    struct SubmitResult {
        GpuSyncPoint completion{};
    };

    /** @brief Submission description associated with an acquired swapchain frame. */
    struct FrameSubmitDesc {
        FrameToken frame{};
        QueueClass queue{QueueClass::kGraphics};
        std::span<const CommandListHandle> command_lists{};
        std::span<const GpuWait> waits{};
    };

    /** @brief Presentation request for an acquired frame. */
    struct PresentDesc {
        FrameToken frame{};
    };

    /** @brief Presentation status returned by present(). */
    struct PresentResult {
        bool suboptimal{false};
    };

    /**
     * @brief Backend-neutral device creation preferences.
     *
     * application_name is borrowed for the duration of device creation. An
     * adapter index of UINT32_MAX requests the backend's comprehensive automatic
     * selection policy; another value requests that enumerated adapter after
     * validation.
     */
    struct DeviceCreateDesc {
        BackendType backend{BackendType::kVulkan};
        std::string_view application_name{"AEG Engine"};
        // UINT32_MAX selects automatically using the adapter score. Any other
        // value explicitly selects that enumerated adapter after validation.
        uint32_t preferred_adapter_index{std::numeric_limits<uint32_t>::max()};
        bool enable_validation{config::kValidationEnabled};
        bool enable_debug{config::kDebugEnabled};
    };

    /** @brief Broad hardware class reported by DeviceCapabilities. */
    enum class AdapterType : uint8_t { kOther, kIntegrated, kDiscrete, kVirtual, kCpu };

    /** @brief Optional device features exposed as a combinable mask. */
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

    /**
     * @brief Stable capability summary for the selected physical adapter.
     *
     * Values describe support available through the common RHI, not native API
     * extension details. The adapter_name string is owned by the capabilities
     * object and remains valid for the lifetime of its Device.
     */
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
