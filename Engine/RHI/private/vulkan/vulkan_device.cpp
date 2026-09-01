#include <logging.hpp>

#include "../../public/rhi.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <vulkan/vulkan.h>
#ifdef DeviceCapabilities
#undef DeviceCapabilities
#endif
#include <vk_mem_alloc.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aeg::rhi {
namespace {
constexpr uint32_t kInvalidIndex = std::numeric_limits<uint32_t>::max();

template <typename T, typename Tag>
class SlotMap {
public:
    [[nodiscard]] Handle<Tag> insert(T value) {
        uint32_t slot_index = kInvalidIndex;
        if (!free_.empty()) {
            slot_index = free_.back();
            free_.pop_back();
        } else {
            slot_index = static_cast<uint32_t>(slots_.size());
            slots_.emplace_back(Slot{});
        }

        const auto dense_index = static_cast<uint32_t>(dense_.size());

        dense_.emplace_back(std::move(value));
        dense_to_sparse_.emplace_back(slot_index);

        Slot& slot = slots_[slot_index];
        slot.dense_index = dense_index;

        return make_handle(slot_index, slot.generation);
    }

    [[nodiscard]] T* get(Handle<Tag> handle) {
        const auto decoded = decode(handle);
        if (!decoded.has_value()) {
            return nullptr;
        }
        return &dense_[slots_[decoded->first].dense_index];
    }

    [[nodiscard]] const T* get(Handle<Tag> handle) const {
        const auto decoded = decode(handle);
        if (!decoded.has_value()) {
            return nullptr;
        }
        return &dense_[slots_[decoded->first].dense_index];
    }

    [[nodiscard]] bool erase(Handle<Tag> handle) {
        const auto decoded = decode(handle);
        if (!decoded.has_value()) {
            return false;
        }
        Slot& slot = slots_[decoded->first];
        const uint32_t dense_index = slot.dense_index;
        const uint32_t last_dense_index = static_cast<uint32_t>(dense_.size()) - 1;

        if (dense_index != last_dense_index) {
            dense_[dense_index] = std::move(dense_.back());

            const uint32_t moved_slot_index = dense_to_sparse_.back();
            dense_to_sparse_[dense_index] = moved_slot_index;
            slots_[moved_slot_index].dense_index = dense_index;
        }

        dense_.pop_back();
        dense_to_sparse_.pop_back();

        ++slot.generation;
        if (slot.generation == 0) {
            slot.generation = 1;
        }

        slot.dense_index = kInvalidIndex;
        free_.push_back(decoded->first);

        return true;
    }

    template <typename Fn>
    void for_each(Fn&& fn) {
        for (T& item : dense_) {
            fn(item);
        }
    }

    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (const T& item : dense_) {
            fn(item);
        }
    }

    [[nodiscard]] bool empty() const noexcept { return dense_.empty(); }
    [[nodiscard]] uint32_t size() const noexcept { return dense_.size(); }
    [[nodiscard]] std::span<T> data() noexcept { return dense_; }
    [[nodiscard]] std::span<const T> data() const noexcept { return dense_; }

private:
    struct Slot {
        uint32_t generation{1};
        uint32_t dense_index{kInvalidIndex};
    };

    [[nodiscard]] static Handle<Tag> make_handle(uint32_t index, uint32_t generation) {
        return Handle<Tag>{(static_cast<uint64_t>(generation) << 32u) | (index + 1u)};
    }

    [[nodiscard]] std::optional<std::pair<uint32_t, uint32_t>> decode(Handle<Tag> handle) const {
        if (!handle.is_valid()) {
            return std::nullopt;
        }

        const auto encoded_index = static_cast<uint32_t>(handle.value & 0xffffffffu);
        const auto generation = static_cast<uint32_t>(handle.value >> 32u);
        if (encoded_index == 0) {
            return std::nullopt;
        }

        const uint32_t index = encoded_index - 1u;
        if (index >= slots_.size()) {
            return std::nullopt;
        }

        const Slot& slot = slots_[index];
        if (slot.dense_index == kInvalidIndex || slot.generation != generation) {
            return std::nullopt;
        }
        return std::make_pair(index, generation);
    }

    std::vector<T> dense_{};
    std::vector<uint32_t> dense_to_sparse_{};
    std::vector<Slot> slots_{};
    std::vector<uint32_t> free_{};
};

[[nodiscard]] Result make_vulkan_result(VkResult result, std::string_view operation) {
    if (result == VK_SUCCESS) {
        return Result::success();
    }

    ResultCode code = ResultCode::kVulkanError;
    switch (result) {
        case VK_TIMEOUT:
            code = ResultCode::kTimeout;
            break;
        case VK_NOT_READY:
            code = ResultCode::kNotReady;
            break;
        case VK_ERROR_OUT_OF_HOST_MEMORY:
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:
            code = ResultCode::kOutOfMemory;
            break;
        case VK_ERROR_DEVICE_LOST:
            code = ResultCode::kDeviceLost;
            break;
        case VK_ERROR_OUT_OF_DATE_KHR:
            code = ResultCode::kOutOfDate;
            break;
        case VK_ERROR_SURFACE_LOST_KHR:
            code = ResultCode::kSurfaceLost;
            break;
        case VK_ERROR_FEATURE_NOT_PRESENT:
        case VK_ERROR_EXTENSION_NOT_PRESENT:
            code = ResultCode::kUnsupported;
            break;
        default:
            break;
    }

    return Result::failure(
        code, std::string(operation) + " failed with VkResult=" + std::to_string(static_cast<int>(result)));
}

#if AEG_RHI_ENABLE_VALIDATION
[[nodiscard]] bool contains_name(const std::vector<VkLayerProperties>& layers, const char* name) {
    return std::any_of(layers.begin(), layers.end(),
                       [name](const VkLayerProperties& layer) { return std::strcmp(layer.layerName, name) == 0; });
}
#endif

[[nodiscard]] bool contains_name(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    return std::any_of(extensions.begin(), extensions.end(), [name](const VkExtensionProperties& extension) {
        return std::strcmp(extension.extensionName, name) == 0;
    });
}

[[nodiscard]] bool device_supports_extension(VkPhysicalDevice device, const char* name) {
    uint32_t extension_count = 0;
    if (vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, nullptr) != VK_SUCCESS) {
        return false;
    }

    std::vector<VkExtensionProperties> extensions(extension_count);
    if (extension_count != 0 &&
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, extensions.data()) != VK_SUCCESS) {
        return false;
    }
    return contains_name(extensions, name);
}

#if AEG_RHI_ENABLE_DEBUG_NAMES
template <typename VkHandle>
[[nodiscard]] bool is_null_vk_handle(VkHandle handle) noexcept {
    if constexpr (std::is_pointer_v<VkHandle>) {
        return handle == nullptr;
    } else {
        return handle == 0;
    }
}

template <typename VkHandle>
[[nodiscard]] uint64_t vk_handle_to_u64(VkHandle handle) noexcept {
    if constexpr (std::is_pointer_v<VkHandle>) {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
    } else {
        return static_cast<uint64_t>(handle);
    }
}
#endif

#if AEG_RHI_ENABLE_VALIDATION
[[nodiscard]] std::vector<VkLayerProperties> enumerate_instance_layers() {
    uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) {
        return {};
    }
    std::vector<VkLayerProperties> layers(count);
    if (count != 0 && vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) {
        return {};
    }
    return layers;
}
#endif

[[nodiscard]] std::vector<VkExtensionProperties> enumerate_instance_extensions() {
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VK_SUCCESS) {
        return {};
    }
    std::vector<VkExtensionProperties> extensions(count);
    if (count != 0 && vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()) != VK_SUCCESS) {
        return {};
    }
    return extensions;
}

struct MemorySummary {
    uint64_t device_local_bytes{0};
    uint64_t host_visible_bytes{0};
};

[[nodiscard]] MemorySummary summarize_memory(const VkPhysicalDeviceMemoryProperties& properties) {
    std::array<VkMemoryPropertyFlags, VK_MAX_MEMORY_HEAPS> heap_properties{};
    for (uint32_t type_index = 0; type_index < properties.memoryTypeCount; ++type_index) {
        const VkMemoryType& type = properties.memoryTypes[type_index];
        heap_properties[type.heapIndex] |= type.propertyFlags;
    }

    MemorySummary summary{};
    for (uint32_t heap_index = 0; heap_index < properties.memoryHeapCount; ++heap_index) {
        const VkMemoryPropertyFlags flags = heap_properties[heap_index];
        if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
            summary.device_local_bytes += properties.memoryHeaps[heap_index].size;
        }
        if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
            summary.host_visible_bytes += properties.memoryHeaps[heap_index].size;
        }
    }
    return summary;
}

#if AEG_RHI_ENABLE_VALIDATION || AEG_RHI_ENABLE_DEBUG_UTILS
[[nodiscard]] const char* physical_device_type_name(VkPhysicalDeviceType type) noexcept {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_OTHER:
            return "other";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            return "integrated";
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            return "discrete";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            return "virtual";
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            return "cpu";
        default:
            return "unknown";
    }
}
#endif

[[nodiscard]] PhysicalDeviceType to_rhi_physical_device_type(VkPhysicalDeviceType type) noexcept {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            return PhysicalDeviceType::kIntegrated;
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            return PhysicalDeviceType::kDiscrete;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            return PhysicalDeviceType::kVirtual;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            return PhysicalDeviceType::kCpu;
        case VK_PHYSICAL_DEVICE_TYPE_OTHER:
            return PhysicalDeviceType::kOther;
        default:
            return PhysicalDeviceType::kOther;
    }
}

[[nodiscard]] VkBufferUsageFlags to_vk_buffer_usage(BufferCapability capabilities) {
    VkBufferUsageFlags usage = 0;
    if (has_flag(capabilities, BufferCapability::kTransferSource)) {
        usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    }
    if (has_flag(capabilities, BufferCapability::kTransferDestination)) {
        usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    if (has_flag(capabilities, BufferCapability::kVertex)) {
        usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    }
    if (has_flag(capabilities, BufferCapability::kIndex)) {
        usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }
    if (has_flag(capabilities, BufferCapability::kUniform)) {
        usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    }
    if (has_flag(capabilities, BufferCapability::kStorage)) {
        usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    if (has_flag(capabilities, BufferCapability::kIndirect)) {
        usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    }
    if (has_flag(capabilities, BufferCapability::kTexel)) {
        usage |= VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    }
#ifdef VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
    if (has_flag(capabilities, BufferCapability::kDeviceAddress)) {
        usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
#endif
    return usage;
}

[[nodiscard]] VkImageUsageFlags to_vk_image_usage(TextureCapability capabilities) {
    VkImageUsageFlags usage = 0;
    if (has_flag(capabilities, TextureCapability::kTransferSource)) {
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if (has_flag(capabilities, TextureCapability::kTransferDestination)) {
        usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    if (has_flag(capabilities, TextureCapability::kSampled)) {
        usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (has_flag(capabilities, TextureCapability::kStorage)) {
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    if (has_flag(capabilities, TextureCapability::kColorAttachment)) {
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (has_flag(capabilities, TextureCapability::kDepthStencil)) {
        usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if (has_flag(capabilities, TextureCapability::kInputAttachment)) {
        usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    }
    return usage;
}

[[nodiscard]] std::optional<VkFormat> to_vk_format(Format format) {
    switch (format) {
        case Format::kR8Unorm:
            return VK_FORMAT_R8_UNORM;
        case Format::kRg8Unorm:
            return VK_FORMAT_R8G8_UNORM;
        case Format::kRgba8Unorm:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case Format::kBgra8Unorm:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case Format::kRgba8Srgb:
            return VK_FORMAT_R8G8B8A8_SRGB;
        case Format::kRgba16Float:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case Format::kRgba32Float:
            return VK_FORMAT_R32G32B32A32_SFLOAT;
        case Format::kR32Uint:
            return VK_FORMAT_R32_UINT;
        case Format::kR32Float:
            return VK_FORMAT_R32_SFLOAT;
        case Format::kRg32Float:
            return VK_FORMAT_R32G32_SFLOAT;
        case Format::kD24UnormS8Uint:
            return VK_FORMAT_D24_UNORM_S8_UINT;
        case Format::kD32Float:
            return VK_FORMAT_D32_SFLOAT;
        case Format::kD32FloatS8Uint:
            return VK_FORMAT_D32_SFLOAT_S8_UINT;
        case Format::kBc1RgbaUnorm:
            return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case Format::kBc3RgbaUnorm:
            return VK_FORMAT_BC3_UNORM_BLOCK;
        case Format::kBc5Unorm:
            return VK_FORMAT_BC5_UNORM_BLOCK;
        case Format::kBc7RgbaUnorm:
            return VK_FORMAT_BC7_UNORM_BLOCK;
        case Format::kUnknown:
            break;
    }
    return std::nullopt;
}

[[nodiscard]] Format from_vk_format(VkFormat format) noexcept {
    switch (format) {
        case VK_FORMAT_R8_UNORM:
            return Format::kR8Unorm;
        case VK_FORMAT_R8G8_UNORM:
            return Format::kRg8Unorm;
        case VK_FORMAT_R8G8B8A8_UNORM:
            return Format::kRgba8Unorm;
        case VK_FORMAT_B8G8R8A8_UNORM:
            return Format::kBgra8Unorm;
        case VK_FORMAT_R8G8B8A8_SRGB:
            return Format::kRgba8Srgb;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return Format::kRgba16Float;
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            return Format::kRgba32Float;
        case VK_FORMAT_R32_UINT:
            return Format::kR32Uint;
        case VK_FORMAT_R32_SFLOAT:
            return Format::kR32Float;
        case VK_FORMAT_R32G32_SFLOAT:
            return Format::kRg32Float;
        case VK_FORMAT_D24_UNORM_S8_UINT:
            return Format::kD24UnormS8Uint;
        case VK_FORMAT_D32_SFLOAT:
            return Format::kD32Float;
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return Format::kD32FloatS8Uint;
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
            return Format::kBc1RgbaUnorm;
        case VK_FORMAT_BC3_UNORM_BLOCK:
            return Format::kBc3RgbaUnorm;
        case VK_FORMAT_BC5_UNORM_BLOCK:
            return Format::kBc5Unorm;
        case VK_FORMAT_BC7_UNORM_BLOCK:
            return Format::kBc7RgbaUnorm;
        default:
            return Format::kUnknown;
    }
}

[[nodiscard]] VkPresentModeKHR to_vk_present_mode(PresentMode mode) noexcept {
    switch (mode) {
        case PresentMode::kImmediate:
            return VK_PRESENT_MODE_IMMEDIATE_KHR;
        case PresentMode::kMailbox:
            return VK_PRESENT_MODE_MAILBOX_KHR;
        case PresentMode::kFifo:
            return VK_PRESENT_MODE_FIFO_KHR;
        case PresentMode::kFifoRelaxed:
            return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

[[nodiscard]] PresentMode from_vk_present_mode(VkPresentModeKHR mode) noexcept {
    switch (mode) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR:
            return PresentMode::kImmediate;
        case VK_PRESENT_MODE_MAILBOX_KHR:
            return PresentMode::kMailbox;
        case VK_PRESENT_MODE_FIFO_KHR:
            return PresentMode::kFifo;
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
            return PresentMode::kFifoRelaxed;
        default:
            return PresentMode::kFifo;
    }
}

[[nodiscard]] bool is_depth_format(Format format) {
    return format == Format::kD24UnormS8Uint || format == Format::kD32Float || format == Format::kD32FloatS8Uint;
}

[[nodiscard]] AspectMask default_aspect(Format format) {
    if (format == Format::kD24UnormS8Uint || format == Format::kD32FloatS8Uint) {
        return Aspect::kDepth | Aspect::kStencil;
    }
    if (is_depth_format(format)) {
        return Aspect::kDepth;
    }
    return Aspect::kColor;
}

[[nodiscard]] VkImageAspectFlags to_vk_aspect(AspectMask aspects) {
    VkImageAspectFlags flags = 0;
    if (has_flag(aspects, Aspect::kColor)) {
        flags |= VK_IMAGE_ASPECT_COLOR_BIT;
    }
    if (has_flag(aspects, Aspect::kDepth)) {
        flags |= VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    if (has_flag(aspects, Aspect::kStencil)) {
        flags |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    return flags;
}

[[nodiscard]] VkSampleCountFlagBits to_vk_sample_count(SampleCount samples) {
    return static_cast<VkSampleCountFlagBits>(static_cast<uint32_t>(samples));
}

[[nodiscard]] VkShaderStageFlags to_vk_shader_stages(ShaderStageMask stages) {
    if (has_flag(stages, ShaderStage::kAll)) {
        return VK_SHADER_STAGE_ALL;
    }

    VkShaderStageFlags flags = 0;
    if (has_flag(stages, ShaderStage::kVertex))
        flags |= VK_SHADER_STAGE_VERTEX_BIT;
    if (has_flag(stages, ShaderStage::kTessellationControl))
        flags |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    if (has_flag(stages, ShaderStage::kTessellationEvaluation))
        flags |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    if (has_flag(stages, ShaderStage::kGeometry))
        flags |= VK_SHADER_STAGE_GEOMETRY_BIT;
    if (has_flag(stages, ShaderStage::kFragment))
        flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
    if (has_flag(stages, ShaderStage::kCompute))
        flags |= VK_SHADER_STAGE_COMPUTE_BIT;
#ifdef VK_SHADER_STAGE_RAYGEN_BIT_KHR
    if (has_flag(stages, ShaderStage::kRayTracing)) {
        flags |= VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                 VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR |
                 VK_SHADER_STAGE_INTERSECTION_BIT_KHR | VK_SHADER_STAGE_CALLABLE_BIT_KHR;
    }
#endif
#ifdef VK_SHADER_STAGE_MESH_BIT_EXT
    if (has_flag(stages, ShaderStage::kMesh)) {
        flags |= VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT;
    }
#endif
    return flags;
}

[[nodiscard]] VkPipelineStageFlags to_vk_pipeline_stages(PipelineStageMask stages) {
    if (has_flag(stages, PipelineStage::kAllCommands)) {
        return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }

    VkPipelineStageFlags flags = 0;
    if (has_flag(stages, PipelineStage::kCopy))
        flags |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (has_flag(stages, PipelineStage::kVertexInput))
        flags |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    if (has_flag(stages, PipelineStage::kVertexShader))
        flags |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
    if (has_flag(stages, PipelineStage::kFragmentShader))
        flags |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    if (has_flag(stages, PipelineStage::kEarlyDepth))
        flags |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    if (has_flag(stages, PipelineStage::kLateDepth))
        flags |= VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    if (has_flag(stages, PipelineStage::kColorOutput))
        flags |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (has_flag(stages, PipelineStage::kCompute))
        flags |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
#ifdef VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR
    if (has_flag(stages, PipelineStage::kRayTracing))
        flags |= VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
#endif
    if (has_flag(stages, PipelineStage::kHost))
        flags |= VK_PIPELINE_STAGE_HOST_BIT;
    return flags == 0 ? static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT) : flags;
}

[[nodiscard]] VkFilter to_vk_filter(Filter filter) {
    return filter == Filter::kNearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

[[nodiscard]] VkSamplerAddressMode to_vk_address_mode(AddressMode mode) {
    switch (mode) {
        case AddressMode::kRepeat:
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case AddressMode::kMirroredRepeat:
            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case AddressMode::kClampToEdge:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case AddressMode::kClampToBorder:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

[[nodiscard]] VkComponentSwizzle to_vk_swizzle(ComponentSwizzle swizzle) {
    switch (swizzle) {
        case ComponentSwizzle::kIdentity:
            return VK_COMPONENT_SWIZZLE_IDENTITY;
        case ComponentSwizzle::kZero:
            return VK_COMPONENT_SWIZZLE_ZERO;
        case ComponentSwizzle::kOne:
            return VK_COMPONENT_SWIZZLE_ONE;
        case ComponentSwizzle::kR:
            return VK_COMPONENT_SWIZZLE_R;
        case ComponentSwizzle::kG:
            return VK_COMPONENT_SWIZZLE_G;
        case ComponentSwizzle::kB:
            return VK_COMPONENT_SWIZZLE_B;
        case ComponentSwizzle::kA:
            return VK_COMPONENT_SWIZZLE_A;
    }
    return VK_COMPONENT_SWIZZLE_IDENTITY;
}

[[nodiscard]] std::optional<VkDescriptorType> to_vk_descriptor_type(BindingType type) {
    switch (type) {
        case BindingType::kSampler:
            return VK_DESCRIPTOR_TYPE_SAMPLER;
        case BindingType::kSampledTexture:
            return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case BindingType::kStorageTexture:
            return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case BindingType::kUniformBuffer:
            return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case BindingType::kStorageBuffer:
            return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case BindingType::kUniformTexelBuffer:
            return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        case BindingType::kStorageTexelBuffer:
            return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
        case BindingType::kAccelerationStructure:
#ifdef VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
            return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
#else
            return std::nullopt;
#endif
    }
    return std::nullopt;
}

[[nodiscard]] VkImageViewType to_vk_image_view_type(TextureViewDimension dimension, TextureDimension texture_dimension,
                                                    uint32_t layer_count) {
    if (dimension == TextureViewDimension::kAuto) {
        if (texture_dimension == TextureDimension::kTex1D) {
            return layer_count > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
        }
        if (texture_dimension == TextureDimension::kTex3D) {
            return VK_IMAGE_VIEW_TYPE_3D;
        }
        return layer_count > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }

    switch (dimension) {
        case TextureViewDimension::kTex1D:
            return VK_IMAGE_VIEW_TYPE_1D;
        case TextureViewDimension::kTex1DArray:
            return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
        case TextureViewDimension::kTex2D:
            return VK_IMAGE_VIEW_TYPE_2D;
        case TextureViewDimension::kTex2DArray:
            return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        case TextureViewDimension::kCube:
            return VK_IMAGE_VIEW_TYPE_CUBE;
        case TextureViewDimension::kCubeArray:
            return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        case TextureViewDimension::kTex3D:
            return VK_IMAGE_VIEW_TYPE_3D;
        case TextureViewDimension::kAuto:
            break;
    }
    return VK_IMAGE_VIEW_TYPE_2D;
}

[[nodiscard]] VmaAllocationCreateInfo to_vma_allocation_create_info(MemoryClass memory_class,
                                                                    bool prefer_device_local) {
    VmaAllocationCreateInfo allocation_info{};
    allocation_info.usage = VMA_MEMORY_USAGE_UNKNOWN;

    switch (memory_class) {
        case MemoryClass::kAuto:
            allocation_info.preferredFlags = prefer_device_local ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : 0;
            break;
        case MemoryClass::kDeviceLocal:
            allocation_info.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            break;
        case MemoryClass::kUpload:
            allocation_info.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
            allocation_info.preferredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            break;
        case MemoryClass::kReadback:
            allocation_info.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
            allocation_info.preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            break;
    }

    return allocation_info;
}

struct Allocation {
    VmaAllocation handle{nullptr};
    VmaAllocationInfo info{};
    VkMemoryPropertyFlags properties{0};
};

struct BufferResource {
    VkBuffer buffer{VK_NULL_HANDLE};
    Allocation allocation{};
    uint64_t size{0};
    BufferCapability capabilities{BufferCapability::kNone};
};

struct TextureResource {
    VkImage image{VK_NULL_HANDLE};
    Allocation allocation{};
    TextureDesc desc{};
    std::vector<Format> compatible_formats{};
    bool external{false};
    SwapchainHandle owner_swapchain{};
};

struct BufferViewResource {
    BufferViewDesc desc{};
    VkBufferView view{VK_NULL_HANDLE};
};

struct TextureViewResource {
    TextureViewDesc desc{};
    VkImageView view{VK_NULL_HANDLE};
    bool owned_by_swapchain{false};
    SwapchainHandle owner_swapchain{};
};

struct SamplerResource {
    VkSampler sampler{VK_NULL_HANDLE};
};

struct BindingLayoutResource {
    VkDescriptorSetLayout layout{VK_NULL_HANDLE};
    uint16_t group{0};
    std::vector<BindingLayoutEntry> entries{};
};

struct BindingSetResource {
    VkDescriptorPool pool{VK_NULL_HANDLE};
    VkDescriptorSet set{VK_NULL_HANDLE};
    BindingLayoutHandle layout{};
    std::vector<BindingWrite> writes{};
};

struct CommandListResource {
    VkCommandPool pool{VK_NULL_HANDLE};
    VkCommandBuffer command_buffer{VK_NULL_HANDLE};
    QueueClass queue{QueueClass::kGraphics};
    bool one_time_submit{true};
    bool recording{false};
    bool ended{false};
    GpuSyncPoint last_submitted{};
};

struct QueueState {
    QueueClass logical_class{QueueClass::kGraphics};
    uint32_t family_index{kInvalidIndex};
    VkQueue queue{VK_NULL_HANDLE};
    VkCommandPool command_pool{VK_NULL_HANDLE};
    VkSemaphore timeline{VK_NULL_HANDLE};
    TimelineHandle timeline_handle{};
    uint64_t next_signal_value{1};
};

struct SurfaceResource {
    VkSurfaceKHR surface{VK_NULL_HANDLE};
    uint32_t present_family{kInvalidIndex};
    VkQueue present_queue{VK_NULL_HANDLE};
};

struct FrameSlot {
    VkSemaphore image_available{VK_NULL_HANDLE};
    VkSemaphore render_finished{VK_NULL_HANDLE};
    VkFence in_flight{VK_NULL_HANDLE};
    uint32_t image_index{kRemaining};
    bool acquired{false};
    bool submitted{false};
    GpuSyncPoint completion{};
};

struct SwapchainResource {
    SwapchainHandle self{};
    SurfaceHandle surface{};
    VkSwapchainKHR swapchain{VK_NULL_HANDLE};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkColorSpaceKHR color_space{VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    VkPresentModeKHR present_mode{VK_PRESENT_MODE_FIFO_KHR};
    VkExtent2D extent{};
    std::vector<SwapchainImage> images{};
    std::vector<uint32_t> image_in_flight_frame{};
    std::vector<FrameSlot> frames{};
    uint32_t current_frame{0};
    uint64_t generation{1};
    bool needs_recreate{false};
};

class VulkanDevice final : public Device {
public:
    ~VulkanDevice() override { shutdown(); }

    [[nodiscard]] Result initialize(const DeviceCreateDesc& desc);

    [[nodiscard]] const DeviceCapabilities& capabilities() const noexcept override { return capabilities_; }

    [[nodiscard]] ResultValue<SurfaceHandle> create_surface(const SurfaceDesc& desc) override;

    [[nodiscard]] Result destroy_surface(SurfaceHandle surface) override;

    [[nodiscard]] ResultValue<SwapchainHandle> create_swapchain(const SwapchainDesc& desc) override;

    [[nodiscard]] Result destroy_swapchain(SwapchainHandle swapchain) override;

    [[nodiscard]] Result recreate_swapchain(SwapchainHandle swapchain, const SwapchainDesc& desc) override;

    [[nodiscard]] ResultValue<SwapchainInfo> get_swapchain_info(SwapchainHandle swapchain) const override;

    [[nodiscard]] ResultValue<FrameAcquireResult> acquire_next_image(SwapchainHandle swapchain) override;

    [[nodiscard]] ResultValue<SubmitResult> submit_frame(const FrameSubmitDesc& desc) override;

    [[nodiscard]] ResultValue<PresentResult> present(const PresentDesc& desc) override;

    [[nodiscard]] Result cancel_frame(const FrameToken& frame) override;

    [[nodiscard]] ResultValue<BufferHandle> create_buffer(const BufferDesc& desc) override;

    [[nodiscard]] Result destroy_buffer(BufferHandle buffer) override;

    [[nodiscard]] Result write_buffer(BufferHandle buffer, uint64_t offset, std::span<const std::byte> data) override;

    [[nodiscard]] ResultValue<TextureHandle> create_texture(const TextureDesc& desc) override;

    [[nodiscard]] Result destroy_texture(TextureHandle texture) override;

    [[nodiscard]] ResultValue<BufferViewHandle> create_buffer_view(const BufferViewDesc& desc) override;

    [[nodiscard]] Result destroy_buffer_view(BufferViewHandle view) override;

    [[nodiscard]] ResultValue<TextureViewHandle> create_texture_view(const TextureViewDesc& desc) override;

    [[nodiscard]] Result destroy_texture_view(TextureViewHandle view) override;

    [[nodiscard]] ResultValue<SamplerHandle> create_sampler(const SamplerDesc& desc) override;

    [[nodiscard]] Result destroy_sampler(SamplerHandle sampler) override;

    [[nodiscard]] ResultValue<BindingLayoutHandle> create_binding_layout(const BindingLayoutDesc& desc) override;

    [[nodiscard]] Result destroy_binding_layout(BindingLayoutHandle layout) override;

    [[nodiscard]] ResultValue<BindingSetHandle> create_binding_set(BindingLayoutHandle layout) override;

    [[nodiscard]] Result update_binding_set(BindingSetHandle set, std::span<const BindingWrite> writes) override;

    [[nodiscard]] Result destroy_binding_set(BindingSetHandle set) override;

    [[nodiscard]] ResultValue<CommandListHandle> create_command_list(const CommandListDesc& desc) override;

    [[nodiscard]] Result begin_command_list(CommandListHandle command_list) override;

    [[nodiscard]] Result end_command_list(CommandListHandle command_list) override;

    [[nodiscard]] Result reset_command_list(CommandListHandle command_list) override;

    [[nodiscard]] Result destroy_command_list(CommandListHandle command_list) override;

    [[nodiscard]] ResultValue<SubmitResult> submit(const SubmitDesc& desc) override;

    [[nodiscard]] Result wait(const GpuSyncPoint& point, uint64_t timeout_nanoseconds) override;

    [[nodiscard]] bool is_complete(const GpuSyncPoint& point) const override;

    [[nodiscard]] Result wait_idle() override;

private:
    struct QueueSelection {
        uint32_t graphics{kInvalidIndex};
        uint32_t compute{kInvalidIndex};
        uint32_t copy{kInvalidIndex};
        uint32_t family_count{0};
        uint64_t quality_score{0};
        bool dedicated_compute{false};
        bool dedicated_copy{false};
    };

    struct FeatureState {
        bool sampler_anisotropy{false};
        bool timeline_semaphore{false};
        bool synchronization2{false};
        bool dynamic_rendering{false};
        bool descriptor_indexing{false};
        bool buffer_device_address{false};
        bool swapchain{false};
    };

    struct PhysicalDeviceCandidate {
        uint32_t index{kInvalidIndex};
        VkPhysicalDevice device{VK_NULL_HANDLE};
        VkPhysicalDeviceProperties properties{};
        VkPhysicalDeviceMemoryProperties memory_properties{};
        QueueSelection queues{};
        FeatureState features{};
        uint64_t device_local_memory_bytes{0};
        uint64_t host_visible_memory_bytes{0};
        uint64_t score{0};
        bool viable{false};
        std::string rejection_reason{};
    };

    [[nodiscard]] Result create_instance(const DeviceCreateDesc& desc);

    [[nodiscard]] Result select_physical_device(const DeviceCreateDesc& desc, QueueSelection& selection,
                                                FeatureState& supported_features);

    [[nodiscard]] Result create_logical_device(const QueueSelection& selection, const FeatureState& supported_features);

    [[nodiscard]] Result create_allocator(const FeatureState& supported_features);

    [[nodiscard]] Result create_queue_state(QueueState& state, QueueClass logical_class, uint32_t family_index,
                                            uint32_t handle_value);

    [[nodiscard]] Result initialize_swapchain(SwapchainResource& resource, SwapchainHandle self,
                                              const SwapchainDesc& desc, VkSwapchainKHR old_swapchain);

    void destroy_swapchain_native(SwapchainResource& resource) noexcept;

    [[nodiscard]] Result validate_swapchain_references(const SwapchainResource& resource) const;

    [[nodiscard]] Result validate_frame_token(const FrameToken& frame, SwapchainResource*& swapchain);

    [[nodiscard]] Result create_frame_slot_sync(FrameSlot& frame);

    void destroy_frame_slot_sync(FrameSlot& frame) noexcept;

    [[nodiscard]] Result consume_acquire_semaphore(FrameSlot& frame);

    [[nodiscard]] QueueSelection choose_queue_families(
        const std::vector<VkQueueFamilyProperties>& queue_families) const;

    [[nodiscard]] FeatureState query_supported_features(VkPhysicalDevice device) const;

    [[nodiscard]] PhysicalDeviceCandidate evaluate_physical_device(VkPhysicalDevice device, uint32_t index) const;

    [[nodiscard]] uint64_t score_physical_device(const PhysicalDeviceCandidate& candidate) const;

    [[nodiscard]] QueueState* queue_state(QueueClass queue);

    [[nodiscard]] QueueState* timeline_queue(TimelineHandle timeline);

    [[nodiscard]] const QueueState* timeline_queue(TimelineHandle timeline) const;

    [[nodiscard]] Result validate_buffer_range(BufferHandle buffer, uint64_t offset, uint64_t size) const;

    [[nodiscard]] Result validate_texture_view(const TextureViewDesc& desc, const TextureResource& texture) const;

    [[nodiscard]] std::optional<BindingLayoutEntry> find_binding_entry(const BindingLayoutResource& layout,
                                                                       BindingLocation location) const;

    [[nodiscard]] Result validate_binding_resource(const BindingLayoutEntry& entry,
                                                   const BindingResource& resource) const;

    [[nodiscard]] bool buffer_is_referenced(BufferHandle buffer) const;

    [[nodiscard]] bool texture_is_referenced(TextureHandle texture) const;

    [[nodiscard]] bool buffer_view_is_referenced(BufferViewHandle view) const;

    [[nodiscard]] bool texture_view_is_referenced(TextureViewHandle view) const;

    [[nodiscard]] bool sampler_is_referenced(SamplerHandle sampler) const;

    void shutdown();
#if AEG_RHI_ENABLE_DEBUG_UTILS
    void destroy_debug_messenger();
#endif

#if AEG_RHI_ENABLE_DEBUG_NAMES
    template <typename VkHandle>
    void set_debug_name(VkObjectType object_type, VkHandle object, DebugName name);
#endif

    VkInstance instance_{VK_NULL_HANDLE};
#if AEG_RHI_ENABLE_DEBUG_UTILS
    VkDebugUtilsMessengerEXT debug_messenger_{VK_NULL_HANDLE};
#endif
    VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    VmaAllocator allocator_{nullptr};
    VkPhysicalDeviceProperties physical_properties_{};
    VkPhysicalDeviceMemoryProperties memory_properties_{};
    DeviceCapabilities capabilities_{};
    FeatureState enabled_features_{};
    bool surface_extension_enabled_{false};
    bool win32_surface_extension_enabled_{false};
    bool headless_surface_extension_enabled_{false};
    std::vector<uint32_t> enabled_queue_families_{};
#if AEG_RHI_ENABLE_DEBUG_UTILS || AEG_RHI_ENABLE_DEBUG_NAMES
    bool debug_utils_enabled_{false};
#endif
#if AEG_RHI_ENABLE_DEBUG_NAMES
    PFN_vkSetDebugUtilsObjectNameEXT set_debug_utils_object_name_{nullptr};
#endif

    std::array<QueueState, 3> queues_{};

    SlotMap<BufferResource, BufferTag> buffers_{};
    SlotMap<TextureResource, TextureTag> textures_{};
    SlotMap<BufferViewResource, BufferViewTag> buffer_views_{};
    SlotMap<TextureViewResource, TextureViewTag> texture_views_{};
    SlotMap<SamplerResource, SamplerTag> samplers_{};
    SlotMap<BindingLayoutResource, BindingLayoutTag> binding_layouts_{};
    SlotMap<BindingSetResource, BindingSetTag> binding_sets_{};
    SlotMap<CommandListResource, CommandListTag> command_lists_{};
    SlotMap<SurfaceResource, SurfaceTag> surfaces_{};
    SlotMap<SwapchainResource, SwapchainTag> swapchains_{};
};

#if AEG_RHI_ENABLE_DEBUG_UTILS
VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* callback_data, void*) {
    const char* label = "INFO";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        label = "ERROR";
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        label = "WARN";
    }
    const char* message = callback_data != nullptr && callback_data->pMessage != nullptr ? callback_data->pMessage : "";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        AEG_LOG_ERROR(aeg::logging::Category::kRhi, "Vulkan validation [{}]: {}", label, message);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        AEG_LOG_WARNING(aeg::logging::Category::kRhi, "Vulkan validation [{}]: {}", label, message);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) {
        AEG_LOG_DEBUG(aeg::logging::Category::kRhi, "Vulkan validation [{}]: {}", label, message);
    } else {
        AEG_LOG_INFO(aeg::logging::Category::kRhi, "Vulkan validation [{}]: {}", label, message);
    }
    return VK_FALSE;
}
#endif
}  // namespace

#if AEG_RHI_ENABLE_DEBUG_NAMES
template <typename VkHandle>
void VulkanDevice::set_debug_name(VkObjectType object_type, VkHandle object, DebugName name) {
    if (name.value.empty() || device_ == VK_NULL_HANDLE || !debug_utils_enabled_ ||
        set_debug_utils_object_name_ == nullptr || is_null_vk_handle(object)) {
        return;
    }

    const std::string object_name(name.value);
    VkDebugUtilsObjectNameInfoEXT name_info{};
    name_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    name_info.objectType = object_type;
    name_info.objectHandle = vk_handle_to_u64(object);
    name_info.pObjectName = object_name.c_str();
    (void)set_debug_utils_object_name_(device_, &name_info);
}
#endif

Result VulkanDevice::create_instance(const DeviceCreateDesc& desc) {
#if !AEG_RHI_ENABLE_VALIDATION
    if (desc.enable_validation) {
        return Result::failure(ResultCode::kUnsupported, "Vulkan validation support was compiled out");
    }
#endif
#if !AEG_RHI_ENABLE_DEBUG_UTILS
    if (desc.enable_debug_utils) {
        return Result::failure(ResultCode::kUnsupported, "Vulkan debug-utils support was compiled out");
    }
#endif

    uint32_t loader_version = VK_API_VERSION_1_0;
    const VkResult version_result = vkEnumerateInstanceVersion(&loader_version);
    if (version_result != VK_SUCCESS) {
        return make_vulkan_result(version_result, "vkEnumerateInstanceVersion");
    }
    if (loader_version < VK_API_VERSION_1_3) {
        return Result::failure(ResultCode::kUnsupported, "AEG RHI requires Vulkan 1.3 or newer");
    }

#if AEG_RHI_ENABLE_VALIDATION
    const auto available_layers = enumerate_instance_layers();
#endif
    const auto available_extensions = enumerate_instance_extensions();

    std::vector<const char*> layers;
#if AEG_RHI_ENABLE_VALIDATION
    if (desc.enable_validation) {
        if (!contains_name(available_layers, "VK_LAYER_KHRONOS_validation")) {
            return Result::failure(ResultCode::kUnsupported, "VK_LAYER_KHRONOS_validation is not available");
        }
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }
#endif

#if AEG_RHI_ENABLE_DEBUG_UTILS
    const bool wants_debug_utils = desc.enable_validation || desc.enable_debug_utils;
    if (wants_debug_utils) {
        if (!contains_name(available_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            if (desc.enable_debug_utils) {
                return Result::failure(ResultCode::kUnsupported, "VK_EXT_debug_utils is not available");
            }
        } else {
            debug_utils_enabled_ = true;
        }
    }
#endif

    std::vector<const char*> extensions;
    const auto add_instance_extension = [&extensions, &available_extensions](const char* name) {
        if (!contains_name(available_extensions, name) ||
            std::find(extensions.begin(), extensions.end(), name) != extensions.end()) {
            return false;
        }
        extensions.push_back(name);
        return true;
    };
#if AEG_RHI_ENABLE_DEBUG_UTILS
    if (debug_utils_enabled_) {
        (void)add_instance_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
#endif

    surface_extension_enabled_ = add_instance_extension(VK_KHR_SURFACE_EXTENSION_NAME);

#if defined(VK_USE_PLATFORM_WIN32_KHR)
    if (surface_extension_enabled_) {
        win32_surface_extension_enabled_ = add_instance_extension(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
    }
#endif

#ifdef VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME
    if (surface_extension_enabled_) {
        headless_surface_extension_enabled_ = add_instance_extension(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
    }
#endif

    const std::string application_name(desc.application_name);
    constexpr char kEngineName[] = "AEG Engine";

    VkApplicationInfo application_info{};
    application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application_info.pApplicationName = application_name.c_str();
    application_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    application_info.pEngineName = kEngineName;
    application_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    application_info.apiVersion = VK_API_VERSION_1_3;

#if AEG_RHI_ENABLE_DEBUG_UTILS
    VkDebugUtilsMessengerCreateInfoEXT debug_create_info{};
    debug_create_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    debug_create_info.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug_create_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug_create_info.pfnUserCallback = debug_callback;
#endif

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
#if AEG_RHI_ENABLE_DEBUG_UTILS
    instance_info.pNext = debug_utils_enabled_ ? &debug_create_info : nullptr;
#endif
    instance_info.pApplicationInfo = &application_info;
    instance_info.enabledLayerCount = static_cast<uint32_t>(layers.size());
    instance_info.ppEnabledLayerNames = layers.data();
    instance_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    instance_info.ppEnabledExtensionNames = extensions.data();

    const VkResult create_result = vkCreateInstance(&instance_info, nullptr, &instance_);
    if (create_result != VK_SUCCESS) {
        return make_vulkan_result(create_result, "vkCreateInstance");
    }

#if AEG_RHI_ENABLE_DEBUG_UTILS
    if (debug_utils_enabled_) {
        const auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (create_messenger != nullptr) {
            const VkResult messenger_result =
                create_messenger(instance_, &debug_create_info, nullptr, &debug_messenger_);
            if (messenger_result != VK_SUCCESS) {
                debug_messenger_ = VK_NULL_HANDLE;
            }
        }
    }
#endif

    return Result::success();
}

VulkanDevice::QueueSelection VulkanDevice::choose_queue_families(
    const std::vector<VkQueueFamilyProperties>& queue_families) const {
    QueueSelection best{};
    best.family_count = static_cast<uint32_t>(queue_families.size());

    const auto supports_transfer = [](VkQueueFlags flags) {
        // Graphics and compute queues support transfer operations even when the
        // dedicated transfer bit is not advertised by the driver.
        return (flags & VK_QUEUE_TRANSFER_BIT) != 0 || (flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0;
    };

    bool found = false;
    for (uint32_t graphics = 0; graphics < queue_families.size(); ++graphics) {
        const VkQueueFamilyProperties& graphics_family = queue_families[graphics];
        if (graphics_family.queueCount == 0 || (graphics_family.queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
            continue;
        }

        for (uint32_t compute = 0; compute < queue_families.size(); ++compute) {
            const VkQueueFamilyProperties& compute_family = queue_families[compute];
            if (compute_family.queueCount == 0 || (compute_family.queueFlags & VK_QUEUE_COMPUTE_BIT) == 0) {
                continue;
            }

            for (uint32_t copy = 0; copy < queue_families.size(); ++copy) {
                const VkQueueFamilyProperties& copy_family = queue_families[copy];
                if (copy_family.queueCount == 0 || !supports_transfer(copy_family.queueFlags)) {
                    continue;
                }

                const bool dedicated_compute = (compute_family.queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0;
                const bool dedicated_copy =
                    (copy_family.queueFlags & VK_QUEUE_TRANSFER_BIT) != 0 &&
                    (copy_family.queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == 0;

                // Queue topology is weighted independently from device type,
                // because it directly affects async scheduling quality.
                uint64_t quality_score = 0;
                quality_score += graphics != compute ? 160u : 0u;
                quality_score += graphics != copy ? 160u : 0u;
                quality_score += compute != copy ? 80u : 0u;
                quality_score += dedicated_compute ? 650u : 0u;
                quality_score += dedicated_copy ? 900u : 0u;
                if (!dedicated_copy && (copy_family.queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                    quality_score += 350u;
                }
                if (!dedicated_copy && (copy_family.queueFlags & VK_QUEUE_COMPUTE_BIT) == 0) {
                    quality_score += 180u;
                }
                quality_score += static_cast<uint64_t>(std::min(graphics_family.queueCount, 4u)) * 20u;
                quality_score += static_cast<uint64_t>(std::min(compute_family.queueCount, 4u)) * 12u;
                quality_score += static_cast<uint64_t>(std::min(copy_family.queueCount, 4u)) * 12u;

                if (!found || quality_score > best.quality_score) {
                    found = true;
                    best.graphics = graphics;
                    best.compute = compute;
                    best.copy = copy;
                    best.quality_score = quality_score;
                    best.dedicated_compute = dedicated_compute;
                    best.dedicated_copy = dedicated_copy;
                }
            }
        }
    }

    return best;
}

VulkanDevice::FeatureState VulkanDevice::query_supported_features(VkPhysicalDevice device) const {
    VkPhysicalDeviceFeatures2 supported2{};
    supported2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

    VkPhysicalDeviceVulkan12Features supported12{};
    supported12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;

    VkPhysicalDeviceVulkan13Features supported13{};
    supported13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

    supported2.pNext = &supported12;
    supported12.pNext = &supported13;
    vkGetPhysicalDeviceFeatures2(device, &supported2);

    FeatureState features{};
    features.sampler_anisotropy = supported2.features.samplerAnisotropy == VK_TRUE;
    features.timeline_semaphore = supported12.timelineSemaphore == VK_TRUE;
    features.synchronization2 = supported13.synchronization2 == VK_TRUE;
    features.dynamic_rendering = supported13.dynamicRendering == VK_TRUE;
    features.descriptor_indexing = supported12.descriptorIndexing == VK_TRUE;
    features.buffer_device_address = supported12.bufferDeviceAddress == VK_TRUE;
    features.swapchain = device_supports_extension(device, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    return features;
}

uint64_t VulkanDevice::score_physical_device(const PhysicalDeviceCandidate& candidate) const {
    constexpr uint64_t kGib = 1024ull * 1024ull * 1024ull;

    // This is a deterministic capability heuristic, not a promise of runtime FPS.
    uint64_t score = 0;
    switch (candidate.properties.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            score += 1200u;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            score += 1050u;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            score += 850u;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_OTHER:
            score += 700u;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            score += 500u;
            break;
        default:
            score += 500u;
            break;
    }

    // Capacity is capped so an unusually large shared-memory heap cannot dominate
    // every other capability dimension.
    score += std::min(candidate.device_local_memory_bytes / kGib, 64ull) * 45u;
    score += std::min(candidate.host_visible_memory_bytes / kGib, 32ull) * 8u;
    score += candidate.queues.quality_score;
    score += std::min(static_cast<uint64_t>(candidate.queues.family_count), 16ull) * 10u;

    if (candidate.features.timeline_semaphore)
        score += 300u;
    if (candidate.features.synchronization2)
        score += 300u;
    if (candidate.features.dynamic_rendering)
        score += 300u;
    if (candidate.features.descriptor_indexing)
        score += 260u;
    if (candidate.features.buffer_device_address)
        score += 240u;
    if (candidate.features.sampler_anisotropy)
        score += 80u;
    if (candidate.features.swapchain)
        score += 500u;

    const VkPhysicalDeviceLimits& limits = candidate.properties.limits;
    score += std::min(static_cast<uint64_t>(limits.maxBoundDescriptorSets), 32ull) * 4u;
    score += std::min(static_cast<uint64_t>(limits.maxPushConstantsSize), 512ull) / 16u;
    score += std::min(static_cast<uint64_t>(limits.maxUniformBufferRange) / 4096ull, 256ull);
    score += std::min(static_cast<uint64_t>(limits.maxComputeWorkGroupInvocations), 1024ull) / 16u;
    score += std::min(static_cast<uint64_t>(limits.maxComputeSharedMemorySize), 65536ull) / 1024u;
    score += std::min(static_cast<uint64_t>(limits.maxImageDimension2D), 16384ull) / 256u;

    const uint32_t api_major = candidate.properties.apiVersion >> 22u;
    const uint32_t api_minor = (candidate.properties.apiVersion >> 12u) & 0x3ffu;
    score += (api_major > 1 || (api_major == 1 && api_minor >= 4)) ? 220u : 100u;
    return score;
}

VulkanDevice::PhysicalDeviceCandidate VulkanDevice::evaluate_physical_device(VkPhysicalDevice device,
                                                                             uint32_t index) const {
    PhysicalDeviceCandidate candidate{};
    candidate.index = index;
    candidate.device = device;
    vkGetPhysicalDeviceProperties(device, &candidate.properties);
    vkGetPhysicalDeviceMemoryProperties(device, &candidate.memory_properties);

    if (candidate.properties.apiVersion < VK_API_VERSION_1_3) {
        candidate.rejection_reason = "Vulkan 1.3 is unavailable";
        return candidate;
    }

    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, nullptr);
    std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
    if (queue_family_count != 0) {
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queue_family_count, queue_families.data());
    }
    candidate.queues = choose_queue_families(queue_families);
    if (candidate.queues.graphics == kInvalidIndex || candidate.queues.compute == kInvalidIndex ||
        candidate.queues.copy == kInvalidIndex) {
        candidate.rejection_reason = "Graphics, compute, and copy queue families are required";
        return candidate;
    }

    candidate.features = query_supported_features(device);
    if (!candidate.features.timeline_semaphore) {
        candidate.rejection_reason = "Timeline semaphores are unavailable";
        return candidate;
    }

    const MemorySummary memory = summarize_memory(candidate.memory_properties);
    candidate.device_local_memory_bytes = memory.device_local_bytes;
    candidate.host_visible_memory_bytes = memory.host_visible_bytes;
    candidate.score = score_physical_device(candidate);
    candidate.viable = true;
    return candidate;
}

Result VulkanDevice::select_physical_device(const DeviceCreateDesc& desc, QueueSelection& selection,
                                            FeatureState& supported_features) {
    uint32_t device_count = 0;
    VkResult result = vkEnumeratePhysicalDevices(instance_, &device_count, nullptr);
    if (result != VK_SUCCESS) {
        return make_vulkan_result(result, "vkEnumeratePhysicalDevices");
    }
    if (device_count == 0) {
        return Result::failure(ResultCode::kNotFound, "No Vulkan physical device was found");
    }

    std::vector<VkPhysicalDevice> devices(device_count);
    result = vkEnumeratePhysicalDevices(instance_, &device_count, devices.data());
    if (result != VK_SUCCESS) {
        return make_vulkan_result(result, "vkEnumeratePhysicalDevices");
    }

    const uint32_t preferred_index = desc.preferred_device_index;
    if (preferred_index != std::numeric_limits<uint32_t>::max() && preferred_index >= devices.size()) {
        return Result::failure(ResultCode::kInvalidArgument, "preferred_device_index is out of range");
    }

    std::vector<PhysicalDeviceCandidate> candidates;
    candidates.reserve(devices.size());
    for (uint32_t index = 0; index < devices.size(); ++index) {
        candidates.push_back(evaluate_physical_device(devices[index], index));
    }

#if AEG_RHI_ENABLE_VALIDATION || AEG_RHI_ENABLE_DEBUG_UTILS
    if (desc.enable_validation || desc.enable_debug_utils) {
        for (const PhysicalDeviceCandidate& candidate : candidates) {
            if (candidate.viable) {
                AEG_LOG_DEBUG(aeg::logging::Category::kRhi,
                              "PhysicalDevice[{}] {} type={} score={} localMemory={} "
                              "dedicatedCompute={} dedicatedCopy={}",
                              candidate.index, std::string_view{candidate.properties.deviceName},
                              physical_device_type_name(candidate.properties.deviceType), candidate.score,
                              candidate.device_local_memory_bytes, candidate.queues.dedicated_compute,
                              candidate.queues.dedicated_copy);
            } else {
                AEG_LOG_DEBUG(aeg::logging::Category::kRhi, "PhysicalDevice[{}] {} type={} rejected={}",
                              candidate.index, std::string_view{candidate.properties.deviceName},
                              physical_device_type_name(candidate.properties.deviceType), candidate.rejection_reason);
            }
        }
    }
#endif

    const PhysicalDeviceCandidate* selected = nullptr;
    if (preferred_index != std::numeric_limits<uint32_t>::max()) {
        const PhysicalDeviceCandidate& preferred = candidates[preferred_index];
        if (!preferred.viable) {
            return Result::failure(ResultCode::kUnsupported, "preferred_device_index " +
                                                                 std::to_string(preferred_index) +
                                                                 " is not suitable: " + preferred.rejection_reason);
        }
        selected = &preferred;
    } else {
        auto is_better = [](const PhysicalDeviceCandidate& a, const PhysicalDeviceCandidate& b) -> bool {
            const auto tuple_a = std::tie(a.viable, a.score, a.device_local_memory_bytes, a.properties.apiVersion);
            const auto tuple_b = std::tie(b.viable, b.score, b.device_local_memory_bytes, b.properties.apiVersion);

            if (tuple_a != tuple_b) {
                return tuple_a < tuple_b;
            }

            return a.index > b.index;
        };

        const auto best = std::max_element(candidates.begin(), candidates.end(), is_better);

        if (best != candidates.end() && best->viable) {
            selected = &*best;
        }
    }

    if (selected == nullptr) {
        std::string message = "No physical device satisfies AEG RHI requirements";
        for (const PhysicalDeviceCandidate& candidate : candidates) {
            message += " [" + std::to_string(candidate.index) + "] " + std::string(candidate.properties.deviceName) +
                       ": " + (candidate.rejection_reason.empty() ? "rejected" : candidate.rejection_reason);
        }
        return Result::failure(ResultCode::kUnsupported, std::move(message));
    }

    physical_device_ = selected->device;
    physical_properties_ = selected->properties;
    memory_properties_ = selected->memory_properties;
    selection = selected->queues;
    supported_features = selected->features;

    capabilities_.api_version = physical_properties_.apiVersion;
    capabilities_.vendor_id = physical_properties_.vendorID;
    capabilities_.device_id = physical_properties_.deviceID;
    capabilities_.device_name = physical_properties_.deviceName;
    capabilities_.device_type = to_rhi_physical_device_type(physical_properties_.deviceType);
    capabilities_.device_local_memory_bytes = selected->device_local_memory_bytes;
    capabilities_.host_visible_memory_bytes = selected->host_visible_memory_bytes;
    capabilities_.queue_family_count = selected->queues.family_count;
    capabilities_.dedicated_compute_queue = selected->queues.dedicated_compute;
    capabilities_.dedicated_copy_queue = selected->queues.dedicated_copy;
    capabilities_.timeline_semaphore = selected->features.timeline_semaphore;
    capabilities_.synchronization2 = selected->features.synchronization2;
    capabilities_.dynamic_rendering = selected->features.dynamic_rendering;
    capabilities_.descriptor_indexing = selected->features.descriptor_indexing;
    capabilities_.buffer_device_address = selected->features.buffer_device_address;
    capabilities_.swapchain = selected->features.swapchain;
    capabilities_.max_uniform_buffer_range = physical_properties_.limits.maxUniformBufferRange;
    capabilities_.non_coherent_atom_size = physical_properties_.limits.nonCoherentAtomSize;

#if AEG_RHI_ENABLE_VALIDATION || AEG_RHI_ENABLE_DEBUG_UTILS
    if (desc.enable_validation || desc.enable_debug_utils) {
        AEG_LOG_INFO(aeg::logging::Category::kRhi, "Selected PhysicalDevice[{}] {} score={}", selected->index,
                     std::string_view{selected->properties.deviceName}, selected->score);
    }
#endif

    return Result::success();
}

Result VulkanDevice::create_allocator(const FeatureState& supported_features) {
    VmaAllocatorCreateInfo allocator_info{};
    allocator_info.physicalDevice = physical_device_;
    allocator_info.device = device_;
    allocator_info.instance = instance_;
    allocator_info.vulkanApiVersion = VK_API_VERSION_1_3;
    if (supported_features.buffer_device_address) {
        allocator_info.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    }

    const VkResult result = vmaCreateAllocator(&allocator_info, &allocator_);
    if (result != VK_SUCCESS) {
        return make_vulkan_result(result, "vmaCreateAllocator");
    }
    return Result::success();
}

Result VulkanDevice::create_logical_device(const QueueSelection& selection, const FeatureState& supported_features) {
    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_family_count, nullptr);
    std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
    if (queue_family_count != 0) {
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &queue_family_count, queue_families.data());
    }

    // Enable one queue from every non-empty family. Surface creation happens
    // after device creation, so this keeps a separate present family available
    // without requiring a second logical device.
    std::vector<uint32_t> unique_families;
    unique_families.reserve(queue_families.size());
    for (uint32_t family = 0; family < queue_families.size(); ++family) {
        if (queue_families[family].queueCount != 0) {
            unique_families.push_back(family);
        }
    }
    if (unique_families.empty() ||
        std::find(unique_families.begin(), unique_families.end(), selection.graphics) == unique_families.end() ||
        std::find(unique_families.begin(), unique_families.end(), selection.compute) == unique_families.end() ||
        std::find(unique_families.begin(), unique_families.end(), selection.copy) == unique_families.end()) {
        return Result::failure(ResultCode::kInvalidState, "Selected queue families are not available");
    }

    std::vector<float> priorities(unique_families.size(), 1.0f);
    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    queue_infos.reserve(unique_families.size());
    for (size_t index = 0; index < unique_families.size(); ++index) {
        VkDeviceQueueCreateInfo queue_info{};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = unique_families[index];
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priorities[index];
        queue_infos.push_back(queue_info);
    }

    VkPhysicalDeviceFeatures2 enabled2{};
    enabled2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    enabled2.features.samplerAnisotropy = supported_features.sampler_anisotropy ? VK_TRUE : VK_FALSE;

    VkPhysicalDeviceVulkan12Features enabled12{};
    enabled12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    enabled12.timelineSemaphore = VK_TRUE;
    enabled12.descriptorIndexing = supported_features.descriptor_indexing ? VK_TRUE : VK_FALSE;
    enabled12.bufferDeviceAddress = supported_features.buffer_device_address ? VK_TRUE : VK_FALSE;

    VkPhysicalDeviceVulkan13Features enabled13{};
    enabled13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    enabled13.synchronization2 = supported_features.synchronization2 ? VK_TRUE : VK_FALSE;
    enabled13.dynamicRendering = supported_features.dynamic_rendering ? VK_TRUE : VK_FALSE;

    enabled2.pNext = &enabled12;
    enabled12.pNext = &enabled13;

    std::vector<const char*> device_extensions;
    if (supported_features.swapchain) {
        device_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }

    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.pNext = &enabled2;
    device_info.queueCreateInfoCount = static_cast<uint32_t>(queue_infos.size());
    device_info.pQueueCreateInfos = queue_infos.data();
    device_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
    device_info.ppEnabledExtensionNames = device_extensions.data();

    const VkResult result = vkCreateDevice(physical_device_, &device_info, nullptr, &device_);
    if (result != VK_SUCCESS) {
        return make_vulkan_result(result, "vkCreateDevice");
    }

    Result allocator_result = create_allocator(supported_features);
    if (!allocator_result) {
        return allocator_result;
    }

#if AEG_RHI_ENABLE_DEBUG_NAMES
    if (debug_utils_enabled_) {
        set_debug_utils_object_name_ = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
            vkGetDeviceProcAddr(device_, "vkSetDebugUtilsObjectNameEXT"));
    }
#endif

    enabled_features_ = supported_features;
    capabilities_.timeline_semaphore = true;
    capabilities_.synchronization2 = supported_features.synchronization2;
    capabilities_.dynamic_rendering = supported_features.dynamic_rendering;
    capabilities_.descriptor_indexing = supported_features.descriptor_indexing;
    capabilities_.buffer_device_address = supported_features.buffer_device_address;
    capabilities_.swapchain = supported_features.swapchain;
    capabilities_.max_descriptor_sets = physical_properties_.limits.maxBoundDescriptorSets;
    capabilities_.max_push_constants_size = physical_properties_.limits.maxPushConstantsSize;

    enabled_queue_families_ = std::move(unique_families);

    Result queue_result = create_queue_state(queues_[0], QueueClass::kGraphics, selection.graphics, 1);
    if (!queue_result) {
        return queue_result;
    }
    queue_result = create_queue_state(queues_[1], QueueClass::kCompute, selection.compute, 2);
    if (!queue_result) {
        return queue_result;
    }
    queue_result = create_queue_state(queues_[2], QueueClass::kCopy, selection.copy, 3);
    if (!queue_result) {
        return queue_result;
    }

    return Result::success();
}

Result VulkanDevice::create_queue_state(QueueState& state, QueueClass logical_class, uint32_t family_index,
                                        uint32_t handle_value) {
    state.logical_class = logical_class;
    state.family_index = family_index;
    state.timeline_handle = TimelineHandle{handle_value};
    vkGetDeviceQueue(device_, family_index, 0, &state.queue);

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = family_index;
    VkResult result = vkCreateCommandPool(device_, &pool_info, nullptr, &state.command_pool);
    if (result != VK_SUCCESS) {
        return make_vulkan_result(result, "vkCreateCommandPool");
    }

    VkSemaphoreTypeCreateInfo timeline_info{};
    timeline_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timeline_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timeline_info.initialValue = 0;

    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semaphore_info.pNext = &timeline_info;
    result = vkCreateSemaphore(device_, &semaphore_info, nullptr, &state.timeline);
    if (result != VK_SUCCESS) {
        return make_vulkan_result(result, "vkCreateSemaphore");
    }

    return Result::success();
}

Result VulkanDevice::initialize(const DeviceCreateDesc& desc) {
    Result result = create_instance(desc);
    if (!result) {
        return result;
    }

    QueueSelection selection{};
    FeatureState supported_features{};
    result = select_physical_device(desc, selection, supported_features);
    if (!result) {
        return result;
    }

    return create_logical_device(selection, supported_features);
}

QueueState* VulkanDevice::queue_state(QueueClass queue) {
    switch (queue) {
        case QueueClass::kAuto:
        case QueueClass::kGraphics:
        case QueueClass::kPresent:
            return &queues_[0];
        case QueueClass::kCompute:
            return &queues_[1];
        case QueueClass::kCopy:
            return &queues_[2];
    }
    return nullptr;
}

QueueState* VulkanDevice::timeline_queue(TimelineHandle timeline) {
    for (QueueState& state : queues_) {
        if (state.timeline_handle == timeline) {
            return &state;
        }
    }
    return nullptr;
}

const QueueState* VulkanDevice::timeline_queue(TimelineHandle timeline) const {
    for (const QueueState& state : queues_) {
        if (state.timeline_handle == timeline) {
            return &state;
        }
    }
    return nullptr;
}

Result VulkanDevice::create_frame_slot_sync(FrameSlot& frame) {
    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkResult result = vkCreateSemaphore(device_, &semaphore_info, nullptr, &frame.image_available);
    if (result != VK_SUCCESS) {
        return make_vulkan_result(result, "vkCreateSemaphore(imageAvailable)");
    }

    result = vkCreateSemaphore(device_, &semaphore_info, nullptr, &frame.render_finished);
    if (result != VK_SUCCESS) {
        vkDestroySemaphore(device_, frame.image_available, nullptr);
        frame.image_available = VK_NULL_HANDLE;
        return make_vulkan_result(result, "vkCreateSemaphore(renderFinished)");
    }

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    result = vkCreateFence(device_, &fence_info, nullptr, &frame.in_flight);
    if (result != VK_SUCCESS) {
        vkDestroySemaphore(device_, frame.render_finished, nullptr);
        vkDestroySemaphore(device_, frame.image_available, nullptr);
        frame.render_finished = VK_NULL_HANDLE;
        frame.image_available = VK_NULL_HANDLE;
        return make_vulkan_result(result, "vkCreateFence(frame)");
    }

    return Result::success();
}

void VulkanDevice::destroy_frame_slot_sync(FrameSlot& frame) noexcept {
    if (device_ != VK_NULL_HANDLE) {
        if (frame.image_available != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, frame.image_available, nullptr);
        }
        if (frame.render_finished != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, frame.render_finished, nullptr);
        }
        if (frame.in_flight != VK_NULL_HANDLE) {
            vkDestroyFence(device_, frame.in_flight, nullptr);
        }
    }
    frame.image_available = VK_NULL_HANDLE;
    frame.render_finished = VK_NULL_HANDLE;
    frame.in_flight = VK_NULL_HANDLE;
    frame.image_index = kRemaining;
    frame.acquired = false;
    frame.submitted = false;
    frame.completion = {};
}

void VulkanDevice::destroy_swapchain_native(SwapchainResource& resource) noexcept {
    for (FrameSlot& frame : resource.frames) {
        destroy_frame_slot_sync(frame);
    }
    resource.frames.clear();

    for (const SwapchainImage& image : resource.images) {
        if (TextureViewResource* view = texture_views_.get(image.view); view != nullptr) {
            if (view->view != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
                vkDestroyImageView(device_, view->view, nullptr);
            }
            (void)texture_views_.erase(image.view);
        }

        if (TextureResource* texture = textures_.get(image.texture); texture != nullptr) {
            if (!texture->external && texture->image != VK_NULL_HANDLE && allocator_ != nullptr) {
                vmaDestroyImage(allocator_, texture->image, texture->allocation.handle);
            }
            (void)textures_.erase(image.texture);
        }
    }
    resource.images.clear();
    resource.image_in_flight_frame.clear();

    if (resource.swapchain != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, resource.swapchain, nullptr);
    }
    resource.swapchain = VK_NULL_HANDLE;
    resource.surface = {};
    resource.format = VK_FORMAT_UNDEFINED;
    resource.color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    resource.present_mode = VK_PRESENT_MODE_FIFO_KHR;
    resource.extent = {};
    resource.current_frame = 0;
    resource.generation = 0;
    resource.needs_recreate = false;
}

Result VulkanDevice::validate_swapchain_references(const SwapchainResource& resource) const {
    for (const SwapchainImage& image : resource.images) {
        bool has_external_view = false;
        texture_views_.for_each([&](const TextureViewResource& view) {
            if (view.desc.texture == image.texture &&
                (!view.owned_by_swapchain || view.owner_swapchain != resource.self)) {
                has_external_view = true;
            }
        });
        if (has_external_view) {
            return Result::failure(ResultCode::kInvalidState, "Swapchain image has a user-owned texture view");
        }

        bool is_bound = false;
        binding_sets_.for_each([&](const BindingSetResource& set) {
            for (const BindingWrite& write : set.writes) {
                if (write.resource.texture_view == image.view) {
                    is_bound = true;
                }
            }
        });
        if (is_bound) {
            return Result::failure(ResultCode::kInvalidState,
                                   "Swapchain image view is still referenced by a binding set");
        }
    }
    return Result::success();
}

Result VulkanDevice::validate_frame_token(const FrameToken& frame, SwapchainResource*& swapchain) {
    swapchain = nullptr;
    if (!frame.is_valid()) {
        return Result::failure(ResultCode::kInvalidArgument, "Invalid frame token");
    }

    SwapchainResource* resource = swapchains_.get(frame.swapchain);
    if (resource == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid swapchain handle in frame token");
    }
    if (resource->generation != frame.generation) {
        return Result::failure(ResultCode::kInvalidState, "Frame token belongs to an old swapchain generation");
    }
    if (frame.frame_slot >= resource->frames.size() || frame.image_index >= resource->images.size()) {
        return Result::failure(ResultCode::kInvalidArgument, "Frame token indices are out of range");
    }
    FrameSlot& slot = resource->frames[frame.frame_slot];
    if (!slot.acquired || slot.image_index != frame.image_index) {
        return Result::failure(ResultCode::kInvalidState, "Frame token is no longer acquired");
    }

    swapchain = resource;
    return Result::success();
}

Result VulkanDevice::initialize_swapchain(SwapchainResource& resource, SwapchainHandle self, const SwapchainDesc& desc,
                                          VkSwapchainKHR old_swapchain) {
    if (device_ == VK_NULL_HANDLE || physical_device_ == VK_NULL_HANDLE) {
        return Result::failure(ResultCode::kInvalidState, "Vulkan device is not initialized");
    }
    if (!surface_extension_enabled_) {
        return Result::failure(ResultCode::kUnsupported, "Vulkan surface support is not available");
    }
    if (!capabilities_.swapchain) {
        return Result::failure(ResultCode::kUnsupported, "VK_KHR_swapchain is not available on the selected device");
    }
    if (!desc.surface.is_valid()) {
        return Result::failure(ResultCode::kInvalidArgument, "Swapchain surface is invalid");
    }
    if (desc.min_image_count == 0 || desc.frames_in_flight == 0) {
        return Result::failure(ResultCode::kInvalidArgument,
                               "Swapchain image and frame counts must be greater than zero");
    }
    if (!has_flag(desc.image_capabilities, TextureCapability::kColorAttachment)) {
        return Result::failure(ResultCode::kInvalidArgument, "Swapchain images must support color attachment usage");
    }

    const SurfaceResource* surface = surfaces_.get(desc.surface);
    if (surface == nullptr || surface->surface == VK_NULL_HANDLE || surface->present_family == kInvalidIndex ||
        surface->present_queue == VK_NULL_HANDLE) {
        return Result::failure(ResultCode::kInvalidState, "Swapchain surface is not available for presentation");
    }

    VkSurfaceCapabilitiesKHR surface_capabilities{};
    VkResult result =
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface->surface, &surface_capabilities);
    if (result != VK_SUCCESS) {
        return make_vulkan_result(result, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    }

    uint32_t surface_format_count = 0;
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface->surface, &surface_format_count, nullptr);
    if (result != VK_SUCCESS) {
        return make_vulkan_result(result, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    }
    if (surface_format_count == 0) {
        return Result::failure(ResultCode::kUnsupported, "Presentation surface exposes no formats");
    }
    std::vector<VkSurfaceFormatKHR> surface_formats(surface_format_count);
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface->surface, &surface_format_count,
                                                  surface_formats.data());
    if (result != VK_SUCCESS) {
        return make_vulkan_result(result, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    }
    surface_formats.resize(surface_format_count);

    VkSurfaceFormatKHR selected_format{};
    const auto requested_format = to_vk_format(desc.format);
    if (desc.format != Format::kUnknown && !requested_format.has_value()) {
        return Result::failure(ResultCode::kInvalidArgument, "Requested swapchain format is not supported by the RHI");
    }

    if (surface_formats.size() == 1 && surface_formats.front().format == VK_FORMAT_UNDEFINED) {
        selected_format.format = requested_format.value_or(VK_FORMAT_B8G8R8A8_UNORM);
        selected_format.colorSpace = surface_formats.front().colorSpace;
    } else {
        const auto choose_format = [&](VkFormat format) {
            return std::find_if(surface_formats.begin(), surface_formats.end(),
                                [format](const VkSurfaceFormatKHR& candidate) { return candidate.format == format; });
        };

        if (requested_format.has_value()) {
            const auto match = choose_format(requested_format.value());
            if (match == surface_formats.end()) {
                return Result::failure(ResultCode::kUnsupported, "Requested swapchain format is unavailable");
            }
            selected_format = *match;
        } else {
            const std::array<VkFormat, 4> preferred_formats{VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                                                            VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB};
            const auto match = std::find_if(preferred_formats.begin(), preferred_formats.end(), [&](VkFormat format) {
                return choose_format(format) != surface_formats.end();
            });
            if (match != preferred_formats.end()) {
                selected_format = *choose_format(*match);
            } else {
                const auto first_supported = std::find_if(
                    surface_formats.begin(), surface_formats.end(), [](const VkSurfaceFormatKHR& candidate) {
                        return from_vk_format(candidate.format) != Format::kUnknown;
                    });
                if (first_supported == surface_formats.end()) {
                    return Result::failure(ResultCode::kUnsupported,
                                           "Surface formats are not representable by the RHI");
                }
                selected_format = *first_supported;
            }
        }
    }

    const Format rhi_format = from_vk_format(selected_format.format);
    if (rhi_format == Format::kUnknown || is_depth_format(rhi_format)) {
        return Result::failure(ResultCode::kUnsupported,
                               "Selected swapchain format is not a color format supported by the RHI");
    }

    uint32_t present_mode_count = 0;
    result =
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface->surface, &present_mode_count, nullptr);
    if (result != VK_SUCCESS) {
        return make_vulkan_result(result, "vkGetPhysicalDeviceSurfacePresentModesKHR");
    }
    if (present_mode_count == 0) {
        return Result::failure(ResultCode::kUnsupported, "Presentation surface exposes no present modes");
    }
    std::vector<VkPresentModeKHR> present_modes(present_mode_count);
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface->surface, &present_mode_count,
                                                       present_modes.data());
    if (result != VK_SUCCESS) {
        return make_vulkan_result(result, "vkGetPhysicalDeviceSurfacePresentModesKHR");
    }
    present_modes.resize(present_mode_count);

    const VkPresentModeKHR requested_present_mode = to_vk_present_mode(desc.present_mode);
    VkPresentModeKHR selected_present_mode = VK_PRESENT_MODE_FIFO_KHR;
    if (std::find(present_modes.begin(), present_modes.end(), requested_present_mode) != present_modes.end()) {
        selected_present_mode = requested_present_mode;
    } else if (std::find(present_modes.begin(), present_modes.end(), VK_PRESENT_MODE_FIFO_KHR) == present_modes.end()) {
        selected_present_mode = present_modes.front();
    }

    Extent2D selected_extent{};
    if (surface_capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        selected_extent = {surface_capabilities.currentExtent.width, surface_capabilities.currentExtent.height};
    } else {
        if (desc.extent.width == 0 || desc.extent.height == 0) {
            return Result::failure(ResultCode::kInvalidArgument,
                                   "Swapchain extent is required when the surface extent is undefined");
        }
        selected_extent.width = std::clamp(desc.extent.width, surface_capabilities.minImageExtent.width,
                                           surface_capabilities.maxImageExtent.width);
        selected_extent.height = std::clamp(desc.extent.height, surface_capabilities.minImageExtent.height,
                                            surface_capabilities.maxImageExtent.height);
    }
    if (selected_extent.width == 0 || selected_extent.height == 0) {
        return Result::failure(ResultCode::kInvalidState, "Surface returned a zero swapchain extent");
    }

    const VkImageUsageFlags image_usage = to_vk_image_usage(desc.image_capabilities);
    if (image_usage == 0 || (image_usage & ~surface_capabilities.supportedUsageFlags) != 0) {
        return Result::failure(ResultCode::kUnsupported,
                               "Requested swapchain image usage is not supported by the surface");
    }

    uint32_t image_count = std::max(desc.min_image_count, surface_capabilities.minImageCount);
    if (surface_capabilities.maxImageCount != 0) {
        image_count = std::min(image_count, surface_capabilities.maxImageCount);
    }
    if (image_count == 0) {
        return Result::failure(ResultCode::kUnsupported, "Surface cannot provide swapchain images");
    }

    const auto choose_composite_alpha = [&] {
        constexpr std::array<VkCompositeAlphaFlagBitsKHR, 4> kCandidates{
            VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
        for (const VkCompositeAlphaFlagBitsKHR candidate : kCandidates) {
            if ((surface_capabilities.supportedCompositeAlpha & candidate) != 0) {
                return candidate;
            }
        }
        return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    }();

    const uint32_t graphics_family = queues_[0].family_index;
    const uint32_t present_family = surface->present_family;
    if (graphics_family == kInvalidIndex || present_family == kInvalidIndex) {
        return Result::failure(ResultCode::kInvalidState, "Graphics or presentation queue family is unavailable");
    }

    std::array<uint32_t, 2> sharing_families{graphics_family, present_family};
    VkSwapchainCreateInfoKHR swapchain_info{};
    swapchain_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchain_info.surface = surface->surface;
    swapchain_info.minImageCount = image_count;
    swapchain_info.imageFormat = selected_format.format;
    swapchain_info.imageColorSpace = selected_format.colorSpace;
    swapchain_info.imageExtent = VkExtent2D{selected_extent.width, selected_extent.height};
    swapchain_info.imageArrayLayers = 1;
    swapchain_info.imageUsage = image_usage;
    if (graphics_family != present_family) {
        swapchain_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        swapchain_info.queueFamilyIndexCount = 2;
        swapchain_info.pQueueFamilyIndices = sharing_families.data();
    } else {
        swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    swapchain_info.preTransform = surface_capabilities.currentTransform;
    swapchain_info.compositeAlpha = choose_composite_alpha;
    swapchain_info.presentMode = selected_present_mode;
    swapchain_info.clipped = desc.clipped ? VK_TRUE : VK_FALSE;
    swapchain_info.oldSwapchain = old_swapchain;

    VkSwapchainKHR vk_swapchain = VK_NULL_HANDLE;
    result = vkCreateSwapchainKHR(device_, &swapchain_info, nullptr, &vk_swapchain);
    if (result != VK_SUCCESS) {
        return make_vulkan_result(result, "vkCreateSwapchainKHR");
    }

    resource.self = self;
    resource.surface = desc.surface;
    resource.swapchain = vk_swapchain;
    resource.format = selected_format.format;
    resource.color_space = selected_format.colorSpace;
    resource.present_mode = selected_present_mode;
    resource.extent = VkExtent2D{selected_extent.width, selected_extent.height};
    resource.current_frame = 0;
    resource.needs_recreate = false;

#if AEG_RHI_ENABLE_DEBUG_NAMES
    set_debug_name(VK_OBJECT_TYPE_SWAPCHAIN_KHR, vk_swapchain, desc.name);
#endif

    uint32_t actual_image_count = 0;
    result = vkGetSwapchainImagesKHR(device_, vk_swapchain, &actual_image_count, nullptr);
    if (result != VK_SUCCESS || actual_image_count == 0) {
        const Result error = result == VK_SUCCESS
                                 ? Result::failure(ResultCode::kUnsupported, "Swapchain returned no images")
                                 : make_vulkan_result(result, "vkGetSwapchainImagesKHR");
        destroy_swapchain_native(resource);
        return error;
    }
    std::vector<VkImage> vk_images(actual_image_count);
    result = vkGetSwapchainImagesKHR(device_, vk_swapchain, &actual_image_count, vk_images.data());
    if (result != VK_SUCCESS) {
        const Result error = make_vulkan_result(result, "vkGetSwapchainImagesKHR");
        destroy_swapchain_native(resource);
        return error;
    }
    vk_images.resize(actual_image_count);

    const uint32_t frame_count = std::min(desc.frames_in_flight, actual_image_count);
    if (frame_count == 0) {
        destroy_swapchain_native(resource);
        return Result::failure(ResultCode::kInvalidArgument, "Swapchain must have at least one frame slot");
    }
    resource.frames.resize(frame_count);
    for (FrameSlot& frame : resource.frames) {
        const Result frame_result = create_frame_slot_sync(frame);
        if (!frame_result) {
            destroy_swapchain_native(resource);
            return frame_result;
        }
    }

    resource.images.reserve(vk_images.size());
    resource.image_in_flight_frame.assign(vk_images.size(), kRemaining);
    TextureDesc texture_desc{};
    texture_desc.extent = {selected_extent.width, selected_extent.height, 1};
    texture_desc.format = rhi_format;
    texture_desc.capabilities = desc.image_capabilities;
    texture_desc.memory = MemoryClass::kDeviceLocal;
    texture_desc.flags = ResourceFlags::kExternal;

    for (uint32_t index = 0; index < vk_images.size(); ++index) {
        TextureResource texture_resource{};
        texture_resource.image = vk_images[index];
        texture_resource.desc = texture_desc;
        texture_resource.external = true;
        texture_resource.owner_swapchain = self;
        const TextureHandle texture_handle = textures_.insert(std::move(texture_resource));

        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = vk_images[index];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = selected_format.format;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.baseMipLevel = 0;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.baseArrayLayer = 0;
        view_info.subresourceRange.layerCount = 1;

        TextureViewResource view_resource{};
        view_resource.desc.texture = texture_handle;
        view_resource.desc.kind = TextureViewKind::kColorAttachment;
        view_resource.desc.format = rhi_format;
        view_resource.desc.subresources.aspects = Aspect::kColor;
        view_resource.desc.subresources.base_mip = 0;
        view_resource.desc.subresources.mip_count = 1;
        view_resource.desc.subresources.base_layer = 0;
        view_resource.desc.subresources.layer_count = 1;
        result = vkCreateImageView(device_, &view_info, nullptr, &view_resource.view);
        if (result != VK_SUCCESS) {
            (void)textures_.erase(texture_handle);
            const Result error = make_vulkan_result(result, "vkCreateImageView(swapchain)");
            destroy_swapchain_native(resource);
            return error;
        }
        view_resource.owned_by_swapchain = true;
        view_resource.owner_swapchain = self;
        const TextureViewHandle view_handle = texture_views_.insert(std::move(view_resource));
        resource.images.push_back(SwapchainImage{texture_handle, view_handle});
    }

    return Result::success();
}

ResultValue<SurfaceHandle> VulkanDevice::create_surface(const SurfaceDesc& desc) {
    if (instance_ == VK_NULL_HANDLE || physical_device_ == VK_NULL_HANDLE || device_ == VK_NULL_HANDLE) {
        return {Result::failure(ResultCode::kInvalidState, "Vulkan device is not initialized"), {}};
    }
    if (!surface_extension_enabled_) {
        return {Result::failure(ResultCode::kUnsupported, "Vulkan surface support is not available"), {}};
    }

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkResult result = VK_SUCCESS;
    switch (desc.type) {
        case SurfaceType::kWin32:
#if defined(VK_USE_PLATFORM_WIN32_KHR)
            if (!win32_surface_extension_enabled_) {
                return {Result::failure(ResultCode::kUnsupported, "VK_KHR_win32_surface is not available"), {}};
            }
            if (desc.native_window_handle == nullptr) {
                return {Result::failure(ResultCode::kInvalidArgument, "Win32 surface requires a native window handle"),
                        {}};
            }
            {
                VkWin32SurfaceCreateInfoKHR create_info{};
                create_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
                create_info.hinstance = desc.native_display_handle != nullptr
                                            ? reinterpret_cast<HINSTANCE>(desc.native_display_handle)
                                            : GetModuleHandleW(nullptr);
                create_info.hwnd = reinterpret_cast<HWND>(desc.native_window_handle);
                result = vkCreateWin32SurfaceKHR(instance_, &create_info, nullptr, &surface);
            }
#else
            return {Result::failure(ResultCode::kUnsupported, "Win32 surfaces are not compiled for this platform"), {}};
#endif
            break;
        case SurfaceType::kHeadless:
#ifdef VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME
            if (!headless_surface_extension_enabled_) {
                return {Result::failure(ResultCode::kUnsupported, "VK_EXT_headless_surface is not available"), {}};
            }
            {
                VkHeadlessSurfaceCreateInfoEXT create_info{};
                create_info.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
                result = vkCreateHeadlessSurfaceEXT(instance_, &create_info, nullptr, &surface);
            }
#else
            return {
                Result::failure(ResultCode::kUnsupported, "Headless surfaces are not supported by the Vulkan headers"),
                {}};
#endif
            break;
    }

    if (result != VK_SUCCESS) {
        return {make_vulkan_result(result, "vkCreateSurface"), {}};
    }

    auto supports_present = [&](uint32_t family) {
        VkBool32 supported = VK_FALSE;
        const VkResult support_result =
            vkGetPhysicalDeviceSurfaceSupportKHR(physical_device_, family, surface, &supported);
        return support_result == VK_SUCCESS && supported == VK_TRUE;
    };

    uint32_t present_family = kInvalidIndex;
    if (queues_[0].family_index != kInvalidIndex && supports_present(queues_[0].family_index)) {
        present_family = queues_[0].family_index;
    } else {
        for (const uint32_t family : enabled_queue_families_) {
            if (supports_present(family)) {
                present_family = family;
                break;
            }
        }
    }

    if (present_family == kInvalidIndex) {
        vkDestroySurfaceKHR(instance_, surface, nullptr);
        return {
            Result::failure(ResultCode::kUnsupported, "No enabled queue family supports presentation to the surface"),
            {}};
    }

    SurfaceResource resource{};
    resource.surface = surface;
    resource.present_family = present_family;
    vkGetDeviceQueue(device_, present_family, 0, &resource.present_queue);
    if (resource.present_queue == VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface, nullptr);
        return {Result::failure(ResultCode::kInvalidState, "Presentation queue could not be retrieved"), {}};
    }

    return {Result::success(), surfaces_.insert(std::move(resource))};
}

Result VulkanDevice::destroy_surface(SurfaceHandle surface) {
    SurfaceResource* resource = surfaces_.get(surface);
    if (resource == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid surface handle");
    }

    bool referenced = false;
    swapchains_.for_each(
        [&](const SwapchainResource& swapchain) { referenced = referenced || swapchain.surface == surface; });
    if (referenced) {
        return Result::failure(ResultCode::kInvalidState, "Surface is still referenced by a swapchain");
    }

    if (resource->surface != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, resource->surface, nullptr);
    }
    (void)surfaces_.erase(surface);
    return Result::success();
}

ResultValue<SwapchainHandle> VulkanDevice::create_swapchain(const SwapchainDesc& desc) {
    if (surfaces_.get(desc.surface) == nullptr) {
        return {Result::failure(ResultCode::kNotFound, "Invalid swapchain surface handle"), {}};
    }

    const SwapchainHandle handle = swapchains_.insert(SwapchainResource{});
    SwapchainResource* resource = swapchains_.get(handle);
    resource->self = handle;
    const Result result = initialize_swapchain(*resource, handle, desc, VK_NULL_HANDLE);
    if (!result) {
        destroy_swapchain_native(*resource);
        (void)swapchains_.erase(handle);
        return {result, {}};
    }
    return {Result::success(), handle};
}

Result VulkanDevice::destroy_swapchain(SwapchainHandle swapchain) {
    SwapchainResource* resource = swapchains_.get(swapchain);
    if (resource == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid swapchain handle");
    }
    const Result reference_result = validate_swapchain_references(*resource);
    if (!reference_result) {
        return reference_result;
    }
    for (const FrameSlot& frame : resource->frames) {
        if (frame.acquired) {
            return Result::failure(ResultCode::kInvalidState, "Cannot destroy a swapchain with an acquired frame");
        }
    }

    const Result idle_result = wait_idle();
    if (!idle_result) {
        return idle_result;
    }
    destroy_swapchain_native(*resource);
    (void)swapchains_.erase(swapchain);
    return Result::success();
}

Result VulkanDevice::recreate_swapchain(SwapchainHandle swapchain, const SwapchainDesc& desc) {
    SwapchainResource* resource = swapchains_.get(swapchain);
    if (resource == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid swapchain handle");
    }
    if (desc.surface.is_valid() && desc.surface != resource->surface) {
        return Result::failure(ResultCode::kInvalidArgument, "Swapchain recreation cannot change its surface");
    }
    for (const FrameSlot& frame : resource->frames) {
        if (frame.acquired) {
            return Result::failure(ResultCode::kInvalidState, "Cannot recreate a swapchain with an acquired frame");
        }
    }
    const Result reference_result = validate_swapchain_references(*resource);
    if (!reference_result) {
        return reference_result;
    }

    const Result idle_result = wait_idle();
    if (!idle_result) {
        return idle_result;
    }

    SwapchainDesc normalized = desc;
    normalized.surface = resource->surface;
    SwapchainResource replacement{};
    replacement.generation =
        resource->generation == std::numeric_limits<uint64_t>::max() ? 1 : resource->generation + 1;
    const Result create_result = initialize_swapchain(replacement, swapchain, normalized, resource->swapchain);
    if (!create_result) {
        destroy_swapchain_native(replacement);
        return create_result;
    }

    destroy_swapchain_native(*resource);
    *resource = std::move(replacement);
    return Result::success();
}

ResultValue<SwapchainInfo> VulkanDevice::get_swapchain_info(SwapchainHandle swapchain) const {
    const SwapchainResource* resource = swapchains_.get(swapchain);
    if (resource == nullptr) {
        return {Result::failure(ResultCode::kNotFound, "Invalid swapchain handle"), {}};
    }

    SwapchainInfo info{};
    info.extent = {resource->extent.width, resource->extent.height};
    info.format = from_vk_format(resource->format);
    info.present_mode = from_vk_present_mode(resource->present_mode);
    info.image_count = static_cast<uint32_t>(resource->images.size());
    info.frames_in_flight = static_cast<uint32_t>(resource->frames.size());
    info.needs_recreate = resource->needs_recreate;
    info.images = std::span<const SwapchainImage>{resource->images};
    return {Result::success(), info};
}

Result VulkanDevice::consume_acquire_semaphore(FrameSlot& frame) {
    QueueState* queue = &queues_[0];
    if (queue->queue == VK_NULL_HANDLE || queue->timeline == VK_NULL_HANDLE ||
        frame.image_available == VK_NULL_HANDLE || frame.in_flight == VK_NULL_HANDLE) {
        return Result::failure(ResultCode::kInvalidState, "Graphics queue is unavailable for frame cancellation");
    }

    const VkResult reset_result = vkResetFences(device_, 1, &frame.in_flight);
    if (reset_result != VK_SUCCESS) {
        return make_vulkan_result(reset_result, "vkResetFences(cancelFrame)");
    }

    const uint64_t signal_value = queue->next_signal_value;
    const std::array<VkSemaphore, 1> wait_semaphores{frame.image_available};
    const std::array<VkPipelineStageFlags, 1> wait_stages{VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    const std::array<uint64_t, 1> wait_values{0};
    const std::array<VkSemaphore, 1> signal_semaphores{queue->timeline};
    const std::array<uint64_t, 1> signal_values{signal_value};

    VkTimelineSemaphoreSubmitInfo timeline_info{};
    timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timeline_info.waitSemaphoreValueCount = 1;
    timeline_info.pWaitSemaphoreValues = wait_values.data();
    timeline_info.signalSemaphoreValueCount = 1;
    timeline_info.pSignalSemaphoreValues = signal_values.data();

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.pNext = &timeline_info;
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = wait_semaphores.data();
    submit_info.pWaitDstStageMask = wait_stages.data();
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = signal_semaphores.data();

    const VkResult submit_result = vkQueueSubmit(queue->queue, 1, &submit_info, frame.in_flight);
    if (submit_result != VK_SUCCESS) {
        return make_vulkan_result(submit_result, "vkQueueSubmit(cancelFrame)");
    }

    ++queue->next_signal_value;
    frame.completion = GpuSyncPoint{queue->timeline_handle, signal_value};
    return Result::success();
}

ResultValue<FrameAcquireResult> VulkanDevice::acquire_next_image(SwapchainHandle swapchain) {
    SwapchainResource* resource = swapchains_.get(swapchain);
    if (resource == nullptr) {
        return {Result::failure(ResultCode::kNotFound, "Invalid swapchain handle"), {}};
    }
    if (resource->swapchain == VK_NULL_HANDLE || resource->frames.empty() || resource->images.empty()) {
        return {Result::failure(ResultCode::kInvalidState, "Swapchain is not initialized"), {}};
    }

    const uint32_t frame_index = resource->current_frame;
    FrameSlot& frame = resource->frames[frame_index];
    if (frame.acquired) {
        return {
            Result::failure(ResultCode::kInvalidState, "The current frame must be submitted, presented, or cancelled"),
            {}};
    }

    VkResult result = vkWaitForFences(device_, 1, &frame.in_flight, VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) {
        return {make_vulkan_result(result, "vkWaitForFences(acquireNextImage)"), {}};
    }

    uint32_t image_index = kRemaining;
    result = vkAcquireNextImageKHR(device_, resource->swapchain, UINT64_MAX, frame.image_available, VK_NULL_HANDLE,
                                   &image_index);
    const bool suboptimal = result == VK_SUBOPTIMAL_KHR;
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        resource->needs_recreate = true;
        return {Result::failure(ResultCode::kOutOfDate, "Swapchain is out of date during image acquisition"), {}};
    }
    if (result == VK_ERROR_SURFACE_LOST_KHR) {
        return {Result::failure(ResultCode::kSurfaceLost, "Presentation surface was lost during image acquisition"),
                {}};
    }
    if (result != VK_SUCCESS && !suboptimal) {
        return {make_vulkan_result(result, "vkAcquireNextImageKHR"), {}};
    }
    if (image_index >= resource->images.size()) {
        return {Result::failure(ResultCode::kInvalidState, "Vulkan returned an invalid swapchain image index"), {}};
    }

    const uint32_t previous_frame = resource->image_in_flight_frame[image_index];
    if (previous_frame != kRemaining && previous_frame != frame_index) {
        FrameSlot& previous_slot = resource->frames[previous_frame];
        result = vkWaitForFences(device_, 1, &previous_slot.in_flight, VK_TRUE, UINT64_MAX);
        if (result != VK_SUCCESS) {
            frame.image_index = image_index;
            frame.acquired = true;
            (void)consume_acquire_semaphore(frame);
            frame.image_index = kRemaining;
            frame.acquired = false;
            return {make_vulkan_result(result, "vkWaitForFences(imageInFlight)"), {}};
        }
    }

    resource->image_in_flight_frame[image_index] = frame_index;
    frame.image_index = image_index;
    frame.acquired = true;
    frame.submitted = false;
    frame.completion = {};
    if (suboptimal) {
        resource->needs_recreate = true;
    }

    FrameAcquireResult acquire_result{};
    acquire_result.frame = FrameToken{swapchain, frame_index, image_index, resource->generation};
    acquire_result.image = resource->images[image_index];
    acquire_result.suboptimal = suboptimal;
    return {Result::success(), acquire_result};
}

ResultValue<SubmitResult> VulkanDevice::submit_frame(const FrameSubmitDesc& desc) {
    SwapchainResource* resource = nullptr;
    const Result token_result = validate_frame_token(desc.frame, resource);
    if (!token_result) {
        return {token_result, {}};
    }
    FrameSlot& frame = resource->frames[desc.frame.frame_slot];
    if (frame.submitted) {
        return {Result::failure(ResultCode::kInvalidState, "Frame has already been submitted"), {}};
    }
    if (desc.queue == QueueClass::kPresent) {
        return {Result::failure(ResultCode::kInvalidArgument, "Present is not a command submission queue"), {}};
    }

    QueueState* queue = queue_state(desc.queue);
    if (queue == nullptr || queue->queue == VK_NULL_HANDLE || queue->timeline == VK_NULL_HANDLE) {
        return {Result::failure(ResultCode::kInvalidState, "Requested frame submit queue is unavailable"), {}};
    }
    if (queue->logical_class != QueueClass::kGraphics) {
        return {
            Result::failure(ResultCode::kInvalidArgument, "Swapchain frames must be submitted on the graphics queue"),
            {}};
    }

    std::vector<VkCommandBuffer> command_buffers;
    command_buffers.reserve(desc.command_lists.size());
    for (const CommandListHandle command_list : desc.command_lists) {
        CommandListResource* command = command_lists_.get(command_list);
        if (command == nullptr) {
            return {Result::failure(ResultCode::kNotFound, "Frame submit contains an invalid command list"), {}};
        }
        if (!command->ended || command->recording) {
            return {
                Result::failure(ResultCode::kInvalidState, "All frame command lists must be ended before submission"),
                {}};
        }
        if (command->queue != queue->logical_class) {
            return {
                Result::failure(ResultCode::kInvalidArgument, "Frame command list queue does not match submit queue"),
                {}};
        }
        if (command->last_submitted.is_valid() && !is_complete(command->last_submitted)) {
            return {Result::failure(ResultCode::kInvalidState, "Frame command list is already in flight"), {}};
        }
        command_buffers.push_back(command->command_buffer);
    }

    std::vector<VkSemaphore> wait_semaphores;
    std::vector<VkPipelineStageFlags> wait_stages;
    std::vector<uint64_t> wait_values;
    wait_semaphores.reserve(desc.waits.size() + 1);
    wait_stages.reserve(desc.waits.size() + 1);
    wait_values.reserve(desc.waits.size() + 1);
    wait_semaphores.push_back(frame.image_available);
    wait_stages.push_back(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    wait_values.push_back(0);

    for (const GpuWait& wait : desc.waits) {
        const QueueState* source_queue = timeline_queue(wait.point.timeline);
        if (source_queue == nullptr || !wait.point.is_valid()) {
            return {Result::failure(ResultCode::kInvalidArgument, "Frame submit contains an invalid GPU wait point"),
                    {}};
        }
        if (wait.point.value >= source_queue->next_signal_value) {
            return {
                Result::failure(ResultCode::kInvalidArgument, "Frame submit waits on an unsubmitted timeline value"),
                {}};
        }
        wait_semaphores.push_back(source_queue->timeline);
        wait_stages.push_back(to_vk_pipeline_stages(wait.destination_stages));
        wait_values.push_back(wait.point.value);
    }

    const VkResult reset_result = vkResetFences(device_, 1, &frame.in_flight);
    if (reset_result != VK_SUCCESS) {
        return {make_vulkan_result(reset_result, "vkResetFences(submitFrame)"), {}};
    }

    const uint64_t signal_value = queue->next_signal_value;
    const std::array<VkSemaphore, 2> signal_semaphores{frame.render_finished, queue->timeline};
    const std::array<uint64_t, 2> signal_values{0, signal_value};

    VkTimelineSemaphoreSubmitInfo timeline_info{};
    timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timeline_info.waitSemaphoreValueCount = static_cast<uint32_t>(wait_values.size());
    timeline_info.pWaitSemaphoreValues = wait_values.data();
    timeline_info.signalSemaphoreValueCount = static_cast<uint32_t>(signal_values.size());
    timeline_info.pSignalSemaphoreValues = signal_values.data();

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.pNext = &timeline_info;
    submit_info.waitSemaphoreCount = static_cast<uint32_t>(wait_semaphores.size());
    submit_info.pWaitSemaphores = wait_semaphores.data();
    submit_info.pWaitDstStageMask = wait_stages.data();
    submit_info.commandBufferCount = static_cast<uint32_t>(command_buffers.size());
    submit_info.pCommandBuffers = command_buffers.data();
    submit_info.signalSemaphoreCount = static_cast<uint32_t>(signal_semaphores.size());
    submit_info.pSignalSemaphores = signal_semaphores.data();

    const VkResult result = vkQueueSubmit(queue->queue, 1, &submit_info, frame.in_flight);
    if (result != VK_SUCCESS) {
        return {make_vulkan_result(result, "vkQueueSubmit(submitFrame)"), {}};
    }

    ++queue->next_signal_value;
    const GpuSyncPoint completion{queue->timeline_handle, signal_value};
    frame.submitted = true;
    frame.completion = completion;
    for (const CommandListHandle command_list : desc.command_lists) {
        command_lists_.get(command_list)->last_submitted = completion;
    }
    return {Result::success(), SubmitResult{completion}};
}

ResultValue<PresentResult> VulkanDevice::present(const PresentDesc& desc) {
    SwapchainResource* resource = nullptr;
    const Result token_result = validate_frame_token(desc.frame, resource);
    if (!token_result) {
        return {token_result, {}};
    }
    FrameSlot& frame = resource->frames[desc.frame.frame_slot];
    if (!frame.submitted) {
        return {Result::failure(ResultCode::kInvalidState, "Frame must be submitted before presentation"), {}};
    }

    SurfaceResource* surface = surfaces_.get(resource->surface);
    if (surface == nullptr || surface->surface == VK_NULL_HANDLE || surface->present_queue == VK_NULL_HANDLE) {
        return {Result::failure(ResultCode::kInvalidState, "Swapchain presentation queue is unavailable"), {}};
    }

    const std::array<VkSemaphore, 1> wait_semaphores{frame.render_finished};
    const std::array<VkSwapchainKHR, 1> swapchains{resource->swapchain};
    const std::array<uint32_t, 1> image_indices{desc.frame.image_index};
    VkPresentInfoKHR present_info{};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = wait_semaphores.data();
    present_info.swapchainCount = 1;
    present_info.pSwapchains = swapchains.data();
    present_info.pImageIndices = image_indices.data();

    const VkResult result = vkQueuePresentKHR(surface->present_queue, &present_info);
    const bool suboptimal = result == VK_SUBOPTIMAL_KHR;
    const bool out_of_date = result == VK_ERROR_OUT_OF_DATE_KHR;
    if (suboptimal || out_of_date) {
        resource->needs_recreate = true;
    }

    frame.acquired = false;
    frame.submitted = false;
    frame.image_index = kRemaining;
    frame.completion = {};
    resource->current_frame = (desc.frame.frame_slot + 1) % static_cast<uint32_t>(resource->frames.size());

    if (out_of_date) {
        return {Result::failure(ResultCode::kOutOfDate, "Swapchain is out of date during presentation"), {}};
    }
    if (result == VK_ERROR_SURFACE_LOST_KHR) {
        return {Result::failure(ResultCode::kSurfaceLost, "Presentation surface was lost"), {}};
    }
    if (result != VK_SUCCESS && !suboptimal) {
        return {make_vulkan_result(result, "vkQueuePresentKHR"), {}};
    }
    return {Result::success(), PresentResult{suboptimal}};
}

Result VulkanDevice::cancel_frame(const FrameToken& frame_token) {
    SwapchainResource* resource = nullptr;
    const Result token_result = validate_frame_token(frame_token, resource);
    if (!token_result) {
        return token_result;
    }
    FrameSlot& frame = resource->frames[frame_token.frame_slot];
    if (frame.submitted) {
        return Result::failure(ResultCode::kInvalidState, "A submitted frame cannot be cancelled");
    }

    const Result consume_result = consume_acquire_semaphore(frame);
    if (!consume_result) {
        return consume_result;
    }
    if (resource->image_in_flight_frame[frame_token.image_index] == frame_token.frame_slot) {
        resource->image_in_flight_frame[frame_token.image_index] = kRemaining;
    }
    frame.acquired = false;
    frame.submitted = false;
    frame.image_index = kRemaining;
    frame.completion = {};
    resource->current_frame = (frame_token.frame_slot + 1) % static_cast<uint32_t>(resource->frames.size());
    return Result::success();
}

Result VulkanDevice::validate_buffer_range(BufferHandle buffer, uint64_t offset, uint64_t size) const {
    const BufferResource* resource = buffers_.get(buffer);
    if (resource == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid buffer handle");
    }
    if (offset > resource->size) {
        return Result::failure(ResultCode::kInvalidArgument, "Buffer offset is out of range");
    }
    if (size == std::numeric_limits<uint64_t>::max()) {
        size = resource->size - offset;
    }
    if (size > resource->size - offset) {
        return Result::failure(ResultCode::kInvalidArgument, "Buffer range exceeds buffer size");
    }
    return Result::success();
}

Result VulkanDevice::validate_texture_view(const TextureViewDesc& desc, const TextureResource& texture) const {
    const Format view_format = desc.format == Format::kUnknown ? texture.desc.format : desc.format;
    if (!to_vk_format(view_format).has_value()) {
        return Result::failure(ResultCode::kInvalidArgument, "Texture view format is not supported by the RHI");
    }

    if (view_format != texture.desc.format) {
        if (!has_flag(texture.desc.flags, ResourceFlags::kMutableFormat)) {
            return Result::failure(ResultCode::kInvalidArgument,
                                   "Texture view format differs from immutable texture format");
        }
        if (std::find(texture.compatible_formats.begin(), texture.compatible_formats.end(), view_format) ==
            texture.compatible_formats.end()) {
            return Result::failure(ResultCode::kInvalidArgument,
                                   "Texture view format is not in the compatible format list");
        }
    }

    const uint32_t mip_count = desc.subresources.mip_count == kRemaining
                                   ? texture.desc.mip_levels - desc.subresources.base_mip
                                   : desc.subresources.mip_count;
    const uint32_t layer_count = desc.subresources.layer_count == kRemaining
                                     ? texture.desc.array_layers - desc.subresources.base_layer
                                     : desc.subresources.layer_count;
    if (desc.subresources.base_mip >= texture.desc.mip_levels || mip_count == 0 ||
        mip_count > texture.desc.mip_levels - desc.subresources.base_mip) {
        return Result::failure(ResultCode::kInvalidArgument, "Texture view mip range is invalid");
    }
    if (desc.subresources.base_layer >= texture.desc.array_layers || layer_count == 0 ||
        layer_count > texture.desc.array_layers - desc.subresources.base_layer) {
        return Result::failure(ResultCode::kInvalidArgument, "Texture view layer range is invalid");
    }

    switch (texture.desc.dimension) {
        case TextureDimension::kTex1D:
            if (desc.dimension != TextureViewDimension::kAuto && desc.dimension != TextureViewDimension::kTex1D &&
                desc.dimension != TextureViewDimension::kTex1DArray) {
                return Result::failure(ResultCode::kInvalidArgument, "1D textures only support 1D view dimensions");
            }
            break;
        case TextureDimension::kTex2D:
            if (desc.dimension == TextureViewDimension::kTex1D || desc.dimension == TextureViewDimension::kTex1DArray ||
                desc.dimension == TextureViewDimension::kTex3D) {
                return Result::failure(ResultCode::kInvalidArgument, "2D textures cannot use 1D or 3D view dimensions");
            }
            break;
        case TextureDimension::kTex3D:
            if (desc.dimension != TextureViewDimension::kAuto && desc.dimension != TextureViewDimension::kTex3D) {
                return Result::failure(ResultCode::kInvalidArgument, "3D textures only support 3D view dimensions");
            }
            break;
    }

    if (desc.dimension == TextureViewDimension::kCube) {
        if (!has_flag(texture.desc.flags, ResourceFlags::kCubeCompatible) || layer_count != 6) {
            return Result::failure(ResultCode::kInvalidArgument,
                                   "Cube views require exactly six cube-compatible layers");
        }
    }
    if (desc.dimension == TextureViewDimension::kCubeArray) {
        if (!has_flag(texture.desc.flags, ResourceFlags::kCubeCompatible) || layer_count < 6 ||
            (layer_count % 6) != 0) {
            return Result::failure(ResultCode::kInvalidArgument, "Cube array views require layers in multiples of six");
        }
    }
    if ((desc.dimension == TextureViewDimension::kTex1D || desc.dimension == TextureViewDimension::kTex2D) &&
        layer_count != 1) {
        return Result::failure(ResultCode::kInvalidArgument, "Non-array texture views must select exactly one layer");
    }

    if (is_depth_format(view_format)) {
        if (has_flag(desc.subresources.aspects, Aspect::kColor)) {
            return Result::failure(ResultCode::kInvalidArgument, "Depth textures cannot use a color aspect");
        }
    } else if (has_flag(desc.subresources.aspects, Aspect::kDepth) ||
               has_flag(desc.subresources.aspects, Aspect::kStencil)) {
        return Result::failure(ResultCode::kInvalidArgument, "Color textures cannot use depth or stencil aspects");
    }

    if (desc.kind == TextureViewKind::kDepthStencil && !is_depth_format(view_format)) {
        return Result::failure(ResultCode::kInvalidArgument, "DepthStencil views require a depth format");
    }
    if (desc.kind == TextureViewKind::kColorAttachment && is_depth_format(view_format)) {
        return Result::failure(ResultCode::kInvalidArgument, "ColorAttachment views cannot use a depth format");
    }

    const TextureCapability required_capability = [&] {
        switch (desc.kind) {
            case TextureViewKind::kSampled:
                return TextureCapability::kSampled;
            case TextureViewKind::kStorage:
                return TextureCapability::kStorage;
            case TextureViewKind::kColorAttachment:
                return TextureCapability::kColorAttachment;
            case TextureViewKind::kDepthStencil:
                return TextureCapability::kDepthStencil;
            case TextureViewKind::kInputAttachment:
                return TextureCapability::kInputAttachment;
        }
        return TextureCapability::kNone;
    }();
    if (!has_flag(texture.desc.capabilities, required_capability)) {
        return Result::failure(ResultCode::kInvalidArgument,
                               "Texture view kind is not enabled by texture capabilities");
    }
    return Result::success();
}

std::optional<BindingLayoutEntry> VulkanDevice::find_binding_entry(const BindingLayoutResource& layout,
                                                                   BindingLocation location) const {
    const auto iterator =
        std::find_if(layout.entries.begin(), layout.entries.end(),
                     [location](const BindingLayoutEntry& entry) { return entry.location == location; });
    if (iterator == layout.entries.end()) {
        return std::nullopt;
    }
    return *iterator;
}

Result VulkanDevice::validate_binding_resource(const BindingLayoutEntry& entry, const BindingResource& resource) const {
    if (entry.type != resource.type) {
        return Result::failure(ResultCode::kInvalidArgument, "Binding resource type does not match layout");
    }

    switch (entry.type) {
        case BindingType::kSampler:
            return samplers_.get(resource.sampler) != nullptr
                       ? Result::success()
                       : Result::failure(ResultCode::kNotFound, "Invalid sampler handle");
        case BindingType::kSampledTexture:
        case BindingType::kStorageTexture: {
            const TextureViewResource* view = texture_views_.get(resource.texture_view);
            if (view == nullptr) {
                return Result::failure(ResultCode::kNotFound, "Invalid texture view handle");
            }
            const TextureViewKind expected_kind =
                entry.type == BindingType::kSampledTexture ? TextureViewKind::kSampled : TextureViewKind::kStorage;
            if (view->desc.kind != expected_kind) {
                return Result::failure(ResultCode::kInvalidArgument, "Texture view kind does not match binding type");
            }
            return Result::success();
        }
        case BindingType::kUniformBuffer:
        case BindingType::kStorageBuffer: {
            if (resource.buffer_range.buffer.is_valid()) {
                const BufferResource* buffer = buffers_.get(resource.buffer_range.buffer);
                const BufferCapability required =
                    entry.type == BindingType::kUniformBuffer ? BufferCapability::kUniform : BufferCapability::kStorage;
                if (buffer == nullptr || !has_flag(buffer->capabilities, required)) {
                    return Result::failure(ResultCode::kInvalidArgument,
                                           "Buffer capabilities do not match binding type");
                }
                return validate_buffer_range(resource.buffer_range.buffer, resource.buffer_range.offset,
                                             resource.buffer_range.size);
            }
            if (const BufferViewResource* view = buffer_views_.get(resource.buffer_view); view != nullptr) {
                const bool uniform = view->desc.kind == BufferViewKind::kUniform;
                const bool storage = view->desc.kind == BufferViewKind::kStorage ||
                                     view->desc.kind == BufferViewKind::kRaw ||
                                     view->desc.kind == BufferViewKind::kStructured;
                if ((entry.type == BindingType::kUniformBuffer && uniform) ||
                    (entry.type == BindingType::kStorageBuffer && storage)) {
                    return Result::success();
                }
                return Result::failure(ResultCode::kInvalidArgument, "Buffer view kind does not match binding type");
            }
            return Result::failure(ResultCode::kNotFound, "Invalid buffer range or buffer view");
        }
        case BindingType::kUniformTexelBuffer:
        case BindingType::kStorageTexelBuffer: {
            const BufferViewResource* view = buffer_views_.get(resource.buffer_view);
            if (view == nullptr || view->view == VK_NULL_HANDLE) {
                return Result::failure(ResultCode::kInvalidArgument,
                                       "Texel buffer binding requires a typed buffer view");
            }
            return Result::success();
        }
        case BindingType::kAccelerationStructure:
            return Result::failure(ResultCode::kUnsupported, "Acceleration structure bindings are not implemented yet");
    }
    return Result::failure(ResultCode::kInvalidArgument, "Unknown binding resource type");
}

ResultValue<BufferHandle> VulkanDevice::create_buffer(const BufferDesc& desc) {
    if (device_ == VK_NULL_HANDLE || allocator_ == nullptr) {
        return {Result::failure(ResultCode::kInvalidState, "Vulkan device is not initialized"), {}};
    }
    if (desc.size == 0) {
        return {Result::failure(ResultCode::kInvalidArgument, "Buffer size must be greater than zero"), {}};
    }
    if (desc.capabilities == BufferCapability::kNone) {
        return {Result::failure(ResultCode::kInvalidArgument, "Buffer capabilities must not be empty"), {}};
    }
    if (desc.flags != ResourceFlags::kNone) {
        return {Result::failure(ResultCode::kUnsupported, "Buffer resource flags are not implemented yet"), {}};
    }
    if (has_flag(desc.capabilities, BufferCapability::kAccelerationInput)) {
        return {Result::failure(ResultCode::kUnsupported, "Acceleration structure buffers are not implemented yet"),
                {}};
    }
    if (has_flag(desc.capabilities, BufferCapability::kDeviceAddress) && !capabilities_.buffer_device_address) {
        return {Result::failure(ResultCode::kUnsupported, "Buffer device address is not enabled for this RHI device"),
                {}};
    }

    const VkBufferUsageFlags usage = to_vk_buffer_usage(desc.capabilities);
    if (usage == 0) {
        return {Result::failure(ResultCode::kInvalidArgument, "Buffer capabilities produced no Vulkan usage"), {}};
    }

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = desc.size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation vma_allocation = nullptr;
    VmaAllocationInfo allocation_info{};
    const VmaAllocationCreateInfo allocation_create_info = to_vma_allocation_create_info(desc.memory, true);
    const VkResult result =
        vmaCreateBuffer(allocator_, &buffer_info, &allocation_create_info, &buffer, &vma_allocation, &allocation_info);
    if (result != VK_SUCCESS) {
        return {make_vulkan_result(result, "vmaCreateBuffer"), {}};
    }

    BufferResource resource{};
    resource.buffer = buffer;
    resource.allocation.handle = vma_allocation;
    resource.allocation.info = allocation_info;
    resource.allocation.properties = memory_properties_.memoryTypes[allocation_info.memoryType].propertyFlags;
    resource.size = desc.size;
    resource.capabilities = desc.capabilities;
#if AEG_RHI_ENABLE_DEBUG_NAMES
    set_debug_name(VK_OBJECT_TYPE_BUFFER, buffer, desc.name);
#endif
    return {Result::success(), buffers_.insert(std::move(resource))};
}

Result VulkanDevice::destroy_buffer(BufferHandle buffer) {
    const BufferResource* resource = buffers_.get(buffer);
    if (resource == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid buffer handle");
    }
    if (buffer_is_referenced(buffer)) {
        return Result::failure(ResultCode::kInvalidState, "Buffer is still referenced by a view or binding set");
    }

    vmaDestroyBuffer(allocator_, resource->buffer, resource->allocation.handle);
    (void)buffers_.erase(buffer);
    return Result::success();
}

Result VulkanDevice::write_buffer(BufferHandle buffer, uint64_t offset, std::span<const std::byte> data) {
    BufferResource* resource = buffers_.get(buffer);
    if (resource == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid buffer handle");
    }
    if (offset > resource->size || data.size() > resource->size - offset) {
        return Result::failure(ResultCode::kInvalidArgument, "Buffer write exceeds buffer size");
    }
    if ((resource->allocation.properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
        return Result::failure(ResultCode::kInvalidState, "Buffer memory is not host visible");
    }
    if (data.empty()) {
        return Result::success();
    }

    void* mapped = nullptr;
    VkResult result = vmaMapMemory(allocator_, resource->allocation.handle, &mapped);
    if (result != VK_SUCCESS) {
        return make_vulkan_result(result, "vmaMapMemory");
    }
    std::memcpy(static_cast<std::byte*>(mapped) + offset, data.data(), data.size());

    if ((resource->allocation.properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
        result = vmaFlushAllocation(allocator_, resource->allocation.handle, offset, data.size());
        if (result != VK_SUCCESS) {
            vmaUnmapMemory(allocator_, resource->allocation.handle);
            return make_vulkan_result(result, "vmaFlushAllocation");
        }
    }

    vmaUnmapMemory(allocator_, resource->allocation.handle);
    return Result::success();
}

ResultValue<TextureHandle> VulkanDevice::create_texture(const TextureDesc& desc) {
    if (device_ == VK_NULL_HANDLE || allocator_ == nullptr) {
        return {Result::failure(ResultCode::kInvalidState, "Vulkan device is not initialized"), {}};
    }
    if (desc.format == Format::kUnknown || !to_vk_format(desc.format).has_value()) {
        return {Result::failure(ResultCode::kInvalidArgument, "Texture format is invalid"), {}};
    }
    if (desc.extent.width == 0 || desc.extent.height == 0 || desc.extent.depth == 0 || desc.mip_levels == 0 ||
        desc.array_layers == 0) {
        return {Result::failure(ResultCode::kInvalidArgument, "Texture dimensions must be non-zero"), {}};
    }
    if (desc.capabilities == TextureCapability::kNone) {
        return {Result::failure(ResultCode::kInvalidArgument, "Texture capabilities must not be empty"), {}};
    }
    if (desc.memory == MemoryClass::kUpload || desc.memory == MemoryClass::kReadback) {
        return {Result::failure(ResultCode::kUnsupported,
                                "Host-visible optimal textures are not supported "
                                "by this RHI version"),
                {}};
    }
    if (has_flag(desc.flags, ResourceFlags::kExternal)) {
        return {Result::failure(ResultCode::kUnsupported, "External texture imports are not implemented yet"), {}};
    }
    if (has_flag(desc.flags, ResourceFlags::kAllowAlias)) {
        return {Result::failure(ResultCode::kUnsupported, "Aliased texture allocation is not implemented yet"), {}};
    }
    if (desc.dimension == TextureDimension::kTex3D && desc.array_layers != 1) {
        return {Result::failure(ResultCode::kInvalidArgument, "3D textures must use exactly one array layer"), {}};
    }
    if (has_flag(desc.flags, ResourceFlags::kCubeCompatible) &&
        (desc.dimension != TextureDimension::kTex2D || desc.array_layers < 6 || (desc.array_layers % 6) != 0)) {
        return {Result::failure(ResultCode::kInvalidArgument,
                                "Cube-compatible textures require 2D layers in multiples of six"),
                {}};
    }

    const VkImageUsageFlags usage = to_vk_image_usage(desc.capabilities);
    if (usage == 0) {
        return {Result::failure(ResultCode::kInvalidArgument, "Texture capabilities produced no Vulkan usage"), {}};
    }

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = desc.dimension == TextureDimension::kTex1D
                               ? VK_IMAGE_TYPE_1D
                               : (desc.dimension == TextureDimension::kTex3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D);
    image_info.format = to_vk_format(desc.format).value();
    image_info.extent = VkExtent3D{desc.extent.width, desc.extent.height, desc.extent.depth};
    image_info.mipLevels = desc.mip_levels;
    image_info.arrayLayers = desc.array_layers;
    image_info.samples = to_vk_sample_count(desc.samples);
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = usage;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (has_flag(desc.flags, ResourceFlags::kCubeCompatible)) {
        image_info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }
    if (has_flag(desc.flags, ResourceFlags::kMutableFormat)) {
        image_info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    }

    std::vector<VkFormat> compatible_formats;
    VkImageFormatListCreateInfo format_list_info{};
    if (!desc.compatible_view_formats.empty()) {
        if (!has_flag(desc.flags, ResourceFlags::kMutableFormat)) {
            return {Result::failure(ResultCode::kInvalidArgument, "Compatible view formats require MutableFormat"), {}};
        }
        compatible_formats.reserve(desc.compatible_view_formats.size() + 1);
        compatible_formats.push_back(to_vk_format(desc.format).value());
        for (const Format format : desc.compatible_view_formats) {
            const auto vk_format = to_vk_format(format);
            if (!vk_format.has_value()) {
                return {Result::failure(ResultCode::kInvalidArgument,
                                        "Texture compatible format is not supported by the RHI"),
                        {}};
            }
            if (std::find(compatible_formats.begin(), compatible_formats.end(), vk_format.value()) ==
                compatible_formats.end()) {
                compatible_formats.push_back(vk_format.value());
            }
        }
        format_list_info.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
        format_list_info.viewFormatCount = static_cast<uint32_t>(compatible_formats.size());
        format_list_info.pViewFormats = compatible_formats.data();
        image_info.pNext = &format_list_info;
    }

    VkImage image = VK_NULL_HANDLE;
    VmaAllocation vma_allocation = nullptr;
    VmaAllocationInfo allocation_info{};
    const VmaAllocationCreateInfo allocation_create_info = to_vma_allocation_create_info(desc.memory, true);
    const VkResult result =
        vmaCreateImage(allocator_, &image_info, &allocation_create_info, &image, &vma_allocation, &allocation_info);
    if (result != VK_SUCCESS) {
        return {make_vulkan_result(result, "vmaCreateImage"), {}};
    }

    TextureResource resource{};
    resource.image = image;
    resource.allocation.handle = vma_allocation;
    resource.allocation.info = allocation_info;
    resource.allocation.properties = memory_properties_.memoryTypes[allocation_info.memoryType].propertyFlags;
    resource.desc = desc;
    resource.desc.compatible_view_formats = {};
    resource.desc.name = {};
    resource.compatible_formats.assign(desc.compatible_view_formats.begin(), desc.compatible_view_formats.end());
#if AEG_RHI_ENABLE_DEBUG_NAMES
    set_debug_name(VK_OBJECT_TYPE_IMAGE, image, desc.name);
#endif
    return {Result::success(), textures_.insert(std::move(resource))};
}

Result VulkanDevice::destroy_texture(TextureHandle texture) {
    TextureResource* resource = textures_.get(texture);
    if (resource == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid texture handle");
    }
    if (resource->external) {
        return Result::failure(ResultCode::kInvalidState,
                               "Swapchain-owned textures must be destroyed with the swapchain");
    }
    if (texture_is_referenced(texture)) {
        return Result::failure(ResultCode::kInvalidState, "Texture is still referenced by a view or binding set");
    }

    vmaDestroyImage(allocator_, resource->image, resource->allocation.handle);
    (void)textures_.erase(texture);
    return Result::success();
}

ResultValue<BufferViewHandle> VulkanDevice::create_buffer_view(const BufferViewDesc& desc) {
    BufferResource* buffer = buffers_.get(desc.buffer);
    if (buffer == nullptr) {
        return {Result::failure(ResultCode::kNotFound, "Invalid buffer handle"), {}};
    }

    const uint64_t size = desc.size == std::numeric_limits<uint64_t>::max()
                              ? buffer->size - std::min(desc.offset, buffer->size)
                              : desc.size;
    if (desc.offset > buffer->size || size > buffer->size - desc.offset || size == 0) {
        return {Result::failure(ResultCode::kInvalidArgument, "Buffer view range is invalid"), {}};
    }

    switch (desc.kind) {
        case BufferViewKind::kUniform:
            if (!has_flag(buffer->capabilities, BufferCapability::kUniform)) {
                return {
                    Result::failure(ResultCode::kInvalidArgument, "Uniform buffer view requires Uniform capability"),
                    {}};
            }
            break;
        case BufferViewKind::kStorage:
        case BufferViewKind::kRaw:
        case BufferViewKind::kStructured:
            if (!has_flag(buffer->capabilities, BufferCapability::kStorage)) {
                return {
                    Result::failure(ResultCode::kInvalidArgument, "Storage buffer view requires Storage capability"),
                    {}};
            }
            if (desc.kind == BufferViewKind::kStructured && desc.structure_stride == 0) {
                return {
                    Result::failure(ResultCode::kInvalidArgument, "Structured buffer view requires a non-zero stride"),
                    {}};
            }
            break;
        case BufferViewKind::kTyped:
            if (!has_flag(buffer->capabilities, BufferCapability::kTexel) || desc.format == Format::kUnknown) {
                return {Result::failure(ResultCode::kInvalidArgument,
                                        "Typed buffer view requires Texel capability and a format"),
                        {}};
            }
            break;
    }

    BufferViewResource resource{};
    resource.desc = desc;
    resource.desc.size = size;
    resource.desc.name = {};

    if (desc.kind == BufferViewKind::kTyped) {
        const auto format = to_vk_format(desc.format);
        if (!format.has_value()) {
            return {
                Result::failure(ResultCode::kInvalidArgument, "Typed buffer view format is not supported by the RHI"),
                {}};
        }

        VkBufferViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
        view_info.buffer = buffer->buffer;
        view_info.format = format.value();
        view_info.offset = desc.offset;
        view_info.range = size;
        const VkResult result = vkCreateBufferView(device_, &view_info, nullptr, &resource.view);
        if (result != VK_SUCCESS) {
            return {make_vulkan_result(result, "vkCreateBufferView"), {}};
        }
    }

#if AEG_RHI_ENABLE_DEBUG_NAMES
    set_debug_name(VK_OBJECT_TYPE_BUFFER_VIEW, resource.view, desc.name);
#endif
    return {Result::success(), buffer_views_.insert(std::move(resource))};
}

ResultValue<TextureViewHandle> VulkanDevice::create_texture_view(const TextureViewDesc& desc) {
    TextureResource* texture = textures_.get(desc.texture);
    if (texture == nullptr) {
        return {Result::failure(ResultCode::kNotFound, "Invalid texture handle"), {}};
    }

    TextureViewDesc normalized = desc;
    const Format view_format = desc.format == Format::kUnknown ? texture->desc.format : desc.format;
    if (is_depth_format(view_format) && normalized.subresources.aspects == Aspect::kColor) {
        normalized.subresources.aspects = default_aspect(view_format);
    }

    const Result validation = validate_texture_view(normalized, *texture);
    if (!validation) {
        return {validation, {}};
    }

    const uint32_t mip_count = normalized.subresources.mip_count == kRemaining
                                   ? texture->desc.mip_levels - normalized.subresources.base_mip
                                   : normalized.subresources.mip_count;
    const uint32_t layer_count = normalized.subresources.layer_count == kRemaining
                                     ? texture->desc.array_layers - normalized.subresources.base_layer
                                     : normalized.subresources.layer_count;
    normalized.subresources.mip_count = mip_count;
    normalized.subresources.layer_count = layer_count;
    normalized.format = view_format;
    normalized.name = {};

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = texture->image;
    view_info.viewType = to_vk_image_view_type(normalized.dimension, texture->desc.dimension, layer_count);
    view_info.format = to_vk_format(view_format).value();
    view_info.components.r = to_vk_swizzle(normalized.components.r);
    view_info.components.g = to_vk_swizzle(normalized.components.g);
    view_info.components.b = to_vk_swizzle(normalized.components.b);
    view_info.components.a = to_vk_swizzle(normalized.components.a);
    view_info.subresourceRange.aspectMask = to_vk_aspect(normalized.subresources.aspects);
    view_info.subresourceRange.baseMipLevel = normalized.subresources.base_mip;
    view_info.subresourceRange.levelCount = mip_count;
    view_info.subresourceRange.baseArrayLayer = normalized.subresources.base_layer;
    view_info.subresourceRange.layerCount = layer_count;

    TextureViewResource resource{};
    resource.desc = normalized;
    const VkResult result = vkCreateImageView(device_, &view_info, nullptr, &resource.view);
    if (result != VK_SUCCESS) {
        return {make_vulkan_result(result, "vkCreateImageView"), {}};
    }
#if AEG_RHI_ENABLE_DEBUG_NAMES
    set_debug_name(VK_OBJECT_TYPE_IMAGE_VIEW, resource.view, desc.name);
#endif
    return {Result::success(), texture_views_.insert(std::move(resource))};
}

ResultValue<SamplerHandle> VulkanDevice::create_sampler(const SamplerDesc& desc) {
    if (device_ == VK_NULL_HANDLE) {
        return {Result::failure(ResultCode::kInvalidState, "Vulkan device is not initialized"), {}};
    }
    if (desc.anisotropy_enable && !enabled_features_.sampler_anisotropy) {
        return {Result::failure(ResultCode::kUnsupported, "Sampler anisotropy is not enabled for this device"), {}};
    }
    if (desc.anisotropy_enable && desc.max_anisotropy < 1.0f) {
        return {Result::failure(ResultCode::kInvalidArgument, "Sampler maxAnisotropy must be at least one"), {}};
    }

    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = to_vk_filter(desc.mag_filter);
    sampler_info.minFilter = to_vk_filter(desc.min_filter);
    sampler_info.mipmapMode =
        desc.mip_filter == Filter::kNearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.addressModeU = to_vk_address_mode(desc.address_u);
    sampler_info.addressModeV = to_vk_address_mode(desc.address_v);
    sampler_info.addressModeW = to_vk_address_mode(desc.address_w);
    sampler_info.mipLodBias = desc.mip_lod_bias;
    sampler_info.minLod = desc.min_lod;
    sampler_info.maxLod = desc.max_lod;
    sampler_info.anisotropyEnable = desc.anisotropy_enable ? VK_TRUE : VK_FALSE;
    sampler_info.maxAnisotropy = desc.max_anisotropy;
    sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    sampler_info.unnormalizedCoordinates = VK_FALSE;

    SamplerResource resource{};
    const VkResult result = vkCreateSampler(device_, &sampler_info, nullptr, &resource.sampler);
    if (result != VK_SUCCESS) {
        return {make_vulkan_result(result, "vkCreateSampler"), {}};
    }
#if AEG_RHI_ENABLE_DEBUG_NAMES
    set_debug_name(VK_OBJECT_TYPE_SAMPLER, resource.sampler, desc.name);
#endif
    return {Result::success(), samplers_.insert(std::move(resource))};
}

Result VulkanDevice::destroy_sampler(SamplerHandle sampler) {
    SamplerResource* resource = samplers_.get(sampler);
    if (resource == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid sampler handle");
    }
    if (sampler_is_referenced(sampler)) {
        return Result::failure(ResultCode::kInvalidState, "Sampler is still referenced by a binding set");
    }
    vkDestroySampler(device_, resource->sampler, nullptr);
    (void)samplers_.erase(sampler);
    return Result::success();
}

ResultValue<BindingLayoutHandle> VulkanDevice::create_binding_layout(const BindingLayoutDesc& desc) {
    if (device_ == VK_NULL_HANDLE) {
        return {Result::failure(ResultCode::kInvalidState, "Vulkan device is not initialized"), {}};
    }

    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(desc.entries.size());
    std::vector<BindingLayoutEntry> copied_entries;
    copied_entries.reserve(desc.entries.size());
    uint16_t group = 0;
    bool group_initialized = false;

    for (const BindingLayoutEntry& entry : desc.entries) {
        if (!group_initialized) {
            group = entry.location.group;
            group_initialized = true;
        } else if (entry.location.group != group) {
            return {
                Result::failure(ResultCode::kInvalidArgument, "A binding layout must contain entries from one group"),
                {}};
        }
        if (std::any_of(copied_entries.begin(), copied_entries.end(),
                        [&entry](const BindingLayoutEntry& other) { return other.location == entry.location; })) {
            return {Result::failure(ResultCode::kInvalidArgument,
                                    "Binding layout contains duplicate group/binding locations"),
                    {}};
        }
        if (entry.array_mode == BindingArrayMode::kVariable || entry.array_mode == BindingArrayMode::kUnbounded ||
            has_flag(entry.flags, BindingFlags::kBindless) ||
            has_flag(entry.flags, BindingFlags::kPartiallyPopulated)) {
            return {Result::failure(ResultCode::kUnsupported,
                                    "Variable and bindless descriptor arrays are not "
                                    "implemented yet"),
                    {}};
        }
        if (entry.array_mode == BindingArrayMode::kFixed && entry.array_count == 0) {
            return {Result::failure(ResultCode::kInvalidArgument, "Fixed binding arrays must have a non-zero count"),
                    {}};
        }
        if (has_flag(entry.flags, BindingFlags::kDynamicOffset) || has_flag(entry.flags, BindingFlags::kImmutable)) {
            return {Result::failure(ResultCode::kUnsupported,
                                    "Dynamic and immutable binding flags are not implemented yet"),
                    {}};
        }

        const auto descriptor_type = to_vk_descriptor_type(entry.type);
        if (!descriptor_type.has_value() || entry.type == BindingType::kAccelerationStructure) {
            return {
                Result::failure(ResultCode::kUnsupported, "This binding type is not implemented by the Vulkan backend"),
                {}};
        }

        VkDescriptorSetLayoutBinding binding{};
        binding.binding = entry.location.binding;
        binding.descriptorType = descriptor_type.value();
        binding.descriptorCount = entry.array_mode == BindingArrayMode::kSingle ? 1 : entry.array_count;
        binding.stageFlags = to_vk_shader_stages(entry.shader_stages);
        bindings.push_back(binding);
        copied_entries.push_back(entry);
    }

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = static_cast<uint32_t>(bindings.size());
    layout_info.pBindings = bindings.data();

    BindingLayoutResource resource{};
    const VkResult result = vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &resource.layout);
    if (result != VK_SUCCESS) {
        return {make_vulkan_result(result, "vkCreateDescriptorSetLayout"), {}};
    }
    resource.group = group;
    resource.entries = std::move(copied_entries);
#if AEG_RHI_ENABLE_DEBUG_NAMES
    set_debug_name(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, resource.layout, desc.name);
#endif
    return {Result::success(), binding_layouts_.insert(std::move(resource))};
}

ResultValue<BindingSetHandle> VulkanDevice::create_binding_set(BindingLayoutHandle layout) {
    BindingLayoutResource* layout_resource = binding_layouts_.get(layout);
    if (layout_resource == nullptr) {
        return {Result::failure(ResultCode::kNotFound, "Invalid binding layout handle"), {}};
    }

    std::unordered_map<VkDescriptorType, uint32_t> descriptor_counts;
    for (const BindingLayoutEntry& entry : layout_resource->entries) {
        const auto descriptor_type = to_vk_descriptor_type(entry.type);
        if (!descriptor_type.has_value()) {
            return {Result::failure(ResultCode::kUnsupported, "Binding layout contains an unsupported descriptor type"),
                    {}};
        }
        const uint32_t count = entry.array_mode == BindingArrayMode::kSingle ? 1 : entry.array_count;
        descriptor_counts[descriptor_type.value()] += count;
    }

    std::vector<VkDescriptorPoolSize> pool_sizes;
    pool_sizes.reserve(descriptor_counts.size());
    for (const auto& [type, count] : descriptor_counts) {
        pool_sizes.push_back(VkDescriptorPoolSize{type, count});
    }

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
    pool_info.pPoolSizes = pool_sizes.data();

    BindingSetResource resource{};
    resource.layout = layout;
    VkResult result = vkCreateDescriptorPool(device_, &pool_info, nullptr, &resource.pool);
    if (result != VK_SUCCESS) {
        return {make_vulkan_result(result, "vkCreateDescriptorPool"), {}};
    }

    VkDescriptorSetAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate_info.descriptorPool = resource.pool;
    allocate_info.descriptorSetCount = 1;
    allocate_info.pSetLayouts = &layout_resource->layout;
    result = vkAllocateDescriptorSets(device_, &allocate_info, &resource.set);
    if (result != VK_SUCCESS) {
        vkDestroyDescriptorPool(device_, resource.pool, nullptr);
        return {make_vulkan_result(result, "vkAllocateDescriptorSets"), {}};
    }

    return {Result::success(), binding_sets_.insert(std::move(resource))};
}

Result VulkanDevice::update_binding_set(BindingSetHandle set, std::span<const BindingWrite> writes) {
    BindingSetResource* set_resource = binding_sets_.get(set);
    if (set_resource == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid binding set handle");
    }
    const BindingLayoutResource* layout_resource = binding_layouts_.get(set_resource->layout);
    if (layout_resource == nullptr) {
        return Result::failure(ResultCode::kInvalidState, "Binding set layout no longer exists");
    }

    std::vector<VkWriteDescriptorSet> vk_writes;
    std::vector<VkDescriptorBufferInfo> buffer_infos;
    std::vector<VkDescriptorImageInfo> image_infos;
    std::vector<VkBufferView> texel_views;
    vk_writes.reserve(writes.size());
    buffer_infos.reserve(writes.size());
    image_infos.reserve(writes.size());
    texel_views.reserve(writes.size());

    for (const BindingWrite& write : writes) {
        const auto entry = find_binding_entry(*layout_resource, write.location);
        if (!entry.has_value()) {
            return Result::failure(ResultCode::kInvalidArgument, "Binding write location is absent from layout");
        }
        const uint32_t array_count = entry->array_mode == BindingArrayMode::kSingle ? 1 : entry->array_count;
        if (write.array_index >= array_count) {
            return Result::failure(ResultCode::kInvalidArgument, "Binding write array index is out of range");
        }
        const Result resource_validation = validate_binding_resource(entry.value(), write.resource);
        if (!resource_validation) {
            return resource_validation;
        }

        const auto descriptor_type = to_vk_descriptor_type(entry->type);
        if (!descriptor_type.has_value()) {
            return Result::failure(ResultCode::kUnsupported, "Binding descriptor type is not supported");
        }

        VkWriteDescriptorSet vk_write{};
        vk_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        vk_write.dstSet = set_resource->set;
        vk_write.dstBinding = write.location.binding;
        vk_write.dstArrayElement = write.array_index;
        vk_write.descriptorCount = 1;
        vk_write.descriptorType = descriptor_type.value();

        switch (entry->type) {
            case BindingType::kSampler: {
                const SamplerResource* sampler = samplers_.get(write.resource.sampler);
                image_infos.push_back(
                    VkDescriptorImageInfo{sampler->sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
                vk_write.pImageInfo = &image_infos.back();
                break;
            }
            case BindingType::kSampledTexture:
            case BindingType::kStorageTexture: {
                const TextureViewResource* view = texture_views_.get(write.resource.texture_view);
                const TextureResource* texture = textures_.get(view->desc.texture);
                const bool depth_texture = texture != nullptr && is_depth_format(texture->desc.format);
                const VkImageLayout image_layout =
                    entry->type == BindingType::kStorageTexture
                        ? VK_IMAGE_LAYOUT_GENERAL
                        : (depth_texture ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                         : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                image_infos.push_back(VkDescriptorImageInfo{VK_NULL_HANDLE, view->view, image_layout});
                vk_write.pImageInfo = &image_infos.back();
                break;
            }
            case BindingType::kUniformBuffer:
            case BindingType::kStorageBuffer: {
                BufferHandle buffer_handle = write.resource.buffer_range.buffer;
                uint64_t offset = write.resource.buffer_range.offset;
                uint64_t size = write.resource.buffer_range.size;
                if (!buffer_handle.is_valid()) {
                    const BufferViewResource* view = buffer_views_.get(write.resource.buffer_view);
                    buffer_handle = view->desc.buffer;
                    offset = view->desc.offset;
                    size = view->desc.size;
                }
                const BufferResource* buffer = buffers_.get(buffer_handle);
                if (size == std::numeric_limits<uint64_t>::max()) {
                    size = buffer->size - offset;
                }
                buffer_infos.push_back(VkDescriptorBufferInfo{buffer->buffer, offset, size});
                vk_write.pBufferInfo = &buffer_infos.back();
                break;
            }
            case BindingType::kUniformTexelBuffer:
            case BindingType::kStorageTexelBuffer: {
                const BufferViewResource* view = buffer_views_.get(write.resource.buffer_view);
                texel_views.push_back(view->view);
                vk_write.pTexelBufferView = &texel_views.back();
                break;
            }
            case BindingType::kAccelerationStructure:
                return Result::failure(ResultCode::kUnsupported,
                                       "Acceleration structure bindings are not implemented yet");
        }
        vk_writes.push_back(vk_write);
    }

    vkUpdateDescriptorSets(device_, static_cast<uint32_t>(vk_writes.size()), vk_writes.data(), 0, nullptr);

    for (const BindingWrite& write : writes) {
        const auto iterator = std::find_if(
            set_resource->writes.begin(), set_resource->writes.end(), [&write](const BindingWrite& existing) {
                return existing.location == write.location && existing.array_index == write.array_index;
            });
        if (iterator != set_resource->writes.end()) {
            *iterator = write;
        } else {
            set_resource->writes.push_back(write);
        }
    }
    return Result::success();
}

Result VulkanDevice::destroy_buffer_view(BufferViewHandle view) {
    BufferViewResource* resource = buffer_views_.get(view);
    if (resource == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid buffer view handle");
    }
    if (buffer_view_is_referenced(view)) {
        return Result::failure(ResultCode::kInvalidState, "Buffer view is still referenced by a binding set");
    }

    if (resource->view != VK_NULL_HANDLE) {
        vkDestroyBufferView(device_, resource->view, nullptr);
    }
    (void)buffer_views_.erase(view);
    return Result::success();
}

Result VulkanDevice::destroy_texture_view(TextureViewHandle view) {
    TextureViewResource* resource = texture_views_.get(view);
    if (resource == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid texture view handle");
    }
    if (resource->owned_by_swapchain) {
        return Result::failure(ResultCode::kInvalidState,
                               "Swapchain-owned texture views must be destroyed with the swapchain");
    }
    if (texture_view_is_referenced(view)) {
        return Result::failure(ResultCode::kInvalidState, "Texture view is still referenced by a binding set");
    }

    vkDestroyImageView(device_, resource->view, nullptr);
    (void)texture_views_.erase(view);
    return Result::success();
}

Result VulkanDevice::destroy_binding_layout(BindingLayoutHandle layout) {
    BindingLayoutResource* resource = binding_layouts_.get(layout);
    if (resource == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid binding layout handle");
    }

    bool referenced = false;
    binding_sets_.for_each([&](const BindingSetResource& set) { referenced = referenced || set.layout == layout; });
    if (referenced) {
        return Result::failure(ResultCode::kInvalidState, "Binding layout is still used by a binding set");
    }

    vkDestroyDescriptorSetLayout(device_, resource->layout, nullptr);
    (void)binding_layouts_.erase(layout);
    return Result::success();
}

Result VulkanDevice::destroy_binding_set(BindingSetHandle set) {
    BindingSetResource* resource = binding_sets_.get(set);
    if (resource == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid binding set handle");
    }

    vkDestroyDescriptorPool(device_, resource->pool, nullptr);
    (void)binding_sets_.erase(set);
    return Result::success();
}

ResultValue<CommandListHandle> VulkanDevice::create_command_list(const CommandListDesc& desc) {
    QueueState* queue = queue_state(desc.queue);
    if (device_ == VK_NULL_HANDLE || queue == nullptr || queue->queue == VK_NULL_HANDLE) {
        return {Result::failure(ResultCode::kInvalidState, "Requested command queue is unavailable"), {}};
    }

    VkCommandBufferAllocateInfo allocate_info{};
    allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.commandPool = queue->command_pool;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;

    CommandListResource resource{};
    resource.pool = queue->command_pool;
    resource.queue = queue->logical_class;
    resource.one_time_submit = desc.one_time_submit;
    const VkResult result = vkAllocateCommandBuffers(device_, &allocate_info, &resource.command_buffer);
    if (result != VK_SUCCESS) {
        return {make_vulkan_result(result, "vkAllocateCommandBuffers"), {}};
    }
#if AEG_RHI_ENABLE_DEBUG_NAMES
    set_debug_name(VK_OBJECT_TYPE_COMMAND_BUFFER, resource.command_buffer, desc.name);
#endif
    return {Result::success(), command_lists_.insert(std::move(resource))};
}

Result VulkanDevice::begin_command_list(CommandListHandle command_list) {
    CommandListResource* resource = command_lists_.get(command_list);
    if (resource == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid command list handle");
    }
    if (resource->recording || resource->ended) {
        return Result::failure(ResultCode::kInvalidState, "Command list must be reset before recording");
    }

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = resource->one_time_submit ? VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT : 0;
    const VkResult result = vkBeginCommandBuffer(resource->command_buffer, &begin_info);
    if (result != VK_SUCCESS) {
        return make_vulkan_result(result, "vkBeginCommandBuffer");
    }
    resource->recording = true;
    return Result::success();
}

Result VulkanDevice::end_command_list(CommandListHandle command_list) {
    CommandListResource* resource = command_lists_.get(command_list);
    if (resource == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid command list handle");
    }
    if (!resource->recording) {
        return Result::failure(ResultCode::kInvalidState, "Command list is not recording");
    }

    const VkResult result = vkEndCommandBuffer(resource->command_buffer);
    if (result != VK_SUCCESS) {
        return make_vulkan_result(result, "vkEndCommandBuffer");
    }
    resource->recording = false;
    resource->ended = true;
    return Result::success();
}

Result VulkanDevice::reset_command_list(CommandListHandle command_list) {
    CommandListResource* resource = command_lists_.get(command_list);
    if (resource == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid command list handle");
    }
    if (resource->recording) {
        return Result::failure(ResultCode::kInvalidState, "Cannot reset a recording command list");
    }
    if (resource->last_submitted.is_valid() && !is_complete(resource->last_submitted)) {
        return Result::failure(ResultCode::kInvalidState, "Command list is still in flight");
    }

    const VkResult result = vkResetCommandBuffer(resource->command_buffer, 0);
    if (result != VK_SUCCESS) {
        return make_vulkan_result(result, "vkResetCommandBuffer");
    }
    resource->ended = false;
    resource->last_submitted = {};
    return Result::success();
}

Result VulkanDevice::destroy_command_list(CommandListHandle command_list) {
    CommandListResource* resource = command_lists_.get(command_list);
    if (resource == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid command list handle");
    }
    if (resource->recording) {
        return Result::failure(ResultCode::kInvalidState, "Cannot destroy a recording command list");
    }
    if (resource->last_submitted.is_valid() && !is_complete(resource->last_submitted)) {
        return Result::failure(ResultCode::kInvalidState, "Command list is still in flight");
    }

    vkFreeCommandBuffers(device_, resource->pool, 1, &resource->command_buffer);
    (void)command_lists_.erase(command_list);
    return Result::success();
}

ResultValue<SubmitResult> VulkanDevice::submit(const SubmitDesc& desc) {
    QueueState* queue = queue_state(desc.queue);
    if (device_ == VK_NULL_HANDLE || queue == nullptr || queue->queue == VK_NULL_HANDLE) {
        return {Result::failure(ResultCode::kInvalidState, "Requested submit queue is unavailable"), {}};
    }

    std::vector<VkCommandBuffer> command_buffers;
    command_buffers.reserve(desc.command_lists.size());
    for (const CommandListHandle command_list : desc.command_lists) {
        CommandListResource* resource = command_lists_.get(command_list);
        if (resource == nullptr) {
            return {Result::failure(ResultCode::kNotFound, "Submit contains an invalid command list"), {}};
        }
        if (!resource->ended || resource->recording) {
            return {Result::failure(ResultCode::kInvalidState, "All command lists must be ended before submission"),
                    {}};
        }
        if (resource->queue != queue->logical_class) {
            return {Result::failure(ResultCode::kInvalidArgument, "Command list queue does not match submit queue"),
                    {}};
        }
        if (resource->last_submitted.is_valid() && !is_complete(resource->last_submitted)) {
            return {Result::failure(ResultCode::kInvalidState, "Command list is already in flight"), {}};
        }
        command_buffers.push_back(resource->command_buffer);
    }

    std::vector<VkSemaphore> wait_semaphores;
    std::vector<VkPipelineStageFlags> wait_stages;
    std::vector<uint64_t> wait_values;
    wait_semaphores.reserve(desc.waits.size());
    wait_stages.reserve(desc.waits.size());
    wait_values.reserve(desc.waits.size());
    for (const GpuWait& wait : desc.waits) {
        const QueueState* source_queue = timeline_queue(wait.point.timeline);
        if (source_queue == nullptr || !wait.point.is_valid()) {
            return {Result::failure(ResultCode::kInvalidArgument, "Submit contains an invalid GPU wait point"), {}};
        }
        if (wait.point.value >= source_queue->next_signal_value) {
            return {Result::failure(ResultCode::kInvalidArgument,
                                    "Submit waits on a timeline value that has not been submitted"),
                    {}};
        }
        wait_semaphores.push_back(source_queue->timeline);
        wait_stages.push_back(to_vk_pipeline_stages(wait.destination_stages));
        wait_values.push_back(wait.point.value);
    }

    const uint64_t signal_value = queue->next_signal_value;
    const std::array<VkSemaphore, 1> signal_semaphores{queue->timeline};
    const std::array<uint64_t, 1> signal_values{signal_value};

    VkTimelineSemaphoreSubmitInfo timeline_info{};
    timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timeline_info.waitSemaphoreValueCount = static_cast<uint32_t>(wait_values.size());
    timeline_info.pWaitSemaphoreValues = wait_values.data();
    timeline_info.signalSemaphoreValueCount = static_cast<uint32_t>(signal_values.size());
    timeline_info.pSignalSemaphoreValues = signal_values.data();

    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.pNext = &timeline_info;
    submit_info.waitSemaphoreCount = static_cast<uint32_t>(wait_semaphores.size());
    submit_info.pWaitSemaphores = wait_semaphores.data();
    submit_info.pWaitDstStageMask = wait_stages.data();
    submit_info.commandBufferCount = static_cast<uint32_t>(command_buffers.size());
    submit_info.pCommandBuffers = command_buffers.data();
    submit_info.signalSemaphoreCount = static_cast<uint32_t>(signal_semaphores.size());
    submit_info.pSignalSemaphores = signal_semaphores.data();

    const VkResult result = vkQueueSubmit(queue->queue, 1, &submit_info, VK_NULL_HANDLE);
    if (result != VK_SUCCESS) {
        return {make_vulkan_result(result, "vkQueueSubmit"), {}};
    }

    ++queue->next_signal_value;
    SubmitResult submit_result{GpuSyncPoint{queue->timeline_handle, signal_value}};
    for (const CommandListHandle command_list : desc.command_lists) {
        command_lists_.get(command_list)->last_submitted = submit_result.completion;
    }
    return {Result::success(), submit_result};
}

Result VulkanDevice::wait(const GpuSyncPoint& point, uint64_t timeout_nanoseconds) {
    const QueueState* queue = timeline_queue(point.timeline);
    if (queue == nullptr || !point.is_valid()) {
        return Result::failure(ResultCode::kInvalidArgument, "Invalid GPU sync point");
    }
    if (point.value >= queue->next_signal_value) {
        return Result::failure(ResultCode::kInvalidArgument,
                               "Cannot wait on a timeline value that has not been submitted");
    }

    VkSemaphoreWaitInfo wait_info{};
    wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wait_info.semaphoreCount = 1;
    wait_info.pSemaphores = &queue->timeline;
    wait_info.pValues = &point.value;
    return make_vulkan_result(vkWaitSemaphores(device_, &wait_info, timeout_nanoseconds), "vkWaitSemaphores");
}

bool VulkanDevice::is_complete(const GpuSyncPoint& point) const {
    const QueueState* queue = timeline_queue(point.timeline);
    if (queue == nullptr || !point.is_valid()) {
        return false;
    }

    uint64_t completed_value = 0;
    if (vkGetSemaphoreCounterValue(device_, queue->timeline, &completed_value) != VK_SUCCESS) {
        return false;
    }
    return completed_value >= point.value;
}

Result VulkanDevice::wait_idle() {
    if (device_ == VK_NULL_HANDLE) {
        return Result::success();
    }
    return make_vulkan_result(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle");
}

bool VulkanDevice::buffer_view_is_referenced(BufferViewHandle view) const {
    bool referenced = false;
    binding_sets_.for_each([&](const BindingSetResource& set) {
        for (const BindingWrite& write : set.writes) {
            referenced = referenced || write.resource.buffer_view == view;
        }
    });
    return referenced;
}

bool VulkanDevice::texture_view_is_referenced(TextureViewHandle view) const {
    bool referenced = false;
    binding_sets_.for_each([&](const BindingSetResource& set) {
        for (const BindingWrite& write : set.writes) {
            referenced = referenced || write.resource.texture_view == view;
        }
    });
    return referenced;
}

bool VulkanDevice::sampler_is_referenced(SamplerHandle sampler) const {
    bool referenced = false;
    binding_sets_.for_each([&](const BindingSetResource& set) {
        for (const BindingWrite& write : set.writes) {
            referenced = referenced || write.resource.sampler == sampler;
        }
    });
    return referenced;
}

bool VulkanDevice::buffer_is_referenced(BufferHandle buffer) const {
    bool referenced = false;
    buffer_views_.for_each(
        [&](const BufferViewResource& view) { referenced = referenced || view.desc.buffer == buffer; });
    binding_sets_.for_each([&](const BindingSetResource& set) {
        for (const BindingWrite& write : set.writes) {
            if (write.resource.buffer_range.buffer == buffer) {
                referenced = true;
            }
            if (const BufferViewResource* view = buffer_views_.get(write.resource.buffer_view);
                view != nullptr && view->desc.buffer == buffer) {
                referenced = true;
            }
        }
    });
    return referenced;
}

bool VulkanDevice::texture_is_referenced(TextureHandle texture) const {
    bool referenced = false;
    texture_views_.for_each(
        [&](const TextureViewResource& view) { referenced = referenced || view.desc.texture == texture; });
    binding_sets_.for_each([&](const BindingSetResource& set) {
        for (const BindingWrite& write : set.writes) {
            if (const TextureViewResource* view = texture_views_.get(write.resource.texture_view);
                view != nullptr && view->desc.texture == texture) {
                referenced = true;
            }
        }
    });
    return referenced;
}

#if AEG_RHI_ENABLE_DEBUG_UTILS
void VulkanDevice::destroy_debug_messenger() {
    if (instance_ == VK_NULL_HANDLE || debug_messenger_ == VK_NULL_HANDLE) {
        return;
    }
    const auto destroy_messenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
    if (destroy_messenger != nullptr) {
        destroy_messenger(instance_, debug_messenger_, nullptr);
    }
    debug_messenger_ = VK_NULL_HANDLE;
}
#endif

void VulkanDevice::shutdown() {
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);

        swapchains_.for_each([&](SwapchainResource& resource) { destroy_swapchain_native(resource); });
        surfaces_.for_each([&](SurfaceResource& resource) {
            if (resource.surface != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
                vkDestroySurfaceKHR(instance_, resource.surface, nullptr);
                resource.surface = VK_NULL_HANDLE;
            }
            resource.present_family = kInvalidIndex;
            resource.present_queue = VK_NULL_HANDLE;
        });

        command_lists_.for_each([&](CommandListResource& resource) {
            if (resource.command_buffer != VK_NULL_HANDLE) {
                vkFreeCommandBuffers(device_, resource.pool, 1, &resource.command_buffer);
                resource.command_buffer = VK_NULL_HANDLE;
            }
        });
        binding_sets_.for_each([&](BindingSetResource& resource) {
            if (resource.pool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(device_, resource.pool, nullptr);
                resource.pool = VK_NULL_HANDLE;
                resource.set = VK_NULL_HANDLE;
            }
        });
        binding_layouts_.for_each([&](BindingLayoutResource& resource) {
            if (resource.layout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(device_, resource.layout, nullptr);
                resource.layout = VK_NULL_HANDLE;
            }
        });
        buffer_views_.for_each([&](BufferViewResource& resource) {
            if (resource.view != VK_NULL_HANDLE) {
                vkDestroyBufferView(device_, resource.view, nullptr);
                resource.view = VK_NULL_HANDLE;
            }
        });
        texture_views_.for_each([&](TextureViewResource& resource) {
            if (resource.view != VK_NULL_HANDLE) {
                vkDestroyImageView(device_, resource.view, nullptr);
                resource.view = VK_NULL_HANDLE;
            }
        });
        samplers_.for_each([&](SamplerResource& resource) {
            if (resource.sampler != VK_NULL_HANDLE) {
                vkDestroySampler(device_, resource.sampler, nullptr);
                resource.sampler = VK_NULL_HANDLE;
            }
        });
        buffers_.for_each([&](BufferResource& resource) {
            if (resource.buffer != VK_NULL_HANDLE) {
                vmaDestroyBuffer(allocator_, resource.buffer, resource.allocation.handle);
                resource.buffer = VK_NULL_HANDLE;
            }
            resource.allocation.handle = nullptr;
        });
        textures_.for_each([&](TextureResource& resource) {
            if (!resource.external && resource.image != VK_NULL_HANDLE) {
                vmaDestroyImage(allocator_, resource.image, resource.allocation.handle);
                resource.image = VK_NULL_HANDLE;
            }
            resource.allocation.handle = nullptr;
            resource.external = false;
            resource.owner_swapchain = {};
        });
        for (QueueState& state : queues_) {
            if (state.timeline != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_, state.timeline, nullptr);
                state.timeline = VK_NULL_HANDLE;
            }
            if (state.command_pool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(device_, state.command_pool, nullptr);
                state.command_pool = VK_NULL_HANDLE;
            }
        }

        if (allocator_ != nullptr) {
            vmaDestroyAllocator(allocator_);
            allocator_ = nullptr;
        }
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
        physical_device_ = VK_NULL_HANDLE;
    }

#if AEG_RHI_ENABLE_DEBUG_UTILS
    destroy_debug_messenger();
#endif
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

ResultValue<std::unique_ptr<Device>> create_vulkan_device(const DeviceCreateDesc& desc) {
    auto device = std::make_unique<VulkanDevice>();
    const Result result = device->initialize(desc);
    if (!result) {
        return {result, nullptr};
    }

    std::unique_ptr<Device> public_device = std::move(device);
    return {Result::success(), std::move(public_device)};
}

const char* result_code_name(ResultCode code) noexcept {
    switch (code) {
        case ResultCode::kSuccess:
            return "Success";
        case ResultCode::kInvalidArgument:
            return "InvalidArgument";
        case ResultCode::kInvalidState:
            return "InvalidState";
        case ResultCode::kUnsupported:
            return "Unsupported";
        case ResultCode::kNotFound:
            return "NotFound";
        case ResultCode::kOutOfMemory:
            return "OutOfMemory";
        case ResultCode::kTimeout:
            return "Timeout";
        case ResultCode::kDeviceLost:
            return "DeviceLost";
        case ResultCode::kVulkanError:
            return "VulkanError";
        case ResultCode::kNotReady:
            return "NotReady";
        case ResultCode::kOutOfDate:
            return "OutOfDate";
        case ResultCode::kSurfaceLost:
            return "SurfaceLost";
        case ResultCode::kUnknown:
            return "Unknown";
    }
    return "Unknown";
}
}  // namespace aeg::rhi
