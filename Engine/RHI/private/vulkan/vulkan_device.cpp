/**
 * @file vulkan_device.cpp
 * @brief Implements the Vulkan backend behind the backend-neutral RHI.
 *
 * This translation unit owns Vulkan object creation, command recording,
 * resource state tracking, queue synchronization, presentation, and deferred
 * destruction. Vulkan types intentionally remain private to this backend.
 */
#include <logging.hpp>

#include "../../public/rhi.hpp"
#include "vulkan_config.hpp"

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
#include <bit>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace aeg::rhi {
using foundation::Result;
using foundation::ResultCode;
using foundation::ResultValue;

namespace {
constexpr uint32_t kInvalidIndex = std::numeric_limits<uint32_t>::max();

/**
 * @brief Stores handles in dense storage while preserving generation checks.
 *
 * Dense storage keeps iteration and resource scans cache-friendly;
 * sparse slots make stale handles fail validation after deletion.
 */

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

template <typename T>
void add_unique(std::vector<T>& values, T value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

[[nodiscard]] Result make_vulkan_result(VkResult result, std::string_view operation) {
    if (result == VK_SUCCESS) {
        return Result::success();
    }

    ResultCode code = ResultCode::kBackendError;
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

#if AEG_VULKAN_ENABLE_VALIDATION
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

#if AEG_VULKAN_ENABLE_DEBUG_NAMES
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

#if AEG_VULKAN_ENABLE_VALIDATION
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

#if AEG_VULKAN_ENABLE_VALIDATION || AEG_VULKAN_ENABLE_DEBUG_UTILS
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

[[nodiscard]] AdapterType to_rhi_adapter_type(VkPhysicalDeviceType type) noexcept {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            return AdapterType::kIntegrated;
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            return AdapterType::kDiscrete;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            return AdapterType::kVirtual;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            return AdapterType::kCpu;
        case VK_PHYSICAL_DEVICE_TYPE_OTHER:
            return AdapterType::kOther;
        default:
            return AdapterType::kOther;
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
    if (has_flag(capabilities, BufferCapability::kTypedView)) {
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
    return usage;
}

struct ImageUsageState {
    VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    VkPipelineStageFlags stages{VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT};
    VkAccessFlags access{0};
};

[[nodiscard]] std::optional<ImageUsageState> to_vk_image_usage_state(ResourceUsage usage) {
    switch (usage) {
        case ResourceUsage::kUndefined:
            return ImageUsageState{VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0};
        case ResourceUsage::kCopySource:
            return ImageUsageState{VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_ACCESS_TRANSFER_READ_BIT};
        case ResourceUsage::kCopyDestination:
            return ImageUsageState{VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_ACCESS_TRANSFER_WRITE_BIT};
        case ResourceUsage::kVertexInput:
            return ImageUsageState{VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                                   VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT};
        case ResourceUsage::kIndexInput:
            return ImageUsageState{VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                                   VK_ACCESS_INDEX_READ_BIT};
        case ResourceUsage::kIndirectArguments:
            return ImageUsageState{VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                                   VK_ACCESS_INDIRECT_COMMAND_READ_BIT};
        case ResourceUsage::kUniformRead:
        case ResourceUsage::kSampledRead:
        case ResourceUsage::kTypedRead:
            return ImageUsageState{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                   VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   VK_ACCESS_SHADER_READ_BIT};
        case ResourceUsage::kStorageRead:
            return ImageUsageState{VK_IMAGE_LAYOUT_GENERAL,
                                   VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   VK_ACCESS_SHADER_READ_BIT};
        case ResourceUsage::kStorageWrite:
            return ImageUsageState{VK_IMAGE_LAYOUT_GENERAL,
                                   VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   VK_ACCESS_SHADER_WRITE_BIT};
        case ResourceUsage::kStorageReadWrite:
            return ImageUsageState{VK_IMAGE_LAYOUT_GENERAL,
                                   VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
        case ResourceUsage::kColorAttachmentRead:
            return ImageUsageState{VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT};
        case ResourceUsage::kColorAttachmentWrite:
            return ImageUsageState{VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
        case ResourceUsage::kDepthStencilRead:
            return ImageUsageState{
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT};
        case ResourceUsage::kDepthStencilWrite:
            return ImageUsageState{
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
        case ResourceUsage::kResolveSource:
            return ImageUsageState{VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT};
        case ResourceUsage::kResolveDestination:
            return ImageUsageState{VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
        case ResourceUsage::kPresent:
            return ImageUsageState{VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                   VK_ACCESS_MEMORY_READ_BIT};
        case ResourceUsage::kHostRead:
            return ImageUsageState{VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT};
        case ResourceUsage::kHostWrite:
            return ImageUsageState{VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_WRITE_BIT};
        case ResourceUsage::kAccelerationStructureRead:
        case ResourceUsage::kAccelerationStructureBuildRead:
        case ResourceUsage::kAccelerationStructureBuildWrite:
            return ImageUsageState{VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    }
    return std::nullopt;
}

[[nodiscard]] VkAttachmentLoadOp to_vk_load_op(LoadOp operation) {
    switch (operation) {
        case LoadOp::kLoad:
            return VK_ATTACHMENT_LOAD_OP_LOAD;
        case LoadOp::kClear:
            return VK_ATTACHMENT_LOAD_OP_CLEAR;
        case LoadOp::kDontCare:
            return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
}

[[nodiscard]] VkAttachmentStoreOp to_vk_store_op(StoreOp operation) {
    switch (operation) {
        case StoreOp::kStore:
            return VK_ATTACHMENT_STORE_OP_STORE;
        case StoreOp::kDontCare:
            return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_STORE_OP_DONT_CARE;
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
        case Format::kRgb32Float:
            return VK_FORMAT_R32G32B32_SFLOAT;
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
        case VK_FORMAT_R32G32B32_SFLOAT:
            return Format::kRgb32Float;
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
        case PresentMode::kLowLatency:
            return VK_PRESENT_MODE_MAILBOX_KHR;
        case PresentMode::kVsync:
            return VK_PRESENT_MODE_FIFO_KHR;
        case PresentMode::kAdaptiveVsync:
            return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

[[nodiscard]] PresentMode from_vk_present_mode(VkPresentModeKHR mode) noexcept {
    switch (mode) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR:
            return PresentMode::kImmediate;
        case VK_PRESENT_MODE_MAILBOX_KHR:
            return PresentMode::kLowLatency;
        case VK_PRESENT_MODE_FIFO_KHR:
            return PresentMode::kVsync;
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
            return PresentMode::kAdaptiveVsync;
        default:
            return PresentMode::kVsync;
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

[[nodiscard]] std::optional<VkShaderStageFlagBits> to_vk_shader_stage(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::kVertex:
            return VK_SHADER_STAGE_VERTEX_BIT;
        case ShaderStage::kTessellationControl:
            return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        case ShaderStage::kTessellationEvaluation:
            return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        case ShaderStage::kGeometry:
            return VK_SHADER_STAGE_GEOMETRY_BIT;
        case ShaderStage::kFragment:
            return VK_SHADER_STAGE_FRAGMENT_BIT;
        case ShaderStage::kCompute:
            return VK_SHADER_STAGE_COMPUTE_BIT;
        case ShaderStage::kRayTracing:
#ifdef VK_SHADER_STAGE_RAYGEN_BIT_KHR
            return VK_SHADER_STAGE_RAYGEN_BIT_KHR;
#else
            return std::nullopt;
#endif
        case ShaderStage::kMesh:
#ifdef VK_SHADER_STAGE_MESH_BIT_EXT
            return VK_SHADER_STAGE_MESH_BIT_EXT;
#else
            return std::nullopt;
#endif
        case ShaderStage::kNone:
        case ShaderStage::kAll:
            break;
    }
    return std::nullopt;
}

[[nodiscard]] VkPrimitiveTopology to_vk_primitive_topology(PrimitiveTopology topology) {
    switch (topology) {
        case PrimitiveTopology::kPointList:
            return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case PrimitiveTopology::kLineList:
            return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case PrimitiveTopology::kLineStrip:
            return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case PrimitiveTopology::kTriangleList:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case PrimitiveTopology::kTriangleStrip:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case PrimitiveTopology::kTriangleFan:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        case PrimitiveTopology::kLineListWithAdjacency:
            return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
        case PrimitiveTopology::kLineStripWithAdjacency:
            return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
        case PrimitiveTopology::kTriangleListWithAdjacency:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
        case PrimitiveTopology::kTriangleStripWithAdjacency:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
        case PrimitiveTopology::kPatchList:
            return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

[[nodiscard]] VkVertexInputRate to_vk_vertex_input_rate(VertexInputRate rate) {
    return rate == VertexInputRate::kInstance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
}

[[nodiscard]] VkPolygonMode to_vk_polygon_mode(PolygonMode mode) {
    switch (mode) {
        case PolygonMode::kFill:
            return VK_POLYGON_MODE_FILL;
        case PolygonMode::kLine:
            return VK_POLYGON_MODE_LINE;
        case PolygonMode::kPoint:
            return VK_POLYGON_MODE_POINT;
    }
    return VK_POLYGON_MODE_FILL;
}

[[nodiscard]] VkCullModeFlags to_vk_cull_mode(CullMode mode) {
    switch (mode) {
        case CullMode::kNone:
            return VK_CULL_MODE_NONE;
        case CullMode::kFront:
            return VK_CULL_MODE_FRONT_BIT;
        case CullMode::kBack:
            return VK_CULL_MODE_BACK_BIT;
        case CullMode::kFrontAndBack:
            return VK_CULL_MODE_FRONT_AND_BACK;
    }
    return VK_CULL_MODE_BACK_BIT;
}

[[nodiscard]] VkFrontFace to_vk_front_face(FrontFace front_face) {
    return front_face == FrontFace::kClockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
}

[[nodiscard]] VkCompareOp to_vk_compare_op(CompareOp operation) {
    switch (operation) {
        case CompareOp::kNever:
            return VK_COMPARE_OP_NEVER;
        case CompareOp::kLess:
            return VK_COMPARE_OP_LESS;
        case CompareOp::kEqual:
            return VK_COMPARE_OP_EQUAL;
        case CompareOp::kLessOrEqual:
            return VK_COMPARE_OP_LESS_OR_EQUAL;
        case CompareOp::kGreater:
            return VK_COMPARE_OP_GREATER;
        case CompareOp::kNotEqual:
            return VK_COMPARE_OP_NOT_EQUAL;
        case CompareOp::kGreaterOrEqual:
            return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case CompareOp::kAlways:
            return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_ALWAYS;
}

[[nodiscard]] VkBlendFactor to_vk_blend_factor(BlendFactor factor) {
    switch (factor) {
        case BlendFactor::kZero:
            return VK_BLEND_FACTOR_ZERO;
        case BlendFactor::kOne:
            return VK_BLEND_FACTOR_ONE;
        case BlendFactor::kSourceColor:
            return VK_BLEND_FACTOR_SRC_COLOR;
        case BlendFactor::kOneMinusSourceColor:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case BlendFactor::kDestinationColor:
            return VK_BLEND_FACTOR_DST_COLOR;
        case BlendFactor::kOneMinusDestinationColor:
            return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case BlendFactor::kSourceAlpha:
            return VK_BLEND_FACTOR_SRC_ALPHA;
        case BlendFactor::kOneMinusSourceAlpha:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::kDestinationAlpha:
            return VK_BLEND_FACTOR_DST_ALPHA;
        case BlendFactor::kOneMinusDestinationAlpha:
            return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case BlendFactor::kConstantColor:
            return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case BlendFactor::kOneMinusConstantColor:
            return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case BlendFactor::kConstantAlpha:
            return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case BlendFactor::kOneMinusConstantAlpha:
            return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        case BlendFactor::kSourceAlphaSaturate:
            return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case BlendFactor::kSourceOneColor:
            return VK_BLEND_FACTOR_SRC1_COLOR;
        case BlendFactor::kOneMinusSourceOneColor:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
        case BlendFactor::kSourceOneAlpha:
            return VK_BLEND_FACTOR_SRC1_ALPHA;
        case BlendFactor::kOneMinusSourceOneAlpha:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}

[[nodiscard]] bool uses_dual_source_blend(BlendFactor factor) {
    return factor == BlendFactor::kSourceOneColor || factor == BlendFactor::kOneMinusSourceOneColor ||
           factor == BlendFactor::kSourceOneAlpha || factor == BlendFactor::kOneMinusSourceOneAlpha;
}

[[nodiscard]] VkBlendOp to_vk_blend_op(BlendOp operation) {
    switch (operation) {
        case BlendOp::kAdd:
            return VK_BLEND_OP_ADD;
        case BlendOp::kSubtract:
            return VK_BLEND_OP_SUBTRACT;
        case BlendOp::kReverseSubtract:
            return VK_BLEND_OP_REVERSE_SUBTRACT;
        case BlendOp::kMin:
            return VK_BLEND_OP_MIN;
        case BlendOp::kMax:
            return VK_BLEND_OP_MAX;
    }
    return VK_BLEND_OP_ADD;
}

[[nodiscard]] VkColorComponentFlags to_vk_color_write_mask(ColorComponent components) {
    VkColorComponentFlags flags = 0;
    if (has_flag(components, ColorComponent::kR)) {
        flags |= VK_COLOR_COMPONENT_R_BIT;
    }
    if (has_flag(components, ColorComponent::kG)) {
        flags |= VK_COLOR_COMPONENT_G_BIT;
    }
    if (has_flag(components, ColorComponent::kB)) {
        flags |= VK_COLOR_COMPONENT_B_BIT;
    }
    if (has_flag(components, ColorComponent::kA)) {
        flags |= VK_COLOR_COMPONENT_A_BIT;
    }
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

[[nodiscard]] VkPipelineStageFlags2 to_vk_pipeline_stages2(PipelineStageMask stages) {
    if (has_flag(stages, PipelineStage::kAllCommands)) {
        return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }

    VkPipelineStageFlags2 flags = 0;
    if (has_flag(stages, PipelineStage::kCopy)) {
        flags |= VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    }
    if (has_flag(stages, PipelineStage::kVertexInput)) {
        flags |= VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT;
    }
    if (has_flag(stages, PipelineStage::kVertexShader)) {
        flags |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    }
    if (has_flag(stages, PipelineStage::kFragmentShader)) {
        flags |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    }
    if (has_flag(stages, PipelineStage::kEarlyDepth)) {
        flags |= VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
    }
    if (has_flag(stages, PipelineStage::kLateDepth)) {
        flags |= VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    }
    if (has_flag(stages, PipelineStage::kColorOutput)) {
        flags |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    if (has_flag(stages, PipelineStage::kCompute)) {
        flags |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    }
    if (has_flag(stages, PipelineStage::kRayTracing)) {
        flags |= VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    }
    if (has_flag(stages, PipelineStage::kHost)) {
        flags |= VK_PIPELINE_STAGE_2_HOST_BIT;
    }
    return flags == 0 ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : flags;
}

[[nodiscard]] VkPipelineStageFlags2 to_vk_shader_pipeline_stages2(ShaderStageMask stages) {
    if (has_flag(stages, ShaderStage::kAll)) {
        return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }

    VkPipelineStageFlags2 flags = 0;
    if (has_flag(stages, ShaderStage::kVertex)) {
        flags |= VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    }
    if (has_flag(stages, ShaderStage::kTessellationControl)) {
        flags |= VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT;
    }
    if (has_flag(stages, ShaderStage::kTessellationEvaluation)) {
        flags |= VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT;
    }
    if (has_flag(stages, ShaderStage::kGeometry)) {
        flags |= VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT;
    }
    if (has_flag(stages, ShaderStage::kFragment)) {
        flags |= VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    }
    if (has_flag(stages, ShaderStage::kCompute)) {
        flags |= VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    }
    if (has_flag(stages, ShaderStage::kRayTracing)) {
        flags |= VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    }
    if (has_flag(stages, ShaderStage::kMesh)) {
        flags |= VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
    }
    return flags == 0 ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : flags;
}

[[nodiscard]] VkAccessFlags2 to_vk_access2(ResourceUsage usage) {
    switch (usage) {
        case ResourceUsage::kUndefined:
            return 0;
        case ResourceUsage::kCopySource:
            return VK_ACCESS_2_TRANSFER_READ_BIT;
        case ResourceUsage::kCopyDestination:
            return VK_ACCESS_2_TRANSFER_WRITE_BIT;
        case ResourceUsage::kVertexInput:
            return VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
        case ResourceUsage::kIndexInput:
            return VK_ACCESS_2_INDEX_READ_BIT;
        case ResourceUsage::kIndirectArguments:
            return VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        case ResourceUsage::kUniformRead:
            return VK_ACCESS_2_UNIFORM_READ_BIT;
        case ResourceUsage::kSampledRead:
        case ResourceUsage::kTypedRead:
            return VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        case ResourceUsage::kStorageRead:
            return VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        case ResourceUsage::kStorageWrite:
            return VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        case ResourceUsage::kStorageReadWrite:
            return VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        case ResourceUsage::kColorAttachmentRead:
            return VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
        case ResourceUsage::kColorAttachmentWrite:
            return VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        case ResourceUsage::kDepthStencilRead:
            return VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        case ResourceUsage::kDepthStencilWrite:
            return VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        case ResourceUsage::kResolveSource:
            return VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
        case ResourceUsage::kResolveDestination:
            return VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        case ResourceUsage::kPresent:
            return VK_ACCESS_2_MEMORY_READ_BIT;
        case ResourceUsage::kHostRead:
            return VK_ACCESS_2_HOST_READ_BIT;
        case ResourceUsage::kHostWrite:
            return VK_ACCESS_2_HOST_WRITE_BIT;
        case ResourceUsage::kAccelerationStructureRead:
            return VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        case ResourceUsage::kAccelerationStructureBuildRead:
            return VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        case ResourceUsage::kAccelerationStructureBuildWrite:
            return VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    }
    return VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
}

struct VulkanResourceState {
    VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    VkPipelineStageFlags stages{VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT};
    VkAccessFlags access{0};
    VkPipelineStageFlags2 stages2{VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT};
    VkAccessFlags2 access2{0};
};

[[nodiscard]] VulkanResourceState to_vk_resource_state(const ResourceState& state, bool image) {
    const auto image_state = to_vk_image_usage_state(state.usage);
    VulkanResourceState result{};
    if (image_state.has_value()) {
        result.layout = image ? image_state->layout : VK_IMAGE_LAYOUT_UNDEFINED;
        result.stages = image_state->stages;
        result.access = image_state->access;
    }
    result.access2 = to_vk_access2(state.usage);
    result.stages2 = to_vk_pipeline_stages2(state.pipeline_stages);
    if (state.pipeline_stages == PipelineStage::kAllCommands && state.shader_stages != ShaderStage::kAll &&
        (state.usage == ResourceUsage::kUniformRead || state.usage == ResourceUsage::kSampledRead ||
         state.usage == ResourceUsage::kTypedRead || state.usage == ResourceUsage::kStorageRead ||
         state.usage == ResourceUsage::kStorageWrite || state.usage == ResourceUsage::kStorageReadWrite)) {
        result.stages2 = to_vk_shader_pipeline_stages2(state.shader_stages);
        result.stages = 0;
        if (has_flag(state.shader_stages, ShaderStage::kVertex)) {
            result.stages |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
        }
        if (has_flag(state.shader_stages, ShaderStage::kFragment)) {
            result.stages |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        }
        if (has_flag(state.shader_stages, ShaderStage::kCompute)) {
            result.stages |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        }
        if (result.stages == 0) {
            result.stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        }
    }
    return result;
}

[[nodiscard]] VulkanResourceState to_vk_discard_source_state(const ResourceState& state, bool image) {
    VulkanResourceState result = to_vk_resource_state(state, image);
    if (image) {
        result.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    result.stages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    result.access = 0;
    result.stages2 = VK_PIPELINE_STAGE_2_NONE;
    result.access2 = 0;
    return result;
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
        case BindingType::kReadOnlyTypedBuffer:
            return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        case BindingType::kReadWriteTypedBuffer:
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

[[nodiscard]] std::optional<uint32_t> descriptor_type_index(VkDescriptorType type) noexcept {
    switch (type) {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
            return 0;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            return 1;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            return 2;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            return 3;
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            return 4;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            return 5;
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            return 6;
        default:
            return std::nullopt;
    }
}

[[nodiscard]] VkDescriptorType descriptor_type_from_index(uint32_t index) noexcept {
    switch (index) {
        case 0:
            return VK_DESCRIPTOR_TYPE_SAMPLER;
        case 1:
            return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        case 2:
            return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case 3:
            return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case 4:
            return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case 5:
            return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        case 6:
            return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
        default:
            return VK_DESCRIPTOR_TYPE_SAMPLER;
    }
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

constexpr size_t kQueueCount = 3;
constexpr uint32_t kAspectPlaneCount = 3;
constexpr size_t kDescriptorTypeCount = 7;
constexpr uint32_t kDescriptorPageInitialSets = 64;
constexpr uint32_t kDescriptorPageMaxSets = 4096;
constexpr uint32_t kDescriptorPageMinimumDescriptors = 256;
constexpr uint32_t kDescriptorLifetimeCount = 2;
constexpr uint32_t kDescriptorFragmentationScoreMax = 1000;
constexpr uint32_t kDescriptorFragmentationScoreThreshold = 700;
constexpr uint32_t kDescriptorFragmentationFailurePenalty = 450;
constexpr uint32_t kDescriptorFragmentationSuccessDecay = 64;
using QueueValues = std::array<uint64_t, kQueueCount>;
using DescriptorCounts = std::array<uint32_t, kDescriptorTypeCount>;

struct DescriptorPoolRequirements {
    DescriptorCounts descriptor_counts{};
    uint32_t descriptor_mask{0};
    uint32_t total_descriptor_count{0};
    uint32_t set_count{1};
};

[[nodiscard]] constexpr uint32_t descriptor_lifetime_index(BindingSetLifetime lifetime) noexcept {
    return lifetime == BindingSetLifetime::kFrameLocal ? 1u : 0u;
}

[[nodiscard]] uint32_t queue_value_index(QueueClass queue) noexcept {
    switch (queue) {
        case QueueClass::kCompute:
            return 1;
        case QueueClass::kCopy:
            return 2;
        case QueueClass::kAuto:
        case QueueClass::kGraphics:
        case QueueClass::kPresent:
            return 0;
    }
    return 0;
}

[[nodiscard]] bool resource_state_matches(const ResourceState& actual, const ResourceState& expected) noexcept {
    if (actual.usage != expected.usage) {
        return false;
    }
    if (expected.queue != QueueClass::kAuto && actual.queue != expected.queue) {
        return false;
    }
    if (expected.pipeline_stages != PipelineStage::kAllCommands && actual.pipeline_stages != expected.pipeline_stages) {
        return false;
    }
    if (expected.shader_stages != ShaderStage::kAll && actual.shader_stages != expected.shader_stages) {
        return false;
    }
    return true;
}

[[nodiscard]] bool is_storage_usage(ResourceUsage usage) noexcept {
    return usage == ResourceUsage::kStorageRead || usage == ResourceUsage::kStorageWrite ||
           usage == ResourceUsage::kStorageReadWrite;
}

struct BufferStateSegment {
    uint64_t offset{0};
    uint64_t size{0};
    ResourceState state{};
};

[[nodiscard]] bool resource_usage_matches(const ResourceState& actual, const ResourceState& expected) noexcept {
    const bool exact_usage_match = actual.usage == expected.usage;
    const bool compatible_storage_usage =
        is_storage_usage(actual.usage) && is_storage_usage(expected.usage) &&
        (actual.usage == ResourceUsage::kStorageReadWrite || expected.usage == ResourceUsage::kStorageReadWrite);
    return (exact_usage_match || compatible_storage_usage) &&
           (expected.queue == QueueClass::kAuto || actual.queue == expected.queue);
}

void split_buffer_state_segment(std::vector<BufferStateSegment>& segments, uint64_t position) {
    for (size_t index = 0; index < segments.size(); ++index) {
        BufferStateSegment& segment = segments[index];
        const uint64_t segment_end = segment.offset + segment.size;
        if (position <= segment.offset || position >= segment_end) {
            continue;
        }
        const BufferStateSegment right{position, segment_end - position, segment.state};
        segment.size = position - segment.offset;
        segments.insert(segments.begin() + static_cast<std::ptrdiff_t>(index + 1), right);
        return;
    }
}

void merge_buffer_state_segments(std::vector<BufferStateSegment>& segments) noexcept {
    for (size_t index = 1; index < segments.size();) {
        BufferStateSegment& previous = segments[index - 1];
        BufferStateSegment& current = segments[index];
        if (previous.offset + previous.size == current.offset && previous.state == current.state) {
            previous.size += current.size;
            segments.erase(segments.begin() + static_cast<std::ptrdiff_t>(index));
            continue;
        }
        ++index;
    }
}

void initialize_buffer_state_segments(std::vector<BufferStateSegment>& segments, uint64_t buffer_size,
                                      const ResourceState& initial_state) {
    if (segments.empty() && buffer_size != 0) {
        segments.push_back(BufferStateSegment{0, buffer_size, initial_state});
    }
}

[[nodiscard]] bool resolve_buffer_range(uint64_t buffer_size, uint64_t offset, uint64_t size, uint64_t& end) noexcept {
    if (offset > buffer_size) {
        return false;
    }
    if (size == std::numeric_limits<uint64_t>::max()) {
        end = buffer_size;
        return true;
    }
    if (size > buffer_size - offset) {
        return false;
    }
    end = offset + size;
    return true;
}

struct BufferStateChange {
    BufferHandle buffer{};
    uint64_t offset{0};
    uint64_t size{0};
    ResourceState before{};
    ResourceState after{};
};

struct CommandBufferState {
    BufferHandle buffer{};
    std::vector<BufferStateSegment> segments{};
};

struct TextureStateChange {
    TextureHandle texture{};
    uint32_t subresource_index{0};
    ResourceState before{};
    ResourceState after{};
};

enum class QueueOwnershipPhase : uint8_t { kNone, kRelease, kAcquire };

struct BufferOwnershipTransfer {
    BufferHandle buffer{};
    uint64_t offset{0};
    uint64_t size{0};
    ResourceState before{};
    ResourceState after{};
    bool discard_contents{false};
    uint32_t source_family{kInvalidIndex};
    uint32_t destination_family{kInvalidIndex};
    QueueOwnershipPhase phase{QueueOwnershipPhase::kNone};
};

struct TextureOwnershipTransfer {
    TextureHandle texture{};
    VkImageSubresourceRange range{};
    ResourceState before{};
    ResourceState after{};
    bool discard_contents{false};
    uint32_t source_family{kInvalidIndex};
    uint32_t destination_family{kInvalidIndex};
    QueueOwnershipPhase phase{QueueOwnershipPhase::kNone};
};

struct TextureStateKey {
    uint64_t texture{0};
    uint32_t subresource_index{0};

    friend constexpr bool operator==(TextureStateKey, TextureStateKey) = default;
};

struct TextureStateKeyHash {
    [[nodiscard]] size_t operator()(const TextureStateKey& key) const noexcept {
        uint64_t hash = key.texture + 0x9e3779b97f4a7c15ull;
        hash ^= hash >> 30u;
        hash *= 0xbf58476d1ce4e5b9ull;
        hash ^= hash >> 27u;
        hash *= 0x94d049bb133111ebull;
        hash ^= hash >> 31u;
        hash ^= static_cast<uint64_t>(key.subresource_index) + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
        return static_cast<size_t>(hash);
    }
};

struct CopyFormatInfo {
    uint32_t bytes_per_block{0};
    uint32_t block_width{1};
    uint32_t block_height{1};
    uint32_t block_depth{1};
    bool compressed{false};
};

[[nodiscard]] std::optional<CopyFormatInfo> copy_format_info(Format format, AspectMask aspects) noexcept {
    const uint32_t aspect_bits = static_cast<uint32_t>(aspects);
    if (aspect_bits == static_cast<uint32_t>(Aspect::kColor)) {
        switch (format) {
            case Format::kR8Unorm:
                return CopyFormatInfo{1, 1, 1, 1, false};
            case Format::kRg8Unorm:
                return CopyFormatInfo{2, 1, 1, 1, false};
            case Format::kRgba8Unorm:
            case Format::kBgra8Unorm:
            case Format::kRgba8Srgb:
            case Format::kR32Uint:
            case Format::kR32Float:
                return CopyFormatInfo{4, 1, 1, 1, false};
            case Format::kRgba16Float:
            case Format::kRg32Float:
                return CopyFormatInfo{8, 1, 1, 1, false};
            case Format::kRgb32Float:
                return CopyFormatInfo{12, 1, 1, 1, false};
            case Format::kRgba32Float:
                return CopyFormatInfo{16, 1, 1, 1, false};
            case Format::kBc1RgbaUnorm:
                return CopyFormatInfo{8, 4, 4, 1, true};
            case Format::kBc3RgbaUnorm:
            case Format::kBc5Unorm:
            case Format::kBc7RgbaUnorm:
                return CopyFormatInfo{16, 4, 4, 1, true};
            case Format::kD24UnormS8Uint:
            case Format::kD32Float:
            case Format::kD32FloatS8Uint:
            case Format::kUnknown:
                return std::nullopt;
        }
    }
    if (aspect_bits == static_cast<uint32_t>(Aspect::kDepth)) {
        switch (format) {
            case Format::kD24UnormS8Uint:
            case Format::kD32Float:
            case Format::kD32FloatS8Uint:
                return CopyFormatInfo{4, 1, 1, 1, false};
            case Format::kR8Unorm:
            case Format::kRg8Unorm:
            case Format::kRgba8Unorm:
            case Format::kBgra8Unorm:
            case Format::kRgba8Srgb:
            case Format::kRgba16Float:
            case Format::kRgba32Float:
            case Format::kR32Uint:
            case Format::kR32Float:
            case Format::kRg32Float:
            case Format::kRgb32Float:
            case Format::kBc1RgbaUnorm:
            case Format::kBc3RgbaUnorm:
            case Format::kBc5Unorm:
            case Format::kBc7RgbaUnorm:
            case Format::kUnknown:
                return std::nullopt;
        }
    }
    if (aspect_bits == static_cast<uint32_t>(Aspect::kStencil)) {
        switch (format) {
            case Format::kD24UnormS8Uint:
            case Format::kD32FloatS8Uint:
                return CopyFormatInfo{1, 1, 1, 1, false};
            case Format::kR8Unorm:
            case Format::kRg8Unorm:
            case Format::kRgba8Unorm:
            case Format::kBgra8Unorm:
            case Format::kRgba8Srgb:
            case Format::kRgba16Float:
            case Format::kRgba32Float:
            case Format::kR32Uint:
            case Format::kR32Float:
            case Format::kRg32Float:
            case Format::kRgb32Float:
            case Format::kD32Float:
            case Format::kBc1RgbaUnorm:
            case Format::kBc3RgbaUnorm:
            case Format::kBc5Unorm:
            case Format::kBc7RgbaUnorm:
            case Format::kUnknown:
                return std::nullopt;
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool checked_add_u64(uint64_t lhs, uint64_t rhs, uint64_t& result) noexcept {
    if (rhs > std::numeric_limits<uint64_t>::max() - lhs) {
        return false;
    }
    result = lhs + rhs;
    return true;
}

[[nodiscard]] bool checked_mul_u64(uint64_t lhs, uint64_t rhs, uint64_t& result) noexcept {
    if (lhs != 0 && rhs > std::numeric_limits<uint64_t>::max() / lhs) {
        return false;
    }
    result = lhs * rhs;
    return true;
}

[[nodiscard]] uint64_t ceil_div_u64(uint64_t value, uint64_t divisor) noexcept {
    return value / divisor + (value % divisor == 0 ? 0 : 1);
}

[[nodiscard]] Result validate_copy_buffer_range(Format format, AspectMask aspects, uint32_t layer_count,
                                                const Extent3D& extent, uint32_t row_length, uint32_t image_height,
                                                uint64_t buffer_offset, uint64_t buffer_size,
                                                uint64_t* required_end = nullptr) {
    const std::optional<CopyFormatInfo> info = copy_format_info(format, aspects);
    if (!info.has_value() || layer_count == 0 || extent.width == 0 || extent.height == 0 || extent.depth == 0) {
        return Result::failure(ResultCode::kInvalidArgument, "Texture copy footprint is invalid");
    }
    if ((buffer_offset & 3u) != 0) {
        return Result::failure(ResultCode::kInvalidArgument, "Texture copy buffer offset must be four-byte aligned");
    }

    const uint64_t effective_row_length = row_length == 0 ? extent.width : row_length;
    const uint64_t effective_image_height = image_height == 0 ? extent.height : image_height;
    if (effective_row_length < extent.width || effective_image_height < extent.height) {
        return Result::failure(ResultCode::kInvalidArgument, "Texture copy row length or image height is too small");
    }
    if (info->compressed && ((row_length != 0 && row_length % info->block_width != 0) ||
                             (image_height != 0 && image_height % info->block_height != 0))) {
        return Result::failure(ResultCode::kInvalidArgument, "Compressed texture copy strides must be block aligned");
    }

    const uint64_t blocks_x = ceil_div_u64(extent.width, info->block_width);
    const uint64_t blocks_y = ceil_div_u64(extent.height, info->block_height);
    const uint64_t blocks_z = ceil_div_u64(extent.depth, info->block_depth);
    const uint64_t row_blocks = ceil_div_u64(effective_row_length, info->block_width);
    const uint64_t image_rows = ceil_div_u64(effective_image_height, info->block_height);

    uint64_t row_stride = 0;
    uint64_t image_stride = 0;
    uint64_t layer_stride = 0;
    if (!checked_mul_u64(row_blocks, info->bytes_per_block, row_stride) ||
        !checked_mul_u64(row_stride, image_rows, image_stride) ||
        !checked_mul_u64(image_stride, blocks_z, layer_stride)) {
        return Result::failure(ResultCode::kInvalidArgument, "Texture copy footprint overflows");
    }

    uint64_t last_layer = 0;
    uint64_t last_depth_slice = 0;
    uint64_t last_row = 0;
    uint64_t copied_row = 0;
    if (!checked_mul_u64(static_cast<uint64_t>(layer_count - 1), layer_stride, last_layer) ||
        !checked_mul_u64(blocks_z - 1, image_stride, last_depth_slice) ||
        !checked_mul_u64(blocks_y - 1, row_stride, last_row) ||
        !checked_mul_u64(blocks_x, info->bytes_per_block, copied_row)) {
        return Result::failure(ResultCode::kInvalidArgument, "Texture copy footprint overflows");
    }

    uint64_t last_offset = 0;
    uint64_t required_bytes = 0;
    if (!checked_add_u64(last_layer, last_depth_slice, last_offset) ||
        !checked_add_u64(last_offset, last_row, last_offset) ||
        !checked_add_u64(last_offset, copied_row, last_offset) ||
        !checked_add_u64(buffer_offset, last_offset, required_bytes) || required_bytes > buffer_size) {
        return Result::failure(ResultCode::kInvalidArgument, "Texture copy exceeds the buffer range");
    }
    if (required_end != nullptr) {
        *required_end = required_bytes;
    }
    return Result::success();
}

struct RetireBuffer {
    QueueValues required_values{};
    VkBuffer buffer{VK_NULL_HANDLE};
    VmaAllocation allocation{nullptr};
    bool mapped{false};
};

struct RetireTexture {
    QueueValues required_values{};
    VkImage image{VK_NULL_HANDLE};
    VmaAllocation allocation{nullptr};
};

struct RetireBufferView {
    QueueValues required_values{};
    VkBufferView view{VK_NULL_HANDLE};
};

struct RetireTextureView {
    QueueValues required_values{};
    VkImageView view{VK_NULL_HANDLE};
};

struct RetireSampler {
    QueueValues required_values{};
    VkSampler sampler{VK_NULL_HANDLE};
};

struct RetireBindingLayout {
    QueueValues required_values{};
    VkDescriptorSetLayout layout{VK_NULL_HANDLE};
};

struct RetireBindingSet {
    QueueValues required_values{};
    BindingSetLifetime lifetime{BindingSetLifetime::kPersistent};
    uint32_t page_index{kInvalidIndex};
    VkDescriptorSet set{VK_NULL_HANDLE};
    DescriptorPoolRequirements requirements{};
};

struct RetirePipeline {
    QueueValues required_values{};
    VkPipeline pipeline{VK_NULL_HANDLE};
    VkPipelineLayout layout{VK_NULL_HANDLE};
};

struct RetireCommandBuffer {
    QueueValues required_values{};
    VkCommandPool pool{VK_NULL_HANDLE};
    VkCommandBuffer command_buffer{VK_NULL_HANDLE};
};

using RetireRecord = std::variant<RetireBuffer, RetireTexture, RetireBufferView, RetireTextureView, RetireSampler,
                                  RetireBindingLayout, RetireBindingSet, RetirePipeline, RetireCommandBuffer>;

struct BufferResource {
    VkBuffer buffer{VK_NULL_HANDLE};
    Allocation allocation{};
    uint64_t size{0};
    BufferCapability capabilities{BufferCapability::kNone};
    ResourceState state{};
    std::vector<BufferStateSegment> state_segments{};
    QueueValues last_use{};
    void* mapped_data{nullptr};
};

struct TextureResource {
    VkImage image{VK_NULL_HANDLE};
    Allocation allocation{};
    TextureDesc desc{};
    std::vector<Format> compatible_formats{};
    ResourceState state{};
    std::vector<ResourceState> subresource_states{};
    QueueValues last_use{};
    bool external{false};
    SwapchainHandle owner_swapchain{};
};

struct BufferViewResource {
    BufferViewDesc desc{};
    VkBufferView view{VK_NULL_HANDLE};
    QueueValues last_use{};
};

struct TextureViewResource {
    TextureViewDesc desc{};
    VkImageView view{VK_NULL_HANDLE};
    bool owned_by_swapchain{false};
    SwapchainHandle owner_swapchain{};
    QueueValues last_use{};
};

struct SamplerResource {
    VkSampler sampler{VK_NULL_HANDLE};
    QueueValues last_use{};
};

struct BindingLayoutResource {
    VkDescriptorSetLayout layout{VK_NULL_HANDLE};
    uint16_t group{0};
    std::vector<BindingLayoutEntry> entries{};
    QueueValues last_use{};
};

enum class DescriptorPoolPageListState : uint8_t { kPreferred, kDeferred, kFull };

struct DescriptorPoolPage {
    VkDescriptorPool pool{VK_NULL_HANDLE};
    BindingSetLifetime lifetime{BindingSetLifetime::kPersistent};
    uint32_t max_sets{0};
    uint32_t allocated_sets{0};
    uint32_t live_sets{0};
    DescriptorCounts descriptor_capacity{};
    DescriptorCounts descriptor_usage{};
    uint32_t descriptor_capacity_mask{0};
    uint32_t available_descriptor_mask{0};
    uint32_t fragmentation_score{0};
    uint32_t allocation_failures{0};
    uint32_t list_position{kInvalidIndex};
    DescriptorPoolPageListState list_state{DescriptorPoolPageListState::kFull};
    bool allocation_blocked{false};
    bool driver_fragmented{false};
};

[[nodiscard]] bool descriptor_pool_can_fit(const DescriptorPoolPage& page,
                                           const DescriptorPoolRequirements& requirements) noexcept {
    if (page.pool == VK_NULL_HANDLE || requirements.set_count == 0 || requirements.set_count > page.max_sets ||
        page.allocated_sets > page.max_sets - requirements.set_count || page.allocation_blocked ||
        page.driver_fragmented) {
        return false;
    }
    if ((requirements.descriptor_mask & ~page.descriptor_capacity_mask) != 0 ||
        (requirements.descriptor_mask & ~page.available_descriptor_mask) != 0) {
        return false;
    }

    uint32_t descriptor_mask = requirements.descriptor_mask;
    while (descriptor_mask != 0) {
        const uint32_t index = std::countr_zero(descriptor_mask);
        const uint32_t required = requirements.descriptor_counts[index];
        if (required > page.descriptor_capacity[index] ||
            page.descriptor_usage[index] > page.descriptor_capacity[index] - required) {
            return false;
        }
        descriptor_mask &= descriptor_mask - 1u;
    }
    return true;
}

[[nodiscard]] uint32_t descriptor_pool_shape_fragmentation_score(const DescriptorPoolPage& page) noexcept {
    if (page.max_sets == 0 || page.allocated_sets >= page.max_sets) {
        return kDescriptorFragmentationScoreMax;
    }

    uint32_t minimum_free_ratio = kDescriptorFragmentationScoreMax;
    uint32_t maximum_free_ratio = 0;
    bool has_descriptor_capacity = false;
    for (uint32_t index = 0; index < kDescriptorTypeCount; ++index) {
        const uint32_t capacity = page.descriptor_capacity[index];
        if (capacity == 0) {
            continue;
        }

        has_descriptor_capacity = true;
        const uint32_t used = std::min(page.descriptor_usage[index], capacity);
        const uint32_t available = capacity - used;
        const uint32_t free_ratio =
            static_cast<uint32_t>((static_cast<uint64_t>(available) * kDescriptorFragmentationScoreMax) / capacity);
        minimum_free_ratio = std::min(minimum_free_ratio, free_ratio);
        maximum_free_ratio = std::max(maximum_free_ratio, free_ratio);
    }

    if (!has_descriptor_capacity) {
        return 0;
    }
    return maximum_free_ratio - minimum_free_ratio;
}

void refresh_descriptor_pool_page_state(DescriptorPoolPage& page) noexcept {
    page.descriptor_capacity_mask = 0;
    page.available_descriptor_mask = 0;
    for (uint32_t index = 0; index < kDescriptorTypeCount; ++index) {
        const uint32_t bit = 1u << index;
        if (page.descriptor_capacity[index] == 0) {
            continue;
        }
        page.descriptor_capacity_mask |= bit;
        if (page.descriptor_usage[index] < page.descriptor_capacity[index]) {
            page.available_descriptor_mask |= bit;
        }
    }

    const uint32_t shape_score = descriptor_pool_shape_fragmentation_score(page);
    page.fragmentation_score = std::max(page.fragmentation_score, shape_score);
    if (page.driver_fragmented) {
        page.fragmentation_score = kDescriptorFragmentationScoreMax;
    }
    if (page.allocation_blocked) {
        page.fragmentation_score = std::max(page.fragmentation_score, kDescriptorFragmentationScoreThreshold);
    }
}

[[nodiscard]] uint64_t descriptor_pool_fit_score(const DescriptorPoolPage& page,
                                                 const DescriptorPoolRequirements& requirements) noexcept {
    constexpr uint64_t kFitScale = 1'000'000ull;

    // Fragmentation is the primary factor.
    uint64_t score = static_cast<uint64_t>(page.fragmentation_score) * kFitScale;

    // Remaining sets, guarded against underflow.
    if (page.max_sets > 0) {
        const uint32_t remaining_sets = page.max_sets - page.allocated_sets - requirements.set_count;
        score += (static_cast<uint64_t>(remaining_sets) * kFitScale) / page.max_sets;
    } else {
        score += kFitScale;  // Apply a penalty.
    }

    uint32_t descriptor_mask = requirements.descriptor_mask;
    while (descriptor_mask != 0) {
        const uint32_t index = std::countr_zero(descriptor_mask);
        const uint32_t capacity = page.descriptor_capacity[index];
        if (capacity == 0) {
            score += kFitScale;  // This should not happen; apply a penalty.
        } else {
            const uint32_t remaining = capacity - page.descriptor_usage[index] - requirements.descriptor_counts[index];
            score += (static_cast<uint64_t>(remaining) * kFitScale) / capacity;
        }
        descriptor_mask &= descriptor_mask - 1u;
    }
    return score;
}

void record_descriptor_pool_allocation_failure(DescriptorPoolPage& page, VkResult result) noexcept {
    if (page.allocation_failures != std::numeric_limits<uint32_t>::max()) {
        ++page.allocation_failures;
    }
    page.allocation_blocked = true;
    if (result == VK_ERROR_FRAGMENTED_POOL) {
        page.driver_fragmented = true;
        page.fragmentation_score = kDescriptorFragmentationScoreMax;
    } else {
        page.fragmentation_score = std::max(page.fragmentation_score, kDescriptorFragmentationFailurePenalty);
    }
}

void record_descriptor_pool_allocation_success(DescriptorPoolPage& page) noexcept {
    page.allocation_blocked = false;
    if (!page.driver_fragmented) {
        page.fragmentation_score = page.fragmentation_score > kDescriptorFragmentationSuccessDecay
                                       ? page.fragmentation_score - kDescriptorFragmentationSuccessDecay
                                       : 0;
    }
    refresh_descriptor_pool_page_state(page);
}

[[nodiscard]] bool descriptor_pool_page_has_free_capacity(const DescriptorPoolPage& page) noexcept {
    if (page.pool == VK_NULL_HANDLE || page.max_sets == 0 || page.allocated_sets >= page.max_sets) {
        return false;
    }
    return page.descriptor_capacity_mask == 0 || page.available_descriptor_mask != 0;
}

[[nodiscard]] bool descriptor_pool_page_is_preferred(const DescriptorPoolPage& page) noexcept {
    return descriptor_pool_page_has_free_capacity(page) && !page.allocation_blocked && !page.driver_fragmented &&
           page.fragmentation_score < kDescriptorFragmentationScoreThreshold;
}

struct BindingSetResource {
    VkDescriptorSet set{VK_NULL_HANDLE};
    BindingLayoutHandle layout{};
    BindingSetLifetime lifetime{BindingSetLifetime::kPersistent};
    uint32_t descriptor_pool_page{kInvalidIndex};
    DescriptorPoolRequirements requirements{};
    std::vector<BindingWrite> writes{};
    QueueValues last_use{};
};

struct PipelineResource {
    VkPipeline pipeline{VK_NULL_HANDLE};
    VkPipelineLayout layout{VK_NULL_HANDLE};
    PipelineType type{PipelineType::kGraphics};
    std::vector<BindingLayoutHandle> binding_layouts{};
    std::vector<InlineConstantRange> inline_constant_ranges{};
    QueueValues last_use{};
};

struct CommandListResource {
    VkCommandPool pool{VK_NULL_HANDLE};
    VkCommandBuffer command_buffer{VK_NULL_HANDLE};
    QueueClass queue{QueueClass::kGraphics};
    bool one_time_submit{true};
    BarrierMode barrier_mode{BarrierMode::kImplicit};
    bool recording{false};
    bool ended{false};
    bool rendering{false};
    PipelineHandle bound_pipeline{};
    bool index_buffer_bound{false};
    BufferHandle bound_index_buffer{};
    uint64_t bound_index_offset{0};
    IndexFormat bound_index_format{IndexFormat::kUint32};
    GpuSyncPoint last_submitted{};
    std::vector<ColorAttachmentDesc> active_color_attachments{};
    DepthStencilAttachmentDesc active_depth_stencil{};
    std::vector<BufferHandle> referenced_buffers{};
    std::vector<TextureHandle> referenced_textures{};
    std::vector<TextureViewHandle> referenced_texture_views{};
    std::vector<PipelineHandle> referenced_pipelines{};
    std::vector<BindingSetHandle> referenced_binding_sets{};
    std::vector<BufferViewHandle> referenced_buffer_views{};
    std::vector<SamplerHandle> referenced_samplers{};
    std::vector<BufferStateChange> buffer_state_changes{};
    std::vector<CommandBufferState> buffer_states{};
    std::vector<TextureStateChange> texture_state_changes{};
    std::vector<BufferOwnershipTransfer> buffer_ownership_transfers{};
    std::vector<TextureOwnershipTransfer> texture_ownership_transfers{};
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
    VkFence in_flight{VK_NULL_HANDLE};
    uint32_t image_index{kRemaining};
    bool acquired{false};
    bool submitted{false};
    bool acquire_waited{false};
    bool present_signaled{false};
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
    std::vector<VkSemaphore> render_finished{};
    std::vector<uint32_t> image_in_flight_frame{};
    std::vector<FrameSlot> frames{};
    uint32_t current_frame{0};
    uint64_t generation{1};
    bool needs_recreate{false};
};

/**
 * @brief Vulkan implementation of the backend-neutral RHI device.
 *
 * The class owns all native Vulkan objects and maps them to typed RHI
 * handles. Its command recording path also tracks resource states so
 * barriers and queue ownership transfers can be emitted consistently.
 */

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

    [[nodiscard]] ResultValue<BufferStateSnapshot> query_buffer_state(BufferHandle buffer) const override;

    [[nodiscard]] Result write_buffer(BufferHandle buffer, uint64_t offset, std::span<const std::byte> data) override;

    [[nodiscard]] Result read_buffer(BufferHandle buffer, uint64_t offset, std::span<std::byte> data) override;

    [[nodiscard]] ResultValue<PipelineHandle> create_pipeline(const PipelineDesc& desc) override;

    [[nodiscard]] Result destroy_pipeline(PipelineHandle pipeline) override;

    [[nodiscard]] ResultValue<TextureHandle> create_texture(const TextureDesc& desc) override;

    [[nodiscard]] Result destroy_texture(TextureHandle texture) override;

    [[nodiscard]] ResultValue<TextureStateSnapshot> query_texture_state(TextureHandle texture) const override;

    [[nodiscard]] ResultValue<BufferViewHandle> create_buffer_view(const BufferViewDesc& desc) override;

    [[nodiscard]] Result destroy_buffer_view(BufferViewHandle view) override;

    [[nodiscard]] ResultValue<TextureViewHandle> create_texture_view(const TextureViewDesc& desc) override;

    [[nodiscard]] Result destroy_texture_view(TextureViewHandle view) override;

    [[nodiscard]] ResultValue<SamplerHandle> create_sampler(const SamplerDesc& desc) override;

    [[nodiscard]] Result destroy_sampler(SamplerHandle sampler) override;

    [[nodiscard]] ResultValue<BindingLayoutHandle> create_binding_layout(const BindingLayoutDesc& desc) override;

    [[nodiscard]] Result destroy_binding_layout(BindingLayoutHandle layout) override;

    [[nodiscard]] ResultValue<BindingSetHandle> create_binding_set(const BindingSetDesc& desc) override;

    [[nodiscard]] Result update_binding_set(BindingSetHandle set, std::span<const BindingWrite> writes) override;

    [[nodiscard]] Result destroy_binding_set(BindingSetHandle set) override;

    [[nodiscard]] ResultValue<CommandListHandle> create_command_list(const CommandListDesc& desc) override;

    [[nodiscard]] Result begin_command_list(CommandListHandle command_list) override;

    [[nodiscard]] Result begin_rendering(CommandListHandle command_list, const RenderingDesc& desc) override;

    [[nodiscard]] Result end_rendering(CommandListHandle command_list) override;

    [[nodiscard]] Result barrier(CommandListHandle command_list, const BarrierDesc& desc) override;

    [[nodiscard]] Result set_viewport(CommandListHandle command_list, const Viewport& viewport) override;

    [[nodiscard]] Result set_scissor(CommandListHandle command_list, const ScissorRect& scissor) override;

    [[nodiscard]] Result bind_pipeline(CommandListHandle command_list, PipelineHandle pipeline) override;

    [[nodiscard]] Result bind_binding_set(CommandListHandle command_list, uint32_t group, BindingSetHandle set,
                                          std::span<const uint32_t> dynamic_offsets) override;

    [[nodiscard]] Result set_inline_constants(CommandListHandle command_list, uint32_t offset,
                                              ShaderStageMask shader_stages, std::span<const std::byte> data) override;

    [[nodiscard]] Result bind_vertex_buffer(CommandListHandle command_list, uint32_t binding, BufferHandle buffer,
                                            uint64_t offset) override;

    [[nodiscard]] Result bind_index_buffer(CommandListHandle command_list, BufferHandle buffer, uint64_t offset,
                                           IndexFormat format) override;

    [[nodiscard]] Result draw(CommandListHandle command_list, const DrawDesc& desc) override;

    [[nodiscard]] Result draw_indexed(CommandListHandle command_list, const DrawIndexedDesc& desc) override;

    [[nodiscard]] Result draw_indirect(CommandListHandle command_list, const DrawIndirectDesc& desc) override;

    [[nodiscard]] Result dispatch(CommandListHandle command_list, const DispatchDesc& desc) override;

    [[nodiscard]] Result dispatch_indirect(CommandListHandle command_list, const DispatchIndirectDesc& desc) override;

    [[nodiscard]] Result copy_buffer(CommandListHandle command_list, const BufferCopyDesc& desc) override;

    [[nodiscard]] Result copy_buffer_to_texture(CommandListHandle command_list,
                                                const BufferTextureCopyDesc& desc) override;

    [[nodiscard]] Result copy_texture_to_buffer(CommandListHandle command_list,
                                                const TextureBufferCopyDesc& desc) override;

    [[nodiscard]] Result copy_texture(CommandListHandle command_list, const TextureCopyDesc& desc) override;

    [[nodiscard]] Result clear_texture(CommandListHandle command_list, const ClearTextureDesc& desc) override;

    [[nodiscard]] Result end_command_list(CommandListHandle command_list) override;

    [[nodiscard]] Result reset_command_list(CommandListHandle command_list) override;

    [[nodiscard]] Result destroy_command_list(CommandListHandle command_list) override;

    [[nodiscard]] ResultValue<SubmitResult> submit(const SubmitDesc& desc) override;

    [[nodiscard]] Result wait(const GpuSyncPoint& point, uint64_t timeout_nanoseconds) override;

    [[nodiscard]] bool is_complete(const GpuSyncPoint& point) const override;

    [[nodiscard]] Result collect_garbage() override;

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
        bool depth_clamp{false};
        bool fill_mode_non_solid{false};
        bool wide_lines{false};
        bool dual_src_blend{false};
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

    [[nodiscard]] Result submit_ownership_releases(std::span<const CommandListHandle> command_lists,
                                                   QueueClass destination_queue,
                                                   std::vector<VkSemaphore>& wait_semaphores,
                                                   std::vector<VkPipelineStageFlags>& wait_stages,
                                                   std::vector<uint64_t>& wait_values);

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

    [[nodiscard]] Result validate_command_recording(const CommandListResource& command) const;

    [[nodiscard]] Result ensure_command_buffer_state(CommandListResource& command, BufferHandle buffer);

    [[nodiscard]] CommandBufferState* find_command_buffer_state(CommandListResource& command,
                                                                BufferHandle buffer) noexcept;

    [[nodiscard]] Result validate_buffer_usage(CommandListResource& command, BufferHandle buffer, uint64_t offset,
                                               uint64_t size, const ResourceState& expected);

    [[nodiscard]] ResourceState current_texture_state(const CommandListResource& command, TextureHandle texture,
                                                      uint32_t subresource_index) const;

    [[nodiscard]] Result record_barriers(CommandListResource& command, std::span<const BufferBarrier> buffers,
                                         std::span<const TextureBarrier> textures);

    [[nodiscard]] Result transition_buffer(CommandListResource& command, BufferHandle buffer, uint64_t offset,
                                           uint64_t size, const ResourceState& after);

    [[nodiscard]] Result transition_texture(CommandListResource& command, TextureHandle texture,
                                            const SubresourceRange& subresources, const ResourceState& after);

    [[nodiscard]] Result validate_texture_subresource(const TextureResource& texture,
                                                      const SubresourceRange& subresources) const;

    [[nodiscard]] Result validate_texture_copy_region(const TextureResource& texture,
                                                      const TextureSubresourceLayers& subresources,
                                                      const Offset3D& offset, const Extent3D& extent) const;

    [[nodiscard]] uint32_t texture_subresource_index(const TextureResource& texture, Aspect aspect, uint32_t mip,
                                                     uint32_t layer) const;

    void commit_command_states(const CommandListResource& command);

    [[nodiscard]] Result validate_command_batch_states(std::span<const CommandListHandle> command_lists);

    void mark_command_resources_in_use(const CommandListResource& command, const GpuSyncPoint& completion);

    [[nodiscard]] bool command_is_unsubmitted(const CommandListResource& command) const;

    [[nodiscard]] bool buffer_has_unsubmitted_command_reference(BufferHandle buffer) const;

    [[nodiscard]] bool texture_has_unsubmitted_command_reference(TextureHandle texture) const;

    [[nodiscard]] bool buffer_view_has_unsubmitted_command_reference(BufferViewHandle view) const;

    [[nodiscard]] bool texture_view_has_unsubmitted_command_reference(TextureViewHandle view) const;

    [[nodiscard]] bool sampler_has_unsubmitted_command_reference(SamplerHandle sampler) const;

    [[nodiscard]] bool binding_set_has_unsubmitted_command_reference(BindingSetHandle set) const;

    [[nodiscard]] bool pipeline_has_unsubmitted_command_reference(PipelineHandle pipeline) const;

    void enqueue_retire(RetireRecord record);

    [[nodiscard]] bool retire_record_complete(const RetireRecord& record) const;

    void destroy_retire_record(RetireRecord& record) noexcept;

    [[nodiscard]] Result create_pipeline_layout(const PipelineLayoutDesc& desc, VkPipelineLayout& layout,
                                                std::vector<BindingLayoutHandle>& binding_layouts) const;

    [[nodiscard]] Result transition_texture(CommandListResource& command, const TextureViewResource& view,
                                            ResourceUsage usage);

    [[nodiscard]] Result validate_texture_view(const TextureViewDesc& desc, const TextureResource& texture) const;

    [[nodiscard]] std::optional<BindingLayoutEntry> find_binding_entry(const BindingLayoutResource& layout,
                                                                       BindingLocation location) const;

    [[nodiscard]] Result validate_binding_resource(const BindingLayoutEntry& entry,
                                                   const BindingResource& resource) const;

    [[nodiscard]] ResultValue<DescriptorPoolRequirements> descriptor_pool_requirements(
        const BindingLayoutResource& layout) const;

    [[nodiscard]] ResultValue<uint32_t> create_descriptor_pool_page_locked(
        BindingSetLifetime lifetime, const DescriptorPoolRequirements& requirements);

    struct DescriptorSetAllocation {
        uint32_t page_index{kInvalidIndex};
        VkDescriptorSet set{VK_NULL_HANDLE};
    };

    [[nodiscard]] ResultValue<DescriptorSetAllocation> allocate_descriptor_set(
        BindingSetLifetime lifetime, const DescriptorPoolRequirements& requirements, VkDescriptorSetLayout layout);

    void release_descriptor_set(BindingSetLifetime lifetime, uint32_t page_index, VkDescriptorSet set,
                                const DescriptorPoolRequirements& requirements) noexcept;

    void update_descriptor_pool_page_free_list_locked(uint32_t page_index) noexcept;

    void destroy_descriptor_pool_pages() noexcept;

    [[nodiscard]] bool buffer_is_referenced(BufferHandle buffer) const;

    [[nodiscard]] bool texture_is_referenced(TextureHandle texture) const;

    [[nodiscard]] bool buffer_view_is_referenced(BufferViewHandle view) const;

    [[nodiscard]] bool texture_view_is_referenced(TextureViewHandle view) const;

    [[nodiscard]] bool sampler_is_referenced(SamplerHandle sampler) const;

    [[nodiscard]] bool binding_layout_is_referenced(BindingLayoutHandle layout) const;

    void shutdown();
#if AEG_VULKAN_ENABLE_DEBUG_UTILS
    void destroy_debug_messenger();
#endif

#if AEG_VULKAN_ENABLE_DEBUG_NAMES
    template <typename VkHandle>
    void set_debug_name(VkObjectType object_type, VkHandle object, DebugName name);
#endif

    VkInstance instance_{VK_NULL_HANDLE};
#if AEG_VULKAN_ENABLE_DEBUG_UTILS
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
#if AEG_VULKAN_ENABLE_DEBUG_UTILS || AEG_VULKAN_ENABLE_DEBUG_NAMES
    bool debug_utils_enabled_{false};
#endif
#if AEG_VULKAN_ENABLE_DEBUG_NAMES
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
    std::array<std::mutex, kDescriptorLifetimeCount> descriptor_pool_lifetime_mutexes_{};
    std::shared_mutex descriptor_pool_pages_mutex_{};
    std::deque<DescriptorPoolPage> descriptor_pool_pages_{};
    std::array<std::vector<uint32_t>, kDescriptorLifetimeCount> descriptor_pool_free_lists_{};
    // The counters describe each lifetime's free-list partition. `free_count` is the exact number
    // of pages with request-independent physical capacity; fit checks remain request-specific.
    std::array<uint32_t, kDescriptorLifetimeCount> descriptor_pool_free_counts_{};
    std::array<uint32_t, kDescriptorLifetimeCount> descriptor_pool_preferred_counts_{};
    SlotMap<PipelineResource, PipelineTag> pipelines_{};
    SlotMap<CommandListResource, CommandListTag> command_lists_{};
    SlotMap<SurfaceResource, SurfaceTag> surfaces_{};
    SlotMap<SwapchainResource, SwapchainTag> swapchains_{};
    std::vector<RetireRecord> retire_records_{};
};

#if AEG_VULKAN_ENABLE_DEBUG_UTILS
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

#if AEG_VULKAN_ENABLE_DEBUG_NAMES
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
#if !AEG_VULKAN_ENABLE_VALIDATION
    if (desc.enable_validation) {
        return Result::failure(ResultCode::kUnsupported, "Vulkan validation support was compiled out");
    }
#endif
#if !AEG_VULKAN_ENABLE_DEBUG_UTILS
    if (desc.enable_debug) {
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

#if AEG_VULKAN_ENABLE_VALIDATION
    const auto available_layers = enumerate_instance_layers();
#endif
    const auto available_extensions = enumerate_instance_extensions();

    std::vector<const char*> layers;
#if AEG_VULKAN_ENABLE_VALIDATION
    if (desc.enable_validation) {
        if (!contains_name(available_layers, "VK_LAYER_KHRONOS_validation")) {
            return Result::failure(ResultCode::kUnsupported, "VK_LAYER_KHRONOS_validation is not available");
        }
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }
#endif

#if AEG_VULKAN_ENABLE_DEBUG_UTILS
    const bool wants_debug_utils = desc.enable_validation || desc.enable_debug;
    if (wants_debug_utils) {
        if (!contains_name(available_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            if (desc.enable_debug) {
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
#if AEG_VULKAN_ENABLE_DEBUG_UTILS
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

#if AEG_VULKAN_ENABLE_DEBUG_UTILS
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
#if AEG_VULKAN_ENABLE_DEBUG_UTILS
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

#if AEG_VULKAN_ENABLE_DEBUG_UTILS
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
    features.depth_clamp = supported2.features.depthClamp == VK_TRUE;
    features.fill_mode_non_solid = supported2.features.fillModeNonSolid == VK_TRUE;
    features.wide_lines = supported2.features.wideLines == VK_TRUE;
    features.dual_src_blend = supported2.features.dualSrcBlend == VK_TRUE;
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

    const uint32_t preferred_index = desc.preferred_adapter_index;
    if (preferred_index != std::numeric_limits<uint32_t>::max() && preferred_index >= devices.size()) {
        return Result::failure(ResultCode::kInvalidArgument, "preferred_adapter_index is out of range");
    }

    std::vector<PhysicalDeviceCandidate> candidates;
    candidates.reserve(devices.size());
    for (uint32_t index = 0; index < devices.size(); ++index) {
        candidates.push_back(evaluate_physical_device(devices[index], index));
    }

#if AEG_VULKAN_ENABLE_VALIDATION || AEG_VULKAN_ENABLE_DEBUG_UTILS
    if (desc.enable_validation || desc.enable_debug) {
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
            return Result::failure(ResultCode::kUnsupported, "preferred_adapter_index " +
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

    capabilities_.vendor_id = physical_properties_.vendorID;
    capabilities_.device_id = physical_properties_.deviceID;
    capabilities_.adapter_name = physical_properties_.deviceName;
    capabilities_.adapter_type = to_rhi_adapter_type(physical_properties_.deviceType);
    capabilities_.local_memory_bytes = selected->device_local_memory_bytes;
    capabilities_.cpu_visible_memory_bytes = selected->host_visible_memory_bytes;
    capabilities_.dedicated_compute_queue = selected->queues.dedicated_compute;
    capabilities_.dedicated_copy_queue = selected->queues.dedicated_copy;
    capabilities_.supported_features = DeviceFeature::kNone;
    if (selected->features.timeline_semaphore) {
        capabilities_.supported_features |= DeviceFeature::kTimelineSynchronization;
    }
    if (selected->features.descriptor_indexing) {
        capabilities_.supported_features |= DeviceFeature::kBindlessResources;
    }
    if (selected->features.buffer_device_address) {
        capabilities_.supported_features |= DeviceFeature::kGpuVirtualAddress;
    }
    capabilities_.supports_swapchain = selected->features.swapchain;
    capabilities_.max_uniform_buffer_bytes = physical_properties_.limits.maxUniformBufferRange;
    capabilities_.max_inline_constant_bytes = physical_properties_.limits.maxPushConstantsSize;

#if AEG_VULKAN_ENABLE_VALIDATION || AEG_VULKAN_ENABLE_DEBUG_UTILS
    if (desc.enable_validation || desc.enable_debug) {
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
    enabled2.features.depthClamp = supported_features.depth_clamp ? VK_TRUE : VK_FALSE;
    enabled2.features.fillModeNonSolid = supported_features.fill_mode_non_solid ? VK_TRUE : VK_FALSE;
    enabled2.features.wideLines = supported_features.wide_lines ? VK_TRUE : VK_FALSE;
    enabled2.features.dualSrcBlend = supported_features.dual_src_blend ? VK_TRUE : VK_FALSE;

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

#if AEG_VULKAN_ENABLE_DEBUG_NAMES
    if (debug_utils_enabled_) {
        set_debug_utils_object_name_ = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
            vkGetDeviceProcAddr(device_, "vkSetDebugUtilsObjectNameEXT"));
    }
#endif

    enabled_features_ = supported_features;

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

Result VulkanDevice::submit_ownership_releases(std::span<const CommandListHandle> command_lists,
                                               QueueClass destination_queue, std::vector<VkSemaphore>& wait_semaphores,
                                               std::vector<VkPipelineStageFlags>& wait_stages,
                                               std::vector<uint64_t>& wait_values) {
    QueueState* destination_state = queue_state(destination_queue);
    if (destination_state == nullptr || destination_state->queue == VK_NULL_HANDLE ||
        destination_state->timeline == VK_NULL_HANDLE) {
        return Result::failure(ResultCode::kInvalidState, "Destination queue is unavailable for ownership transfer");
    }

    struct ReleaseBatch {
        QueueState* source_queue{nullptr};
        std::vector<const BufferOwnershipTransfer*> buffers{};
        std::vector<const TextureOwnershipTransfer*> textures{};
        VkPipelineStageFlags destination_stages{0};
    };

    std::array<ReleaseBatch, kQueueCount> batches{};

    const auto has_unsubmitted_buffer_use = [&](BufferHandle buffer, QueueClass queue) {
        bool found = false;
        command_lists_.for_each([&](const CommandListResource& command) {
            if (!found && command.queue == queue && command_is_unsubmitted(command) &&
                std::find(command.referenced_buffers.begin(), command.referenced_buffers.end(), buffer) !=
                    command.referenced_buffers.end()) {
                found = true;
            }
        });
        return found;
    };
    const auto has_unsubmitted_texture_use = [&](TextureHandle texture, QueueClass queue) {
        bool found = false;
        command_lists_.for_each([&](const CommandListResource& command) {
            if (!found && command.queue == queue && command_is_unsubmitted(command) &&
                std::find(command.referenced_textures.begin(), command.referenced_textures.end(), texture) !=
                    command.referenced_textures.end()) {
                found = true;
            }
        });
        return found;
    };

    const auto append_wait = [&](QueueState& source_queue, uint64_t signal_value,
                                 VkPipelineStageFlags destination_stages) {
        const auto iterator = std::find(wait_semaphores.begin(), wait_semaphores.end(), source_queue.timeline);
        const VkPipelineStageFlags stages = destination_stages == 0
                                                ? static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)
                                                : destination_stages;
        if (iterator == wait_semaphores.end()) {
            wait_semaphores.push_back(source_queue.timeline);
            wait_stages.push_back(stages);
            wait_values.push_back(signal_value);
            return;
        }

        const size_t index = static_cast<size_t>(iterator - wait_semaphores.begin());
        wait_stages[index] |= stages;
        wait_values[index] = std::max(wait_values[index], signal_value);
    };

    const auto append_buffer_transfer = [&](ReleaseBatch& batch, const BufferOwnershipTransfer& transfer) {
        const bool duplicate =
            std::any_of(batch.buffers.begin(), batch.buffers.end(), [&](const BufferOwnershipTransfer* existing) {
                return existing->buffer == transfer.buffer && existing->offset == transfer.offset &&
                       existing->size == transfer.size && existing->source_family == transfer.source_family &&
                       existing->destination_family == transfer.destination_family &&
                       existing->before == transfer.before && existing->after == transfer.after &&
                       existing->discard_contents == transfer.discard_contents;
            });
        if (!duplicate) {
            batch.buffers.push_back(&transfer);
        }
    };
    const auto append_texture_transfer = [&](ReleaseBatch& batch, const TextureOwnershipTransfer& transfer) {
        const bool duplicate =
            std::any_of(batch.textures.begin(), batch.textures.end(), [&](const TextureOwnershipTransfer* existing) {
                return existing->texture == transfer.texture &&
                       existing->range.aspectMask == transfer.range.aspectMask &&
                       existing->range.baseMipLevel == transfer.range.baseMipLevel &&
                       existing->range.levelCount == transfer.range.levelCount &&
                       existing->range.baseArrayLayer == transfer.range.baseArrayLayer &&
                       existing->range.layerCount == transfer.range.layerCount &&
                       existing->source_family == transfer.source_family &&
                       existing->destination_family == transfer.destination_family &&
                       existing->before == transfer.before && existing->after == transfer.after &&
                       existing->discard_contents == transfer.discard_contents;
            });
        if (!duplicate) {
            batch.textures.push_back(&transfer);
        }
    };

    for (const CommandListHandle command_list : command_lists) {
        const CommandListResource* command = command_lists_.get(command_list);
        if (command == nullptr) {
            return Result::failure(ResultCode::kNotFound, "Submit contains an invalid command list");
        }

        for (const BufferOwnershipTransfer& transfer : command->buffer_ownership_transfers) {
            if (transfer.phase != QueueOwnershipPhase::kAcquire) {
                continue;
            }
            if (transfer.after.queue != destination_queue || transfer.before.queue == QueueClass::kAuto ||
                transfer.before.queue == QueueClass::kPresent || transfer.after.queue == QueueClass::kPresent) {
                return Result::failure(ResultCode::kInvalidArgument,
                                       "Buffer ownership transfer does not target the submitted queue");
            }
            QueueState* source_queue = queue_state(transfer.before.queue);
            if (source_queue == nullptr || source_queue->queue == VK_NULL_HANDLE ||
                source_queue->timeline == VK_NULL_HANDLE || source_queue == destination_state ||
                source_queue->family_index != transfer.source_family ||
                destination_state->family_index != transfer.destination_family) {
                return Result::failure(ResultCode::kInvalidState,
                                       "Buffer ownership transfer queue families are no longer valid");
            }
            if (has_unsubmitted_buffer_use(transfer.buffer, transfer.before.queue)) {
                return Result::failure(
                    ResultCode::kInvalidState,
                    "Buffer ownership transfer requires the source queue work to be submitted first");
            }

            ReleaseBatch& batch = batches[queue_value_index(source_queue->logical_class)];
            batch.source_queue = source_queue;
            append_buffer_transfer(batch, transfer);
            batch.destination_stages |= to_vk_resource_state(transfer.after, false).stages;
        }

        for (const TextureOwnershipTransfer& transfer : command->texture_ownership_transfers) {
            if (transfer.phase != QueueOwnershipPhase::kAcquire) {
                continue;
            }
            if (transfer.after.queue != destination_queue || transfer.before.queue == QueueClass::kAuto ||
                transfer.before.queue == QueueClass::kPresent || transfer.after.queue == QueueClass::kPresent) {
                return Result::failure(ResultCode::kInvalidArgument,
                                       "Texture ownership transfer does not target the submitted queue");
            }
            QueueState* source_queue = queue_state(transfer.before.queue);
            if (source_queue == nullptr || source_queue->queue == VK_NULL_HANDLE ||
                source_queue->timeline == VK_NULL_HANDLE || source_queue == destination_state ||
                source_queue->family_index != transfer.source_family ||
                destination_state->family_index != transfer.destination_family) {
                return Result::failure(ResultCode::kInvalidState,
                                       "Texture ownership transfer queue families are no longer valid");
            }
            if (has_unsubmitted_texture_use(transfer.texture, transfer.before.queue)) {
                return Result::failure(
                    ResultCode::kInvalidState,
                    "Texture ownership transfer requires the source queue work to be submitted first");
            }

            ReleaseBatch& batch = batches[queue_value_index(source_queue->logical_class)];
            batch.source_queue = source_queue;
            append_texture_transfer(batch, transfer);
            // Present is an external operation rather than a pipeline stage that can
            // safely serve as the semaphore wait point.  The graphics queue must wait
            // before it executes the acquire/layout transition for the image.
            batch.destination_stages |= transfer.after.usage == ResourceUsage::kPresent
                                            ? static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)
                                            : to_vk_resource_state(transfer.after, true).stages;
        }
    }

    for (ReleaseBatch& batch : batches) {
        if (batch.source_queue == nullptr || (batch.buffers.empty() && batch.textures.empty())) {
            continue;
        }

        QueueState& source_queue = *batch.source_queue;
        std::vector<VkBufferMemoryBarrier2> buffer_barriers2;
        std::vector<VkImageMemoryBarrier2> image_barriers2;
        std::vector<VkBufferMemoryBarrier> buffer_barriers;
        std::vector<VkImageMemoryBarrier> image_barriers;
        buffer_barriers2.reserve(batch.buffers.size());
        image_barriers2.reserve(batch.textures.size());
        buffer_barriers.reserve(batch.buffers.size());
        image_barriers.reserve(batch.textures.size());

        VkPipelineStageFlags legacy_source_stages = 0;
        VkPipelineStageFlags legacy_destination_stages = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        for (const BufferOwnershipTransfer* transfer : batch.buffers) {
            const BufferResource* resource = buffers_.get(transfer->buffer);
            if (resource == nullptr) {
                return Result::failure(ResultCode::kNotFound, "Ownership transfer references a destroyed buffer");
            }
            const VulkanResourceState source = transfer->discard_contents
                                                   ? to_vk_discard_source_state(transfer->before, false)
                                                   : to_vk_resource_state(transfer->before, false);
            VkBufferMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            barrier.srcStageMask = source.stages2;
            barrier.srcAccessMask = source.access2;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
            barrier.dstAccessMask = 0;
            barrier.srcQueueFamilyIndex = transfer->source_family;
            barrier.dstQueueFamilyIndex = transfer->destination_family;
            barrier.buffer = resource->buffer;
            barrier.offset = transfer->offset;
            barrier.size = transfer->size;
            buffer_barriers2.push_back(barrier);

            VkBufferMemoryBarrier legacy{};
            legacy.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            legacy.srcAccessMask = source.access;
            legacy.dstAccessMask = 0;
            legacy.srcQueueFamilyIndex = transfer->source_family;
            legacy.dstQueueFamilyIndex = transfer->destination_family;
            legacy.buffer = resource->buffer;
            legacy.offset = transfer->offset;
            legacy.size = transfer->size;
            buffer_barriers.push_back(legacy);
            legacy_source_stages |= source.stages;
        }

        for (const TextureOwnershipTransfer* transfer : batch.textures) {
            const TextureResource* resource = textures_.get(transfer->texture);
            if (resource == nullptr) {
                return Result::failure(ResultCode::kNotFound, "Ownership transfer references a destroyed texture");
            }
            const VulkanResourceState source = transfer->discard_contents
                                                   ? to_vk_discard_source_state(transfer->before, true)
                                                   : to_vk_resource_state(transfer->before, true);
            const VulkanResourceState destination = to_vk_resource_state(transfer->after, true);
            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask = source.stages2;
            barrier.srcAccessMask = source.access2;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
            barrier.dstAccessMask = 0;
            barrier.oldLayout = source.layout;
            barrier.newLayout = destination.layout;
            barrier.srcQueueFamilyIndex = transfer->source_family;
            barrier.dstQueueFamilyIndex = transfer->destination_family;
            barrier.image = resource->image;
            barrier.subresourceRange = transfer->range;
            image_barriers2.push_back(barrier);

            VkImageMemoryBarrier legacy{};
            legacy.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            legacy.srcAccessMask = source.access;
            legacy.dstAccessMask = 0;
            legacy.oldLayout = source.layout;
            legacy.newLayout = destination.layout;
            legacy.srcQueueFamilyIndex = transfer->source_family;
            legacy.dstQueueFamilyIndex = transfer->destination_family;
            legacy.image = resource->image;
            legacy.subresourceRange = transfer->range;
            image_barriers.push_back(legacy);
            legacy_source_stages |= source.stages;
        }

        VkCommandBufferAllocateInfo allocate_info{};
        allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate_info.commandPool = source_queue.command_pool;
        allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate_info.commandBufferCount = 1;

        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        VkResult result = vkAllocateCommandBuffers(device_, &allocate_info, &command_buffer);
        if (result != VK_SUCCESS) {
            return make_vulkan_result(result, "vkAllocateCommandBuffers(ownershipRelease)");
        }

        const auto free_command_buffer = [&] {
            if (command_buffer != VK_NULL_HANDLE) {
                vkFreeCommandBuffers(device_, source_queue.command_pool, 1, &command_buffer);
                command_buffer = VK_NULL_HANDLE;
            }
        };

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(command_buffer, &begin_info);
        if (result != VK_SUCCESS) {
            free_command_buffer();
            return make_vulkan_result(result, "vkBeginCommandBuffer(ownershipRelease)");
        }

        if (enabled_features_.synchronization2) {
            VkDependencyInfo dependency{};
            dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dependency.bufferMemoryBarrierCount = static_cast<uint32_t>(buffer_barriers2.size());
            dependency.pBufferMemoryBarriers = buffer_barriers2.data();
            dependency.imageMemoryBarrierCount = static_cast<uint32_t>(image_barriers2.size());
            dependency.pImageMemoryBarriers = image_barriers2.data();
            vkCmdPipelineBarrier2(command_buffer, &dependency);
        } else {
            if (legacy_source_stages == 0) {
                legacy_source_stages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            }
            vkCmdPipelineBarrier(command_buffer, legacy_source_stages, legacy_destination_stages, 0, 0, nullptr,
                                 static_cast<uint32_t>(buffer_barriers.size()), buffer_barriers.data(),
                                 static_cast<uint32_t>(image_barriers.size()), image_barriers.data());
        }

        result = vkEndCommandBuffer(command_buffer);
        if (result != VK_SUCCESS) {
            free_command_buffer();
            return make_vulkan_result(result, "vkEndCommandBuffer(ownershipRelease)");
        }

        const uint64_t signal_value = source_queue.next_signal_value;
        const std::array<VkSemaphore, 1> signal_semaphores{source_queue.timeline};
        const std::array<uint64_t, 1> signal_values{signal_value};
        VkTimelineSemaphoreSubmitInfo timeline_info{};
        timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timeline_info.signalSemaphoreValueCount = 1;
        timeline_info.pSignalSemaphoreValues = signal_values.data();

        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.pNext = &timeline_info;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command_buffer;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = signal_semaphores.data();
        result = vkQueueSubmit(source_queue.queue, 1, &submit_info, VK_NULL_HANDLE);
        if (result != VK_SUCCESS) {
            free_command_buffer();
            return make_vulkan_result(result, "vkQueueSubmit(ownershipRelease)");
        }

        ++source_queue.next_signal_value;
        const GpuSyncPoint completion{source_queue.timeline_handle, signal_value};
        const uint32_t source_queue_index = queue_value_index(source_queue.logical_class);
        for (const BufferOwnershipTransfer* transfer : batch.buffers) {
            if (BufferResource* resource = buffers_.get(transfer->buffer); resource != nullptr) {
                resource->last_use[source_queue_index] = std::max(resource->last_use[source_queue_index], signal_value);
            }
        }
        for (const TextureOwnershipTransfer* transfer : batch.textures) {
            if (TextureResource* resource = textures_.get(transfer->texture); resource != nullptr) {
                resource->last_use[source_queue_index] = std::max(resource->last_use[source_queue_index], signal_value);
            }
        }

        RetireCommandBuffer retire{};
        retire.required_values[source_queue_index] = signal_value;
        retire.pool = source_queue.command_pool;
        retire.command_buffer = command_buffer;
        enqueue_retire(std::move(retire));
        command_buffer = VK_NULL_HANDLE;
        append_wait(source_queue, completion.value, batch.destination_stages);
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

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    result = vkCreateFence(device_, &fence_info, nullptr, &frame.in_flight);
    if (result != VK_SUCCESS) {
        vkDestroySemaphore(device_, frame.image_available, nullptr);
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
        if (frame.in_flight != VK_NULL_HANDLE) {
            vkDestroyFence(device_, frame.in_flight, nullptr);
        }
    }
    frame.image_available = VK_NULL_HANDLE;
    frame.in_flight = VK_NULL_HANDLE;
    frame.image_index = kRemaining;
    frame.acquired = false;
    frame.submitted = false;
    frame.acquire_waited = false;
    frame.present_signaled = false;
    frame.completion = {};
}

void VulkanDevice::destroy_swapchain_native(SwapchainResource& resource) noexcept {
    for (FrameSlot& frame : resource.frames) {
        destroy_frame_slot_sync(frame);
    }
    resource.frames.clear();

    if (device_ != VK_NULL_HANDLE) {
        for (VkSemaphore& semaphore : resource.render_finished) {
            if (semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_, semaphore, nullptr);
            }
            semaphore = VK_NULL_HANDLE;
        }
    }
    resource.render_finished.clear();

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
    if (!enabled_features_.swapchain) {
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
    swapchain_info.clipped = VK_TRUE;
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

#if AEG_VULKAN_ENABLE_DEBUG_NAMES
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

    VkSemaphoreCreateInfo semaphore_info{};
    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    resource.render_finished.resize(vk_images.size(), VK_NULL_HANDLE);
    for (VkSemaphore& semaphore : resource.render_finished) {
        result = vkCreateSemaphore(device_, &semaphore_info, nullptr, &semaphore);
        if (result != VK_SUCCESS) {
            const Result error = make_vulkan_result(result, "vkCreateSemaphore(renderFinished)");
            destroy_swapchain_native(resource);
            return error;
        }
    }

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
        texture_resource.state = {};
        texture_resource.subresource_states.assign(kAspectPlaneCount, texture_resource.state);
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
        case SurfaceType::kWindow:
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
    frame.acquire_waited = false;
    frame.present_signaled = false;
    frame.completion = {};
    if (suboptimal) {
        resource->needs_recreate = true;
    }

    FrameAcquireResult acquire_result{};
    acquire_result.frame = FrameToken{swapchain, frame_index, image_index, resource->generation};
    acquire_result.image = resource->images[image_index];
    const TextureResource* image_texture = textures_.get(acquire_result.image.texture);
    if (image_texture == nullptr || image_texture->subresource_states.empty()) {
        frame.image_index = kRemaining;
        frame.acquired = false;
        resource->image_in_flight_frame[image_index] = kRemaining;
        (void)consume_acquire_semaphore(frame);
        return {Result::failure(ResultCode::kInvalidState,
                                "Swapchain image resource state is unavailable after acquisition"),
                {}};
    }
    acquire_result.initial_state = image_texture->subresource_states.front();
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
    if (frame.present_signaled) {
        return {Result::failure(ResultCode::kInvalidState, "No frame submission is allowed after the present signal"),
                {}};
    }
    if (desc.wait_for_acquire == frame.acquire_waited) {
        return {Result::failure(ResultCode::kInvalidState, desc.wait_for_acquire
                                                               ? "Acquire semaphore has already been consumed"
                                                               : "First frame submit must wait for acquire"),
                {}};
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
    for (size_t index = 0; index < desc.command_lists.size(); ++index) {
        const CommandListHandle command_list = desc.command_lists[index];
        if (std::find(desc.command_lists.begin(), desc.command_lists.begin() + index, command_list) !=
            desc.command_lists.begin() + index) {
            return {Result::failure(ResultCode::kInvalidArgument, "Frame submit contains a duplicate command list"),
                    {}};
        }
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

    const Result state_result = validate_command_batch_states(desc.command_lists);
    if (!state_result) {
        return {state_result, {}};
    }

    std::vector<VkSemaphore> wait_semaphores;
    std::vector<VkPipelineStageFlags> wait_stages;
    std::vector<uint64_t> wait_values;
    wait_semaphores.reserve(desc.waits.size() + kQueueCount + (desc.wait_for_acquire ? 1u : 0u));
    wait_stages.reserve(desc.waits.size() + kQueueCount + (desc.wait_for_acquire ? 1u : 0u));
    wait_values.reserve(desc.waits.size() + kQueueCount + (desc.wait_for_acquire ? 1u : 0u));
    if (desc.wait_for_acquire) {
        wait_semaphores.push_back(frame.image_available);
        // The first command may transition the acquired image before rendering begins.
        // Wait at all commands so the acquire semaphore also protects that transition.
        wait_stages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        wait_values.push_back(0);
    }

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

    if (desc.frame.image_index >= resource->render_finished.size() ||
        resource->render_finished[desc.frame.image_index] == VK_NULL_HANDLE) {
        return {Result::failure(ResultCode::kInvalidState, "Swapchain render-finished semaphore is unavailable"), {}};
    }

    const Result ownership_result =
        submit_ownership_releases(desc.command_lists, queue->logical_class, wait_semaphores, wait_stages, wait_values);
    if (!ownership_result) {
        return {ownership_result, {}};
    }

    VkFence submit_fence = VK_NULL_HANDLE;
    if (desc.signal_present) {
        const VkResult reset_result = vkResetFences(device_, 1, &frame.in_flight);
        if (reset_result != VK_SUCCESS) {
            return {make_vulkan_result(reset_result, "vkResetFences(submitFrame)"), {}};
        }
        submit_fence = frame.in_flight;
    }

    const uint64_t signal_value = queue->next_signal_value;
    std::vector<VkSemaphore> signal_semaphores;
    std::vector<uint64_t> signal_values;
    signal_semaphores.reserve(desc.signal_present ? 2u : 1u);
    signal_values.reserve(desc.signal_present ? 2u : 1u);
    if (desc.signal_present) {
        signal_semaphores.push_back(resource->render_finished[desc.frame.image_index]);
        signal_values.push_back(0);
    }
    signal_semaphores.push_back(queue->timeline);
    signal_values.push_back(signal_value);

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

    const VkResult result = vkQueueSubmit(queue->queue, 1, &submit_info, submit_fence);
    if (result != VK_SUCCESS) {
        return {make_vulkan_result(result, "vkQueueSubmit(submitFrame)"), {}};
    }

    ++queue->next_signal_value;
    const GpuSyncPoint completion{queue->timeline_handle, signal_value};
    frame.submitted = true;
    frame.acquire_waited = frame.acquire_waited || desc.wait_for_acquire;
    frame.present_signaled = frame.present_signaled || desc.signal_present;
    frame.completion = completion;
    for (const CommandListHandle command_list : desc.command_lists) {
        CommandListResource* command = command_lists_.get(command_list);
        commit_command_states(*command);
        mark_command_resources_in_use(*command, completion);
        command->last_submitted = completion;
    }
    (void)collect_garbage();
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

    if (!frame.present_signaled) {
        return {
            Result::failure(ResultCode::kInvalidState, "Frame must signal the present semaphore before presentation"),
            {}};
    }

    SurfaceResource* surface = surfaces_.get(resource->surface);
    if (surface == nullptr || surface->surface == VK_NULL_HANDLE || surface->present_queue == VK_NULL_HANDLE) {
        return {Result::failure(ResultCode::kInvalidState, "Swapchain presentation queue is unavailable"), {}};
    }

    if (desc.frame.image_index >= resource->render_finished.size() ||
        resource->render_finished[desc.frame.image_index] == VK_NULL_HANDLE) {
        return {Result::failure(ResultCode::kInvalidState, "Swapchain render-finished semaphore is unavailable"), {}};
    }

    const std::array<VkSemaphore, 1> wait_semaphores{resource->render_finished[desc.frame.image_index]};
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
    frame.acquire_waited = false;
    frame.present_signaled = false;
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
    frame.acquire_waited = false;
    frame.present_signaled = false;
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

Result VulkanDevice::create_pipeline_layout(const PipelineLayoutDesc& desc, VkPipelineLayout& layout,
                                            std::vector<BindingLayoutHandle>& binding_layouts) const {
    layout = VK_NULL_HANDLE;
    binding_layouts.clear();

    if (desc.binding_layouts.size() > std::numeric_limits<uint32_t>::max() ||
        desc.inline_constant_ranges.size() > std::numeric_limits<uint32_t>::max()) {
        return Result::failure(ResultCode::kInvalidArgument, "Pipeline layout contains too many entries");
    }
    if (desc.binding_layouts.size() > physical_properties_.limits.maxBoundDescriptorSets) {
        return Result::failure(ResultCode::kInvalidArgument, "Pipeline layout exceeds the device descriptor-set limit");
    }

    std::vector<std::pair<uint16_t, VkDescriptorSetLayout>> ordered_layouts;
    ordered_layouts.reserve(desc.binding_layouts.size());
    for (const BindingLayoutHandle binding_layout : desc.binding_layouts) {
        const BindingLayoutResource* resource = binding_layouts_.get(binding_layout);
        if (resource == nullptr) {
            return Result::failure(ResultCode::kNotFound, "Pipeline layout contains an invalid binding layout");
        }
        if (std::any_of(ordered_layouts.begin(), ordered_layouts.end(),
                        [resource](const auto& existing) { return existing.first == resource->group; })) {
            return Result::failure(ResultCode::kInvalidArgument,
                                   "Pipeline layout contains duplicate binding layout groups");
        }
        ordered_layouts.emplace_back(resource->group, resource->layout);
    }

    std::sort(ordered_layouts.begin(), ordered_layouts.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });

    std::vector<VkDescriptorSetLayout> vk_layouts;
    vk_layouts.reserve(ordered_layouts.size());
    binding_layouts.reserve(ordered_layouts.size());
    for (size_t index = 0; index < ordered_layouts.size(); ++index) {
        if (ordered_layouts[index].first != index) {
            return Result::failure(ResultCode::kInvalidArgument,
                                   "Pipeline binding layout groups must be contiguous and start at zero");
        }
        vk_layouts.push_back(ordered_layouts[index].second);
        const auto binding_layout =
            std::find_if(desc.binding_layouts.begin(), desc.binding_layouts.end(), [&](BindingLayoutHandle handle) {
                const BindingLayoutResource* resource = binding_layouts_.get(handle);
                return resource != nullptr && resource->group == ordered_layouts[index].first;
            });
        if (binding_layout == desc.binding_layouts.end()) {
            return Result::failure(ResultCode::kInvalidState, "Pipeline binding layout lookup failed");
        }
        binding_layouts.push_back(*binding_layout);
    }

    std::vector<VkPushConstantRange> push_constant_ranges;
    push_constant_ranges.reserve(desc.inline_constant_ranges.size());
    for (const InlineConstantRange& range : desc.inline_constant_ranges) {
        if (range.size == 0 || (range.offset % 4u) != 0 || (range.size % 4u) != 0) {
            return Result::failure(ResultCode::kInvalidArgument,
                                   "Push constant offset and size must be non-zero and four-byte aligned");
        }
        if (range.size > std::numeric_limits<uint32_t>::max() - range.offset ||
            range.offset + range.size > capabilities_.max_inline_constant_bytes) {
            return Result::failure(ResultCode::kInvalidArgument, "Push constant range exceeds device limits");
        }

        const VkShaderStageFlags shader_stages = to_vk_shader_stages(range.shader_stages);
        if (shader_stages == 0) {
            return Result::failure(ResultCode::kInvalidArgument, "Push constant range has no shader stages");
        }
        push_constant_ranges.push_back(VkPushConstantRange{shader_stages, range.offset, range.size});
    }

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = static_cast<uint32_t>(vk_layouts.size());
    layout_info.pSetLayouts = vk_layouts.data();
    layout_info.pushConstantRangeCount = static_cast<uint32_t>(push_constant_ranges.size());
    layout_info.pPushConstantRanges = push_constant_ranges.data();

    const VkResult result = vkCreatePipelineLayout(device_, &layout_info, nullptr, &layout);
    if (result != VK_SUCCESS) {
        return make_vulkan_result(result, "vkCreatePipelineLayout");
    }
    return Result::success();
}

Result VulkanDevice::validate_command_recording(const CommandListResource& command) const {
    if (!command.recording) {
        return Result::failure(ResultCode::kInvalidState, "Command list is not recording");
    }
    return Result::success();
}

Result VulkanDevice::ensure_command_buffer_state(CommandListResource& command, BufferHandle buffer) {
    if (find_command_buffer_state(command, buffer) != nullptr) {
        return Result::success();
    }
    BufferResource* resource = buffers_.get(buffer);
    if (resource == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid buffer handle");
    }
    try {
        CommandBufferState state{};
        state.buffer = buffer;
        state.segments = resource->state_segments;
        initialize_buffer_state_segments(state.segments, resource->size, resource->state);
        command.buffer_states.push_back(std::move(state));
    } catch (const std::bad_alloc&) {
        return Result::failure(ResultCode::kOutOfMemory, "Buffer state tracking allocation failed");
    }
    return Result::success();
}

CommandBufferState* VulkanDevice::find_command_buffer_state(CommandListResource& command,
                                                            BufferHandle buffer) noexcept {
    const auto iterator = std::find_if(command.buffer_states.begin(), command.buffer_states.end(),
                                       [buffer](const CommandBufferState& state) { return state.buffer == buffer; });
    return iterator == command.buffer_states.end() ? nullptr : &*iterator;
}

Result VulkanDevice::validate_buffer_usage(CommandListResource& command, BufferHandle buffer, uint64_t offset,
                                           uint64_t size, const ResourceState& expected) {
    const Result state_result = ensure_command_buffer_state(command, buffer);
    if (!state_result) {
        return state_result;
    }
    BufferResource* resource = buffers_.get(buffer);
    CommandBufferState* local = find_command_buffer_state(command, buffer);
    if (resource == nullptr || local == nullptr) {
        return Result::failure(ResultCode::kInvalidState, "Buffer state tracking is unavailable");
    }
    uint64_t end = 0;
    if (!resolve_buffer_range(resource->size, offset, size, end)) {
        return Result::failure(ResultCode::kInvalidArgument, "Buffer range exceeds buffer size");
    }
    for (const BufferStateSegment& segment : local->segments) {
        const uint64_t segment_end = segment.offset + segment.size;
        if (segment_end <= offset || segment.offset >= end) {
            continue;
        }
        if (!resource_usage_matches(segment.state, expected)) {
            return Result::failure(ResultCode::kInvalidState,
                                   "Explicit command list buffer state does not match the declared usage");
        }
    }
    return Result::success();
}

ResourceState VulkanDevice::current_texture_state(const CommandListResource& command, TextureHandle texture,
                                                  uint32_t subresource_index) const {
    for (auto iterator = command.texture_state_changes.rbegin(); iterator != command.texture_state_changes.rend();
         ++iterator) {
        if (iterator->texture == texture && iterator->subresource_index == subresource_index) {
            return iterator->after;
        }
    }
    const TextureResource* resource = textures_.get(texture);
    if (resource == nullptr || subresource_index >= resource->subresource_states.size()) {
        return ResourceState{};
    }
    return resource->subresource_states[subresource_index];
}

Result VulkanDevice::validate_texture_subresource(const TextureResource& texture,
                                                  const SubresourceRange& subresources) const {
    if (subresources.aspects == static_cast<AspectMask>(0)) {
        return Result::failure(ResultCode::kInvalidArgument, "Texture subresource aspects must not be empty");
    }
    const AspectMask valid_aspects = default_aspect(texture.desc.format);
    if (has_flag(subresources.aspects, Aspect::kColor) && !has_flag(valid_aspects, Aspect::kColor)) {
        return Result::failure(ResultCode::kInvalidArgument, "Texture range contains an invalid color aspect");
    }
    if (has_flag(subresources.aspects, Aspect::kDepth) && !has_flag(valid_aspects, Aspect::kDepth)) {
        return Result::failure(ResultCode::kInvalidArgument, "Texture range contains an invalid depth aspect");
    }
    if (has_flag(subresources.aspects, Aspect::kStencil) && !has_flag(valid_aspects, Aspect::kStencil)) {
        return Result::failure(ResultCode::kInvalidArgument, "Texture range contains an invalid stencil aspect");
    }

    const uint32_t mip_count =
        subresources.mip_count == kRemaining ? texture.desc.mip_levels - subresources.base_mip : subresources.mip_count;
    const uint32_t layer_count = subresources.layer_count == kRemaining
                                     ? texture.desc.array_layers - subresources.base_layer
                                     : subresources.layer_count;
    if (subresources.base_mip >= texture.desc.mip_levels || mip_count == 0 ||
        mip_count > texture.desc.mip_levels - subresources.base_mip) {
        return Result::failure(ResultCode::kInvalidArgument, "Texture range mip selection is invalid");
    }
    if (subresources.base_layer >= texture.desc.array_layers || layer_count == 0 ||
        layer_count > texture.desc.array_layers - subresources.base_layer) {
        return Result::failure(ResultCode::kInvalidArgument, "Texture range layer selection is invalid");
    }
    return Result::success();
}

Result VulkanDevice::validate_texture_copy_region(const TextureResource& texture,
                                                  const TextureSubresourceLayers& subresources, const Offset3D& offset,
                                                  const Extent3D& extent) const {
    const uint32_t aspect_bits = static_cast<uint32_t>(subresources.aspects);
    if (aspect_bits != static_cast<uint32_t>(Aspect::kColor) && aspect_bits != static_cast<uint32_t>(Aspect::kDepth) &&
        aspect_bits != static_cast<uint32_t>(Aspect::kStencil)) {
        return Result::failure(ResultCode::kInvalidArgument, "Texture copy must select exactly one aspect");
    }
    if (subresources.layer_count == 0) {
        return Result::failure(ResultCode::kInvalidArgument, "Texture copy layer count must be non-zero");
    }

    const SubresourceRange range{subresources.aspects, subresources.mip_level, 1, subresources.base_array_layer,
                                 subresources.layer_count};
    const Result range_result = validate_texture_subresource(texture, range);
    if (!range_result) {
        return range_result;
    }
    if (extent.width == 0 || extent.height == 0 || extent.depth == 0 || offset.x < 0 || offset.y < 0 || offset.z < 0) {
        return Result::failure(ResultCode::kInvalidArgument, "Texture copy offset or extent is invalid");
    }

    if (texture.desc.dimension != TextureDimension::kTex3D && (offset.z != 0 || extent.depth != 1)) {
        return Result::failure(ResultCode::kInvalidArgument,
                               "Array and 2D texture copies require a single depth slice");
    }
    if (texture.desc.dimension == TextureDimension::kTex3D &&
        (subresources.base_array_layer != 0 || subresources.layer_count != 1)) {
        return Result::failure(ResultCode::kInvalidArgument, "3D texture copies must use one array layer");
    }

    const Extent3D available{std::max(1u, texture.desc.extent.width >> subresources.mip_level),
                             std::max(1u, texture.desc.extent.height >> subresources.mip_level),
                             std::max(1u, texture.desc.extent.depth >> subresources.mip_level)};
    if (static_cast<uint64_t>(offset.x) + extent.width > available.width ||
        static_cast<uint64_t>(offset.y) + extent.height > available.height ||
        static_cast<uint64_t>(offset.z) + extent.depth > available.depth) {
        return Result::failure(ResultCode::kInvalidArgument, "Texture copy extent is out of bounds");
    }

    const std::optional<CopyFormatInfo> info = copy_format_info(texture.desc.format, subresources.aspects);
    if (!info.has_value()) {
        return Result::failure(ResultCode::kInvalidArgument, "Texture copy format or aspect is invalid");
    }
    if (info->compressed && (static_cast<uint32_t>(offset.x) % info->block_width != 0 ||
                             static_cast<uint32_t>(offset.y) % info->block_height != 0 ||
                             static_cast<uint32_t>(offset.z) % info->block_depth != 0)) {
        return Result::failure(ResultCode::kInvalidArgument, "Compressed texture copy offsets must be block aligned");
    }
    if (info->compressed &&
        ((extent.width % info->block_width != 0 && static_cast<uint64_t>(offset.x) + extent.width != available.width) ||
         (extent.height % info->block_height != 0 &&
          static_cast<uint64_t>(offset.y) + extent.height != available.height) ||
         (extent.depth % info->block_depth != 0 &&
          static_cast<uint64_t>(offset.z) + extent.depth != available.depth))) {
        return Result::failure(ResultCode::kInvalidArgument, "Compressed texture copy extent is not block aligned");
    }
    return Result::success();
}

uint32_t VulkanDevice::texture_subresource_index(const TextureResource& texture, Aspect aspect, uint32_t mip,
                                                 uint32_t layer) const {
    uint32_t aspect_index = 0;
    if (aspect == Aspect::kDepth) {
        aspect_index = 1;
    } else if (aspect == Aspect::kStencil) {
        aspect_index = 2;
    }
    return (aspect_index * texture.desc.mip_levels + mip) * texture.desc.array_layers + layer;
}

Result VulkanDevice::record_barriers(CommandListResource& command, std::span<const BufferBarrier> buffers,
                                     std::span<const TextureBarrier> textures) {
    const Result recording_result = validate_command_recording(command);
    if (!recording_result) {
        return recording_result;
    }
    if (buffers.empty() && textures.empty()) {
        return Result::success();
    }
    if (command.rendering) {
        return Result::failure(ResultCode::kInvalidState, "Barriers cannot be recorded inside rendering");
    }

    struct ResolvedBufferBarrier {
        BufferHandle handle{};
        uint64_t offset{0};
        uint64_t size{0};
        ResourceState before{};
        ResourceState after{};
        bool discard_contents{false};
        bool requires_memory_dependency{false};
    };
    struct ResolvedTextureBarrier {
        TextureHandle handle{};
        VkImageSubresourceRange range{};
        ResourceState before{};
        ResourceState after{};
        bool discard_contents{false};
        bool requires_memory_dependency{false};
        std::vector<uint32_t> state_indices{};
    };

    struct WorkingBufferState {
        BufferHandle buffer{};
        std::vector<BufferStateSegment> segments{};
    };

    std::vector<WorkingBufferState> working_buffer_states;
    std::vector<ResolvedBufferBarrier> resolved_buffers;
    std::vector<ResolvedTextureBarrier> resolved_textures;
    std::vector<BufferStateChange> staged_buffer_changes;
    std::vector<TextureStateChange> staged_texture_changes;
    std::vector<BufferOwnershipTransfer> staged_buffer_ownership_transfers;
    std::vector<TextureOwnershipTransfer> staged_texture_ownership_transfers;
    std::vector<BufferHandle> staged_referenced_buffers;
    std::vector<TextureHandle> staged_referenced_textures;

    try {
        working_buffer_states.reserve(buffers.size());
        resolved_buffers.reserve(buffers.size());
        resolved_textures.reserve(textures.size());
        staged_buffer_changes.reserve(buffers.size());
        staged_texture_changes.reserve(textures.size());
        staged_buffer_ownership_transfers.reserve(buffers.size());
        staged_texture_ownership_transfers.reserve(textures.size());
        staged_referenced_buffers.reserve(buffers.size());
        staged_referenced_textures.reserve(textures.size());
    } catch (const std::bad_alloc&) {
        return Result::failure(ResultCode::kOutOfMemory, "Barrier state staging allocation failed");
    }

    const auto normalize_after_state = [&](ResourceState state) {
        if (state.queue == QueueClass::kAuto) {
            state.queue = command.queue;
        }
        return state;
    };
    const auto has_local_texture_state = [&](TextureHandle texture, uint32_t subresource_index) {
        return std::any_of(command.texture_state_changes.begin(), command.texture_state_changes.end(),
                           [texture, subresource_index](const TextureStateChange& change) {
                               return change.texture == texture && change.subresource_index == subresource_index;
                           });
    };
    const auto find_working_buffer_state = [&](BufferHandle buffer) -> WorkingBufferState* {
        const auto iterator =
            std::find_if(working_buffer_states.begin(), working_buffer_states.end(),
                         [buffer](const WorkingBufferState& state) { return state.buffer == buffer; });
        return iterator == working_buffer_states.end() ? nullptr : &*iterator;
    };
    const auto find_staged_texture_state = [&](TextureHandle texture,
                                               uint32_t subresource_index) -> const TextureStateChange* {
        const auto iterator =
            std::find_if(staged_texture_changes.rbegin(), staged_texture_changes.rend(),
                         [texture, subresource_index](const TextureStateChange& change) {
                             return change.texture == texture && change.subresource_index == subresource_index;
                         });
        return iterator == staged_texture_changes.rend() ? nullptr : &*iterator;
    };
    const auto current_staged_texture_state = [&](TextureHandle texture, uint32_t subresource_index) {
        if (const TextureStateChange* change = find_staged_texture_state(texture, subresource_index);
            change != nullptr) {
            return change->after;
        }
        return current_texture_state(command, texture, subresource_index);
    };
    const auto stage_texture_state = [&](TextureHandle texture, uint32_t subresource_index, ResourceState before,
                                         ResourceState after) {
        if (before == after) {
            return;
        }
        auto iterator =
            std::find_if(staged_texture_changes.begin(), staged_texture_changes.end(),
                         [texture, subresource_index](const TextureStateChange& change) {
                             return change.texture == texture && change.subresource_index == subresource_index;
                         });
        if (iterator == staged_texture_changes.end()) {
            staged_texture_changes.push_back(TextureStateChange{texture, subresource_index, before, after});
        } else {
            iterator->after = after;
        }
    };
    const auto queue_family = [&](QueueClass queue) {
        if (queue == QueueClass::kAuto) {
            return VK_QUEUE_FAMILY_IGNORED;
        }
        const QueueState* state = queue_state(queue);
        return state == nullptr ? VK_QUEUE_FAMILY_IGNORED : state->family_index;
    };

    const auto is_cross_family_transfer = [&](const ResourceState& before, const ResourceState& after,
                                              uint32_t& source_family, uint32_t& destination_family) {
        source_family = VK_QUEUE_FAMILY_IGNORED;
        destination_family = VK_QUEUE_FAMILY_IGNORED;
        if (before.queue == QueueClass::kAuto || after.queue == QueueClass::kAuto ||
            before.queue == QueueClass::kPresent || after.queue == QueueClass::kPresent) {
            return false;
        }
        source_family = queue_family(before.queue);
        destination_family = queue_family(after.queue);
        return source_family != VK_QUEUE_FAMILY_IGNORED && destination_family != VK_QUEUE_FAMILY_IGNORED &&
               source_family != destination_family;
    };

    try {
        for (const BufferBarrier& barrier : buffers) {
            BufferResource* resource = buffers_.get(barrier.buffer);
            if (resource == nullptr) {
                return Result::failure(ResultCode::kNotFound, "Barrier contains an invalid buffer handle");
            }
            const Result range_result = validate_buffer_range(barrier.buffer, barrier.offset, barrier.size);
            if (!range_result) {
                return range_result;
            }
            WorkingBufferState* local = find_working_buffer_state(barrier.buffer);
            if (local == nullptr) {
                WorkingBufferState working{};
                working.buffer = barrier.buffer;
                if (const CommandBufferState* command_state = find_command_buffer_state(command, barrier.buffer);
                    command_state != nullptr) {
                    working.segments = command_state->segments;
                } else {
                    working.segments = resource->state_segments;
                    initialize_buffer_state_segments(working.segments, resource->size, resource->state);
                }
                try {
                    working_buffer_states.push_back(std::move(working));
                } catch (const std::bad_alloc&) {
                    return Result::failure(ResultCode::kOutOfMemory, "Buffer state tracking allocation failed");
                }
                local = &working_buffer_states.back();
            }
            uint64_t range_end = 0;
            if (!resolve_buffer_range(resource->size, barrier.offset, barrier.size, range_end)) {
                return Result::failure(ResultCode::kInvalidArgument, "Buffer range exceeds buffer size");
            }
            try {
                split_buffer_state_segment(local->segments, barrier.offset);
                split_buffer_state_segment(local->segments, range_end);
            } catch (const std::bad_alloc&) {
                return Result::failure(ResultCode::kOutOfMemory, "Buffer state tracking allocation failed");
            }
            ResourceState after = normalize_after_state(barrier.after);
            const bool discard_contents = after.discard_contents;
            after.discard_contents = false;
            for (BufferStateSegment& segment : local->segments) {
                const uint64_t segment_end = segment.offset + segment.size;
                if (segment.offset < barrier.offset || segment_end > range_end) {
                    continue;
                }
                if (!resource_state_matches(segment.state, barrier.before)) {
                    return Result::failure(ResultCode::kInvalidState,
                                           "Buffer barrier before-state does not match the tracked state");
                }
                resolved_buffers.push_back(ResolvedBufferBarrier{barrier.buffer, segment.offset, segment.size,
                                                                 segment.state, after, discard_contents,
                                                                 barrier.requires_memory_dependency});
                if (segment.state != after) {
                    staged_buffer_changes.push_back(
                        BufferStateChange{barrier.buffer, segment.offset, segment.size, segment.state, after});
                }
                segment.state = after;
            }
            merge_buffer_state_segments(local->segments);
        }

        for (const TextureBarrier& barrier : textures) {
            const TextureResource* resource = textures_.get(barrier.texture);
            if (resource == nullptr) {
                return Result::failure(ResultCode::kNotFound, "Barrier contains an invalid texture handle");
            }
            const Result range_result = validate_texture_subresource(*resource, barrier.subresources);
            if (!range_result) {
                return range_result;
            }

            const uint32_t mip_count = barrier.subresources.mip_count == kRemaining
                                           ? resource->desc.mip_levels - barrier.subresources.base_mip
                                           : barrier.subresources.mip_count;
            const uint32_t layer_count = barrier.subresources.layer_count == kRemaining
                                             ? resource->desc.array_layers - barrier.subresources.base_layer
                                             : barrier.subresources.layer_count;
            ResourceState after = normalize_after_state(barrier.after);
            const bool discard_contents = after.discard_contents;
            after.discard_contents = false;
            std::array<Aspect, 3> aspects{Aspect::kColor, Aspect::kDepth, Aspect::kStencil};
            for (const Aspect aspect : aspects) {
                if (!has_flag(barrier.subresources.aspects, aspect)) {
                    continue;
                }

                std::vector<uint32_t> indices;
                indices.reserve(static_cast<size_t>(mip_count) * layer_count);
                for (uint32_t mip = barrier.subresources.base_mip; mip < barrier.subresources.base_mip + mip_count;
                     ++mip) {
                    for (uint32_t layer = barrier.subresources.base_layer;
                         layer < barrier.subresources.base_layer + layer_count; ++layer) {
                        const uint32_t index = texture_subresource_index(*resource, aspect, mip, layer);
                        const ResourceState current = current_staged_texture_state(barrier.texture, index);
                        if ((command.barrier_mode == BarrierMode::kExplicit ||
                             has_local_texture_state(barrier.texture, index) ||
                             find_staged_texture_state(barrier.texture, index) != nullptr) &&
                            !resource_state_matches(current, barrier.before)) {
                            return Result::failure(ResultCode::kInvalidState,
                                                   "Texture barrier before-state does not match the tracked state");
                        }
                        indices.push_back(index);
                        stage_texture_state(barrier.texture, index, current, after);
                    }
                }

                if (indices.empty()) {
                    continue;
                }
                VkImageSubresourceRange range{};
                range.aspectMask = to_vk_aspect(aspect);
                range.baseMipLevel = barrier.subresources.base_mip;
                range.levelCount = mip_count;
                range.baseArrayLayer = barrier.subresources.base_layer;
                range.layerCount = layer_count;
                resolved_textures.push_back(ResolvedTextureBarrier{barrier.texture, range, barrier.before, after,
                                                                   discard_contents, barrier.requires_memory_dependency,
                                                                   std::move(indices)});
                add_unique(staged_referenced_textures, barrier.texture);
            }
        }

        for (const ResolvedBufferBarrier& resolved : resolved_buffers) {
            uint32_t source_family = VK_QUEUE_FAMILY_IGNORED;
            uint32_t destination_family = VK_QUEUE_FAMILY_IGNORED;
            if (is_cross_family_transfer(resolved.before, resolved.after, source_family, destination_family) &&
                resolved.after.queue != command.queue) {
                return Result::failure(ResultCode::kInvalidArgument,
                                       "Cross-queue buffer barriers must be recorded on the destination queue");
            }
        }
        for (const ResolvedTextureBarrier& resolved : resolved_textures) {
            uint32_t source_family = VK_QUEUE_FAMILY_IGNORED;
            uint32_t destination_family = VK_QUEUE_FAMILY_IGNORED;
            if (is_cross_family_transfer(resolved.before, resolved.after, source_family, destination_family) &&
                resolved.after.queue != command.queue) {
                return Result::failure(ResultCode::kInvalidArgument,
                                       "Cross-queue texture barriers must be recorded on the destination queue");
            }
        }

        std::vector<VkBufferMemoryBarrier2> buffer_barriers2;
        std::vector<VkImageMemoryBarrier2> image_barriers2;
        std::vector<VkBufferMemoryBarrier> buffer_barriers;
        std::vector<VkImageMemoryBarrier> image_barriers;
        buffer_barriers2.reserve(resolved_buffers.size());
        image_barriers2.reserve(resolved_textures.size());
        buffer_barriers.reserve(resolved_buffers.size());
        image_barriers.reserve(resolved_textures.size());

        VkPipelineStageFlags legacy_source_stages = 0;
        VkPipelineStageFlags legacy_destination_stages = 0;
        for (const ResolvedBufferBarrier& resolved : resolved_buffers) {
            add_unique(staged_referenced_buffers, resolved.handle);
            const VulkanResourceState source = resolved.discard_contents
                                                   ? to_vk_discard_source_state(resolved.before, false)
                                                   : to_vk_resource_state(resolved.before, false);
            const VulkanResourceState destination = to_vk_resource_state(resolved.after, false);
            if (!resolved.discard_contents && !resolved.requires_memory_dependency &&
                source.access2 == destination.access2 && source.stages2 == destination.stages2 &&
                resolved.before.queue == resolved.after.queue) {
                continue;
            }

            uint32_t source_family = VK_QUEUE_FAMILY_IGNORED;
            uint32_t destination_family = VK_QUEUE_FAMILY_IGNORED;
            const bool cross_family =
                is_cross_family_transfer(resolved.before, resolved.after, source_family, destination_family);
            VkBufferMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            barrier.srcStageMask = cross_family ? VK_PIPELINE_STAGE_2_NONE : source.stages2;
            barrier.srcAccessMask = cross_family ? 0 : source.access2;
            barrier.dstStageMask = destination.stages2;
            barrier.dstAccessMask = destination.access2;
            barrier.srcQueueFamilyIndex = cross_family ? source_family : VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = cross_family ? destination_family : VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = buffers_.get(resolved.handle)->buffer;
            barrier.offset = resolved.offset;
            barrier.size = resolved.size;
            buffer_barriers2.push_back(barrier);

            VkBufferMemoryBarrier legacy{};
            legacy.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            legacy.srcAccessMask = cross_family ? 0 : source.access;
            legacy.dstAccessMask = destination.access;
            legacy.srcQueueFamilyIndex = cross_family ? source_family : VK_QUEUE_FAMILY_IGNORED;
            legacy.dstQueueFamilyIndex = cross_family ? destination_family : VK_QUEUE_FAMILY_IGNORED;
            legacy.buffer = barrier.buffer;
            legacy.offset = barrier.offset;
            legacy.size = barrier.size;
            buffer_barriers.push_back(legacy);
            legacy_source_stages |=
                cross_family ? static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT) : source.stages;
            legacy_destination_stages |= destination.stages;

            if (cross_family) {
                staged_buffer_ownership_transfers.push_back(BufferOwnershipTransfer{
                    resolved.handle, resolved.offset, resolved.size, resolved.before, resolved.after,
                    resolved.discard_contents, source_family, destination_family, QueueOwnershipPhase::kAcquire});
            }
        }

        for (const ResolvedTextureBarrier& resolved : resolved_textures) {
            const VulkanResourceState source = resolved.discard_contents
                                                   ? to_vk_discard_source_state(resolved.before, true)
                                                   : to_vk_resource_state(resolved.before, true);
            const VulkanResourceState destination = to_vk_resource_state(resolved.after, true);
            if (!resolved.discard_contents && !resolved.requires_memory_dependency &&
                source.layout == destination.layout && source.access2 == destination.access2 &&
                source.stages2 == destination.stages2 && resolved.before.queue == resolved.after.queue) {
                continue;
            }

            uint32_t source_family = VK_QUEUE_FAMILY_IGNORED;
            uint32_t destination_family = VK_QUEUE_FAMILY_IGNORED;
            const bool cross_family =
                is_cross_family_transfer(resolved.before, resolved.after, source_family, destination_family);
            VkImageMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            barrier.srcStageMask = cross_family ? VK_PIPELINE_STAGE_2_NONE : source.stages2;
            barrier.srcAccessMask = cross_family ? 0 : source.access2;
            barrier.dstStageMask = destination.stages2;
            barrier.dstAccessMask = destination.access2;
            barrier.oldLayout = source.layout;
            barrier.newLayout = destination.layout;
            barrier.srcQueueFamilyIndex = cross_family ? source_family : VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = cross_family ? destination_family : VK_QUEUE_FAMILY_IGNORED;
            barrier.image = textures_.get(resolved.handle)->image;
            barrier.subresourceRange = resolved.range;
            image_barriers2.push_back(barrier);

            VkImageMemoryBarrier legacy{};
            legacy.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            legacy.srcAccessMask = cross_family ? 0 : source.access;
            legacy.dstAccessMask = destination.access;
            legacy.oldLayout = source.layout;
            legacy.newLayout = destination.layout;
            legacy.srcQueueFamilyIndex = cross_family ? source_family : VK_QUEUE_FAMILY_IGNORED;
            legacy.dstQueueFamilyIndex = cross_family ? destination_family : VK_QUEUE_FAMILY_IGNORED;
            legacy.image = barrier.image;
            legacy.subresourceRange = barrier.subresourceRange;
            image_barriers.push_back(legacy);
            legacy_source_stages |=
                cross_family ? static_cast<VkPipelineStageFlags>(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT) : source.stages;
            legacy_destination_stages |= destination.stages;

            if (cross_family) {
                staged_texture_ownership_transfers.push_back(TextureOwnershipTransfer{
                    resolved.handle, resolved.range, resolved.before, resolved.after, resolved.discard_contents,
                    source_family, destination_family, QueueOwnershipPhase::kAcquire});
            }
        }

        const auto reserve_additional = [](auto& values, size_t additional) {
            if (additional > std::numeric_limits<size_t>::max() - values.size()) {
                return false;
            }
            values.reserve(values.size() + additional);
            return true;
        };
        size_t new_buffer_state_count = 0;
        for (const WorkingBufferState& working : working_buffer_states) {
            if (find_command_buffer_state(command, working.buffer) == nullptr) {
                if (new_buffer_state_count == std::numeric_limits<size_t>::max()) {
                    return Result::failure(ResultCode::kOutOfMemory, "Buffer state count overflowed");
                }
                ++new_buffer_state_count;
            }
        }
        try {
            if (!reserve_additional(command.buffer_states, new_buffer_state_count) ||
                !reserve_additional(command.buffer_state_changes, staged_buffer_changes.size()) ||
                !reserve_additional(command.texture_state_changes, staged_texture_changes.size()) ||
                !reserve_additional(command.buffer_ownership_transfers, staged_buffer_ownership_transfers.size()) ||
                !reserve_additional(command.texture_ownership_transfers, staged_texture_ownership_transfers.size()) ||
                !reserve_additional(command.referenced_buffers, staged_referenced_buffers.size()) ||
                !reserve_additional(command.referenced_textures, staged_referenced_textures.size())) {
                return Result::failure(ResultCode::kOutOfMemory, "Barrier state capacity calculation overflowed");
            }
        } catch (const std::bad_alloc&) {
            return Result::failure(ResultCode::kOutOfMemory, "Barrier state commit allocation failed");
        }

        if (!buffer_barriers2.empty() || !image_barriers2.empty()) {
            if (enabled_features_.synchronization2) {
                VkDependencyInfo dependency{};
                dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                dependency.bufferMemoryBarrierCount = static_cast<uint32_t>(buffer_barriers2.size());
                dependency.pBufferMemoryBarriers = buffer_barriers2.data();
                dependency.imageMemoryBarrierCount = static_cast<uint32_t>(image_barriers2.size());
                dependency.pImageMemoryBarriers = image_barriers2.data();
                vkCmdPipelineBarrier2(command.command_buffer, &dependency);
            } else {
                if (legacy_source_stages == 0) {
                    legacy_source_stages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
                }
                if (legacy_destination_stages == 0) {
                    legacy_destination_stages = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
                }
                vkCmdPipelineBarrier(command.command_buffer, legacy_source_stages, legacy_destination_stages, 0, 0,
                                     nullptr, static_cast<uint32_t>(buffer_barriers.size()), buffer_barriers.data(),
                                     static_cast<uint32_t>(image_barriers.size()), image_barriers.data());
            }
        }

        for (WorkingBufferState& working : working_buffer_states) {
            CommandBufferState* existing = find_command_buffer_state(command, working.buffer);
            if (existing == nullptr) {
                command.buffer_states.push_back(CommandBufferState{working.buffer, std::move(working.segments)});
            } else {
                existing->segments = std::move(working.segments);
            }
        }
        for (BufferStateChange& change : staged_buffer_changes) {
            command.buffer_state_changes.push_back(std::move(change));
        }
        for (TextureStateChange& change : staged_texture_changes) {
            auto iterator = std::find_if(command.texture_state_changes.begin(), command.texture_state_changes.end(),
                                         [&](const TextureStateChange& existing) {
                                             return existing.texture == change.texture &&
                                                    existing.subresource_index == change.subresource_index;
                                         });
            if (iterator == command.texture_state_changes.end()) {
                command.texture_state_changes.push_back(std::move(change));
            } else {
                iterator->after = change.after;
            }
        }
        for (BufferOwnershipTransfer& transfer : staged_buffer_ownership_transfers) {
            command.buffer_ownership_transfers.push_back(std::move(transfer));
        }
        for (TextureOwnershipTransfer& transfer : staged_texture_ownership_transfers) {
            command.texture_ownership_transfers.push_back(std::move(transfer));
        }
        for (const BufferHandle buffer : staged_referenced_buffers) {
            add_unique(command.referenced_buffers, buffer);
        }
        for (const TextureHandle texture : staged_referenced_textures) {
            add_unique(command.referenced_textures, texture);
        }
        return Result::success();
    } catch (const std::bad_alloc&) {
        return Result::failure(ResultCode::kOutOfMemory, "Barrier state staging allocation failed");
    }
}

Result VulkanDevice::transition_buffer(CommandListResource& command, BufferHandle buffer, uint64_t offset,
                                       uint64_t size, const ResourceState& after) {
    BufferResource* resource = buffers_.get(buffer);
    if (resource == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid buffer handle");
    }
    const Result range_result = validate_buffer_range(buffer, offset, size);
    if (!range_result) {
        return range_result;
    }
    ResourceState normalized_after = after;
    if (normalized_after.queue == QueueClass::kAuto) {
        normalized_after.queue = command.queue;
    }

    const Result state_result = ensure_command_buffer_state(command, buffer);
    if (!state_result) {
        return state_result;
    }
    CommandBufferState* local = find_command_buffer_state(command, buffer);
    if (local == nullptr) {
        return Result::failure(ResultCode::kInvalidState, "Buffer state tracking is unavailable");
    }

    uint64_t end = 0;
    if (!resolve_buffer_range(resource->size, offset, size, end)) {
        return Result::failure(ResultCode::kInvalidArgument, "Buffer range exceeds buffer size");
    }
    if (command.barrier_mode == BarrierMode::kExplicit) {
        return validate_buffer_usage(command, buffer, offset, size, normalized_after);
    }

    try {
        split_buffer_state_segment(local->segments, offset);
        split_buffer_state_segment(local->segments, end);
    } catch (const std::bad_alloc&) {
        return Result::failure(ResultCode::kOutOfMemory, "Buffer state tracking allocation failed");
    }

    std::vector<BufferBarrier> barriers;
    try {
        barriers.reserve(local->segments.size());
        for (const BufferStateSegment& segment : local->segments) {
            const uint64_t segment_end = segment.offset + segment.size;
            if (segment_end <= offset || segment.offset >= end) {
                continue;
            }
            if (segment.state != normalized_after) {
                barriers.push_back(
                    BufferBarrier{buffer, segment.offset, segment.size, segment.state, normalized_after, false});
            }
        }
    } catch (const std::bad_alloc&) {
        return Result::failure(ResultCode::kOutOfMemory, "Buffer state transition allocation failed");
    }

    if (barriers.empty()) {
        return Result::success();
    }
    return record_barriers(command, barriers, {});
}

Result VulkanDevice::transition_texture(CommandListResource& command, TextureHandle texture,
                                        const SubresourceRange& subresources, const ResourceState& after) {
    const TextureResource* resource = textures_.get(texture);
    if (resource == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid texture handle");
    }
    const Result range_result = validate_texture_subresource(*resource, subresources);
    if (!range_result) {
        return range_result;
    }

    ResourceState normalized_after = after;
    if (normalized_after.queue == QueueClass::kAuto) {
        normalized_after.queue = command.queue;
    }
    const uint32_t mip_count = subresources.mip_count == kRemaining ? resource->desc.mip_levels - subresources.base_mip
                                                                    : subresources.mip_count;
    const uint32_t layer_count = subresources.layer_count == kRemaining
                                     ? resource->desc.array_layers - subresources.base_layer
                                     : subresources.layer_count;
    if (command.barrier_mode == BarrierMode::kExplicit) {
        const std::array<Aspect, 3> aspects{Aspect::kColor, Aspect::kDepth, Aspect::kStencil};
        for (const Aspect aspect : aspects) {
            if (!has_flag(subresources.aspects, aspect)) {
                continue;
            }
            for (uint32_t mip = subresources.base_mip; mip < subresources.base_mip + mip_count; ++mip) {
                for (uint32_t layer = subresources.base_layer; layer < subresources.base_layer + layer_count; ++layer) {
                    const uint32_t index = texture_subresource_index(*resource, aspect, mip, layer);
                    if (!resource_usage_matches(current_texture_state(command, texture, index), normalized_after)) {
                        return Result::failure(ResultCode::kInvalidState,
                                               "Explicit command list texture state does not match the declared usage");
                    }
                }
            }
        }
        return Result::success();
    }
    std::vector<TextureBarrier> barriers;
    barriers.reserve(3);
    std::array<Aspect, 3> aspects{Aspect::kColor, Aspect::kDepth, Aspect::kStencil};
    for (const Aspect aspect : aspects) {
        if (!has_flag(subresources.aspects, aspect)) {
            continue;
        }
        const uint32_t index =
            texture_subresource_index(*resource, aspect, subresources.base_mip, subresources.base_layer);
        barriers.push_back(TextureBarrier{
            texture, SubresourceRange{aspect, subresources.base_mip, mip_count, subresources.base_layer, layer_count},
            current_texture_state(command, texture, index), normalized_after});
    }
    return record_barriers(command, {}, barriers);
}

Result VulkanDevice::transition_texture(CommandListResource& command, const TextureViewResource& view,
                                        ResourceUsage usage) {
    ResourceState after{};
    after.usage = usage;
    after.queue = command.queue;
    after.pipeline_stages = PipelineStage::kAllCommands;
    return transition_texture(command, view.desc.texture, view.desc.subresources, after);
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
        case BindingType::kReadOnlyTypedBuffer:
        case BindingType::kReadWriteTypedBuffer: {
            const BufferViewResource* view = buffer_views_.get(resource.buffer_view);
            if (view == nullptr || view->view == VK_NULL_HANDLE) {
                return Result::failure(ResultCode::kInvalidArgument,
                                       "Typed buffer binding requires a typed buffer view");
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
    if (has_flag(desc.capabilities, BufferCapability::kDeviceAddress) && !enabled_features_.buffer_device_address) {
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
    resource.state.usage = desc.initial_usage;
    resource.state.queue = desc.initial_queue;
    resource.state.pipeline_stages = desc.initial_stages;
    if ((resource.allocation.properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
        void* mapped = nullptr;
        const VkResult map_result = vmaMapMemory(allocator_, vma_allocation, &mapped);
        if (map_result != VK_SUCCESS) {
            vmaDestroyBuffer(allocator_, buffer, vma_allocation);
            return {make_vulkan_result(map_result, "vmaMapMemory(createBuffer)"), {}};
        }
        resource.mapped_data = mapped;
    }
#if AEG_VULKAN_ENABLE_DEBUG_NAMES
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
    if (buffer_has_unsubmitted_command_reference(buffer)) {
        return Result::failure(ResultCode::kInvalidState,
                               "Buffer is referenced by a command list that has not been submitted");
    }

    RetireBuffer retire{resource->last_use, resource->buffer, resource->allocation.handle,
                        resource->mapped_data != nullptr};
    (void)buffers_.erase(buffer);
    enqueue_retire(std::move(retire));
    return Result::success();
}

ResultValue<BufferStateSnapshot> VulkanDevice::query_buffer_state(BufferHandle buffer) const {
    const BufferResource* resource = buffers_.get(buffer);
    if (resource == nullptr) {
        return {Result::failure(ResultCode::kNotFound, "Invalid buffer handle"), {}};
    }
    if (resource->size == 0) {
        return {Result::failure(ResultCode::kInvalidState, "Buffer state has no resource size"), {}};
    }

    BufferStateSnapshot snapshot{};
    try {
        if (resource->state_segments.empty()) {
            snapshot.segments.push_back({0, resource->size, resource->state});
        } else {
            uint64_t expected_offset = 0;
            snapshot.segments.reserve(resource->state_segments.size());
            for (const BufferStateSegment& segment : resource->state_segments) {
                if (segment.size == 0 || segment.offset != expected_offset || segment.offset > resource->size ||
                    segment.size > resource->size - segment.offset) {
                    return {
                        Result::failure(ResultCode::kInvalidState, "Buffer state segments do not cover the resource"),
                        {}};
                }
                snapshot.segments.push_back({segment.offset, segment.size, segment.state});
                expected_offset += segment.size;
            }
            if (expected_offset != resource->size) {
                return {Result::failure(ResultCode::kInvalidState, "Buffer state segments do not cover the resource"),
                        {}};
            }
        }

        for (uint32_t index = 0; index < kQueueCount; ++index) {
            const uint64_t value = resource->last_use[index];
            if (value == 0) {
                continue;
            }
            const TimelineHandle timeline = queues_[index].timeline_handle;
            if (!timeline.is_valid() || !snapshot.last_use.add(GpuSyncPoint{timeline, value})) {
                return {Result::failure(ResultCode::kInvalidState, "Buffer state completion timeline is unavailable"),
                        {}};
            }
        }
    } catch (const std::bad_alloc&) {
        return {Result::failure(ResultCode::kOutOfMemory, "Buffer state snapshot allocation failed"), {}};
    }
    return {Result::success(), std::move(snapshot)};
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

    void* mapped = resource->mapped_data;
    bool mapped_for_operation = false;
    VkResult result = VK_SUCCESS;
    if (mapped == nullptr) {
        result = vmaMapMemory(allocator_, resource->allocation.handle, &mapped);
        if (result != VK_SUCCESS) {
            return make_vulkan_result(result, "vmaMapMemory");
        }
        mapped_for_operation = true;
    }
    std::memcpy(static_cast<std::byte*>(mapped) + offset, data.data(), data.size());

    if ((resource->allocation.properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
        result = vmaFlushAllocation(allocator_, resource->allocation.handle, offset, data.size());
        if (result != VK_SUCCESS) {
            if (mapped_for_operation) {
                vmaUnmapMemory(allocator_, resource->allocation.handle);
            }
            return make_vulkan_result(result, "vmaFlushAllocation");
        }
    }

    if (mapped_for_operation) {
        vmaUnmapMemory(allocator_, resource->allocation.handle);
    }

    // A host write is an externally recorded state transition. Keep the
    // tracked range in HostWrite so a later explicit command-list barrier
    // starts from the state that actually precedes the GPU copy.
    const uint64_t write_size = static_cast<uint64_t>(data.size());
    const uint64_t write_end = offset + write_size;
    try {
        initialize_buffer_state_segments(resource->state_segments, resource->size, resource->state);
        split_buffer_state_segment(resource->state_segments, offset);
        split_buffer_state_segment(resource->state_segments, write_end);
        for (BufferStateSegment& segment : resource->state_segments) {
            const uint64_t segment_end = segment.offset + segment.size;
            if (segment.offset < offset || segment_end > write_end) {
                continue;
            }
            segment.state.usage = ResourceUsage::kHostWrite;
            segment.state.shader_stages = ShaderStage::kAll;
            segment.state.pipeline_stages = PipelineStage::kHost;
            segment.state.discard_contents = false;
        }
        merge_buffer_state_segments(resource->state_segments);
        if (!resource->state_segments.empty()) {
            const ResourceState state = resource->state_segments.front().state;
            const bool uniform =
                std::all_of(resource->state_segments.begin(), resource->state_segments.end(),
                            [state](const BufferStateSegment& segment) { return segment.state == state; });
            resource->state = uniform ? state : ResourceState{};
        }
    } catch (const std::bad_alloc&) {
        return Result::failure(ResultCode::kOutOfMemory, "Buffer host-write state tracking allocation failed");
    }
    return Result::success();
}

Result VulkanDevice::read_buffer(BufferHandle buffer, uint64_t offset, std::span<std::byte> data) {
    BufferResource* resource = buffers_.get(buffer);
    if (resource == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid buffer handle");
    }
    if (offset > resource->size || data.size() > resource->size - offset) {
        return Result::failure(ResultCode::kInvalidArgument, "Buffer read exceeds buffer size");
    }
    if ((resource->allocation.properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) {
        return Result::failure(ResultCode::kInvalidState, "Buffer memory is not host visible");
    }
    if (data.empty()) {
        return Result::success();
    }

    void* mapped = resource->mapped_data;
    bool mapped_for_operation = false;
    VkResult result = VK_SUCCESS;
    if (mapped == nullptr) {
        result = vmaMapMemory(allocator_, resource->allocation.handle, &mapped);
        if (result != VK_SUCCESS) {
            return make_vulkan_result(result, "vmaMapMemory(readBuffer)");
        }
        mapped_for_operation = true;
    }
    if ((resource->allocation.properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
        result = vmaInvalidateAllocation(allocator_, resource->allocation.handle, offset, data.size());
        if (result != VK_SUCCESS) {
            if (mapped_for_operation) {
                vmaUnmapMemory(allocator_, resource->allocation.handle);
            }
            return make_vulkan_result(result, "vmaInvalidateAllocation");
        }
    }
    std::memcpy(data.data(), static_cast<const std::byte*>(mapped) + offset, data.size());
    if (mapped_for_operation) {
        vmaUnmapMemory(allocator_, resource->allocation.handle);
    }
    return Result::success();
}

ResultValue<PipelineHandle> VulkanDevice::create_pipeline(const PipelineDesc& desc) {
    if (device_ == VK_NULL_HANDLE) {
        return {Result::failure(ResultCode::kInvalidState, "Vulkan device is not initialized"), {}};
    }
    if (desc.type != PipelineType::kGraphics && desc.type != PipelineType::kCompute) {
        return {Result::failure(ResultCode::kInvalidArgument, "Pipeline type is invalid"), {}};
    }
    if (desc.shader_stages.empty() || desc.shader_stages.size() > std::numeric_limits<uint32_t>::max()) {
        return {Result::failure(ResultCode::kInvalidArgument, "Pipeline must contain at least one shader stage"), {}};
    }

    const bool is_compute = desc.type == PipelineType::kCompute;
    bool has_vertex_shader = false;
    uint32_t shader_stage_mask = 0;
    for (const ShaderStageDesc& shader : desc.shader_stages) {
        const auto vk_stage = to_vk_shader_stage(shader.stage);
        if (!vk_stage.has_value()) {
            return {Result::failure(ResultCode::kInvalidArgument, "Pipeline shader stage is invalid"), {}};
        }
        if (shader.code.empty()) {
            return {Result::failure(ResultCode::kInvalidArgument, "Pipeline shader bytecode is empty"), {}};
        }
        if (shader.entry_point.empty()) {
            return {Result::failure(ResultCode::kInvalidArgument, "Pipeline shader entry point is empty"), {}};
        }

        const uint32_t stage_value = static_cast<uint32_t>(shader.stage);
        if ((shader_stage_mask & stage_value) != 0) {
            return {Result::failure(ResultCode::kInvalidArgument, "Pipeline contains a duplicate shader stage"), {}};
        }
        shader_stage_mask |= stage_value;

        if (is_compute) {
            if (shader.stage != ShaderStage::kCompute) {
                return {Result::failure(ResultCode::kInvalidArgument, "Compute pipelines only accept a compute shader"),
                        {}};
            }
        } else {
            if (shader.stage == ShaderStage::kVertex) {
                has_vertex_shader = true;
            } else if (shader.stage != ShaderStage::kFragment) {
                return {Result::failure(ResultCode::kUnsupported,
                                        "This Vulkan backend currently supports vertex and fragment graphics stages"),
                        {}};
            }
        }
    }

    if (is_compute && desc.shader_stages.size() != 1) {
        return {Result::failure(ResultCode::kInvalidArgument, "Compute pipelines require exactly one shader stage"),
                {}};
    }
    if (!is_compute && !has_vertex_shader) {
        return {Result::failure(ResultCode::kInvalidArgument, "Graphics pipelines require a vertex shader"), {}};
    }

    if (!is_compute) {
        if (!enabled_features_.dynamic_rendering) {
            return {Result::failure(ResultCode::kUnsupported, "Graphics pipelines require dynamic rendering support"),
                    {}};
        }
        if (desc.graphics.color_formats.size() > physical_properties_.limits.maxColorAttachments) {
            return {Result::failure(ResultCode::kInvalidArgument, "Graphics pipeline has too many color attachments"),
                    {}};
        }
        if (desc.graphics.color_formats.empty() && desc.graphics.depth_stencil_format == Format::kUnknown &&
            !desc.graphics.rasterizer_discard_enable) {
            return {Result::failure(ResultCode::kInvalidArgument,
                                    "Graphics pipeline must declare an attachment or discard rasterization"),
                    {}};
        }
        if (desc.graphics.topology == PrimitiveTopology::kPatchList) {
            return {Result::failure(ResultCode::kUnsupported,
                                    "Patch-list topology requires tessellation state, which is not implemented"),
                    {}};
        }
        if (!desc.graphics.blend_attachments.empty() &&
            desc.graphics.blend_attachments.size() != desc.graphics.color_formats.size()) {
            return {
                Result::failure(ResultCode::kInvalidArgument, "Blend attachment count must match color format count"),
                {}};
        }
        if (std::any_of(desc.graphics.blend_attachments.begin(), desc.graphics.blend_attachments.end(),
                        [](const BlendAttachmentDesc& blend) {
                            return uses_dual_source_blend(blend.source_color_factor) ||
                                   uses_dual_source_blend(blend.destination_color_factor) ||
                                   uses_dual_source_blend(blend.source_alpha_factor) ||
                                   uses_dual_source_blend(blend.destination_alpha_factor);
                        }) &&
            !enabled_features_.dual_src_blend) {
            return {Result::failure(ResultCode::kUnsupported,
                                    "Dual-source blending is not supported by the selected device"),
                    {}};
        }
        if (desc.graphics.depth_stencil_format != Format::kUnknown &&
            !is_depth_format(desc.graphics.depth_stencil_format)) {
            return {
                Result::failure(ResultCode::kInvalidArgument, "Depth-stencil pipeline format must be a depth format"),
                {}};
        }
        if ((desc.graphics.depth_test_enable || desc.graphics.depth_write_enable) &&
            desc.graphics.depth_stencil_format == Format::kUnknown) {
            return {Result::failure(ResultCode::kInvalidArgument,
                                    "Depth testing or writing requires a depth-stencil format"),
                    {}};
        }
        if (desc.graphics.stencil_test_enable) {
            return {Result::failure(ResultCode::kUnsupported, "Stencil testing is not exposed by this RHI version"),
                    {}};
        }
        if (desc.graphics.line_width <= 0.0f) {
            return {Result::failure(ResultCode::kInvalidArgument, "Graphics pipeline line width must be positive"), {}};
        }
        if (desc.graphics.depth_clamp_enable && !enabled_features_.depth_clamp) {
            return {Result::failure(ResultCode::kUnsupported, "Depth clamp is not supported by the selected device"),
                    {}};
        }
        if (desc.graphics.polygon_mode != PolygonMode::kFill && !enabled_features_.fill_mode_non_solid) {
            return {Result::failure(ResultCode::kUnsupported,
                                    "Non-solid polygon mode is not supported by the selected device"),
                    {}};
        }
        if (desc.graphics.line_width != 1.0f && !enabled_features_.wide_lines) {
            return {Result::failure(ResultCode::kUnsupported, "Wide lines are not supported by the selected device"),
                    {}};
        }
        for (const Format format : desc.graphics.color_formats) {
            if (format == Format::kUnknown || is_depth_format(format) || !to_vk_format(format).has_value()) {
                return {Result::failure(ResultCode::kInvalidArgument, "Graphics color format is invalid"), {}};
            }
        }
        if (desc.graphics.depth_stencil_format != Format::kUnknown &&
            !to_vk_format(desc.graphics.depth_stencil_format).has_value()) {
            return {Result::failure(ResultCode::kInvalidArgument, "Graphics depth format is invalid"), {}};
        }
    }

    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    std::vector<BindingLayoutHandle> binding_layouts;
    const Result layout_result = create_pipeline_layout(desc.layout, pipeline_layout, binding_layouts);
    if (!layout_result) {
        return {layout_result, {}};
    }

    std::vector<VkShaderModule> shader_modules;
    std::vector<std::string> entry_points;
    std::vector<VkPipelineShaderStageCreateInfo> shader_stages;
    shader_modules.reserve(desc.shader_stages.size());
    entry_points.reserve(desc.shader_stages.size());
    shader_stages.reserve(desc.shader_stages.size());

    const auto fail = [&](Result error) -> ResultValue<PipelineHandle> {
        for (const VkShaderModule shader_module : shader_modules) {
            vkDestroyShaderModule(device_, shader_module, nullptr);
        }
        if (pipeline_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device_, pipeline_layout, nullptr);
        }
        return {std::move(error), {}};
    };

    for (const ShaderStageDesc& shader : desc.shader_stages) {
        const auto vk_stage = to_vk_shader_stage(shader.stage);
        if (shader.code.size() % sizeof(uint32_t) != 0) {
            return fail(
                Result::failure(ResultCode::kInvalidArgument, "Vulkan shader bytecode size must be four-byte aligned"));
        }
        if (reinterpret_cast<std::uintptr_t>(shader.code.data()) % alignof(uint32_t) != 0) {
            return fail(Result::failure(ResultCode::kInvalidArgument,
                                        "Vulkan shader bytecode pointer must be four-byte aligned"));
        }
        VkShaderModuleCreateInfo module_info{};
        module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        module_info.codeSize = shader.code.size();
        module_info.pCode = reinterpret_cast<const uint32_t*>(shader.code.data());

        VkShaderModule shader_module = VK_NULL_HANDLE;
        const VkResult module_result = vkCreateShaderModule(device_, &module_info, nullptr, &shader_module);
        if (module_result != VK_SUCCESS) {
            return fail(make_vulkan_result(module_result, "vkCreateShaderModule"));
        }
        shader_modules.push_back(shader_module);
        entry_points.emplace_back(shader.entry_point);

        VkPipelineShaderStageCreateInfo stage_info{};
        stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage_info.stage = vk_stage.value();
        stage_info.module = shader_module;
        stage_info.pName = entry_points.back().c_str();
        shader_stages.push_back(stage_info);
    }

    PipelineResource resource{};
    resource.layout = pipeline_layout;
    resource.type = desc.type;
    resource.binding_layouts = std::move(binding_layouts);
    resource.inline_constant_ranges.assign(desc.layout.inline_constant_ranges.begin(),
                                           desc.layout.inline_constant_ranges.end());

    if (is_compute) {
        VkComputePipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_info.stage = shader_stages.front();
        pipeline_info.layout = pipeline_layout;

        const VkResult result =
            vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &resource.pipeline);
        for (const VkShaderModule shader_module : shader_modules) {
            vkDestroyShaderModule(device_, shader_module, nullptr);
        }
        shader_modules.clear();
        if (result != VK_SUCCESS) {
            return fail(make_vulkan_result(result, "vkCreateComputePipelines"));
        }
    } else {
        std::vector<VkVertexInputBindingDescription> vertex_bindings;
        std::vector<VkVertexInputAttributeDescription> vertex_attributes;
        vertex_bindings.reserve(desc.graphics.vertex_bindings.size());
        vertex_attributes.reserve(desc.graphics.vertex_attributes.size());

        for (const VertexBindingDesc& binding : desc.graphics.vertex_bindings) {
            if (std::any_of(vertex_bindings.begin(), vertex_bindings.end(),
                            [&binding](const auto& existing) { return existing.binding == binding.binding; })) {
                return fail(Result::failure(ResultCode::kInvalidArgument,
                                            "Graphics pipeline contains a duplicate vertex binding"));
            }
            vertex_bindings.push_back(VkVertexInputBindingDescription{binding.binding, binding.stride,
                                                                      to_vk_vertex_input_rate(binding.input_rate)});
        }

        for (const VertexAttributeDesc& attribute : desc.graphics.vertex_attributes) {
            const auto format = to_vk_format(attribute.format);
            if (!format.has_value() || is_depth_format(attribute.format)) {
                return fail(Result::failure(ResultCode::kInvalidArgument,
                                            "Graphics pipeline vertex attribute format is invalid"));
            }
            if (std::any_of(vertex_attributes.begin(), vertex_attributes.end(),
                            [&attribute](const auto& existing) { return existing.location == attribute.location; })) {
                return fail(Result::failure(ResultCode::kInvalidArgument,
                                            "Graphics pipeline contains a duplicate vertex attribute location"));
            }
            if (std::none_of(vertex_bindings.begin(), vertex_bindings.end(),
                             [&attribute](const auto& existing) { return existing.binding == attribute.binding; })) {
                return fail(Result::failure(ResultCode::kInvalidArgument,
                                            "Vertex attribute references an undefined vertex binding"));
            }
            vertex_attributes.push_back(VkVertexInputAttributeDescription{attribute.location, attribute.binding,
                                                                          format.value(), attribute.offset});
        }

        std::vector<VkFormat> color_formats;
        color_formats.reserve(desc.graphics.color_formats.size());
        for (const Format format : desc.graphics.color_formats) {
            color_formats.push_back(to_vk_format(format).value());
        }

        const auto depth_format = desc.graphics.depth_stencil_format == Format::kUnknown
                                      ? VK_FORMAT_UNDEFINED
                                      : to_vk_format(desc.graphics.depth_stencil_format).value();
        const bool has_stencil = desc.graphics.depth_stencil_format == Format::kD24UnormS8Uint ||
                                 desc.graphics.depth_stencil_format == Format::kD32FloatS8Uint;

        std::vector<BlendAttachmentDesc> blend_descriptions;
        if (desc.graphics.blend_attachments.empty()) {
            blend_descriptions.assign(color_formats.size(), BlendAttachmentDesc{});
        } else {
            blend_descriptions.assign(desc.graphics.blend_attachments.begin(), desc.graphics.blend_attachments.end());
        }
        std::vector<VkPipelineColorBlendAttachmentState> blend_attachments;
        blend_attachments.reserve(blend_descriptions.size());
        for (const BlendAttachmentDesc& blend : blend_descriptions) {
            VkPipelineColorBlendAttachmentState attachment{};
            attachment.blendEnable = blend.blend_enable ? VK_TRUE : VK_FALSE;
            attachment.srcColorBlendFactor = to_vk_blend_factor(blend.source_color_factor);
            attachment.dstColorBlendFactor = to_vk_blend_factor(blend.destination_color_factor);
            attachment.colorBlendOp = to_vk_blend_op(blend.color_operation);
            attachment.srcAlphaBlendFactor = to_vk_blend_factor(blend.source_alpha_factor);
            attachment.dstAlphaBlendFactor = to_vk_blend_factor(blend.destination_alpha_factor);
            attachment.alphaBlendOp = to_vk_blend_op(blend.alpha_operation);
            attachment.colorWriteMask = to_vk_color_write_mask(blend.color_write_mask);
            blend_attachments.push_back(attachment);
        }

        VkPipelineVertexInputStateCreateInfo vertex_input_info{};
        vertex_input_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input_info.vertexBindingDescriptionCount = static_cast<uint32_t>(vertex_bindings.size());
        vertex_input_info.pVertexBindingDescriptions = vertex_bindings.data();
        vertex_input_info.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertex_attributes.size());
        vertex_input_info.pVertexAttributeDescriptions = vertex_attributes.data();

        VkPipelineInputAssemblyStateCreateInfo input_assembly_info{};
        input_assembly_info.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly_info.topology = to_vk_primitive_topology(desc.graphics.topology);
        input_assembly_info.primitiveRestartEnable = desc.graphics.primitive_restart_enable ? VK_TRUE : VK_FALSE;

        VkPipelineViewportStateCreateInfo viewport_info{};
        viewport_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_info.viewportCount = 1;
        viewport_info.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterization_info{};
        rasterization_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization_info.depthClampEnable = desc.graphics.depth_clamp_enable ? VK_TRUE : VK_FALSE;
        rasterization_info.rasterizerDiscardEnable = desc.graphics.rasterizer_discard_enable ? VK_TRUE : VK_FALSE;
        rasterization_info.polygonMode = to_vk_polygon_mode(desc.graphics.polygon_mode);
        rasterization_info.cullMode = to_vk_cull_mode(desc.graphics.cull_mode);
        rasterization_info.frontFace = to_vk_front_face(desc.graphics.front_face);
        rasterization_info.depthBiasEnable = desc.graphics.depth_bias_enable ? VK_TRUE : VK_FALSE;
        rasterization_info.depthBiasConstantFactor = desc.graphics.depth_bias_constant_factor;
        rasterization_info.depthBiasClamp = desc.graphics.depth_bias_clamp;
        rasterization_info.depthBiasSlopeFactor = desc.graphics.depth_bias_slope_factor;
        rasterization_info.lineWidth = desc.graphics.line_width;

        VkPipelineMultisampleStateCreateInfo multisample_info{};
        multisample_info.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample_info.rasterizationSamples = to_vk_sample_count(desc.graphics.samples);
        multisample_info.alphaToCoverageEnable = desc.graphics.alpha_to_coverage_enable ? VK_TRUE : VK_FALSE;

        VkPipelineDepthStencilStateCreateInfo depth_stencil_info{};
        depth_stencil_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil_info.depthTestEnable = desc.graphics.depth_test_enable ? VK_TRUE : VK_FALSE;
        depth_stencil_info.depthWriteEnable = desc.graphics.depth_write_enable ? VK_TRUE : VK_FALSE;
        depth_stencil_info.depthCompareOp = to_vk_compare_op(desc.graphics.depth_compare_operation);

        VkPipelineColorBlendStateCreateInfo color_blend_info{};
        color_blend_info.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blend_info.attachmentCount = static_cast<uint32_t>(blend_attachments.size());
        color_blend_info.pAttachments = blend_attachments.data();

        const std::array<VkDynamicState, 2> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic_state_info{};
        dynamic_state_info.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state_info.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
        dynamic_state_info.pDynamicStates = dynamic_states.data();

        VkPipelineRenderingCreateInfo rendering_info{};
        rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering_info.colorAttachmentCount = static_cast<uint32_t>(color_formats.size());
        rendering_info.pColorAttachmentFormats = color_formats.data();
        rendering_info.depthAttachmentFormat = depth_format;
        rendering_info.stencilAttachmentFormat = has_stencil ? depth_format : VK_FORMAT_UNDEFINED;

        VkGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.pNext = &rendering_info;
        pipeline_info.stageCount = static_cast<uint32_t>(shader_stages.size());
        pipeline_info.pStages = shader_stages.data();
        pipeline_info.pVertexInputState = &vertex_input_info;
        pipeline_info.pInputAssemblyState = &input_assembly_info;
        pipeline_info.pViewportState = &viewport_info;
        pipeline_info.pRasterizationState = &rasterization_info;
        pipeline_info.pMultisampleState = &multisample_info;
        pipeline_info.pDepthStencilState = &depth_stencil_info;
        pipeline_info.pColorBlendState = &color_blend_info;
        pipeline_info.pDynamicState = &dynamic_state_info;
        pipeline_info.layout = pipeline_layout;

        const VkResult result =
            vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &resource.pipeline);
        for (const VkShaderModule shader_module : shader_modules) {
            vkDestroyShaderModule(device_, shader_module, nullptr);
        }
        shader_modules.clear();
        if (result != VK_SUCCESS) {
            return fail(make_vulkan_result(result, "vkCreateGraphicsPipelines"));
        }
    }

#if AEG_VULKAN_ENABLE_DEBUG_NAMES
    set_debug_name(VK_OBJECT_TYPE_PIPELINE, resource.pipeline, desc.name);
    set_debug_name(VK_OBJECT_TYPE_PIPELINE_LAYOUT, resource.layout, desc.name);
#endif
    return {Result::success(), pipelines_.insert(std::move(resource))};
}

Result VulkanDevice::destroy_pipeline(PipelineHandle pipeline) {
    PipelineResource* resource = pipelines_.get(pipeline);
    if (resource == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid pipeline handle");
    }
    if (pipeline_has_unsubmitted_command_reference(pipeline)) {
        return Result::failure(ResultCode::kInvalidState,
                               "Pipeline is referenced by a command list that has not been submitted");
    }

    RetirePipeline retire{resource->last_use, resource->pipeline, resource->layout};
    (void)pipelines_.erase(pipeline);
    enqueue_retire(std::move(retire));
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
    resource.state.usage = desc.initial_usage;
    resource.state.queue = desc.initial_queue;
    resource.state.pipeline_stages = desc.initial_stages;
    resource.subresource_states.assign(static_cast<size_t>(desc.mip_levels) * desc.array_layers * kAspectPlaneCount,
                                       resource.state);
#if AEG_VULKAN_ENABLE_DEBUG_NAMES
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

    if (texture_has_unsubmitted_command_reference(texture)) {
        return Result::failure(ResultCode::kInvalidState,
                               "Texture is referenced by a command list that has not been submitted");
    }

    RetireTexture retire{resource->last_use, resource->image, resource->allocation.handle};
    (void)textures_.erase(texture);
    enqueue_retire(std::move(retire));
    return Result::success();
}

ResultValue<TextureStateSnapshot> VulkanDevice::query_texture_state(TextureHandle texture) const {
    const TextureResource* resource = textures_.get(texture);
    if (resource == nullptr) {
        return {Result::failure(ResultCode::kNotFound, "Invalid texture handle"), {}};
    }
    const size_t expected_state_count =
        static_cast<size_t>(resource->desc.mip_levels) * resource->desc.array_layers * kAspectPlaneCount;
    if (resource->subresource_states.size() != expected_state_count || expected_state_count == 0) {
        return {Result::failure(ResultCode::kInvalidState, "Texture state does not cover all subresources"), {}};
    }

    TextureStateSnapshot snapshot{};
    try {
        snapshot.subresource_states = resource->subresource_states;
        for (uint32_t index = 0; index < kQueueCount; ++index) {
            const uint64_t value = resource->last_use[index];
            if (value == 0) {
                continue;
            }
            const TimelineHandle timeline = queues_[index].timeline_handle;
            if (!timeline.is_valid() || !snapshot.last_use.add(GpuSyncPoint{timeline, value})) {
                return {Result::failure(ResultCode::kInvalidState, "Texture state completion timeline is unavailable"),
                        {}};
            }
        }
    } catch (const std::bad_alloc&) {
        return {Result::failure(ResultCode::kOutOfMemory, "Texture state snapshot allocation failed"), {}};
    }
    return {Result::success(), std::move(snapshot)};
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
            if (!has_flag(buffer->capabilities, BufferCapability::kTypedView) || desc.format == Format::kUnknown) {
                return {Result::failure(ResultCode::kInvalidArgument,
                                        "Typed buffer view requires typed-view capability and a format"),
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

#if AEG_VULKAN_ENABLE_DEBUG_NAMES
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
#if AEG_VULKAN_ENABLE_DEBUG_NAMES
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
#if AEG_VULKAN_ENABLE_DEBUG_NAMES
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
    if (sampler_has_unsubmitted_command_reference(sampler)) {
        return Result::failure(ResultCode::kInvalidState,
                               "Sampler is referenced by a command list that has not been submitted");
    }

    RetireSampler retire{resource->last_use, resource->sampler};
    (void)samplers_.erase(sampler);
    enqueue_retire(std::move(retire));
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
#if AEG_VULKAN_ENABLE_DEBUG_NAMES
    set_debug_name(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, resource.layout, desc.name);
#endif
    return {Result::success(), binding_layouts_.insert(std::move(resource))};
}

ResultValue<DescriptorPoolRequirements> VulkanDevice::descriptor_pool_requirements(
    const BindingLayoutResource& layout) const {
    DescriptorPoolRequirements requirements{};
    for (const BindingLayoutEntry& entry : layout.entries) {
        const auto descriptor_type = to_vk_descriptor_type(entry.type);
        if (!descriptor_type.has_value()) {
            return {Result::failure(ResultCode::kUnsupported, "Binding layout contains an unsupported descriptor type"),
                    {}};
        }
        const auto type_index = descriptor_type_index(descriptor_type.value());
        if (!type_index.has_value()) {
            return {Result::failure(ResultCode::kUnsupported, "Binding layout contains an unsupported descriptor type"),
                    {}};
        }

        const uint64_t count = entry.array_mode == BindingArrayMode::kSingle ? 1u : entry.array_count;
        const uint64_t current = requirements.descriptor_counts[type_index.value()];
        if (count > std::numeric_limits<uint32_t>::max() - current) {
            return {Result::failure(ResultCode::kInvalidArgument,
                                    "Binding layout descriptor count overflows the pool requirement"),
                    {}};
        }
        if (count > std::numeric_limits<uint32_t>::max() - requirements.total_descriptor_count) {
            return {Result::failure(ResultCode::kInvalidArgument,
                                    "Binding layout total descriptor count overflows the pool requirement"),
                    {}};
        }
        requirements.descriptor_counts[type_index.value()] = static_cast<uint32_t>(current + count);
        requirements.descriptor_mask |= 1u << type_index.value();
        requirements.total_descriptor_count += static_cast<uint32_t>(count);
    }
    return {Result::success(), requirements};
}

ResultValue<uint32_t> VulkanDevice::create_descriptor_pool_page_locked(BindingSetLifetime lifetime,
                                                                       const DescriptorPoolRequirements& requirements) {
    uint32_t max_sets = kDescriptorPageInitialSets;
    {
        std::shared_lock<std::shared_mutex> pages_lock(descriptor_pool_pages_mutex_);
        for (auto iterator = descriptor_pool_pages_.rbegin(); iterator != descriptor_pool_pages_.rend(); ++iterator) {
            if (iterator->lifetime != lifetime) {
                continue;
            }
            max_sets = iterator->max_sets > kDescriptorPageMaxSets / 2
                           ? kDescriptorPageMaxSets
                           : std::min(kDescriptorPageMaxSets, iterator->max_sets * 2);
            break;
        }
    }
    max_sets = std::max(max_sets, requirements.set_count);

    std::vector<VkDescriptorPoolSize> pool_sizes;
    try {
        pool_sizes.reserve(kDescriptorTypeCount);
    } catch (const std::bad_alloc&) {
        return {Result::failure(ResultCode::kOutOfMemory, "Descriptor pool page allocation failed"), {}};
    }

    DescriptorPoolPage page{};
    page.lifetime = lifetime;
    page.max_sets = max_sets;
    for (uint32_t index = 0; index < kDescriptorTypeCount; ++index) {
        if (requirements.descriptor_counts[index] == 0) {
            continue;
        }
        const uint64_t descriptor_capacity =
            std::max(static_cast<uint64_t>(kDescriptorPageMinimumDescriptors),
                     static_cast<uint64_t>(requirements.descriptor_counts[index]) * max_sets);
        if (descriptor_capacity > std::numeric_limits<uint32_t>::max()) {
            return {
                Result::failure(ResultCode::kInvalidArgument, "Descriptor pool page capacity exceeds the Vulkan limit"),
                {}};
        }
        page.descriptor_capacity[index] = static_cast<uint32_t>(descriptor_capacity);
        if (descriptor_capacity != 0) {
            pool_sizes.push_back(
                VkDescriptorPoolSize{descriptor_type_from_index(index), static_cast<uint32_t>(descriptor_capacity)});
        }
    }
    refresh_descriptor_pool_page_state(page);

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags =
        lifetime == BindingSetLifetime::kPersistent ? VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT : 0;
    pool_info.maxSets = page.max_sets;
    pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
    pool_info.pPoolSizes = pool_sizes.data();

    const VkResult result = vkCreateDescriptorPool(device_, &pool_info, nullptr, &page.pool);
    if (result != VK_SUCCESS) {
        return {make_vulkan_result(result, "vkCreateDescriptorPool"), {}};
    }

    std::unique_lock<std::shared_mutex> pages_lock(descriptor_pool_pages_mutex_);
    const size_t page_count_before = descriptor_pool_pages_.size();
    try {
        descriptor_pool_pages_.push_back(std::move(page));
        const uint32_t page_index = static_cast<uint32_t>(descriptor_pool_pages_.size() - 1);
        const uint32_t lifetime_index = descriptor_lifetime_index(lifetime);
        auto& free_list = descriptor_pool_free_lists_[lifetime_index];
        free_list.push_back(page_index);
        descriptor_pool_pages_[page_index].list_position = static_cast<uint32_t>(free_list.size() - 1);
        update_descriptor_pool_page_free_list_locked(page_index);
        return {Result::success(), page_index};
    } catch (const std::bad_alloc&) {
        if (descriptor_pool_pages_.size() > page_count_before) {
            DescriptorPoolPage& stored_page = descriptor_pool_pages_.back();
            if (stored_page.pool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(device_, stored_page.pool, nullptr);
            }
            descriptor_pool_pages_.pop_back();
        } else if (page.pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, page.pool, nullptr);
        }
        return {Result::failure(ResultCode::kOutOfMemory, "Descriptor pool page bookkeeping failed"), {}};
    }
}

ResultValue<VulkanDevice::DescriptorSetAllocation> VulkanDevice::allocate_descriptor_set(
    BindingSetLifetime lifetime, const DescriptorPoolRequirements& requirements, VkDescriptorSetLayout layout) {
    const uint32_t lifetime_index = descriptor_lifetime_index(lifetime);
    std::unique_lock<std::mutex> lifetime_lock(descriptor_pool_lifetime_mutexes_[lifetime_index]);
    auto& free_list = descriptor_pool_free_lists_[lifetime_index];

    const auto allocate_from_page = [&](uint32_t page_index) -> ResultValue<DescriptorSetAllocation> {
        DescriptorPoolPage* page = nullptr;
        {
            std::shared_lock<std::shared_mutex> pages_lock(descriptor_pool_pages_mutex_);
            if (page_index >= descriptor_pool_pages_.size()) {
                return {Result::failure(ResultCode::kInvalidState, "Descriptor pool page index is invalid"), {}};
            }
            page = &descriptor_pool_pages_[page_index];
            if (page->pool == VK_NULL_HANDLE || page->lifetime != lifetime) {
                return {Result::failure(ResultCode::kInvalidState, "Descriptor pool page is unavailable"), {}};
            }
        }

        VkDescriptorSetAllocateInfo allocate_info{};
        allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate_info.descriptorPool = page->pool;
        allocate_info.descriptorSetCount = requirements.set_count;
        allocate_info.pSetLayouts = &layout;

        VkDescriptorSet set{VK_NULL_HANDLE};
        const VkResult result = vkAllocateDescriptorSets(device_, &allocate_info, &set);
        if (result == VK_SUCCESS) {
            page->allocated_sets += requirements.set_count;
            page->live_sets += requirements.set_count;
            uint32_t descriptor_mask = requirements.descriptor_mask;
            while (descriptor_mask != 0) {
                const uint32_t index = std::countr_zero(descriptor_mask);
                page->descriptor_usage[index] += requirements.descriptor_counts[index];
                descriptor_mask &= descriptor_mask - 1u;
            }
            record_descriptor_pool_allocation_success(*page);
            {
                std::shared_lock<std::shared_mutex> pages_lock(descriptor_pool_pages_mutex_);
                update_descriptor_pool_page_free_list_locked(page_index);
            }
            return {Result::success(), DescriptorSetAllocation{page_index, set}};
        }
        if (result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL) {
            record_descriptor_pool_allocation_failure(*page, result);
            {
                std::shared_lock<std::shared_mutex> pages_lock(descriptor_pool_pages_mutex_);
                update_descriptor_pool_page_free_list_locked(page_index);
            }
            return {Result::failure(ResultCode::kNotReady, "Descriptor pool page has no available capacity"), {}};
        }
        return {make_vulkan_result(result, "vkAllocateDescriptorSets"), {}};
    };

    while (true) {
        uint32_t best_page_index = kInvalidIndex;
        uint64_t best_score = std::numeric_limits<uint64_t>::max();
        bool free_list_is_valid = true;
        {
            std::shared_lock<std::shared_mutex> pages_lock(descriptor_pool_pages_mutex_);
            const uint32_t free_count = descriptor_pool_free_counts_[lifetime_index];
            const uint32_t preferred_count = descriptor_pool_preferred_counts_[lifetime_index];
            if (free_count > free_list.size() || free_count > descriptor_pool_pages_.size() ||
                preferred_count > free_count) {
                free_list_is_valid = false;
            } else {
                const auto find_best_page = [&](uint32_t begin, uint32_t end) {
                    for (uint32_t position = begin; position < end; ++position) {
                        const uint32_t candidate_index = free_list[position];
                        if (candidate_index >= descriptor_pool_pages_.size()) {
                            free_list_is_valid = false;
                            continue;
                        }
                        const DescriptorPoolPage& page = descriptor_pool_pages_[candidate_index];
                        const bool is_preferred = position < preferred_count;
                        const bool state_matches = is_preferred
                                                       ? page.list_state == DescriptorPoolPageListState::kPreferred
                                                       : page.list_state == DescriptorPoolPageListState::kDeferred;
                        if (page.lifetime != lifetime || !state_matches ||
                            !descriptor_pool_page_has_free_capacity(page)) {
                            free_list_is_valid = false;
                            continue;
                        }
                        if (!descriptor_pool_can_fit(page, requirements)) {
                            continue;
                        }

                        const uint64_t score = descriptor_pool_fit_score(page, requirements);
                        if (score < best_score) {
                            best_score = score;
                            best_page_index = candidate_index;
                        }
                    }
                };

                find_best_page(0, preferred_count);
                if (best_page_index == kInvalidIndex) {
                    find_best_page(preferred_count, free_count);
                }
            }
        }

        if (!free_list_is_valid) {
            return {Result::failure(ResultCode::kInvalidState, "Descriptor pool free-list invariant is broken"), {}};
        }
        if (best_page_index == kInvalidIndex) {
            break;
        }

        auto allocation = allocate_from_page(best_page_index);
        if (allocation || allocation.result.code != ResultCode::kNotReady) {
            return allocation;
        }
    }

    auto page_result = create_descriptor_pool_page_locked(lifetime, requirements);
    if (!page_result) {
        return {page_result.result, {}};
    }
    const uint32_t page_index = page_result.value;
    auto allocation = allocate_from_page(page_index);
    if (!allocation && allocation.result.code == ResultCode::kNotReady) {
        allocation.result =
            Result::failure(ResultCode::kOutOfMemory, "New descriptor pool page cannot allocate the requested set");
    }
    return allocation;
}

ResultValue<BindingSetHandle> VulkanDevice::create_binding_set(const BindingSetDesc& desc) {
    if (device_ == VK_NULL_HANDLE) {
        return {Result::failure(ResultCode::kInvalidState, "Vulkan device is not initialized"), {}};
    }
    if (desc.lifetime != BindingSetLifetime::kPersistent && desc.lifetime != BindingSetLifetime::kFrameLocal) {
        return {Result::failure(ResultCode::kInvalidArgument, "Binding set lifetime is invalid"), {}};
    }

    BindingLayoutResource* layout_resource = binding_layouts_.get(desc.layout);
    if (layout_resource == nullptr) {
        return {Result::failure(ResultCode::kNotFound, "Invalid binding layout handle"), {}};
    }

    auto requirements_result = descriptor_pool_requirements(*layout_resource);
    if (!requirements_result) {
        return {requirements_result.result, {}};
    }
    const DescriptorPoolRequirements requirements = requirements_result.value;
    auto allocation_result = allocate_descriptor_set(desc.lifetime, requirements, layout_resource->layout);
    if (!allocation_result) {
        return {allocation_result.result, {}};
    }

    BindingSetResource resource{};
    resource.set = allocation_result.value.set;
    resource.layout = desc.layout;
    resource.lifetime = desc.lifetime;
    resource.descriptor_pool_page = allocation_result.value.page_index;
    resource.requirements = requirements;
    try {
#if AEG_VULKAN_ENABLE_DEBUG_NAMES
        set_debug_name(VK_OBJECT_TYPE_DESCRIPTOR_SET, resource.set, desc.name);
#endif
        return {Result::success(), binding_sets_.insert(std::move(resource))};
    } catch (const std::bad_alloc&) {
        release_descriptor_set(desc.lifetime, allocation_result.value.page_index, allocation_result.value.set,
                               requirements);
        return {Result::failure(ResultCode::kOutOfMemory, "Binding set bookkeeping allocation failed"), {}};
    }
}

// The caller holds the page lifetime mutex and the page storage lock.
void VulkanDevice::update_descriptor_pool_page_free_list_locked(uint32_t page_index) noexcept {
    if (page_index >= descriptor_pool_pages_.size()) {
        return;
    }

    DescriptorPoolPage& page = descriptor_pool_pages_[page_index];
    const uint32_t lifetime_index = descriptor_lifetime_index(page.lifetime);
    auto& free_list = descriptor_pool_free_lists_[lifetime_index];
    uint32_t& free_count = descriptor_pool_free_counts_[lifetime_index];
    uint32_t& preferred_count = descriptor_pool_preferred_counts_[lifetime_index];

    if (free_count > free_list.size() || free_count > descriptor_pool_pages_.size() || preferred_count > free_count) {
        return;
    }
    if (page.list_position >= free_list.size() || free_list[page.list_position] != page_index) {
        const auto iterator = std::find(free_list.begin(), free_list.end(), page_index);
        if (iterator == free_list.end()) {
            return;
        }
        page.list_position = static_cast<uint32_t>(std::distance(free_list.begin(), iterator));
    }

    const DescriptorPoolPageListState target_state =
        descriptor_pool_page_is_preferred(page)
            ? DescriptorPoolPageListState::kPreferred
            : (descriptor_pool_page_has_free_capacity(page) ? DescriptorPoolPageListState::kDeferred
                                                            : DescriptorPoolPageListState::kFull);
    if (page.list_state == target_state) {
        return;
    }

    const uint32_t current_position = page.list_position;
    if (current_position >= free_list.size()) {
        return;
    }

    const auto can_swap_positions = [&](uint32_t lhs, uint32_t rhs) noexcept {
        if (lhs >= free_list.size() || rhs >= free_list.size()) {
            return false;
        }
        return free_list[lhs] < descriptor_pool_pages_.size() && free_list[rhs] < descriptor_pool_pages_.size();
    };

    const auto swap_positions = [&](uint32_t lhs, uint32_t rhs) noexcept {
        if (!can_swap_positions(lhs, rhs)) {
            return false;
        }
        if (lhs == rhs) {
            return true;
        }
        const uint32_t lhs_page_index = free_list[lhs];
        const uint32_t rhs_page_index = free_list[rhs];
        std::swap(free_list[lhs], free_list[rhs]);
        descriptor_pool_pages_[lhs_page_index].list_position = rhs;
        descriptor_pool_pages_[rhs_page_index].list_position = lhs;
        return true;
    };

    switch (page.list_state) {
        case DescriptorPoolPageListState::kPreferred: {
            if (current_position >= preferred_count || preferred_count == 0) {
                return;
            }
            if (target_state == DescriptorPoolPageListState::kDeferred) {
                const uint32_t new_preferred_count = preferred_count - 1;
                if (!swap_positions(current_position, new_preferred_count)) {
                    return;
                }
                preferred_count = new_preferred_count;
                page.list_state = target_state;
                return;
            }
            if (target_state != DescriptorPoolPageListState::kFull || free_count == 0) {
                return;
            }
            const uint32_t new_preferred_count = preferred_count - 1;
            const uint32_t new_free_count = free_count - 1;
            if (!can_swap_positions(current_position, new_preferred_count) ||
                !can_swap_positions(new_preferred_count, new_free_count)) {
                return;
            }
            swap_positions(current_position, new_preferred_count);
            swap_positions(new_preferred_count, new_free_count);
            preferred_count = new_preferred_count;
            free_count = new_free_count;
            page.list_state = DescriptorPoolPageListState::kFull;
            return;
        }
        case DescriptorPoolPageListState::kDeferred: {
            if (current_position < preferred_count || current_position >= free_count) {
                return;
            }
            if (target_state == DescriptorPoolPageListState::kPreferred) {
                if (!swap_positions(current_position, preferred_count)) {
                    return;
                }
                ++preferred_count;
                page.list_state = target_state;
                return;
            }
            if (target_state != DescriptorPoolPageListState::kFull || free_count == 0) {
                return;
            }
            const uint32_t new_free_count = free_count - 1;
            if (!swap_positions(current_position, new_free_count)) {
                return;
            }
            free_count = new_free_count;
            page.list_state = DescriptorPoolPageListState::kFull;
            return;
        }
        case DescriptorPoolPageListState::kFull: {
            if (current_position < free_count) {
                return;
            }
            if (target_state == DescriptorPoolPageListState::kPreferred) {
                if (free_count >= free_list.size()) {
                    return;
                }
                if (!swap_positions(current_position, preferred_count)) {
                    return;
                }
                ++preferred_count;
                ++free_count;
                page.list_state = target_state;
                return;
            }
            if (target_state == DescriptorPoolPageListState::kDeferred) {
                if (free_count >= free_list.size()) {
                    return;
                }
                if (!swap_positions(current_position, free_count)) {
                    return;
                }
                ++free_count;
                page.list_state = target_state;
                return;
            }
            return;
        }
    }
}

void VulkanDevice::release_descriptor_set(BindingSetLifetime lifetime, uint32_t page_index, VkDescriptorSet set,
                                          const DescriptorPoolRequirements& requirements) noexcept {
    const uint32_t lifetime_index = descriptor_lifetime_index(lifetime);
    std::lock_guard<std::mutex> lifetime_lock(descriptor_pool_lifetime_mutexes_[lifetime_index]);
    DescriptorPoolPage* page = nullptr;
    {
        std::shared_lock<std::shared_mutex> pages_lock(descriptor_pool_pages_mutex_);
        if (device_ == VK_NULL_HANDLE || set == VK_NULL_HANDLE || page_index >= descriptor_pool_pages_.size()) {
            return;
        }

        page = &descriptor_pool_pages_[page_index];
        if (page->pool == VK_NULL_HANDLE || page->lifetime != lifetime) {
            return;
        }
    }

    if (page->live_sets >= requirements.set_count) {
        page->live_sets -= requirements.set_count;
    } else {
        page->live_sets = 0;
    }

    if (page->lifetime == BindingSetLifetime::kFrameLocal) {
        if (page->live_sets != 0) {
            return;
        }
        if (vkResetDescriptorPool(device_, page->pool, 0) == VK_SUCCESS) {
            page->allocated_sets = 0;
            page->descriptor_usage.fill(0);
            page->fragmentation_score = 0;
            page->allocation_failures = 0;
            page->allocation_blocked = false;
            page->driver_fragmented = false;
            refresh_descriptor_pool_page_state(*page);
        } else {
            page->allocation_blocked = true;
            page->fragmentation_score = kDescriptorFragmentationScoreMax;
        }
        {
            std::shared_lock<std::shared_mutex> pages_lock(descriptor_pool_pages_mutex_);
            update_descriptor_pool_page_free_list_locked(page_index);
        }
        return;
    }

    if (vkFreeDescriptorSets(device_, page->pool, 1, &set) != VK_SUCCESS) {
        page->live_sets += requirements.set_count;
        return;
    }
    if (page->allocated_sets >= requirements.set_count) {
        page->allocated_sets -= requirements.set_count;
    } else {
        page->allocated_sets = 0;
    }
    uint32_t descriptor_mask = requirements.descriptor_mask;
    while (descriptor_mask != 0) {
        const uint32_t index = std::countr_zero(descriptor_mask);
        if (page->descriptor_usage[index] >= requirements.descriptor_counts[index]) {
            page->descriptor_usage[index] -= requirements.descriptor_counts[index];
        } else {
            page->descriptor_usage[index] = 0;
        }
        descriptor_mask &= descriptor_mask - 1u;
    }

    page->allocation_blocked = false;
    if (!page->driver_fragmented) {
        page->fragmentation_score = 0;
    }
    refresh_descriptor_pool_page_state(*page);

    if (page->allocated_sets == 0) {
        if (vkResetDescriptorPool(device_, page->pool, 0) == VK_SUCCESS) {
            page->descriptor_usage.fill(0);
            page->fragmentation_score = 0;
            page->allocation_failures = 0;
            page->allocation_blocked = false;
            page->driver_fragmented = false;
            refresh_descriptor_pool_page_state(*page);
        } else {
            page->allocation_blocked = true;
            page->fragmentation_score = kDescriptorFragmentationScoreMax;
        }
    }
    {
        std::shared_lock<std::shared_mutex> pages_lock(descriptor_pool_pages_mutex_);
        update_descriptor_pool_page_free_list_locked(page_index);
    }
}

void VulkanDevice::destroy_descriptor_pool_pages() noexcept {
    std::scoped_lock lifetime_lock(descriptor_pool_lifetime_mutexes_[0], descriptor_pool_lifetime_mutexes_[1]);
    std::unique_lock<std::shared_mutex> pages_lock(descriptor_pool_pages_mutex_);
    for (DescriptorPoolPage& page : descriptor_pool_pages_) {
        if (page.pool != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, page.pool, nullptr);
        }
        page.pool = VK_NULL_HANDLE;
        page.max_sets = 0;
        page.allocated_sets = 0;
        page.live_sets = 0;
        page.descriptor_capacity.fill(0);
        page.descriptor_usage.fill(0);
        page.descriptor_capacity_mask = 0;
        page.available_descriptor_mask = 0;
        page.fragmentation_score = 0;
        page.allocation_failures = 0;
        page.list_position = kInvalidIndex;
        page.list_state = DescriptorPoolPageListState::kFull;
        page.allocation_blocked = false;
        page.driver_fragmented = false;
    }
    for (auto& free_list : descriptor_pool_free_lists_) {
        free_list.clear();
    }
    descriptor_pool_free_counts_.fill(0);
    descriptor_pool_preferred_counts_.fill(0);
    descriptor_pool_pages_.clear();
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
            case BindingType::kReadOnlyTypedBuffer:
            case BindingType::kReadWriteTypedBuffer: {
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

    if (buffer_view_has_unsubmitted_command_reference(view)) {
        return Result::failure(ResultCode::kInvalidState,
                               "Buffer view is referenced by a command list that has not been submitted");
    }

    RetireBufferView retire{resource->last_use, resource->view};
    (void)buffer_views_.erase(view);
    enqueue_retire(std::move(retire));
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

    if (texture_view_has_unsubmitted_command_reference(view)) {
        return Result::failure(ResultCode::kInvalidState,
                               "Texture view is referenced by a command list that has not been submitted");
    }

    RetireTextureView retire{resource->last_use, resource->view};
    (void)texture_views_.erase(view);
    enqueue_retire(std::move(retire));
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
    if (binding_layout_is_referenced(layout)) {
        return Result::failure(ResultCode::kInvalidState, "Binding layout is still used by a pipeline");
    }

    RetireBindingLayout retire{resource->last_use, resource->layout};
    (void)binding_layouts_.erase(layout);
    enqueue_retire(std::move(retire));
    return Result::success();
}

Result VulkanDevice::destroy_binding_set(BindingSetHandle set) {
    BindingSetResource* resource = binding_sets_.get(set);
    if (resource == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid binding set handle");
    }

    if (binding_set_has_unsubmitted_command_reference(set)) {
        return Result::failure(ResultCode::kInvalidState,
                               "Binding set is referenced by a command list that has not been submitted");
    }

    RetireBindingSet retire{resource->last_use, resource->lifetime, resource->descriptor_pool_page, resource->set,
                            resource->requirements};
    (void)binding_sets_.erase(set);
    enqueue_retire(std::move(retire));
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
    resource.barrier_mode = desc.barrier_mode;
    const VkResult result = vkAllocateCommandBuffers(device_, &allocate_info, &resource.command_buffer);
    if (result != VK_SUCCESS) {
        return {make_vulkan_result(result, "vkAllocateCommandBuffers"), {}};
    }
#if AEG_VULKAN_ENABLE_DEBUG_NAMES
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

Result VulkanDevice::begin_rendering(CommandListHandle command_list, const RenderingDesc& desc) {
    CommandListResource* command = command_lists_.get(command_list);
    if (command == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid command list handle");
    }
    if (!command->recording || command->queue != QueueClass::kGraphics) {
        return Result::failure(ResultCode::kInvalidState, "Rendering requires a recording graphics command list");
    }
    if (command->rendering) {
        return Result::failure(ResultCode::kInvalidState, "A command list cannot begin nested rendering");
    }
    if (desc.color_attachments.empty() && !desc.depth_stencil.view.is_valid()) {
        return Result::failure(ResultCode::kInvalidArgument, "Rendering requires at least one attachment");
    }
    if (desc.color_attachments.size() > physical_properties_.limits.maxColorAttachments) {
        return Result::failure(ResultCode::kInvalidArgument, "Rendering has too many color attachments");
    }

    struct ColorAttachmentResources {
        const ColorAttachmentDesc* desc{nullptr};
        TextureViewResource* view{nullptr};
        TextureResource* texture{nullptr};
    };

    std::vector<ColorAttachmentResources> color_resources;
    color_resources.reserve(desc.color_attachments.size());
    TextureViewResource* depth_view = nullptr;
    TextureResource* depth_texture = nullptr;
    Extent3D attachment_extent{};
    SampleCount attachment_samples{SampleCount::kX1};
    bool has_attachment_extent = false;

    const auto validate_attachment_dimensions = [&](const TextureResource& texture) -> Result {
        if (texture.desc.dimension != TextureDimension::kTex2D || texture.desc.extent.depth != 1) {
            return Result::failure(ResultCode::kInvalidArgument,
                                   "Rendering attachments must be two-dimensional textures");
        }
        if (!has_attachment_extent) {
            attachment_extent = texture.desc.extent;
            attachment_samples = texture.desc.samples;
            has_attachment_extent = true;
            return Result::success();
        }
        if (texture.desc.extent.width != attachment_extent.width ||
            texture.desc.extent.height != attachment_extent.height ||
            texture.desc.extent.depth != attachment_extent.depth || texture.desc.samples != attachment_samples) {
            return Result::failure(ResultCode::kInvalidArgument,
                                   "Rendering attachments must have matching extent and sample count");
        }
        return Result::success();
    };

    for (const ColorAttachmentDesc& attachment : desc.color_attachments) {
        TextureViewResource* view = texture_views_.get(attachment.view);
        if (view == nullptr) {
            return Result::failure(ResultCode::kNotFound, "Rendering contains an invalid color attachment view");
        }
        TextureResource* texture = textures_.get(view->desc.texture);
        if (texture == nullptr) {
            return Result::failure(ResultCode::kInvalidState, "Color attachment texture no longer exists");
        }
        if (view->desc.kind != TextureViewKind::kColorAttachment || is_depth_format(view->desc.format) ||
            !has_flag(view->desc.subresources.aspects, Aspect::kColor)) {
            return Result::failure(ResultCode::kInvalidArgument, "View is not a color attachment view");
        }
        if (!to_vk_image_usage_state(attachment.final_usage).has_value()) {
            return Result::failure(ResultCode::kUnsupported, "Color attachment final usage is not supported");
        }
        const Result dimensions_result = validate_attachment_dimensions(*texture);
        if (!dimensions_result) {
            return dimensions_result;
        }
        if (std::any_of(color_resources.begin(), color_resources.end(),
                        [&attachment](const ColorAttachmentResources& existing) {
                            return existing.desc->view == attachment.view;
                        })) {
            return Result::failure(ResultCode::kInvalidArgument, "Rendering contains a duplicate color attachment");
        }
        color_resources.push_back(ColorAttachmentResources{&attachment, view, texture});
    }

    if (desc.depth_stencil.view.is_valid()) {
        depth_view = texture_views_.get(desc.depth_stencil.view);
        if (depth_view == nullptr) {
            return Result::failure(ResultCode::kNotFound, "Rendering contains an invalid depth attachment view");
        }
        depth_texture = textures_.get(depth_view->desc.texture);
        if (depth_texture == nullptr) {
            return Result::failure(ResultCode::kInvalidState, "Depth attachment texture no longer exists");
        }
        if (depth_view->desc.kind != TextureViewKind::kDepthStencil || !is_depth_format(depth_view->desc.format) ||
            has_flag(depth_view->desc.subresources.aspects, Aspect::kColor)) {
            return Result::failure(ResultCode::kInvalidArgument, "View is not a depth-stencil attachment view");
        }
        if (!to_vk_image_usage_state(desc.depth_stencil.final_usage).has_value()) {
            return Result::failure(ResultCode::kUnsupported, "Depth attachment final usage is not supported");
        }
        const Result dimensions_result = validate_attachment_dimensions(*depth_texture);
        if (!dimensions_result) {
            return dimensions_result;
        }
    }

    for (const ColorAttachmentResources& resources : color_resources) {
        const Result transition_result =
            transition_texture(*command, *resources.view, ResourceUsage::kColorAttachmentWrite);
        if (!transition_result) {
            return transition_result;
        }
    }
    if (depth_texture != nullptr) {
        const Result transition_result = transition_texture(*command, *depth_view, ResourceUsage::kDepthStencilWrite);
        if (!transition_result) {
            return transition_result;
        }
    }

    std::vector<VkRenderingAttachmentInfo> color_attachments;
    color_attachments.reserve(color_resources.size());
    for (const ColorAttachmentResources& resources : color_resources) {
        VkRenderingAttachmentInfo attachment{};
        attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        attachment.imageView = resources.view->view;
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = to_vk_load_op(resources.desc->load_op);
        attachment.storeOp = to_vk_store_op(resources.desc->store_op);
        attachment.clearValue.color.float32[0] = resources.desc->clear_color.r;
        attachment.clearValue.color.float32[1] = resources.desc->clear_color.g;
        attachment.clearValue.color.float32[2] = resources.desc->clear_color.b;
        attachment.clearValue.color.float32[3] = resources.desc->clear_color.a;
        color_attachments.push_back(attachment);
    }

    VkRenderingAttachmentInfo depth_attachment{};
    const bool has_stencil_attachment =
        depth_texture != nullptr && has_flag(depth_view->desc.subresources.aspects, Aspect::kStencil);
    if (depth_texture != nullptr) {
        depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth_attachment.imageView = depth_view->view;
        depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth_attachment.loadOp = to_vk_load_op(desc.depth_stencil.depth_load_op);
        depth_attachment.storeOp = to_vk_store_op(desc.depth_stencil.depth_store_op);
        depth_attachment.clearValue.depthStencil.depth = desc.depth_stencil.clear_depth;
        depth_attachment.clearValue.depthStencil.stencil = desc.depth_stencil.clear_stencil;
    }

    VkRenderingInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering_info.renderArea = VkRect2D{{0, 0}, {attachment_extent.width, attachment_extent.height}};
    rendering_info.layerCount = 1;
    rendering_info.colorAttachmentCount = static_cast<uint32_t>(color_attachments.size());
    rendering_info.pColorAttachments = color_attachments.data();
    rendering_info.pDepthAttachment = depth_texture != nullptr ? &depth_attachment : nullptr;
    rendering_info.pStencilAttachment = has_stencil_attachment ? &depth_attachment : nullptr;

    vkCmdBeginRendering(command->command_buffer, &rendering_info);
    command->rendering = true;
    command->active_color_attachments.assign(desc.color_attachments.begin(), desc.color_attachments.end());
    command->active_depth_stencil = desc.depth_stencil;

    for (const ColorAttachmentResources& resources : color_resources) {
        if (std::find(command->referenced_texture_views.begin(), command->referenced_texture_views.end(),
                      resources.desc->view) == command->referenced_texture_views.end()) {
            command->referenced_texture_views.push_back(resources.desc->view);
        }
        if (std::find(command->referenced_textures.begin(), command->referenced_textures.end(),
                      resources.view->desc.texture) == command->referenced_textures.end()) {
            command->referenced_textures.push_back(resources.view->desc.texture);
        }
    }
    if (desc.depth_stencil.view.is_valid()) {
        if (std::find(command->referenced_texture_views.begin(), command->referenced_texture_views.end(),
                      desc.depth_stencil.view) == command->referenced_texture_views.end()) {
            command->referenced_texture_views.push_back(desc.depth_stencil.view);
        }
        if (std::find(command->referenced_textures.begin(), command->referenced_textures.end(),
                      depth_view->desc.texture) == command->referenced_textures.end()) {
            command->referenced_textures.push_back(depth_view->desc.texture);
        }
    }
    return Result::success();
}

Result VulkanDevice::end_rendering(CommandListHandle command_list) {
    CommandListResource* command = command_lists_.get(command_list);
    if (command == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid command list handle");
    }
    if (!command->recording || !command->rendering) {
        return Result::failure(ResultCode::kInvalidState, "Command list is not inside a rendering scope");
    }

    vkCmdEndRendering(command->command_buffer);
    command->rendering = false;

    if (command->barrier_mode == BarrierMode::kExplicit) {
        command->active_color_attachments.clear();
        command->active_depth_stencil = {};
        return Result::success();
    }

    Result transition_result = Result::success();
    for (const ColorAttachmentDesc& attachment : command->active_color_attachments) {
        TextureViewResource* view = texture_views_.get(attachment.view);
        TextureResource* texture = view == nullptr ? nullptr : textures_.get(view->desc.texture);
        if (view == nullptr || texture == nullptr) {
            transition_result =
                Result::failure(ResultCode::kInvalidState, "Color attachment was destroyed during rendering");
            break;
        }
        transition_result = transition_texture(*command, *view, attachment.final_usage);
        if (!transition_result) {
            break;
        }
    }
    if (transition_result && command->active_depth_stencil.view.is_valid()) {
        TextureViewResource* view = texture_views_.get(command->active_depth_stencil.view);
        TextureResource* texture = view == nullptr ? nullptr : textures_.get(view->desc.texture);
        if (view == nullptr || texture == nullptr) {
            transition_result =
                Result::failure(ResultCode::kInvalidState, "Depth attachment was destroyed during rendering");
        } else {
            transition_result = transition_texture(*command, *view, command->active_depth_stencil.final_usage);
        }
    }

    command->active_color_attachments.clear();
    command->active_depth_stencil = {};
    return transition_result;
}

Result VulkanDevice::barrier(CommandListHandle command_list, const BarrierDesc& desc) {
    CommandListResource* command = command_lists_.get(command_list);
    if (command == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid command list handle");
    }
    return record_barriers(*command, desc.buffers, desc.textures);
}

Result VulkanDevice::set_viewport(CommandListHandle command_list, const Viewport& viewport) {
    CommandListResource* command = command_lists_.get(command_list);
    if (command == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid command list handle");
    }
    if (!command->recording || !command->rendering) {
        return Result::failure(ResultCode::kInvalidState, "Viewport can only be set inside rendering");
    }
    if (viewport.width == 0.0f || viewport.height == 0.0f || viewport.min_depth > viewport.max_depth) {
        return Result::failure(ResultCode::kInvalidArgument, "Viewport dimensions or depth range are invalid");
    }

    const VkViewport vk_viewport{viewport.x,      viewport.y,         viewport.width,
                                 viewport.height, viewport.min_depth, viewport.max_depth};
    vkCmdSetViewport(command->command_buffer, 0, 1, &vk_viewport);
    return Result::success();
}

Result VulkanDevice::set_scissor(CommandListHandle command_list, const ScissorRect& scissor) {
    CommandListResource* command = command_lists_.get(command_list);
    if (command == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid command list handle");
    }
    if (!command->recording || !command->rendering) {
        return Result::failure(ResultCode::kInvalidState, "Scissor can only be set inside rendering");
    }
    if (scissor.width == 0 || scissor.height == 0) {
        return Result::failure(ResultCode::kInvalidArgument, "Scissor dimensions must be non-zero");
    }

    const VkRect2D vk_scissor{{scissor.x, scissor.y}, {scissor.width, scissor.height}};
    vkCmdSetScissor(command->command_buffer, 0, 1, &vk_scissor);
    return Result::success();
}

Result VulkanDevice::bind_pipeline(CommandListHandle command_list, PipelineHandle pipeline) {
    CommandListResource* command_resource = command_lists_.get(command_list);
    if (command_resource == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid command list handle");
    }
    PipelineResource* pipeline_resource = pipelines_.get(pipeline);
    if (pipeline_resource == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid pipeline handle");
    }
    if (!command_resource->recording) {
        return Result::failure(ResultCode::kInvalidState, "Pipeline can only be bound while recording a command list");
    }

    if (pipeline_resource->type == PipelineType::kGraphics && command_resource->queue != QueueClass::kGraphics) {
        return Result::failure(ResultCode::kInvalidArgument,
                               "Graphics pipelines can only be bound on a graphics command list");
    }
    if (pipeline_resource->type == PipelineType::kCompute && command_resource->queue != QueueClass::kGraphics &&
        command_resource->queue != QueueClass::kCompute) {
        return Result::failure(ResultCode::kInvalidArgument,
                               "Compute pipelines can only be bound on graphics or compute command lists");
    }

    const VkPipelineBindPoint bind_point = pipeline_resource->type == PipelineType::kCompute
                                               ? VK_PIPELINE_BIND_POINT_COMPUTE
                                               : VK_PIPELINE_BIND_POINT_GRAPHICS;
    vkCmdBindPipeline(command_resource->command_buffer, bind_point, pipeline_resource->pipeline);
    command_resource->bound_pipeline = pipeline;
    if (std::find(command_resource->referenced_pipelines.begin(), command_resource->referenced_pipelines.end(),
                  pipeline) == command_resource->referenced_pipelines.end()) {
        command_resource->referenced_pipelines.push_back(pipeline);
    }
    return Result::success();
}

Result VulkanDevice::bind_binding_set(CommandListHandle command_list, uint32_t group, BindingSetHandle set,
                                      std::span<const uint32_t> dynamic_offsets) {
    CommandListResource* command = command_lists_.get(command_list);
    if (command == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid command list handle");
    }
    const Result recording_result = validate_command_recording(*command);
    if (!recording_result) {
        return recording_result;
    }
    if (!command->bound_pipeline.is_valid()) {
        return Result::failure(ResultCode::kInvalidState, "Binding sets require a bound pipeline");
    }
    if (!dynamic_offsets.empty()) {
        return Result::failure(ResultCode::kUnsupported,
                               "Dynamic descriptor offsets are not implemented by this RHI device");
    }

    const PipelineResource* pipeline = pipelines_.get(command->bound_pipeline);
    const BindingSetResource* set_resource = binding_sets_.get(set);
    if (pipeline == nullptr || set_resource == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid pipeline or binding set handle");
    }
    const BindingLayoutResource* layout = binding_layouts_.get(set_resource->layout);
    if (layout == nullptr) {
        return Result::failure(ResultCode::kInvalidState, "Binding set layout no longer exists");
    }
    if (group >= pipeline->binding_layouts.size() || pipeline->binding_layouts[group] != set_resource->layout ||
        layout->group != group) {
        return Result::failure(ResultCode::kInvalidArgument,
                               "Binding set group does not match the bound pipeline layout");
    }

    for (const BindingWrite& write : set_resource->writes) {
        switch (write.resource.type) {
            case BindingType::kSampler:
                add_unique(command->referenced_samplers, write.resource.sampler);
                break;
            case BindingType::kSampledTexture:
            case BindingType::kStorageTexture: {
                const TextureViewResource* view = texture_views_.get(write.resource.texture_view);
                if (view == nullptr) {
                    return Result::failure(ResultCode::kNotFound, "Binding set contains an invalid texture view");
                }
                ResourceState state{};
                state.usage = write.resource.type == BindingType::kSampledTexture ? ResourceUsage::kSampledRead
                                                                                  : ResourceUsage::kStorageReadWrite;
                state.queue = command->queue;
                state.shader_stages = ShaderStage::kAll;
                state.pipeline_stages = PipelineStage::kAllCommands;
                const Result transition_result =
                    transition_texture(*command, view->desc.texture, view->desc.subresources, state);
                if (!transition_result) {
                    return transition_result;
                }
                add_unique(command->referenced_texture_views, write.resource.texture_view);
                add_unique(command->referenced_textures, view->desc.texture);
                break;
            }
            case BindingType::kUniformBuffer:
            case BindingType::kStorageBuffer: {
                BufferHandle buffer_handle = write.resource.buffer_range.buffer;
                uint64_t offset = write.resource.buffer_range.offset;
                uint64_t size = write.resource.buffer_range.size;
                if (!buffer_handle.is_valid()) {
                    const BufferViewResource* view = buffer_views_.get(write.resource.buffer_view);
                    if (view == nullptr) {
                        return Result::failure(ResultCode::kNotFound, "Binding set contains an invalid buffer view");
                    }
                    buffer_handle = view->desc.buffer;
                    offset = view->desc.offset;
                    size = view->desc.size;
                    add_unique(command->referenced_buffer_views, write.resource.buffer_view);
                }
                ResourceState state{};
                state.usage = write.resource.type == BindingType::kUniformBuffer ? ResourceUsage::kUniformRead
                                                                                 : ResourceUsage::kStorageReadWrite;
                state.queue = command->queue;
                state.pipeline_stages = PipelineStage::kAllCommands;
                const Result transition_result = transition_buffer(*command, buffer_handle, offset, size, state);
                if (!transition_result) {
                    return transition_result;
                }
                add_unique(command->referenced_buffers, buffer_handle);
                break;
            }
            case BindingType::kReadOnlyTypedBuffer:
            case BindingType::kReadWriteTypedBuffer: {
                const BufferViewResource* view = buffer_views_.get(write.resource.buffer_view);
                if (view == nullptr) {
                    return Result::failure(ResultCode::kNotFound, "Binding set contains an invalid buffer view");
                }
                ResourceState state{};
                state.usage = write.resource.type == BindingType::kReadOnlyTypedBuffer
                                  ? ResourceUsage::kTypedRead
                                  : ResourceUsage::kStorageReadWrite;
                state.queue = command->queue;
                state.pipeline_stages = PipelineStage::kAllCommands;
                const Result transition_result =
                    transition_buffer(*command, view->desc.buffer, view->desc.offset, view->desc.size, state);
                if (!transition_result) {
                    return transition_result;
                }
                add_unique(command->referenced_buffer_views, write.resource.buffer_view);
                add_unique(command->referenced_buffers, view->desc.buffer);
                break;
            }
            case BindingType::kAccelerationStructure:
                return Result::failure(ResultCode::kUnsupported,
                                       "Acceleration structure bindings are not implemented yet");
        }
    }

    const VkPipelineBindPoint bind_point =
        pipeline->type == PipelineType::kCompute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
    vkCmdBindDescriptorSets(command->command_buffer, bind_point, pipeline->layout, group, 1, &set_resource->set,
                            static_cast<uint32_t>(dynamic_offsets.size()), dynamic_offsets.data());
    add_unique(command->referenced_binding_sets, set);
    return Result::success();
}

Result VulkanDevice::set_inline_constants(CommandListHandle command_list, uint32_t offset,
                                          ShaderStageMask shader_stages, std::span<const std::byte> data) {
    CommandListResource* command = command_lists_.get(command_list);
    if (command == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid command list handle");
    }
    if (!command->recording) {
        return Result::failure(ResultCode::kInvalidState,
                               "Inline constants can only be set while recording a command list");
    }
    if (!command->bound_pipeline.is_valid()) {
        return Result::failure(ResultCode::kInvalidState, "Inline constants require a bound pipeline");
    }
    if (data.empty() || (offset % 4u) != 0 || (data.size() % 4u) != 0 ||
        data.size() > std::numeric_limits<uint32_t>::max() ||
        offset > std::numeric_limits<uint32_t>::max() - data.size()) {
        return Result::failure(ResultCode::kInvalidArgument,
                               "Inline constant offset and size must be non-zero and four-byte aligned");
    }

    const PipelineResource* pipeline = pipelines_.get(command->bound_pipeline);
    if (pipeline == nullptr) {
        return Result::failure(ResultCode::kInvalidState, "Bound pipeline no longer exists");
    }
    const VkShaderStageFlags vk_shader_stages = to_vk_shader_stages(shader_stages);
    if (vk_shader_stages == 0) {
        return Result::failure(ResultCode::kInvalidArgument, "Inline constants require at least one shader stage");
    }

    const uint32_t size = static_cast<uint32_t>(data.size());
    const bool covered =
        std::any_of(pipeline->inline_constant_ranges.begin(), pipeline->inline_constant_ranges.end(),
                    [offset, size, shader_stages](const InlineConstantRange& range) {
                        if (range.offset > offset || range.size < size || offset - range.offset > range.size - size) {
                            return false;
                        }
                        return has_flag(range.shader_stages, shader_stages);
                    });
    if (!covered) {
        return Result::failure(ResultCode::kInvalidArgument,
                               "Inline constant write is not covered by the bound pipeline layout");
    }

    vkCmdPushConstants(command->command_buffer, pipeline->layout, vk_shader_stages, offset, size, data.data());
    return Result::success();
}

Result VulkanDevice::bind_vertex_buffer(CommandListHandle command_list, uint32_t binding, BufferHandle buffer,
                                        uint64_t offset) {
    CommandListResource* command = command_lists_.get(command_list);
    if (command == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid command list handle");
    }
    if (!command->recording || command->queue != QueueClass::kGraphics) {
        return Result::failure(ResultCode::kInvalidState, "Vertex buffers require a recording graphics command list");
    }
    if (binding >= physical_properties_.limits.maxVertexInputBindings) {
        return Result::failure(ResultCode::kInvalidArgument, "Vertex binding index exceeds device limits");
    }

    BufferResource* resource = buffers_.get(buffer);
    if (resource == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid vertex buffer handle");
    }
    if (!has_flag(resource->capabilities, BufferCapability::kVertex)) {
        return Result::failure(ResultCode::kInvalidArgument, "Buffer does not have Vertex capability");
    }
    if (offset >= resource->size) {
        return Result::failure(ResultCode::kInvalidArgument, "Vertex buffer offset is out of range");
    }

    ResourceState state{};
    state.usage = ResourceUsage::kVertexInput;
    state.queue = command->queue;
    state.shader_stages = ShaderStage::kVertex;
    state.pipeline_stages = PipelineStage::kVertexInput;
    const Result transition_result =
        transition_buffer(*command, buffer, offset, std::numeric_limits<uint64_t>::max(), state);
    if (!transition_result) {
        return transition_result;
    }

    const VkBuffer vk_buffer = resource->buffer;
    const VkDeviceSize vk_offset = offset;
    vkCmdBindVertexBuffers(command->command_buffer, binding, 1, &vk_buffer, &vk_offset);
    if (std::find(command->referenced_buffers.begin(), command->referenced_buffers.end(), buffer) ==
        command->referenced_buffers.end()) {
        command->referenced_buffers.push_back(buffer);
    }
    return Result::success();
}

Result VulkanDevice::bind_index_buffer(CommandListHandle command_list, BufferHandle buffer, uint64_t offset,
                                       IndexFormat format) {
    CommandListResource* command = command_lists_.get(command_list);
    if (command == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid command list handle");
    }
    if (!command->recording || command->queue != QueueClass::kGraphics) {
        return Result::failure(ResultCode::kInvalidState, "Index buffers require a recording graphics command list");
    }
    BufferResource* resource = buffers_.get(buffer);
    if (resource == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid index buffer handle");
    }
    if (!has_flag(resource->capabilities, BufferCapability::kIndex)) {
        return Result::failure(ResultCode::kInvalidArgument, "Buffer does not have Index capability");
    }
    const uint64_t alignment = format == IndexFormat::kUint16 ? 2u : 4u;
    if ((offset % alignment) != 0 || offset >= resource->size) {
        return Result::failure(ResultCode::kInvalidArgument, "Index buffer offset is invalid");
    }

    ResourceState state{};
    state.usage = ResourceUsage::kIndexInput;
    state.queue = command->queue;
    state.shader_stages = ShaderStage::kVertex;
    state.pipeline_stages = PipelineStage::kVertexInput;
    const Result transition_result =
        transition_buffer(*command, buffer, offset, std::numeric_limits<uint64_t>::max(), state);
    if (!transition_result) {
        return transition_result;
    }

    vkCmdBindIndexBuffer(command->command_buffer, resource->buffer, offset,
                         format == IndexFormat::kUint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    command->index_buffer_bound = true;
    command->bound_index_buffer = buffer;
    command->bound_index_offset = offset;
    command->bound_index_format = format;
    add_unique(command->referenced_buffers, buffer);
    return Result::success();
}

Result VulkanDevice::draw(CommandListHandle command_list, const DrawDesc& desc) {
    CommandListResource* command = command_lists_.get(command_list);
    if (command == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid command list handle");
    }
    if (!command->recording || !command->rendering) {
        return Result::failure(ResultCode::kInvalidState, "Draw requires an active rendering scope");
    }
    if (!command->bound_pipeline.is_valid()) {
        return Result::failure(ResultCode::kInvalidState, "Draw requires a bound graphics pipeline");
    }
    const PipelineResource* pipeline = pipelines_.get(command->bound_pipeline);
    if (pipeline == nullptr) {
        return Result::failure(ResultCode::kInvalidState, "Bound graphics pipeline no longer exists");
    }
    if (pipeline->type != PipelineType::kGraphics) {
        return Result::failure(ResultCode::kInvalidArgument, "Draw requires a graphics pipeline");
    }
    if (desc.vertex_count == 0 || desc.instance_count == 0) {
        return Result::failure(ResultCode::kInvalidArgument, "Draw counts must be non-zero");
    }

    vkCmdDraw(command->command_buffer, desc.vertex_count, desc.instance_count, desc.first_vertex, desc.first_instance);
    return Result::success();
}

Result VulkanDevice::draw_indexed(CommandListHandle command_list, const DrawIndexedDesc& desc) {
    CommandListResource* command = command_lists_.get(command_list);
    if (command == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid command list handle");
    }
    if (!command->recording || !command->rendering) {
        return Result::failure(ResultCode::kInvalidState, "Indexed draw requires an active rendering scope");
    }
    if (!command->bound_pipeline.is_valid() || !command->index_buffer_bound) {
        return Result::failure(ResultCode::kInvalidState,
                               "Indexed draw requires a bound graphics pipeline and index buffer");
    }
    const PipelineResource* pipeline = pipelines_.get(command->bound_pipeline);
    const BufferResource* index_buffer = buffers_.get(command->bound_index_buffer);
    if (pipeline == nullptr || index_buffer == nullptr) {
        return Result::failure(ResultCode::kInvalidState, "Indexed draw resource no longer exists");
    }
    if (pipeline->type != PipelineType::kGraphics) {
        return Result::failure(ResultCode::kInvalidArgument, "Indexed draw requires a graphics pipeline");
    }
    if (desc.index_count == 0 || desc.instance_count == 0) {
        return Result::failure(ResultCode::kInvalidArgument, "Indexed draw counts must be non-zero");
    }
    const uint64_t index_size = command->bound_index_format == IndexFormat::kUint16 ? 2u : 4u;
    if (desc.first_index > (std::numeric_limits<uint64_t>::max() - command->bound_index_offset) / index_size) {
        return Result::failure(ResultCode::kInvalidArgument, "Indexed draw range overflows");
    }
    const uint64_t index_offset = command->bound_index_offset + desc.first_index * index_size;
    if (index_offset > index_buffer->size || desc.index_count > (index_buffer->size - index_offset) / index_size) {
        return Result::failure(ResultCode::kInvalidArgument, "Indexed draw range is out of bounds");
    }

    vkCmdDrawIndexed(command->command_buffer, desc.index_count, desc.instance_count, desc.first_index,
                     desc.vertex_offset, desc.first_instance);
    return Result::success();
}

Result VulkanDevice::draw_indirect(CommandListHandle command_list, const DrawIndirectDesc& desc) {
    CommandListResource* command = command_lists_.get(command_list);
    if (command == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid command list handle");
    }
    if (!command->recording || !command->rendering) {
        return Result::failure(ResultCode::kInvalidState, "Indirect draw requires rendering");
    }
    if (!command->bound_pipeline.is_valid()) {
        return Result::failure(ResultCode::kInvalidState, "Indirect draw requires a bound pipeline");
    }
    const PipelineResource* pipeline = pipelines_.get(command->bound_pipeline);
    BufferResource* buffer = buffers_.get(desc.buffer);
    if (pipeline == nullptr || buffer == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Indirect draw contains an invalid resource");
    }
    if (pipeline->type != PipelineType::kGraphics) {
        return Result::failure(ResultCode::kInvalidArgument, "Indirect draw requires a graphics pipeline");
    }
    if (!has_flag(buffer->capabilities, BufferCapability::kIndirect) || desc.draw_count == 0 ||
        (desc.offset % 4u) != 0 || desc.stride < sizeof(uint32_t) * 4 || (desc.stride % 4u) != 0) {
        return Result::failure(ResultCode::kInvalidArgument, "Indirect draw arguments are invalid");
    }
    const uint64_t command_size = sizeof(uint32_t) * 4;
    if (desc.draw_count - 1 > (std::numeric_limits<uint64_t>::max() - command_size) / desc.stride) {
        return Result::failure(ResultCode::kInvalidArgument, "Indirect draw range overflows");
    }
    const uint64_t required_size = (desc.draw_count - 1) * desc.stride + command_size;
    const Result range_result = validate_buffer_range(desc.buffer, desc.offset, required_size);
    if (!range_result) {
        return range_result;
    }

    ResourceState state{};
    state.usage = ResourceUsage::kIndirectArguments;
    state.queue = command->queue;
    state.pipeline_stages = PipelineStage::kVertexInput;
    const Result transition_result = transition_buffer(*command, desc.buffer, desc.offset, required_size, state);
    if (!transition_result) {
        return transition_result;
    }
    vkCmdDrawIndirect(command->command_buffer, buffer->buffer, desc.offset, desc.draw_count, desc.stride);
    add_unique(command->referenced_buffers, desc.buffer);
    return Result::success();
}

Result VulkanDevice::dispatch(CommandListHandle command_list, const DispatchDesc& desc) {
    CommandListResource* command = command_lists_.get(command_list);
    if (command == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid command list handle");
    }
    if (!command->recording || command->rendering ||
        (command->queue != QueueClass::kGraphics && command->queue != QueueClass::kCompute)) {
        return Result::failure(ResultCode::kInvalidState, "Dispatch requires a recording compute command list");
    }
    if (!command->bound_pipeline.is_valid()) {
        return Result::failure(ResultCode::kInvalidState, "Dispatch requires a bound pipeline");
    }
    const PipelineResource* pipeline = pipelines_.get(command->bound_pipeline);
    if (pipeline == nullptr) {
        return Result::failure(ResultCode::kInvalidState, "Bound compute pipeline no longer exists");
    }
    if (pipeline->type != PipelineType::kCompute) {
        return Result::failure(ResultCode::kInvalidArgument, "Dispatch requires a compute pipeline");
    }
    if (desc.group_count_x == 0 || desc.group_count_y == 0 || desc.group_count_z == 0) {
        return Result::failure(ResultCode::kInvalidArgument, "Dispatch group counts must be non-zero");
    }
    vkCmdDispatch(command->command_buffer, desc.group_count_x, desc.group_count_y, desc.group_count_z);
    return Result::success();
}

Result VulkanDevice::dispatch_indirect(CommandListHandle command_list, const DispatchIndirectDesc& desc) {
    CommandListResource* command = command_lists_.get(command_list);
    if (command == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid command list handle");
    }
    if (!command->recording || command->rendering ||
        (command->queue != QueueClass::kGraphics && command->queue != QueueClass::kCompute)) {
        return Result::failure(ResultCode::kInvalidState,
                               "Indirect dispatch requires a recording compute command list");
    }
    if (!command->bound_pipeline.is_valid()) {
        return Result::failure(ResultCode::kInvalidState, "Indirect dispatch requires a bound pipeline");
    }
    const PipelineResource* pipeline = pipelines_.get(command->bound_pipeline);
    BufferResource* buffer = buffers_.get(desc.buffer);
    if (pipeline == nullptr || buffer == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Indirect dispatch contains an invalid resource");
    }
    if (pipeline->type != PipelineType::kCompute || !has_flag(buffer->capabilities, BufferCapability::kIndirect) ||
        (desc.offset % 4u) != 0) {
        return Result::failure(ResultCode::kInvalidArgument, "Indirect dispatch arguments are invalid");
    }
    constexpr uint64_t kDispatchCommandSize = sizeof(uint32_t) * 3;
    const Result range_result = validate_buffer_range(desc.buffer, desc.offset, kDispatchCommandSize);
    if (!range_result) {
        return range_result;
    }
    ResourceState state{};
    state.usage = ResourceUsage::kIndirectArguments;
    state.queue = command->queue;
    state.pipeline_stages = PipelineStage::kCompute;
    const Result transition_result = transition_buffer(*command, desc.buffer, desc.offset, kDispatchCommandSize, state);
    if (!transition_result) {
        return transition_result;
    }
    vkCmdDispatchIndirect(command->command_buffer, buffer->buffer, desc.offset);
    add_unique(command->referenced_buffers, desc.buffer);
    return Result::success();
}

Result VulkanDevice::copy_buffer(CommandListHandle command_list, const BufferCopyDesc& desc) {
    CommandListResource* command = command_lists_.get(command_list);
    if (command == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid command list handle");
    }
    if (!command->recording || command->rendering || command->queue == QueueClass::kPresent) {
        return Result::failure(ResultCode::kInvalidState, "Buffer copy requires a recording copy-capable command list");
    }
    BufferResource* source = buffers_.get(desc.source);
    BufferResource* destination = buffers_.get(desc.destination);
    if (source == nullptr || destination == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Buffer copy contains an invalid buffer handle");
    }
    if (!has_flag(source->capabilities, BufferCapability::kTransferSource) ||
        !has_flag(destination->capabilities, BufferCapability::kTransferDestination) || desc.size == 0) {
        return Result::failure(ResultCode::kInvalidArgument, "Buffer copy capabilities or size are invalid");
    }
    const Result source_range = validate_buffer_range(desc.source, desc.source_offset, desc.size);
    if (!source_range) {
        return source_range;
    }
    const Result destination_range = validate_buffer_range(desc.destination, desc.destination_offset, desc.size);
    if (!destination_range) {
        return destination_range;
    }
    ResourceState source_state{};
    source_state.usage = ResourceUsage::kCopySource;
    source_state.queue = command->queue;
    source_state.pipeline_stages = PipelineStage::kCopy;
    ResourceState destination_state = source_state;
    destination_state.usage = ResourceUsage::kCopyDestination;
    if (const Result result = transition_buffer(*command, desc.source, desc.source_offset, desc.size, source_state);
        !result) {
        return result;
    }
    if (const Result result =
            transition_buffer(*command, desc.destination, desc.destination_offset, desc.size, destination_state);
        !result) {
        return result;
    }
    const VkBufferCopy region{desc.source_offset, desc.destination_offset, desc.size};
    vkCmdCopyBuffer(command->command_buffer, source->buffer, destination->buffer, 1, &region);
    add_unique(command->referenced_buffers, desc.source);
    add_unique(command->referenced_buffers, desc.destination);
    return Result::success();
}

Result VulkanDevice::copy_buffer_to_texture(CommandListHandle command_list, const BufferTextureCopyDesc& desc) {
    CommandListResource* command = command_lists_.get(command_list);
    if (command == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid command list handle");
    }
    if (!command->recording || command->rendering || command->queue == QueueClass::kPresent) {
        return Result::failure(ResultCode::kInvalidState,
                               "Buffer-to-texture copy requires a recording copy-capable command list");
    }
    BufferResource* source = buffers_.get(desc.source);
    TextureResource* destination = textures_.get(desc.destination);
    if (source == nullptr || destination == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Buffer-to-texture copy contains an invalid resource");
    }
    if (!has_flag(source->capabilities, BufferCapability::kTransferSource) ||
        !has_flag(destination->desc.capabilities, TextureCapability::kTransferDestination) || desc.extent.width == 0 ||
        desc.extent.height == 0 || desc.extent.depth == 0) {
        return Result::failure(ResultCode::kInvalidArgument, "Buffer-to-texture copy arguments are invalid");
    }
    const SubresourceRange range{desc.destination_subresources.aspects, desc.destination_subresources.mip_level, 1,
                                 desc.destination_subresources.base_array_layer,
                                 desc.destination_subresources.layer_count};
    if (const Result result = validate_texture_copy_region(*destination, desc.destination_subresources,
                                                           desc.destination_offset, desc.extent);
        !result) {
        return result;
    }
    uint64_t source_required_end = 0;
    if (const Result result = validate_copy_buffer_range(
            destination->desc.format, desc.destination_subresources.aspects, desc.destination_subresources.layer_count,
            desc.extent, desc.source_row_length, desc.source_image_height, desc.source_offset, source->size,
            &source_required_end);
        !result) {
        return result;
    }
    const ResourceState source_state{ResourceUsage::kCopySource, ShaderStage::kAll, command->queue, false,
                                     PipelineStage::kCopy};
    const ResourceState destination_state{ResourceUsage::kCopyDestination, ShaderStage::kAll, command->queue, false,
                                          PipelineStage::kCopy};
    if (const Result result = transition_buffer(*command, desc.source, desc.source_offset,
                                                source_required_end - desc.source_offset, source_state);
        !result) {
        return result;
    }
    if (const Result result = transition_texture(*command, desc.destination, range, destination_state); !result) {
        return result;
    }
    VkBufferImageCopy region{};
    region.bufferOffset = desc.source_offset;
    region.bufferRowLength = desc.source_row_length;
    region.bufferImageHeight = desc.source_image_height;
    region.imageSubresource.aspectMask = to_vk_aspect(desc.destination_subresources.aspects);
    region.imageSubresource.mipLevel = desc.destination_subresources.mip_level;
    region.imageSubresource.baseArrayLayer = desc.destination_subresources.base_array_layer;
    region.imageSubresource.layerCount = desc.destination_subresources.layer_count;
    region.imageOffset = {desc.destination_offset.x, desc.destination_offset.y, desc.destination_offset.z};
    region.imageExtent = {desc.extent.width, desc.extent.height, desc.extent.depth};
    vkCmdCopyBufferToImage(command->command_buffer, source->buffer, destination->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    add_unique(command->referenced_buffers, desc.source);
    add_unique(command->referenced_textures, desc.destination);
    return Result::success();
}

Result VulkanDevice::copy_texture_to_buffer(CommandListHandle command_list, const TextureBufferCopyDesc& desc) {
    CommandListResource* command = command_lists_.get(command_list);
    if (command == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid command list handle");
    }
    if (!command->recording || command->rendering || command->queue == QueueClass::kPresent) {
        return Result::failure(ResultCode::kInvalidState,
                               "Texture-to-buffer copy requires a recording copy-capable command list");
    }
    TextureResource* source = textures_.get(desc.source);
    BufferResource* destination = buffers_.get(desc.destination);
    if (source == nullptr || destination == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Texture-to-buffer copy contains an invalid resource");
    }
    if (!has_flag(source->desc.capabilities, TextureCapability::kTransferSource) ||
        !has_flag(destination->capabilities, BufferCapability::kTransferDestination) || desc.extent.width == 0 ||
        desc.extent.height == 0 || desc.extent.depth == 0) {
        return Result::failure(ResultCode::kInvalidArgument, "Texture-to-buffer copy arguments are invalid");
    }
    const SubresourceRange range{desc.source_subresources.aspects, desc.source_subresources.mip_level, 1,
                                 desc.source_subresources.base_array_layer, desc.source_subresources.layer_count};
    if (const Result result =
            validate_texture_copy_region(*source, desc.source_subresources, desc.source_offset, desc.extent);
        !result) {
        return result;
    }
    uint64_t destination_required_end = 0;
    if (const Result result = validate_copy_buffer_range(
            source->desc.format, desc.source_subresources.aspects, desc.source_subresources.layer_count, desc.extent,
            desc.destination_row_length, desc.destination_image_height, desc.destination_offset, destination->size,
            &destination_required_end);
        !result) {
        return result;
    }
    const ResourceState source_state{ResourceUsage::kCopySource, ShaderStage::kAll, command->queue, false,
                                     PipelineStage::kCopy};
    const ResourceState destination_state{ResourceUsage::kCopyDestination, ShaderStage::kAll, command->queue, false,
                                          PipelineStage::kCopy};
    if (const Result result = transition_texture(*command, desc.source, range, source_state); !result) {
        return result;
    }
    if (const Result result = transition_buffer(*command, desc.destination, desc.destination_offset,
                                                destination_required_end - desc.destination_offset, destination_state);
        !result) {
        return result;
    }
    VkBufferImageCopy region{};
    region.bufferOffset = desc.destination_offset;
    region.bufferRowLength = desc.destination_row_length;
    region.bufferImageHeight = desc.destination_image_height;
    region.imageSubresource.aspectMask = to_vk_aspect(desc.source_subresources.aspects);
    region.imageSubresource.mipLevel = desc.source_subresources.mip_level;
    region.imageSubresource.baseArrayLayer = desc.source_subresources.base_array_layer;
    region.imageSubresource.layerCount = desc.source_subresources.layer_count;
    region.imageOffset = {desc.source_offset.x, desc.source_offset.y, desc.source_offset.z};
    region.imageExtent = {desc.extent.width, desc.extent.height, desc.extent.depth};
    vkCmdCopyImageToBuffer(command->command_buffer, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           destination->buffer, 1, &region);
    add_unique(command->referenced_buffers, desc.destination);
    add_unique(command->referenced_textures, desc.source);
    return Result::success();
}

Result VulkanDevice::copy_texture(CommandListHandle command_list, const TextureCopyDesc& desc) {
    CommandListResource* command = command_lists_.get(command_list);
    if (command == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid command list handle");
    }
    if (!command->recording || command->rendering || command->queue == QueueClass::kPresent) {
        return Result::failure(ResultCode::kInvalidState,
                               "Texture copy requires a recording copy-capable command list");
    }
    TextureResource* source = textures_.get(desc.source);
    TextureResource* destination = textures_.get(desc.destination);
    if (source == nullptr || destination == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Texture copy contains an invalid texture handle");
    }
    if (!has_flag(source->desc.capabilities, TextureCapability::kTransferSource) ||
        !has_flag(destination->desc.capabilities, TextureCapability::kTransferDestination) || desc.extent.width == 0 ||
        desc.extent.height == 0 || desc.extent.depth == 0) {
        return Result::failure(ResultCode::kInvalidArgument, "Texture copy capabilities or extent are invalid");
    }
    if (source->desc.format != destination->desc.format ||
        desc.source_subresources.aspects != desc.destination_subresources.aspects) {
        return Result::failure(ResultCode::kInvalidArgument,
                               "Texture copy source and destination formats or aspects are incompatible");
    }
    const SubresourceRange source_range{desc.source_subresources.aspects, desc.source_subresources.mip_level, 1,
                                        desc.source_subresources.base_array_layer,
                                        desc.source_subresources.layer_count};
    const SubresourceRange destination_range{
        desc.destination_subresources.aspects, desc.destination_subresources.mip_level, 1,
        desc.destination_subresources.base_array_layer, desc.destination_subresources.layer_count};
    if (desc.source_subresources.layer_count != desc.destination_subresources.layer_count) {
        return Result::failure(ResultCode::kInvalidArgument, "Texture copy layer counts must match");
    }
    if (const Result result =
            validate_texture_copy_region(*source, desc.source_subresources, desc.source_offset, desc.extent);
        !result) {
        return result;
    }
    if (const Result result = validate_texture_copy_region(*destination, desc.destination_subresources,
                                                           desc.destination_offset, desc.extent);
        !result) {
        return result;
    }
    const ResourceState source_state{ResourceUsage::kCopySource, ShaderStage::kAll, command->queue, false,
                                     PipelineStage::kCopy};
    const ResourceState destination_state{ResourceUsage::kCopyDestination, ShaderStage::kAll, command->queue, false,
                                          PipelineStage::kCopy};
    if (const Result result = transition_texture(*command, desc.source, source_range, source_state); !result) {
        return result;
    }
    if (const Result result = transition_texture(*command, desc.destination, destination_range, destination_state);
        !result) {
        return result;
    }
    VkImageCopy region{};
    region.srcSubresource.aspectMask = to_vk_aspect(desc.source_subresources.aspects);
    region.srcSubresource.mipLevel = desc.source_subresources.mip_level;
    region.srcSubresource.baseArrayLayer = desc.source_subresources.base_array_layer;
    region.srcSubresource.layerCount = desc.source_subresources.layer_count;
    region.srcOffset = {desc.source_offset.x, desc.source_offset.y, desc.source_offset.z};
    region.dstSubresource.aspectMask = to_vk_aspect(desc.destination_subresources.aspects);
    region.dstSubresource.mipLevel = desc.destination_subresources.mip_level;
    region.dstSubresource.baseArrayLayer = desc.destination_subresources.base_array_layer;
    region.dstSubresource.layerCount = desc.destination_subresources.layer_count;
    region.dstOffset = {desc.destination_offset.x, desc.destination_offset.y, desc.destination_offset.z};
    region.extent = {desc.extent.width, desc.extent.height, desc.extent.depth};
    vkCmdCopyImage(command->command_buffer, source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination->image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    add_unique(command->referenced_textures, desc.source);
    add_unique(command->referenced_textures, desc.destination);
    return Result::success();
}

Result VulkanDevice::clear_texture(CommandListHandle command_list, const ClearTextureDesc& desc) {
    CommandListResource* command = command_lists_.get(command_list);
    if (command == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid command list handle");
    }
    if (!command->recording || command->rendering || command->queue == QueueClass::kPresent) {
        return Result::failure(ResultCode::kInvalidState,
                               "Texture clear requires a recording copy-capable command list");
    }
    TextureResource* texture = textures_.get(desc.texture);
    if (texture == nullptr) {
        return Result::failure(ResultCode::kNotFound, "Invalid texture handle");
    }
    if (!has_flag(texture->desc.capabilities, TextureCapability::kTransferDestination)) {
        return Result::failure(ResultCode::kInvalidArgument, "Texture does not support transfer clears");
    }
    const Result range_result = validate_texture_subresource(*texture, desc.subresources);
    if (!range_result) {
        return range_result;
    }
    const bool color = has_flag(desc.subresources.aspects, Aspect::kColor);
    const bool depth_or_stencil =
        has_flag(desc.subresources.aspects, Aspect::kDepth) || has_flag(desc.subresources.aspects, Aspect::kStencil);
    if (color == depth_or_stencil) {
        return Result::failure(ResultCode::kInvalidArgument,
                               "Texture clear must select either color or depth/stencil aspects");
    }
    const ResourceState state{ResourceUsage::kCopyDestination, ShaderStage::kAll, command->queue, false,
                              PipelineStage::kCopy};
    if (const Result result = transition_texture(*command, desc.texture, desc.subresources, state); !result) {
        return result;
    }
    const uint32_t mip_count = desc.subresources.mip_count == kRemaining
                                   ? texture->desc.mip_levels - desc.subresources.base_mip
                                   : desc.subresources.mip_count;
    const uint32_t layer_count = desc.subresources.layer_count == kRemaining
                                     ? texture->desc.array_layers - desc.subresources.base_layer
                                     : desc.subresources.layer_count;
    VkImageSubresourceRange range{};
    range.aspectMask = to_vk_aspect(desc.subresources.aspects);
    range.baseMipLevel = desc.subresources.base_mip;
    range.levelCount = mip_count;
    range.baseArrayLayer = desc.subresources.base_layer;
    range.layerCount = layer_count;
    if (color) {
        VkClearColorValue value{};
        value.float32[0] = desc.color.r;
        value.float32[1] = desc.color.g;
        value.float32[2] = desc.color.b;
        value.float32[3] = desc.color.a;
        vkCmdClearColorImage(command->command_buffer, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1,
                             &range);
    } else {
        VkClearDepthStencilValue value{};
        value.depth = desc.depth;
        value.stencil = desc.stencil;
        vkCmdClearDepthStencilImage(command->command_buffer, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    &value, 1, &range);
    }
    add_unique(command->referenced_textures, desc.texture);
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
    if (resource->rendering) {
        return Result::failure(ResultCode::kInvalidState, "Rendering must end before ending a command list");
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
    resource->rendering = false;
    resource->bound_pipeline = {};
    resource->index_buffer_bound = false;
    resource->bound_index_buffer = {};
    resource->bound_index_offset = 0;
    resource->bound_index_format = IndexFormat::kUint32;
    resource->active_color_attachments.clear();
    resource->active_depth_stencil = {};
    resource->referenced_buffers.clear();
    resource->referenced_textures.clear();
    resource->referenced_texture_views.clear();
    resource->referenced_pipelines.clear();
    resource->referenced_binding_sets.clear();
    resource->referenced_buffer_views.clear();
    resource->referenced_samplers.clear();
    resource->buffer_state_changes.clear();
    resource->buffer_states.clear();
    resource->texture_state_changes.clear();
    resource->buffer_ownership_transfers.clear();
    resource->texture_ownership_transfers.clear();
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
    if (resource->ended && !resource->last_submitted.is_valid()) {
        return Result::failure(ResultCode::kInvalidState,
                               "Cannot destroy an ended command list that has not been submitted");
    }
    if (resource->last_submitted.is_valid() && !is_complete(resource->last_submitted)) {
        QueueValues required_values{};
        required_values[queue_value_index(resource->queue)] = resource->last_submitted.value;
        RetireCommandBuffer retire{required_values, resource->pool, resource->command_buffer};
        (void)command_lists_.erase(command_list);
        enqueue_retire(std::move(retire));
        return Result::success();
    }

    RetireCommandBuffer retire{};
    retire.pool = resource->pool;
    retire.command_buffer = resource->command_buffer;
    if (resource->last_submitted.is_valid()) {
        retire.required_values[queue_value_index(resource->queue)] = resource->last_submitted.value;
    }
    (void)command_lists_.erase(command_list);
    enqueue_retire(std::move(retire));
    return Result::success();
}

ResultValue<SubmitResult> VulkanDevice::submit(const SubmitDesc& desc) {
    QueueState* queue = queue_state(desc.queue);
    if (device_ == VK_NULL_HANDLE || queue == nullptr || queue->queue == VK_NULL_HANDLE) {
        return {Result::failure(ResultCode::kInvalidState, "Requested submit queue is unavailable"), {}};
    }

    std::vector<VkCommandBuffer> command_buffers;
    command_buffers.reserve(desc.command_lists.size());
    for (size_t index = 0; index < desc.command_lists.size(); ++index) {
        const CommandListHandle command_list = desc.command_lists[index];
        if (std::find(desc.command_lists.begin(), desc.command_lists.begin() + index, command_list) !=
            desc.command_lists.begin() + index) {
            return {Result::failure(ResultCode::kInvalidArgument, "Submit contains a duplicate command list"), {}};
        }
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

    const Result state_result = validate_command_batch_states(desc.command_lists);
    if (!state_result) {
        return {state_result, {}};
    }

    std::vector<VkSemaphore> wait_semaphores;
    std::vector<VkPipelineStageFlags> wait_stages;
    std::vector<uint64_t> wait_values;
    wait_semaphores.reserve(desc.waits.size() + kQueueCount);
    wait_stages.reserve(desc.waits.size() + kQueueCount);
    wait_values.reserve(desc.waits.size() + kQueueCount);
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

    const Result ownership_result =
        submit_ownership_releases(desc.command_lists, queue->logical_class, wait_semaphores, wait_stages, wait_values);
    if (!ownership_result) {
        return {ownership_result, {}};
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
        CommandListResource* resource = command_lists_.get(command_list);
        commit_command_states(*resource);
        mark_command_resources_in_use(*resource, submit_result.completion);
        resource->last_submitted = submit_result.completion;
    }
    (void)collect_garbage();
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
    const Result result = make_vulkan_result(vkDeviceWaitIdle(device_), "vkDeviceWaitIdle");
    if (!result) {
        return result;
    }
    return collect_garbage();
}

Result VulkanDevice::validate_command_batch_states(std::span<const CommandListHandle> command_lists) {
    std::unordered_map<uint64_t, size_t> buffer_change_counts;
    std::unordered_map<uint64_t, std::vector<BufferStateSegment>> buffer_overrides;
    std::unordered_map<TextureStateKey, ResourceState, TextureStateKeyHash> texture_overrides;
    try {
        buffer_change_counts.reserve(command_lists.size());
        buffer_overrides.reserve(command_lists.size());
        texture_overrides.reserve(command_lists.size());

        for (const CommandListHandle command_list : command_lists) {
            const CommandListResource* command = command_lists_.get(command_list);
            if (command == nullptr) {
                return Result::failure(ResultCode::kNotFound, "Submit contains an invalid command list");
            }
            for (const BufferStateChange& change : command->buffer_state_changes) {
                BufferResource* resource = buffers_.get(change.buffer);
                if (resource == nullptr) {
                    return Result::failure(ResultCode::kNotFound, "Command references a destroyed buffer");
                }

                uint64_t range_end = 0;
                if (!resolve_buffer_range(resource->size, change.offset, change.size, range_end)) {
                    return Result::failure(ResultCode::kInvalidArgument,
                                           "Command contains a buffer state range outside the resource");
                }

                auto [iterator, inserted] = buffer_change_counts.try_emplace(change.buffer.value, 0);
                (void)inserted;
                if (iterator->second == std::numeric_limits<size_t>::max()) {
                    return Result::failure(ResultCode::kOutOfMemory, "Command buffer state change count overflowed");
                }
                ++iterator->second;
            }
        }

        for (const auto& [buffer_value, change_count] : buffer_change_counts) {
            BufferResource* resource = buffers_.get(BufferHandle{buffer_value});
            if (resource == nullptr) {
                return Result::failure(ResultCode::kNotFound, "Command references a destroyed buffer");
            }
            const size_t base_size = std::max<size_t>(resource->state_segments.size(), 1);
            if (change_count > (std::numeric_limits<size_t>::max() - base_size) / 2) {
                return Result::failure(ResultCode::kOutOfMemory,
                                       "Buffer state segment capacity calculation overflowed");
            }
            resource->state_segments.reserve(base_size + change_count * 2);
        }

        for (const CommandListHandle command_list : command_lists) {
            const CommandListResource* command = command_lists_.get(command_list);
            if (command == nullptr) {
                return Result::failure(ResultCode::kNotFound, "Submit contains an invalid command list");
            }

            for (const BufferStateChange& change : command->buffer_state_changes) {
                const BufferResource* resource = buffers_.get(change.buffer);
                if (resource == nullptr) {
                    return Result::failure(ResultCode::kNotFound, "Command references a destroyed buffer");
                }

                uint64_t range_end = 0;
                if (!resolve_buffer_range(resource->size, change.offset, change.size, range_end)) {
                    return Result::failure(ResultCode::kInvalidArgument,
                                           "Command contains a buffer state range outside the resource");
                }

                auto iterator = buffer_overrides.find(change.buffer.value);
                if (iterator == buffer_overrides.end()) {
                    auto inserted = buffer_overrides.emplace(change.buffer.value, resource->state_segments);
                    iterator = inserted.first;
                    initialize_buffer_state_segments(iterator->second, resource->size, resource->state);
                }

                std::vector<BufferStateSegment>& segments = iterator->second;
                split_buffer_state_segment(segments, change.offset);
                split_buffer_state_segment(segments, range_end);

                uint64_t cursor = change.offset;
                for (BufferStateSegment& segment : segments) {
                    const uint64_t segment_end = segment.offset + segment.size;
                    if (segment_end <= change.offset) {
                        continue;
                    }
                    if (segment.offset >= range_end) {
                        break;
                    }
                    if (segment.offset != cursor || segment_end > range_end ||
                        !resource_state_matches(segment.state, change.before)) {
                        return Result::failure(ResultCode::kInvalidState,
                                               "Command batch contains a conflicting buffer state transition");
                    }
                    segment.state = change.after;
                    cursor = segment_end;
                }
                if (cursor != range_end) {
                    return Result::failure(ResultCode::kInvalidState,
                                           "Command batch buffer state tracking does not cover the requested range");
                }
                merge_buffer_state_segments(segments);
            }

            for (const TextureStateChange& change : command->texture_state_changes) {
                const TextureResource* resource = textures_.get(change.texture);
                if (resource == nullptr || change.subresource_index >= resource->subresource_states.size()) {
                    return Result::failure(ResultCode::kNotFound, "Command references a destroyed texture");
                }

                const TextureStateKey key{change.texture.value, change.subresource_index};
                const auto iterator = texture_overrides.find(key);
                const ResourceState actual = iterator == texture_overrides.end()
                                                 ? resource->subresource_states[change.subresource_index]
                                                 : iterator->second;
                if (!resource_state_matches(actual, change.before)) {
                    return Result::failure(ResultCode::kInvalidState,
                                           "Command batch contains a conflicting texture state transition");
                }
                if (iterator == texture_overrides.end()) {
                    texture_overrides.emplace(key, change.after);
                } else {
                    iterator->second = change.after;
                }
            }
        }
    } catch (const std::bad_alloc&) {
        return Result::failure(ResultCode::kOutOfMemory, "Command state validation allocation failed");
    }
    return Result::success();
}

void VulkanDevice::commit_command_states(const CommandListResource& command) {
    for (const BufferStateChange& change : command.buffer_state_changes) {
        if (BufferResource* resource = buffers_.get(change.buffer); resource != nullptr) {
            uint64_t range_end = 0;
            if (!resolve_buffer_range(resource->size, change.offset, change.size, range_end)) {
                continue;
            }
            initialize_buffer_state_segments(resource->state_segments, resource->size, resource->state);
            split_buffer_state_segment(resource->state_segments, change.offset);
            split_buffer_state_segment(resource->state_segments, range_end);
            for (BufferStateSegment& segment : resource->state_segments) {
                const uint64_t segment_end = segment.offset + segment.size;
                if (segment.offset < change.offset || segment_end > range_end) {
                    continue;
                }
                segment.state = change.after;
            }
            merge_buffer_state_segments(resource->state_segments);
            if (!resource->state_segments.empty()) {
                const ResourceState state = resource->state_segments.front().state;
                const bool uniform =
                    std::all_of(resource->state_segments.begin(), resource->state_segments.end(),
                                [state](const BufferStateSegment& segment) { return segment.state == state; });
                resource->state = uniform ? state : ResourceState{};
            }
        }
    }
    for (const TextureStateChange& change : command.texture_state_changes) {
        if (TextureResource* resource = textures_.get(change.texture);
            resource != nullptr && change.subresource_index < resource->subresource_states.size()) {
            resource->subresource_states[change.subresource_index] = change.after;
            resource->state = change.after;
        }
    }
}

void VulkanDevice::mark_command_resources_in_use(const CommandListResource& command, const GpuSyncPoint& completion) {
    const uint32_t queue_index = queue_value_index(command.queue);
    const uint64_t value = completion.value;
    const auto mark_buffer = [&](BufferHandle handle) {
        if (BufferResource* resource = buffers_.get(handle); resource != nullptr) {
            resource->last_use[queue_index] = std::max(resource->last_use[queue_index], value);
        }
    };
    const auto mark_texture = [&](TextureHandle handle) {
        if (TextureResource* resource = textures_.get(handle); resource != nullptr) {
            resource->last_use[queue_index] = std::max(resource->last_use[queue_index], value);
        }
    };
    const auto mark_buffer_view = [&](BufferViewHandle handle) {
        if (BufferViewResource* resource = buffer_views_.get(handle); resource != nullptr) {
            resource->last_use[queue_index] = std::max(resource->last_use[queue_index], value);
        }
    };
    const auto mark_texture_view = [&](TextureViewHandle handle) {
        if (TextureViewResource* resource = texture_views_.get(handle); resource != nullptr) {
            resource->last_use[queue_index] = std::max(resource->last_use[queue_index], value);
        }
    };
    const auto mark_sampler = [&](SamplerHandle handle) {
        if (SamplerResource* resource = samplers_.get(handle); resource != nullptr) {
            resource->last_use[queue_index] = std::max(resource->last_use[queue_index], value);
        }
    };
    const auto mark_binding_set = [&](BindingSetHandle handle) {
        BindingSetResource* resource = binding_sets_.get(handle);
        if (resource == nullptr) {
            return;
        }
        resource->last_use[queue_index] = std::max(resource->last_use[queue_index], value);
        if (BindingLayoutResource* layout = binding_layouts_.get(resource->layout); layout != nullptr) {
            layout->last_use[queue_index] = std::max(layout->last_use[queue_index], value);
        }
        for (const BindingWrite& write : resource->writes) {
            mark_sampler(write.resource.sampler);
            mark_texture_view(write.resource.texture_view);
            mark_buffer_view(write.resource.buffer_view);
            mark_buffer(write.resource.buffer_range.buffer);
            if (const BufferViewResource* view = buffer_views_.get(write.resource.buffer_view); view != nullptr) {
                mark_buffer(view->desc.buffer);
            }
            if (const TextureViewResource* view = texture_views_.get(write.resource.texture_view); view != nullptr) {
                mark_texture(view->desc.texture);
            }
        }
    };

    for (const BufferHandle handle : command.referenced_buffers) {
        mark_buffer(handle);
    }
    for (const TextureHandle handle : command.referenced_textures) {
        mark_texture(handle);
    }
    for (const BufferViewHandle handle : command.referenced_buffer_views) {
        mark_buffer_view(handle);
    }
    for (const TextureViewHandle handle : command.referenced_texture_views) {
        mark_texture_view(handle);
    }
    for (const SamplerHandle handle : command.referenced_samplers) {
        mark_sampler(handle);
    }
    for (const BindingSetHandle handle : command.referenced_binding_sets) {
        mark_binding_set(handle);
    }
    for (const PipelineHandle handle : command.referenced_pipelines) {
        if (PipelineResource* resource = pipelines_.get(handle); resource != nullptr) {
            resource->last_use[queue_index] = std::max(resource->last_use[queue_index], value);
            for (const BindingLayoutHandle layout_handle : resource->binding_layouts) {
                if (BindingLayoutResource* layout = binding_layouts_.get(layout_handle); layout != nullptr) {
                    layout->last_use[queue_index] = std::max(layout->last_use[queue_index], value);
                }
            }
        }
    }
}

bool VulkanDevice::command_is_unsubmitted(const CommandListResource& command) const {
    return command.recording || (command.ended && !command.last_submitted.is_valid());
}

bool VulkanDevice::buffer_has_unsubmitted_command_reference(BufferHandle buffer) const {
    bool referenced = false;
    command_lists_.for_each([&](const CommandListResource& command) {
        referenced = referenced || (command_is_unsubmitted(command) &&
                                    std::find(command.referenced_buffers.begin(), command.referenced_buffers.end(),
                                              buffer) != command.referenced_buffers.end());
    });
    return referenced;
}

bool VulkanDevice::texture_has_unsubmitted_command_reference(TextureHandle texture) const {
    bool referenced = false;
    command_lists_.for_each([&](const CommandListResource& command) {
        referenced = referenced || (command_is_unsubmitted(command) &&
                                    std::find(command.referenced_textures.begin(), command.referenced_textures.end(),
                                              texture) != command.referenced_textures.end());
    });
    return referenced;
}

bool VulkanDevice::buffer_view_has_unsubmitted_command_reference(BufferViewHandle view) const {
    bool referenced = false;
    command_lists_.for_each([&](const CommandListResource& command) {
        referenced = referenced ||
                     (command_is_unsubmitted(command) &&
                      std::find(command.referenced_buffer_views.begin(), command.referenced_buffer_views.end(), view) !=
                          command.referenced_buffer_views.end());
    });
    return referenced;
}

bool VulkanDevice::texture_view_has_unsubmitted_command_reference(TextureViewHandle view) const {
    bool referenced = false;
    command_lists_.for_each([&](const CommandListResource& command) {
        referenced =
            referenced || (command_is_unsubmitted(command) &&
                           std::find(command.referenced_texture_views.begin(), command.referenced_texture_views.end(),
                                     view) != command.referenced_texture_views.end());
    });
    return referenced;
}

bool VulkanDevice::sampler_has_unsubmitted_command_reference(SamplerHandle sampler) const {
    bool referenced = false;
    command_lists_.for_each([&](const CommandListResource& command) {
        referenced = referenced || (command_is_unsubmitted(command) &&
                                    std::find(command.referenced_samplers.begin(), command.referenced_samplers.end(),
                                              sampler) != command.referenced_samplers.end());
    });
    return referenced;
}

bool VulkanDevice::binding_set_has_unsubmitted_command_reference(BindingSetHandle set) const {
    bool referenced = false;
    command_lists_.for_each([&](const CommandListResource& command) {
        referenced = referenced ||
                     (command_is_unsubmitted(command) &&
                      std::find(command.referenced_binding_sets.begin(), command.referenced_binding_sets.end(), set) !=
                          command.referenced_binding_sets.end());
    });
    return referenced;
}

bool VulkanDevice::pipeline_has_unsubmitted_command_reference(PipelineHandle pipeline) const {
    bool referenced = false;
    command_lists_.for_each([&](const CommandListResource& command) {
        referenced = referenced || (command_is_unsubmitted(command) &&
                                    std::find(command.referenced_pipelines.begin(), command.referenced_pipelines.end(),
                                              pipeline) != command.referenced_pipelines.end());
    });
    return referenced;
}

void VulkanDevice::enqueue_retire(RetireRecord record) {
    bool has_wait = false;
    std::visit(
        [&](const auto& value) {
            has_wait = std::any_of(value.required_values.begin(), value.required_values.end(),
                                   [](uint64_t required) { return required != 0; });
        },
        record);
    if (has_wait) {
        retire_records_.push_back(std::move(record));
    } else {
        destroy_retire_record(record);
    }
}

bool VulkanDevice::retire_record_complete(const RetireRecord& record) const {
    bool complete = true;
    std::visit(
        [&](const auto& value) {
            for (uint32_t index = 0; index < kQueueCount; ++index) {
                if (value.required_values[index] == 0) {
                    continue;
                }
                const QueueState& queue = queues_[index];
                uint64_t completed = 0;
                if (queue.timeline == VK_NULL_HANDLE ||
                    vkGetSemaphoreCounterValue(device_, queue.timeline, &completed) != VK_SUCCESS ||
                    completed < value.required_values[index]) {
                    complete = false;
                    return;
                }
            }
        },
        record);
    return complete;
}

void VulkanDevice::destroy_retire_record(RetireRecord& record) noexcept {
    std::visit(
        [&](auto& value) {
            using ValueType = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<ValueType, RetireBuffer>) {
                if (allocator_ != nullptr && value.allocation != nullptr && value.mapped) {
                    vmaUnmapMemory(allocator_, value.allocation);
                }
                if (value.buffer != VK_NULL_HANDLE && allocator_ != nullptr) {
                    vmaDestroyBuffer(allocator_, value.buffer, value.allocation);
                }
                value.buffer = VK_NULL_HANDLE;
                value.allocation = nullptr;
                value.mapped = false;
            } else if constexpr (std::is_same_v<ValueType, RetireTexture>) {
                if (value.image != VK_NULL_HANDLE && allocator_ != nullptr) {
                    vmaDestroyImage(allocator_, value.image, value.allocation);
                }
                value.image = VK_NULL_HANDLE;
                value.allocation = nullptr;
            } else if constexpr (std::is_same_v<ValueType, RetireBufferView>) {
                if (value.view != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
                    vkDestroyBufferView(device_, value.view, nullptr);
                }
                value.view = VK_NULL_HANDLE;
            } else if constexpr (std::is_same_v<ValueType, RetireTextureView>) {
                if (value.view != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
                    vkDestroyImageView(device_, value.view, nullptr);
                }
                value.view = VK_NULL_HANDLE;
            } else if constexpr (std::is_same_v<ValueType, RetireSampler>) {
                if (value.sampler != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
                    vkDestroySampler(device_, value.sampler, nullptr);
                }
                value.sampler = VK_NULL_HANDLE;
            } else if constexpr (std::is_same_v<ValueType, RetireBindingLayout>) {
                if (value.layout != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
                    vkDestroyDescriptorSetLayout(device_, value.layout, nullptr);
                }
                value.layout = VK_NULL_HANDLE;
            } else if constexpr (std::is_same_v<ValueType, RetireBindingSet>) {
                release_descriptor_set(value.lifetime, value.page_index, value.set, value.requirements);
                value.page_index = kInvalidIndex;
                value.set = VK_NULL_HANDLE;
            } else if constexpr (std::is_same_v<ValueType, RetirePipeline>) {
                if (value.pipeline != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
                    vkDestroyPipeline(device_, value.pipeline, nullptr);
                }
                if (value.layout != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
                    vkDestroyPipelineLayout(device_, value.layout, nullptr);
                }
                value.pipeline = VK_NULL_HANDLE;
                value.layout = VK_NULL_HANDLE;
            } else if constexpr (std::is_same_v<ValueType, RetireCommandBuffer>) {
                if (value.command_buffer != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
                    vkFreeCommandBuffers(device_, value.pool, 1, &value.command_buffer);
                }
                value.command_buffer = VK_NULL_HANDLE;
                value.pool = VK_NULL_HANDLE;
            }
            value.required_values.fill(0);
        },
        record);
}

Result VulkanDevice::collect_garbage() {
    if (device_ == VK_NULL_HANDLE || retire_records_.empty()) {
        return Result::success();
    }
    size_t write_index = 0;
    for (size_t index = 0; index < retire_records_.size(); ++index) {
        RetireRecord& record = retire_records_[index];
        if (retire_record_complete(record)) {
            destroy_retire_record(record);
        } else {
            if (write_index != index) {
                retire_records_[write_index] = std::move(record);
            }
            ++write_index;
        }
    }
    retire_records_.resize(write_index);
    return Result::success();
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

bool VulkanDevice::binding_layout_is_referenced(BindingLayoutHandle layout) const {
    bool referenced = false;
    pipelines_.for_each([&](const PipelineResource& pipeline) {
        referenced = referenced || std::find(pipeline.binding_layouts.begin(), pipeline.binding_layouts.end(),
                                             layout) != pipeline.binding_layouts.end();
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

#if AEG_VULKAN_ENABLE_DEBUG_UTILS
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

        for (RetireRecord& record : retire_records_) {
            destroy_retire_record(record);
        }
        retire_records_.clear();

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
            resource.referenced_pipelines.clear();
        });
        pipelines_.for_each([&](PipelineResource& resource) {
            if (resource.pipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_, resource.pipeline, nullptr);
                resource.pipeline = VK_NULL_HANDLE;
            }
            if (resource.layout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device_, resource.layout, nullptr);
                resource.layout = VK_NULL_HANDLE;
            }
            resource.binding_layouts.clear();
        });
        binding_sets_.for_each([&](BindingSetResource& resource) {
            resource.descriptor_pool_page = kInvalidIndex;
            resource.set = VK_NULL_HANDLE;
        });
        destroy_descriptor_pool_pages();
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
            if (resource.mapped_data != nullptr && allocator_ != nullptr && resource.allocation.handle != nullptr) {
                vmaUnmapMemory(allocator_, resource.allocation.handle);
                resource.mapped_data = nullptr;
            }
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

#if AEG_VULKAN_ENABLE_DEBUG_UTILS
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
}  // namespace aeg::rhi
