/**
 * @file shader_contract_tool.cpp
 * @brief Builds serialized shader contracts from SPIR-V reflection data.
 */
#include "shader_asset.hpp"
#include "rhi_struct.hpp"

#include "spirv_reflect.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
    using aeg::rhi::BindingArrayMode;
    using aeg::rhi::BindingFlags;
    using aeg::rhi::BindingType;
    using aeg::rhi::Format;
    using aeg::rhi::ShaderStage;
    using aeg::shader::asset::ConstantRangeRecord;
    using aeg::shader::asset::FileHeader;
    using aeg::shader::asset::ResourceRecord;
    using aeg::shader::asset::StageRecord;
    using aeg::shader::asset::VertexInputRecord;

    constexpr std::uint32_t kInvalidIndex = std::numeric_limits<std::uint32_t>::max();

    struct StageInput {
        ShaderStage stage{ShaderStage::kNone};
        std::filesystem::path path{};
        std::string entry_point{"main"};
    };

    struct Resource {
        std::uint16_t group{0};
        std::uint16_t binding{0};
        BindingType type{BindingType::kSampledTexture};
        BindingArrayMode array_mode{BindingArrayMode::kSingle};
        std::uint32_t array_count{1};
        ShaderStage stages{ShaderStage::kNone};
        BindingFlags flags{BindingFlags::kNone};
        std::string name{};
    };

    struct VertexInput {
        std::uint32_t location{0};
        Format format{Format::kUnknown};
        std::uint32_t semantic_hash{0};
        std::uint32_t semantic_index{0};
        std::string semantic{};
    };

    struct ConstantRange {
        std::uint32_t offset{0};
        std::uint32_t size{0};
        ShaderStage stages{ShaderStage::kNone};
        std::string name{};
    };

    struct StageArtifact {
        StageInput input{};
        std::vector<std::byte> code{};
    };

    [[nodiscard]] std::string reflect_result_name(SpvReflectResult result) {
        return "SPIRV-Reflect error " + std::to_string(static_cast<int>(result));
    }

    [[nodiscard]] bool read_binary_file(const std::filesystem::path& path,
                                        std::vector<std::byte>& output,
                                        std::string& error) {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream) {
            error = "Unable to open shader file: " + path.string();
            return false;
        }

        const std::streampos end = stream.tellg();
        if (end < 0 || static_cast<std::uint64_t>(end) > std::numeric_limits<std::size_t>::max()) {
            error = "Shader file size is invalid: " + path.string();
            return false;
        }
        output.resize(static_cast<std::size_t>(end));
        stream.seekg(0, std::ios::beg);
        if (!output.empty()) {
            stream.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
        }
        if (!stream) {
            error = "Unable to read shader file: " + path.string();
            return false;
        }
        if (output.empty() || output.size() % sizeof(std::uint32_t) != 0) {
            error = "SPIR-V shader file is empty or not four-byte aligned: " + path.string();
            return false;
        }
        return true;
    }

    [[nodiscard]] SpvReflectShaderStageFlagBits to_reflect_stage(ShaderStage stage) {
        switch (stage) {
            case ShaderStage::kVertex:
                return SPV_REFLECT_SHADER_STAGE_VERTEX_BIT;
            case ShaderStage::kTessellationControl:
                return SPV_REFLECT_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
            case ShaderStage::kTessellationEvaluation:
                return SPV_REFLECT_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
            case ShaderStage::kGeometry:
                return SPV_REFLECT_SHADER_STAGE_GEOMETRY_BIT;
            case ShaderStage::kFragment:
                return SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT;
            case ShaderStage::kCompute:
                return SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT;
            case ShaderStage::kMesh:
                return SPV_REFLECT_SHADER_STAGE_MESH_BIT_EXT;
            default:
                return static_cast<SpvReflectShaderStageFlagBits>(0);
        }
    }

    [[nodiscard]] BindingType to_binding_type(const SpvReflectDescriptorBinding& binding,
                                              std::string& error) {
        switch (binding.descriptor_type) {
            case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER:
                return BindingType::kSampler;
            case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                return BindingType::kSampledTexture;
            case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                return BindingType::kStorageTexture;
            case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                return BindingType::kReadOnlyTypedBuffer;
            case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                return BindingType::kReadWriteTypedBuffer;
            case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                return BindingType::kUniformBuffer;
            case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                return BindingType::kStorageBuffer;
            case SPV_REFLECT_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
                return BindingType::kAccelerationStructure;
            case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                error = "Combined image samplers are not representable by the common RHI; "
                        "declare Texture and SamplerState separately";
                return BindingType::kSampledTexture;
            default:
                error = "Unsupported descriptor type " +
                        std::to_string(static_cast<std::uint32_t>(binding.descriptor_type));
                return BindingType::kSampledTexture;
        }
    }

    [[nodiscard]] Format to_format(SpvReflectFormat format) {
        switch (format) {
            case SPV_REFLECT_FORMAT_R32_UINT:
                return Format::kR32Uint;
            case SPV_REFLECT_FORMAT_R32_SFLOAT:
                return Format::kR32Float;
            case SPV_REFLECT_FORMAT_R32G32_SFLOAT:
                return Format::kRg32Float;
            case SPV_REFLECT_FORMAT_R32G32B32_SFLOAT:
                return Format::kRgb32Float;
            case SPV_REFLECT_FORMAT_R32G32B32A32_SFLOAT:
                return Format::kRgba32Float;
            default:
                return Format::kUnknown;
        }
    }

    [[nodiscard]] std::string semantic_name(const SpvReflectInterfaceVariable& variable) {
        std::string value = variable.semantic == nullptr ? std::string{} : std::string{variable.semantic};
        if (value.empty() && variable.name != nullptr) {
            value = variable.name;
            constexpr std::string_view kInputPrefix{"in.var."};
            if (value.starts_with(kInputPrefix)) {
                value.erase(0, kInputPrefix.size());
            } else if (const std::size_t separator = value.rfind('.'); separator != std::string::npos) {
                value.erase(0, separator + 1);
            }
        }

        for (char& character : value) {
            character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
        }
        return value;
    }

    [[nodiscard]] std::uint32_t semantic_hash(std::string_view semantic) {
        std::uint32_t hash = 2166136261u;
        for (const char character : semantic) {
            hash ^= static_cast<std::uint8_t>(character);
            hash *= 16777619u;
        }
        return hash;
    }

    void split_semantic_index(std::string& semantic, std::uint32_t& index) {
        index = 0;
        std::size_t first_digit = semantic.size();
        while (first_digit != 0 && std::isdigit(static_cast<unsigned char>(semantic[first_digit - 1]))) {
            --first_digit;
        }
        if (first_digit == semantic.size()) {
            return;
        }

        const std::string index_text = semantic.substr(first_digit);
        try {
            const unsigned long parsed = std::stoul(index_text);
            if (parsed <= std::numeric_limits<std::uint32_t>::max()) {
                index = static_cast<std::uint32_t>(parsed);
                semantic.erase(first_digit);
            }
        } catch (...) {
            index = 0;
        }
    }

    [[nodiscard]] std::pair<BindingArrayMode, std::uint32_t> to_array_shape(
            const SpvReflectDescriptorBinding& binding) {
        if (binding.array.dims_count == 0) {
            return {BindingArrayMode::kSingle, 1};
        }

        for (std::uint32_t index = 0; index < binding.array.dims_count; ++index) {
            if (binding.array.dims[index] == SPV_REFLECT_ARRAY_DIM_RUNTIME) {
                return {BindingArrayMode::kUnbounded, 0};
            }
        }
        return {BindingArrayMode::kFixed, binding.count == 0 ? 1 : binding.count};
    }

    [[nodiscard]] bool reflect_stage(const StageInput& input,
                                     StageArtifact& artifact,
                                     std::vector<Resource>& resources,
                                     std::vector<VertexInput>& vertex_inputs,
                                     std::vector<ConstantRange>& constant_ranges,
                                     std::string& error) {
        if (!read_binary_file(input.path, artifact.code, error)) {
            return false;
        }

        std::vector<std::uint32_t> words(artifact.code.size() / sizeof(std::uint32_t));
        std::memcpy(words.data(), artifact.code.data(), artifact.code.size());

        SpvReflectShaderModule module{};
        const SpvReflectResult create_result =
                spvReflectCreateShaderModule(artifact.code.size(), words.data(), &module);
        if (create_result != SPV_REFLECT_RESULT_SUCCESS) {
            error = "Unable to reflect " + input.path.string() + ": " + reflect_result_name(create_result);
            return false;
        }

        const auto cleanup = [&module]() { spvReflectDestroyShaderModule(&module); };
        const SpvReflectShaderStageFlagBits expected_stage = to_reflect_stage(input.stage);
        if (expected_stage == 0 || module.shader_stage != expected_stage) {
            error = "Reflected shader stage does not match the file stage: " + input.path.string();
            cleanup();
            return false;
        }

        std::uint32_t descriptor_count = 0;
        SpvReflectResult result = spvReflectEnumerateDescriptorBindings(&module, &descriptor_count, nullptr);
        if (result != SPV_REFLECT_RESULT_SUCCESS) {
            error = "Unable to enumerate descriptor bindings in " + input.path.string() + ": " +
                    reflect_result_name(result);
            cleanup();
            return false;
        }
        std::vector<SpvReflectDescriptorBinding*> descriptors(descriptor_count);
        result = spvReflectEnumerateDescriptorBindings(&module, &descriptor_count, descriptors.data());
        if (result != SPV_REFLECT_RESULT_SUCCESS) {
            error = "Unable to read descriptor bindings in " + input.path.string() + ": " +
                    reflect_result_name(result);
            cleanup();
            return false;
        }
        for (const SpvReflectDescriptorBinding* descriptor : descriptors) {
            if (descriptor == nullptr || descriptor->set > std::numeric_limits<std::uint16_t>::max() ||
                descriptor->binding > std::numeric_limits<std::uint16_t>::max()) {
                error = "Descriptor set or binding exceeds the common RHI range in " + input.path.string();
                cleanup();
                return false;
            }

            std::string type_error;
            const BindingType type = to_binding_type(*descriptor, type_error);
            if (!type_error.empty()) {
                error = "Unable to reflect descriptor '" +
                        std::string{descriptor->name == nullptr ? "" : descriptor->name} + "' in " +
                        input.path.string() + ": " + type_error;
                cleanup();
                return false;
            }

            const auto [array_mode, array_count] = to_array_shape(*descriptor);
            Resource resource{};
            resource.group = static_cast<std::uint16_t>(descriptor->set);
            resource.binding = static_cast<std::uint16_t>(descriptor->binding);
            resource.type = type;
            resource.array_mode = array_mode;
            resource.array_count = array_count;
            resource.stages = input.stage;
            resource.name = descriptor->name == nullptr ? std::string{} : std::string{descriptor->name};
            if (resource.name.empty()) {
                resource.name = "binding_" + std::to_string(resource.group) + "_" +
                                 std::to_string(resource.binding);
            }
            resources.push_back(std::move(resource));
        }

        if (input.stage == ShaderStage::kVertex) {
            std::uint32_t input_count = 0;
            result = spvReflectEnumerateInputVariables(&module, &input_count, nullptr);
            if (result != SPV_REFLECT_RESULT_SUCCESS) {
                error = "Unable to enumerate vertex inputs in " + input.path.string() + ": " +
                        reflect_result_name(result);
                cleanup();
                return false;
            }
            std::vector<SpvReflectInterfaceVariable*> variables(input_count);
            result = spvReflectEnumerateInputVariables(&module, &input_count, variables.data());
            if (result != SPV_REFLECT_RESULT_SUCCESS) {
                error = "Unable to read vertex inputs in " + input.path.string() + ": " +
                        reflect_result_name(result);
                cleanup();
                return false;
            }
            for (const SpvReflectInterfaceVariable* variable : variables) {
                if (variable == nullptr || variable->location == kInvalidIndex || variable->built_in >= 0 ||
                    (variable->decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN) != 0) {
                    continue;
                }
                const Format format = to_format(variable->format);
                if (format == Format::kUnknown) {
                    error = "Vertex input format is not representable by the common RHI in " + input.path.string();
                    cleanup();
                    return false;
                }
                VertexInput vertex_input{};
                vertex_input.location = variable->location;
                vertex_input.format = format;
                vertex_input.semantic = semantic_name(*variable);
                split_semantic_index(vertex_input.semantic, vertex_input.semantic_index);
                vertex_input.semantic_hash = semantic_hash(vertex_input.semantic);
                vertex_inputs.push_back(std::move(vertex_input));
            }
        }

        std::uint32_t push_constant_count = 0;
        result = spvReflectEnumeratePushConstantBlocks(&module, &push_constant_count, nullptr);
        if (result != SPV_REFLECT_RESULT_SUCCESS) {
            error = "Unable to enumerate push constants in " + input.path.string() + ": " +
                    reflect_result_name(result);
            cleanup();
            return false;
        }
        std::vector<SpvReflectBlockVariable*> blocks(push_constant_count);
        result = spvReflectEnumeratePushConstantBlocks(&module, &push_constant_count, blocks.data());
        if (result != SPV_REFLECT_RESULT_SUCCESS) {
            error = "Unable to read push constants in " + input.path.string() + ": " + reflect_result_name(result);
            cleanup();
            return false;
        }
        for (const SpvReflectBlockVariable* block : blocks) {
            if (block == nullptr || block->size == 0 || block->offset > std::numeric_limits<std::uint32_t>::max() -
                                                            block->size) {
                error = "Push constant range is invalid in " + input.path.string();
                cleanup();
                return false;
            }
            ConstantRange range{};
            range.offset = block->offset;
            range.size = block->padded_size == 0 ? block->size : block->padded_size;
            range.stages = input.stage;
            range.name = block->name == nullptr ? std::string{} : std::string{block->name};
            constant_ranges.push_back(std::move(range));
        }

        cleanup();
        return true;
    }

    [[nodiscard]] bool merge_reflection(std::vector<Resource>& resources,
                                        std::vector<VertexInput>& vertex_inputs,
                                        std::vector<ConstantRange>& constant_ranges,
                                        std::string& error) {
        std::sort(resources.begin(), resources.end(), [](const Resource& lhs, const Resource& rhs) {
            return std::pair{lhs.group, lhs.binding} < std::pair{rhs.group, rhs.binding};
        });
        std::vector<Resource> merged_resources;
        merged_resources.reserve(resources.size());
        for (Resource& resource : resources) {
            if (!merged_resources.empty() && merged_resources.back().group == resource.group &&
                merged_resources.back().binding == resource.binding) {
                Resource& existing = merged_resources.back();
                if (existing.type != resource.type || existing.array_mode != resource.array_mode ||
                    existing.array_count != resource.array_count) {
                    error = "Shader contract binding conflict at group " + std::to_string(resource.group) +
                            ", binding " + std::to_string(resource.binding);
                    return false;
                }
                existing.stages |= resource.stages;
            } else {
                merged_resources.push_back(std::move(resource));
            }
        }
        resources = std::move(merged_resources);

        std::sort(vertex_inputs.begin(), vertex_inputs.end(),
                  [](const VertexInput& lhs, const VertexInput& rhs) { return lhs.location < rhs.location; });
        for (std::size_t index = 1; index < vertex_inputs.size(); ++index) {
            if (vertex_inputs[index - 1].location == vertex_inputs[index].location) {
                if (vertex_inputs[index - 1].format != vertex_inputs[index].format) {
                    error = "Shader contract vertex input format conflict at location " +
                            std::to_string(vertex_inputs[index].location);
                    return false;
                }
                vertex_inputs.erase(vertex_inputs.begin() + static_cast<std::ptrdiff_t>(index));
                --index;
            }
        }

        std::sort(constant_ranges.begin(), constant_ranges.end(),
                  [](const ConstantRange& lhs, const ConstantRange& rhs) {
                      return std::pair{lhs.offset, lhs.size} < std::pair{rhs.offset, rhs.size};
                  });
        std::vector<ConstantRange> merged_constants;
        merged_constants.reserve(constant_ranges.size());
        for (ConstantRange& range : constant_ranges) {
            if (!merged_constants.empty() && merged_constants.back().offset == range.offset &&
                merged_constants.back().size == range.size) {
                merged_constants.back().stages |= range.stages;
            } else {
                merged_constants.push_back(std::move(range));
            }
        }
        constant_ranges = std::move(merged_constants);
        return true;
    }

    [[nodiscard]] bool fits_u32(std::size_t value) noexcept {
        return value <= std::numeric_limits<std::uint32_t>::max();
    }

    template<typename T>
    [[nodiscard]] std::optional<std::uint32_t> append_objects(
            std::vector<std::byte>& file, const std::vector<T>& objects) {
        if (!fits_u32(file.size()) || objects.size() > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            return std::nullopt;
        }
        const std::size_t byte_count = objects.size() * sizeof(T);
        if (byte_count > std::numeric_limits<std::uint32_t>::max() - file.size()) {
            return std::nullopt;
        }

        const std::uint32_t offset = static_cast<std::uint32_t>(file.size());
        const std::size_t old_size = file.size();
        file.resize(old_size + byte_count);
        if (byte_count != 0) {
            std::memcpy(file.data() + old_size, objects.data(), byte_count);
        }
        return offset;
    }

    [[nodiscard]] std::optional<std::uint32_t> append_bytes(
            std::vector<std::byte>& file, std::span<const std::byte> bytes) {
        if (!fits_u32(file.size())) {
            return std::nullopt;
        }
        const std::uint64_t aligned_size = (static_cast<std::uint64_t>(file.size()) + 3u) & ~3ull;
        if (aligned_size > std::numeric_limits<std::uint32_t>::max() ||
            bytes.size() > std::numeric_limits<std::uint32_t>::max() - aligned_size) {
            return std::nullopt;
        }

        file.resize(static_cast<std::size_t>(aligned_size));
        const std::uint32_t offset = static_cast<std::uint32_t>(aligned_size);
        const std::size_t old_size = file.size();
        file.resize(old_size + bytes.size());
        if (!bytes.empty()) {
            std::memcpy(file.data() + old_size, bytes.data(), bytes.size());
        }
        return offset;
    }

    struct StringTable {
        std::vector<std::byte> bytes{};
        bool valid{true};

        [[nodiscard]] std::uint32_t add(std::string_view value) {
            if (!valid || !fits_u32(value.size()) || value.size() >
                                                   std::numeric_limits<std::uint32_t>::max() - bytes.size()) {
                valid = false;
                return 0;
            }
            const std::uint32_t offset = static_cast<std::uint32_t>(bytes.size());
            bytes.resize(bytes.size() + value.size());
            if (!value.empty()) {
                std::memcpy(bytes.data() + offset, value.data(), value.size());
            }
            return offset;
        }
    };

    [[nodiscard]] bool write_package(const std::filesystem::path& output_path,
                                     std::string_view program_name,
                                     const std::vector<StageArtifact>& stages,
                                     std::vector<Resource> resources,
                                     std::vector<VertexInput> vertex_inputs,
                                     std::vector<ConstantRange> constant_ranges,
                                     std::string& error) {
        if (!merge_reflection(resources, vertex_inputs, constant_ranges, error)) {
            return false;
        }
        if (!fits_u32(program_name.size()) || stages.size() > std::numeric_limits<std::uint32_t>::max() ||
            resources.size() > std::numeric_limits<std::uint32_t>::max() ||
            vertex_inputs.size() > std::numeric_limits<std::uint32_t>::max() ||
            constant_ranges.size() > std::numeric_limits<std::uint32_t>::max()) {
            error = "Shader contract package contains too much data";
            return false;
        }
        for (const StageArtifact& stage : stages) {
            if (!fits_u32(stage.input.entry_point.size()) || !fits_u32(stage.code.size())) {
                error = "Shader contract package contains an oversized stage";
                return false;
            }
        }
        for (const Resource& resource : resources) {
            if (!fits_u32(resource.name.size())) {
                error = "Shader contract package contains an oversized resource name";
                return false;
            }
        }
        for (const VertexInput& vertex_input : vertex_inputs) {
            if (!fits_u32(vertex_input.semantic.size())) {
                error = "Shader contract package contains an oversized vertex semantic";
                return false;
            }
        }
        for (const ConstantRange& range : constant_ranges) {
            if (!fits_u32(range.name.size())) {
                error = "Shader contract package contains an oversized constant name";
                return false;
            }
        }

        StringTable strings{};
        const std::uint32_t program_name_string = strings.add(program_name);
        std::vector<StageRecord> stage_records(stages.size());
        for (std::size_t index = 0; index < stages.size(); ++index) {
            stage_records[index].stage = static_cast<std::uint32_t>(stages[index].input.stage);
            stage_records[index].entry_point_offset = strings.add(stages[index].input.entry_point);
            stage_records[index].entry_point_size = static_cast<std::uint32_t>(stages[index].input.entry_point.size());
        }

        std::vector<ResourceRecord> resource_records(resources.size());
        for (std::size_t index = 0; index < resources.size(); ++index) {
            const Resource& resource = resources[index];
            ResourceRecord& record = resource_records[index];
            record.group = resource.group;
            record.binding = resource.binding;
            record.type = static_cast<std::uint8_t>(resource.type);
            record.array_mode = static_cast<std::uint8_t>(resource.array_mode);
            record.flags = static_cast<std::uint32_t>(resource.flags);
            record.array_count = resource.array_count;
            record.shader_stages = static_cast<std::uint32_t>(resource.stages);
            record.name_offset = strings.add(resource.name);
            record.name_size = static_cast<std::uint32_t>(resource.name.size());
        }

        std::vector<VertexInputRecord> vertex_records(vertex_inputs.size());
        for (std::size_t index = 0; index < vertex_inputs.size(); ++index) {
            const VertexInput& vertex_input = vertex_inputs[index];
            VertexInputRecord& record = vertex_records[index];
            record.location = vertex_input.location;
            record.format = static_cast<std::uint16_t>(vertex_input.format);
            record.semantic_hash = vertex_input.semantic_hash;
            record.semantic_index = vertex_input.semantic_index;
            record.semantic_offset = strings.add(vertex_input.semantic);
            record.semantic_size = static_cast<std::uint32_t>(vertex_input.semantic.size());
        }

        std::vector<ConstantRangeRecord> constant_records(constant_ranges.size());
        for (std::size_t index = 0; index < constant_ranges.size(); ++index) {
            const ConstantRange& range = constant_ranges[index];
            ConstantRangeRecord& record = constant_records[index];
            record.offset = range.offset;
            record.size = range.size;
            record.shader_stages = static_cast<std::uint32_t>(range.stages);
            record.name_offset = strings.add(range.name);
            record.name_size = static_cast<std::uint32_t>(range.name.size());
        }

        std::vector<std::byte> file(sizeof(FileHeader));
        if (!strings.valid) {
            error = "Shader contract package string table is too large";
            return false;
        }
        const auto string_offset_result = append_bytes(file, strings.bytes);
        if (!string_offset_result.has_value()) {
            error = "Shader contract package is too large";
            return false;
        }
        const std::uint32_t string_offset = string_offset_result.value();

        const auto add_string_offset = [string_offset](std::uint32_t& offset) {
            const std::uint64_t result = static_cast<std::uint64_t>(string_offset) + offset;
            if (result > std::numeric_limits<std::uint32_t>::max()) {
                return false;
            }
            offset = static_cast<std::uint32_t>(result);
            return true;
        };

        for (StageRecord& record : stage_records) {
            if (!add_string_offset(record.entry_point_offset)) {
                error = "Shader contract package string offset overflow";
                return false;
            }
        }
        for (ResourceRecord& record : resource_records) {
            if (!add_string_offset(record.name_offset)) {
                error = "Shader contract package string offset overflow";
                return false;
            }
        }
        for (VertexInputRecord& record : vertex_records) {
            if (!add_string_offset(record.semantic_offset)) {
                error = "Shader contract package string offset overflow";
                return false;
            }
        }
        for (ConstantRangeRecord& record : constant_records) {
            if (!add_string_offset(record.name_offset)) {
                error = "Shader contract package string offset overflow";
                return false;
            }
        }

        for (std::size_t index = 0; index < stages.size(); ++index) {
            const auto code_offset = append_bytes(file, stages[index].code);
            if (!code_offset.has_value()) {
                error = "Shader contract package is too large";
                return false;
            }
            stage_records[index].code_offset = code_offset.value();
            stage_records[index].code_size = static_cast<std::uint32_t>(stages[index].code.size());
        }
        const auto resource_offset_result = append_objects(file, resource_records);
        const auto vertex_offset_result = append_objects(file, vertex_records);
        const auto constant_offset_result = append_objects(file, constant_records);
        const auto stage_offset_result = append_objects(file, stage_records);
        if (!resource_offset_result.has_value() || !vertex_offset_result.has_value() ||
            !constant_offset_result.has_value() || !stage_offset_result.has_value()) {
            error = "Shader contract package is too large";
            return false;
        }
        const std::uint32_t resource_offset = resource_offset_result.value();
        const std::uint32_t vertex_offset = vertex_offset_result.value();
        const std::uint32_t constant_offset = constant_offset_result.value();
        const std::uint32_t stage_offset = stage_offset_result.value();

        FileHeader header{};
        header.magic = aeg::shader::asset::kMagic;
        header.version = aeg::shader::asset::kVersion;
        header.header_size = sizeof(FileHeader);
        header.total_size = file.size();
        const std::uint64_t program_name_offset =
                static_cast<std::uint64_t>(string_offset) + program_name_string;
        if (program_name_offset > std::numeric_limits<std::uint32_t>::max()) {
            error = "Shader contract package string offset overflow";
            return false;
        }
        header.program_name_offset = static_cast<std::uint32_t>(program_name_offset);
        header.program_name_size = static_cast<std::uint32_t>(program_name.size());
        header.stage_offset = stage_offset;
        header.stage_count = static_cast<std::uint32_t>(stage_records.size());
        header.resource_offset = resource_offset;
        header.resource_count = static_cast<std::uint32_t>(resource_records.size());
        header.vertex_input_offset = vertex_offset;
        header.vertex_input_count = static_cast<std::uint32_t>(vertex_records.size());
        header.constant_range_offset = constant_offset;
        header.constant_range_count = static_cast<std::uint32_t>(constant_records.size());
        header.string_offset = string_offset;
        header.string_size = static_cast<std::uint32_t>(strings.bytes.size());
        std::memcpy(file.data(), &header, sizeof(header));

        std::error_code filesystem_error;
        std::filesystem::create_directories(output_path.parent_path(), filesystem_error);
        if (filesystem_error) {
            error = "Unable to create shader package directory: " + filesystem_error.message();
            return false;
        }

        const std::filesystem::path temporary_path = output_path.string() + ".tmp";
        std::ofstream stream(temporary_path, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "Unable to create shader package: " + temporary_path.string();
            return false;
        }
        stream.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
        stream.close();
        if (!stream) {
            error = "Unable to write shader package: " + temporary_path.string();
            return false;
        }

        std::filesystem::remove(output_path, filesystem_error);
        filesystem_error.clear();
        std::filesystem::rename(temporary_path, output_path, filesystem_error);
        if (filesystem_error) {
            error = "Unable to publish shader package: " + filesystem_error.message();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool parse_stage(std::string_view value, ShaderStage& stage) {
        if (value == "vertex") {
            stage = ShaderStage::kVertex;
        } else if (value == "tessellation_control") {
            stage = ShaderStage::kTessellationControl;
        } else if (value == "tessellation_evaluation") {
            stage = ShaderStage::kTessellationEvaluation;
        } else if (value == "geometry") {
            stage = ShaderStage::kGeometry;
        } else if (value == "fragment") {
            stage = ShaderStage::kFragment;
        } else if (value == "compute") {
            stage = ShaderStage::kCompute;
        } else if (value == "mesh") {
            stage = ShaderStage::kMesh;
        } else {
            return false;
        }
        return true;
    }

    [[nodiscard]] bool parse_arguments(int argc,
                                       char** argv,
                                       std::filesystem::path& output,
                                       std::string& name,
                                       std::vector<StageInput>& stages,
                                       std::string& error) {
        std::string pending_stage;
        std::string pending_entry{"main"};
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument{argv[index]};
            const auto next_value = [&](std::string& value) -> bool {
                if (index + 1 >= argc) {
                    error = "Missing value after " + std::string{argument};
                    return false;
                }
                value = argv[++index];
                return true;
            };

            if (argument == "--output") {
                std::string value;
                if (!next_value(value)) {
                    return false;
                }
                output = value;
            } else if (argument == "--name") {
                if (!next_value(name)) {
                    return false;
                }
            } else if (argument == "--stage") {
                if (!next_value(pending_stage)) {
                    return false;
                }
            } else if (argument == "--entry") {
                if (!next_value(pending_entry)) {
                    return false;
                }
            } else if (argument == "--input") {
                std::string value;
                if (!next_value(value)) {
                    return false;
                }
                ShaderStage stage{};
                if (!parse_stage(pending_stage, stage)) {
                    error = "Invalid shader stage: " + pending_stage;
                    return false;
                }
                if (pending_entry.empty()) {
                    error = "Shader entry point must not be empty";
                    return false;
                }
                stages.push_back(StageInput{stage, value, pending_entry});
                pending_stage.clear();
                pending_entry = "main";
            } else if (argument == "--help") {
                std::cout << "Usage: aeg_shader_contract_tool --output file --name name "
                             "--stage stage --entry entry --input file [...]\n";
                return false;
            } else {
                error = "Unknown argument: " + std::string{argument};
                return false;
            }
        }

        if (output.empty() || name.empty() || stages.empty()) {
            error = "--output, --name, and at least one --stage/--input pair are required";
            return false;
        }
        return true;
    }
} // namespace

int main(int argc, char** argv) {
    std::filesystem::path output;
    std::string name;
    std::vector<StageInput> stage_inputs;
    std::string error;
    if (!parse_arguments(argc, argv, output, name, stage_inputs, error)) {
        if (!error.empty()) {
            std::cerr << error << '\n';
            return 2;
        }
        return 0;
    }

    std::vector<StageArtifact> stages;
    stages.reserve(stage_inputs.size());
    std::vector<Resource> resources;
    std::vector<VertexInput> vertex_inputs;
    std::vector<ConstantRange> constant_ranges;
    for (const StageInput& input : stage_inputs) {
        if (std::any_of(stages.begin(), stages.end(), [&input](const StageArtifact& artifact) {
                return artifact.input.stage == input.stage;
            })) {
            std::cerr << "Duplicate shader stage in program\n";
            return 2;
        }
        StageArtifact artifact{};
        artifact.input = input;
        if (!reflect_stage(input, artifact, resources, vertex_inputs, constant_ranges, error)) {
            std::cerr << error << '\n';
            return 2;
        }
        stages.push_back(std::move(artifact));
    }

    if (!write_package(output, name, stages, std::move(resources), std::move(vertex_inputs),
                       std::move(constant_ranges), error)) {
        std::cerr << error << '\n';
        return 2;
    }
    return 0;
}
