/**
 * @file shader_manager.cpp
 * @brief Implements shader-package parsing, reflection validation, and layout caching.
 */
#include "../public/shader_contract.hpp"

#include "../public/shader_asset.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aeg::shader {
    namespace {
        using foundation::Result;
        using foundation::ResultCode;
        using foundation::ResultValue;
        using rhi::BindingArrayMode;
        using rhi::BindingLayoutEntry;
        using rhi::BindingType;
        using rhi::Format;
        using rhi::ShaderStage;

        constexpr std::uint32_t kKnownShaderStageBits =
                static_cast<std::uint32_t>(ShaderStage::kVertex) |
                static_cast<std::uint32_t>(ShaderStage::kTessellationControl) |
                static_cast<std::uint32_t>(ShaderStage::kTessellationEvaluation) |
                static_cast<std::uint32_t>(ShaderStage::kGeometry) |
                static_cast<std::uint32_t>(ShaderStage::kFragment) |
                static_cast<std::uint32_t>(ShaderStage::kCompute) |
                static_cast<std::uint32_t>(ShaderStage::kRayTracing) |
                static_cast<std::uint32_t>(ShaderStage::kMesh);
        constexpr std::uint32_t kKnownBindingFlags =
                static_cast<std::uint32_t>(rhi::BindingFlags::kBindless) |
                static_cast<std::uint32_t>(rhi::BindingFlags::kPartiallyPopulated) |
                static_cast<std::uint32_t>(rhi::BindingFlags::kDynamicOffset) |
                static_cast<std::uint32_t>(rhi::BindingFlags::kImmutable);

        [[nodiscard]] bool is_known_shader_stage(ShaderStage stage) noexcept {
            const std::uint32_t value = static_cast<std::uint32_t>(stage);
            if (value == 0 || (value & ~kKnownShaderStageBits) != 0) {
                return false;
            }
            return (value & (value - 1u)) == 0;
        }

        [[nodiscard]] bool is_known_binding_type(BindingType type) noexcept {
            switch (type) {
                case BindingType::kSampler:
                case BindingType::kSampledTexture:
                case BindingType::kStorageTexture:
                case BindingType::kUniformBuffer:
                case BindingType::kStorageBuffer:
                case BindingType::kReadOnlyTypedBuffer:
                case BindingType::kReadWriteTypedBuffer:
                case BindingType::kAccelerationStructure:
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool is_known_array_mode(BindingArrayMode mode) noexcept {
            switch (mode) {
                case BindingArrayMode::kSingle:
                case BindingArrayMode::kFixed:
                case BindingArrayMode::kVariable:
                case BindingArrayMode::kUnbounded:
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool is_known_format(Format format) noexcept {
            switch (format) {
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
                case Format::kD24UnormS8Uint:
                case Format::kD32Float:
                case Format::kD32FloatS8Uint:
                case Format::kBc1RgbaUnorm:
                case Format::kBc3RgbaUnorm:
                case Format::kBc5Unorm:
                case Format::kBc7RgbaUnorm:
                    return true;
                case Format::kUnknown:
                    return false;
            }
            return false;
        }

        [[nodiscard]] bool is_known_binding_flags(rhi::BindingFlags flags) noexcept {
            return (static_cast<std::uint32_t>(flags) & ~kKnownBindingFlags) == 0;
        }

        [[nodiscard]] bool checked_region(std::size_t data_size,
                                          std::uint64_t offset,
                                          std::uint64_t size) noexcept {
            return offset <= data_size && size <= data_size - offset;
        }

        [[nodiscard]] bool checked_array_region(std::size_t data_size,
                                                std::uint32_t offset,
                                                std::uint32_t count,
                                                std::size_t element_size) noexcept {
            const std::uint64_t byte_size = static_cast<std::uint64_t>(count) * element_size;
            return checked_region(data_size, offset, byte_size);
        }

        [[nodiscard]] bool checked_record_offset(std::uint32_t base,
                                                 std::uint32_t index,
                                                 std::size_t record_size,
                                                 std::uint32_t& offset) noexcept {
            const std::uint64_t candidate = static_cast<std::uint64_t>(base) +
                                             static_cast<std::uint64_t>(index) * record_size;
            if (candidate > std::numeric_limits<std::uint32_t>::max()) {
                return false;
            }
            offset = static_cast<std::uint32_t>(candidate);
            return true;
        }

        [[nodiscard]] bool string_region(const std::vector<std::byte>& data,
                                         const asset::FileHeader& header,
                                         std::uint32_t offset,
                                         std::uint32_t size) noexcept {
            if (!checked_region(data.size(), header.string_offset, header.string_size)) {
                return false;
            }
            if (!checked_region(data.size(), offset, size)) {
                return false;
            }
            if (offset < header.string_offset ||
                static_cast<std::uint64_t>(offset - header.string_offset) + size > header.string_size) {
                return false;
            }
            return true;
        }

        [[nodiscard]] std::string read_string(const std::vector<std::byte>& data,
                                              const asset::FileHeader& header,
                                              std::uint32_t offset,
                                              std::uint32_t size) {
            if (!string_region(data, header, offset, size)) {
                return {};
            }
            const char* begin = reinterpret_cast<const char*>(data.data() + offset);
            return std::string{begin, begin + size};
        }

        template<typename T>
        [[nodiscard]] bool read_object(const std::vector<std::byte>& data,
                                       std::uint32_t offset,
                                       T& object) noexcept {
            if (!checked_region(data.size(), offset, sizeof(T))) {
                return false;
            }
            std::memcpy(&object, data.data() + offset, sizeof(T));
            return true;
        }

        [[nodiscard]] ResultValue<std::vector<std::byte>> read_file(
                const std::filesystem::path& path) {
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            if (!stream) {
                return {Result::failure(ResultCode::kNotFound,
                                         "Unable to open shader package: " + path.string()), {}};
            }

            const std::streampos end = stream.tellg();
            if (end < 0 || static_cast<std::uint64_t>(end) > std::numeric_limits<std::size_t>::max()) {
                return {Result::failure(ResultCode::kInvalidArgument,
                                         "Shader package size is invalid: " + path.string()), {}};
            }

            std::vector<std::byte> data(static_cast<std::size_t>(end));
            stream.seekg(0, std::ios::beg);
            if (!data.empty()) {
                stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
            }
            if (!stream) {
                return {Result::failure(ResultCode::kBackendError,
                                         "Unable to read shader package: " + path.string()), {}};
            }
            return {Result::success(), std::move(data)};
        }

        [[nodiscard]] Result invalid_package(std::string message) {
            return Result::failure(ResultCode::kInvalidArgument, std::move(message));
        }

        [[nodiscard]] Result validate_package_regions(const std::vector<std::byte>& data,
                                                      const asset::FileHeader& header) {
            if (header.magic != asset::kMagic) {
                return invalid_package("Shader package magic is invalid");
            }
            if (header.version != asset::kVersion) {
                return invalid_package("Shader package version is unsupported");
            }
            if (header.header_size != sizeof(asset::FileHeader) || data.size() < header.header_size) {
                return invalid_package("Shader package header size is invalid");
            }
            if (header.total_size != data.size()) {
                return invalid_package("Shader package total size does not match the file");
            }
            if (!checked_region(data.size(), header.string_offset, header.string_size)) {
                return invalid_package("Shader package string table is out of bounds");
            }
            if (!string_region(data, header, header.program_name_offset, header.program_name_size)) {
                return invalid_package("Shader package program name is out of bounds");
            }
            if (!checked_array_region(data.size(), header.stage_offset, header.stage_count,
                                      sizeof(asset::StageRecord)) ||
                !checked_array_region(data.size(), header.resource_offset, header.resource_count,
                                      sizeof(asset::ResourceRecord)) ||
                !checked_array_region(data.size(), header.vertex_input_offset, header.vertex_input_count,
                                      sizeof(asset::VertexInputRecord)) ||
                !checked_array_region(data.size(), header.constant_range_offset, header.constant_range_count,
                                      sizeof(asset::ConstantRangeRecord))) {
                return invalid_package("Shader package record region is out of bounds");
            }
            if (header.stage_count == 0) {
                return invalid_package("Shader package contains no shader stages");
            }
            return Result::success();
        }

        [[nodiscard]] Result parse_stage_records(const std::vector<std::byte>& data,
                                                 const asset::FileHeader& header,
                                                 ShaderProgram& program) {
            program.stages.reserve(header.stage_count);
            for (std::uint32_t index = 0; index < header.stage_count; ++index) {
                asset::StageRecord record{};
                std::uint32_t offset = 0;
                if (!checked_record_offset(header.stage_offset, index, sizeof(record), offset) ||
                    !read_object(data, offset, record)) {
                    return invalid_package("Shader stage record cannot be read");
                }

                const ShaderStage stage = static_cast<ShaderStage>(record.stage);
                if (!is_known_shader_stage(stage)) {
                    return invalid_package("Shader package contains an invalid shader stage");
                }
                if (std::any_of(program.stages.begin(), program.stages.end(),
                                [stage](const ShaderStageBinary& existing) { return existing.stage == stage; })) {
                    return invalid_package("Shader package contains duplicate shader stages");
                }
                if (!string_region(data, header, record.entry_point_offset, record.entry_point_size)) {
                    return invalid_package("Shader entry point is out of bounds");
                }
                if (!checked_region(data.size(), record.code_offset, record.code_size) ||
                    record.code_size == 0 || record.code_size % sizeof(std::uint32_t) != 0) {
                    return invalid_package("Shader stage bytecode region is invalid");
                }

                ShaderStageBinary stage_binary{};
                stage_binary.stage = stage;
                stage_binary.entry_point = read_string(data, header, record.entry_point_offset,
                                                       record.entry_point_size);
                stage_binary.code.resize(record.code_size);
                std::memcpy(stage_binary.code.data(), data.data() + record.code_offset, record.code_size);
                program.stages.push_back(std::move(stage_binary));
            }
            return Result::success();
        }

        [[nodiscard]] Result parse_resource_records(const std::vector<std::byte>& data,
                                                    const asset::FileHeader& header,
                                                    ShaderReflection& reflection) {
            reflection.resources.reserve(header.resource_count);
            for (std::uint32_t index = 0; index < header.resource_count; ++index) {
                asset::ResourceRecord record{};
                std::uint32_t offset = 0;
                if (!checked_record_offset(header.resource_offset, index, sizeof(record), offset) ||
                    !read_object(data, offset, record)) {
                    return invalid_package("Shader resource record cannot be read");
                }

                const BindingType type = static_cast<BindingType>(record.type);
                const BindingArrayMode array_mode = static_cast<BindingArrayMode>(record.array_mode);
                if (!is_known_binding_type(type) || !is_known_array_mode(array_mode)) {
                    return invalid_package("Shader package contains an invalid resource type");
                }
                if (record.shader_stages == 0 ||
                    (record.shader_stages & ~kKnownShaderStageBits) != 0 ||
                    !is_known_binding_flags(static_cast<rhi::BindingFlags>(record.flags))) {
                    return invalid_package("Shader resource stage mask is invalid");
                }
                if ((array_mode == BindingArrayMode::kSingle && record.array_count != 1) ||
                    (array_mode == BindingArrayMode::kFixed && record.array_count == 0) ||
                    ((array_mode == BindingArrayMode::kVariable || array_mode == BindingArrayMode::kUnbounded) &&
                     record.array_count != 0)) {
                    return invalid_package("Shader resource array shape is invalid");
                }
                if (!string_region(data, header, record.name_offset, record.name_size)) {
                    return invalid_package("Shader resource name is out of bounds");
                }

                ShaderResourceBinding resource{};
                resource.group = record.group;
                resource.binding = record.binding;
                resource.type = type;
                resource.array_mode = array_mode;
                resource.array_count = record.array_count;
                resource.shader_stages = static_cast<ShaderStage>(record.shader_stages);
                resource.flags = static_cast<rhi::BindingFlags>(record.flags);
                resource.name = read_string(data, header, record.name_offset, record.name_size);
                reflection.resources.push_back(std::move(resource));
            }
            return Result::success();
        }

        [[nodiscard]] Result parse_vertex_records(const std::vector<std::byte>& data,
                                                  const asset::FileHeader& header,
                                                  ShaderReflection& reflection) {
            reflection.vertex_inputs.reserve(header.vertex_input_count);
            for (std::uint32_t index = 0; index < header.vertex_input_count; ++index) {
                asset::VertexInputRecord record{};
                std::uint32_t offset = 0;
                if (!checked_record_offset(header.vertex_input_offset, index, sizeof(record), offset) ||
                    !read_object(data, offset, record)) {
                    return invalid_package("Shader vertex input record cannot be read");
                }
                if (!is_known_format(static_cast<Format>(record.format)) ||
                    !string_region(data, header, record.semantic_offset, record.semantic_size)) {
                    return invalid_package("Shader vertex input record is invalid");
                }

                ShaderVertexInput vertex_input{};
                vertex_input.location = record.location;
                vertex_input.format = static_cast<Format>(record.format);
                vertex_input.semantic_hash = record.semantic_hash;
                vertex_input.semantic_index = record.semantic_index;
                vertex_input.semantic = read_string(data, header, record.semantic_offset,
                                                    record.semantic_size);
                reflection.vertex_inputs.push_back(std::move(vertex_input));
            }
            return Result::success();
        }

        [[nodiscard]] Result parse_constant_records(const std::vector<std::byte>& data,
                                                    const asset::FileHeader& header,
                                                    ShaderReflection& reflection) {
            reflection.constant_ranges.reserve(header.constant_range_count);
            for (std::uint32_t index = 0; index < header.constant_range_count; ++index) {
                asset::ConstantRangeRecord record{};
                std::uint32_t offset = 0;
                if (!checked_record_offset(header.constant_range_offset, index, sizeof(record), offset) ||
                    !read_object(data, offset, record)) {
                    return invalid_package("Shader constant range record cannot be read");
                }
                if (record.size == 0 || record.offset % 4u != 0 || record.size % 4u != 0 ||
                    record.size > std::numeric_limits<std::uint32_t>::max() - record.offset ||
                    record.shader_stages == 0 || (record.shader_stages & ~kKnownShaderStageBits) != 0 ||
                    !string_region(data, header, record.name_offset, record.name_size)) {
                    return invalid_package("Shader constant range record is invalid");
                }

                ShaderConstantRange range{};
                range.offset = record.offset;
                range.size = record.size;
                range.shader_stages = static_cast<ShaderStage>(record.shader_stages);
                range.name = read_string(data, header, record.name_offset, record.name_size);
                reflection.constant_ranges.push_back(std::move(range));
            }
            return Result::success();
        }

        template<typename T>
        void append_hash_value(std::uint64_t& hash, T value) noexcept {
            static_assert(std::is_trivially_copyable_v<T>);
            std::byte bytes[sizeof(T)];
            std::memcpy(bytes, &value, sizeof(value));
            for (const std::byte byte : bytes) {
                hash ^= static_cast<std::uint8_t>(byte);
                hash *= 1099511628211ull;
            }
        }

        void append_hash_string(std::uint64_t& hash, std::string_view value) noexcept {
            append_hash_value(hash, static_cast<std::uint32_t>(value.size()));
            for (const char character : value) {
                append_hash_value(hash, static_cast<std::uint8_t>(character));
            }
        }

        [[nodiscard]] std::string make_layout_key(
                std::uint16_t group, std::span<const BindingLayoutEntry> entries) {
            std::string key;
            key.resize(sizeof(group) + sizeof(std::uint32_t) + entries.size() * 20u);
            std::size_t offset = 0;
            const auto append = [&key, &offset](const auto& value) {
                std::memcpy(key.data() + offset, &value, sizeof(value));
                offset += sizeof(value);
            };
            append(group);
            const auto count = static_cast<std::uint32_t>(entries.size());
            append(count);
            for (const BindingLayoutEntry& entry : entries) {
                append(entry.location.binding);
                append(static_cast<std::uint8_t>(entry.type));
                append(static_cast<std::uint8_t>(entry.array_mode));
                append(entry.array_count);
                append(static_cast<std::uint32_t>(entry.shader_stages));
                append(static_cast<std::uint32_t>(entry.flags));
            }
            key.resize(offset);
            return key;
        }

        [[nodiscard]] Result validate_reflection(const ShaderProgram& program) {
            if (program.name.empty()) {
                return Result::failure(ResultCode::kInvalidArgument, "Shader program name must not be empty");
            }
            if (program.stages.empty()) {
                return Result::failure(ResultCode::kInvalidArgument, "Shader program contains no shader stages");
            }

            std::uint32_t available_stages = 0;
            for (const ShaderStageBinary& stage : program.stages) {
                if (!is_known_shader_stage(stage.stage)) {
                    return Result::failure(ResultCode::kInvalidArgument, "Shader program contains an invalid stage");
                }
                const std::uint32_t stage_value = static_cast<std::uint32_t>(stage.stage);
                if ((available_stages & stage_value) != 0) {
                    return Result::failure(ResultCode::kInvalidArgument,
                                           "Shader program contains duplicate stages");
                }
                available_stages |= stage_value;
                if (stage.entry_point.empty() || stage.code.empty() || stage.code.size() % sizeof(std::uint32_t) != 0) {
                    return Result::failure(ResultCode::kInvalidArgument, "Shader stage binary is invalid");
                }
            }

            std::map<std::pair<std::uint16_t, std::uint16_t>, const ShaderResourceBinding*> resource_locations;
            for (const ShaderResourceBinding& resource : program.contract.reflection.resources) {
                if (!is_known_binding_type(resource.type) || !is_known_array_mode(resource.array_mode) ||
                    !is_known_binding_flags(resource.flags)) {
                    return Result::failure(ResultCode::kInvalidArgument, "Shader resource description is invalid");
                }
                if (resource.shader_stages == ShaderStage::kNone ||
                    (static_cast<std::uint32_t>(resource.shader_stages) & ~kKnownShaderStageBits) != 0 ||
                    (static_cast<std::uint32_t>(resource.shader_stages) & available_stages) == 0) {
                    return Result::failure(ResultCode::kInvalidArgument,
                                           "Shader resource stage mask does not match program stages");
                }
                if ((resource.array_mode == BindingArrayMode::kSingle && resource.array_count != 1) ||
                    (resource.array_mode == BindingArrayMode::kFixed && resource.array_count == 0) ||
                    ((resource.array_mode == BindingArrayMode::kVariable ||
                      resource.array_mode == BindingArrayMode::kUnbounded) && resource.array_count != 0)) {
                    return Result::failure(ResultCode::kInvalidArgument, "Shader resource array shape is invalid");
                }
                const auto [iterator, inserted] = resource_locations.emplace(
                        std::pair{resource.group, resource.binding}, &resource);
                if (!inserted) {
                    return Result::failure(ResultCode::kInvalidArgument,
                                           "Shader contract contains duplicate resource locations");
                }
                (void) iterator;
            }

            std::set<std::uint32_t> vertex_locations;
            for (const ShaderVertexInput& input : program.contract.reflection.vertex_inputs) {
                if (!is_known_format(input.format) || !vertex_locations.insert(input.location).second) {
                    return Result::failure(ResultCode::kInvalidArgument,
                                           "Shader contract contains invalid vertex inputs");
                }
            }
            if (!program.contract.reflection.vertex_inputs.empty() &&
                (available_stages & static_cast<std::uint32_t>(ShaderStage::kVertex)) == 0) {
                return Result::failure(ResultCode::kInvalidArgument,
                                       "Vertex inputs require a vertex shader stage");
            }

            std::vector<const ShaderConstantRange*> constant_ranges;
            constant_ranges.reserve(program.contract.reflection.constant_ranges.size());
            for (const ShaderConstantRange& range : program.contract.reflection.constant_ranges) {
                if (range.size == 0 || range.offset % 4u != 0 || range.size % 4u != 0 ||
                    range.size > std::numeric_limits<std::uint32_t>::max() - range.offset ||
                    range.shader_stages == ShaderStage::kNone ||
                    (static_cast<std::uint32_t>(range.shader_stages) & ~kKnownShaderStageBits) != 0 ||
                    (static_cast<std::uint32_t>(range.shader_stages) & available_stages) == 0) {
                    return Result::failure(ResultCode::kInvalidArgument,
                                           "Shader contract contains an invalid constant range");
                }
                constant_ranges.push_back(&range);
            }
            std::sort(constant_ranges.begin(), constant_ranges.end(),
                      [](const ShaderConstantRange* lhs, const ShaderConstantRange* rhs) {
                          return std::pair{lhs->offset, lhs->size} < std::pair{rhs->offset, rhs->size};
                      });
            std::uint64_t previous_end = 0;
            bool has_previous_range = false;
            for (const ShaderConstantRange* range : constant_ranges) {
                if (has_previous_range && range->offset < previous_end) {
                    return Result::failure(ResultCode::kInvalidArgument,
                                           "Shader contract contains overlapping constant ranges");
                }
                previous_end = static_cast<std::uint64_t>(range->offset) + range->size;
                has_previous_range = true;
            }
            return Result::success();
        }
    } // namespace

    bool ShaderPipelineLayout::is_valid() const noexcept {
        return owner_ != nullptr;
    }

    std::span<const rhi::BindingLayoutHandle> ShaderPipelineLayout::binding_layouts() const noexcept {
        return binding_layouts_;
    }

    std::span<const rhi::InlineConstantRange> ShaderPipelineLayout::inline_constant_ranges() const noexcept {
        return inline_constant_ranges_;
    }

    rhi::PipelineLayoutDesc ShaderPipelineLayout::desc() const noexcept {
        return rhi::PipelineLayoutDesc{binding_layouts_, inline_constant_ranges_};
    }

    ResultValue<ShaderProgram> load_shader_program(const std::filesystem::path& path) {
        auto file_result = read_file(path);
        if (!file_result) {
            return {file_result.result, {}};
        }

        const std::vector<std::byte>& data = file_result.value;
        asset::FileHeader header{};
        if (!read_object(data, 0, header)) {
            return {invalid_package("Shader package header cannot be read"), {}};
        }
        const Result region_result = validate_package_regions(data, header);
        if (!region_result) {
            return {region_result, {}};
        }

        ShaderProgram program{};
        program.name = read_string(data, header, header.program_name_offset, header.program_name_size);
        if (program.name.empty()) {
            return {invalid_package("Shader package program name is empty"), {}};
        }
        if (const Result result = parse_stage_records(data, header, program); !result) {
            return {result, {}};
        }
        if (const Result result = parse_resource_records(data, header, program.contract.reflection); !result) {
            return {result, {}};
        }
        if (const Result result = parse_vertex_records(data, header, program.contract.reflection); !result) {
            return {result, {}};
        }
        if (const Result result = parse_constant_records(data, header, program.contract.reflection); !result) {
            return {result, {}};
        }

        const Result validation = validate_reflection(program);
        if (!validation) {
            return {validation, {}};
        }
        return {Result::success(), std::move(program)};
    }

    ShaderManager::ShaderManager(rhi::Device& device) : device_(device) {
    }

    ShaderManager::~ShaderManager() {
        (void) clear_layout_cache();
    }

    Result ShaderManager::validate(const ShaderProgram& program) const {
        return validate_reflection(program);
    }

    std::uint64_t ShaderManager::hash_contract(const ShaderProgram& program) const {
        std::uint64_t hash = 1469598103934665603ull;
        std::vector<const ShaderResourceBinding*> resources;
        resources.reserve(program.contract.reflection.resources.size());
        for (const ShaderResourceBinding& resource : program.contract.reflection.resources) {
            resources.push_back(&resource);
        }
        std::sort(resources.begin(), resources.end(), [](const ShaderResourceBinding* lhs,
                                                         const ShaderResourceBinding* rhs) {
            return std::pair{lhs->group, lhs->binding} < std::pair{rhs->group, rhs->binding};
        });
        for (const ShaderResourceBinding* resource : resources) {
            append_hash_value(hash, resource->group);
            append_hash_value(hash, resource->binding);
            append_hash_value(hash, static_cast<std::uint8_t>(resource->type));
            append_hash_value(hash, static_cast<std::uint8_t>(resource->array_mode));
            append_hash_value(hash, resource->array_count);
            append_hash_value(hash, static_cast<std::uint32_t>(resource->shader_stages));
            append_hash_value(hash, static_cast<std::uint32_t>(resource->flags));
            append_hash_string(hash, resource->name);
        }
        std::vector<const ShaderVertexInput*> vertex_inputs;
        vertex_inputs.reserve(program.contract.reflection.vertex_inputs.size());
        for (const ShaderVertexInput& input : program.contract.reflection.vertex_inputs) {
            vertex_inputs.push_back(&input);
        }
        std::sort(vertex_inputs.begin(), vertex_inputs.end(), [](const ShaderVertexInput* lhs,
                                                                const ShaderVertexInput* rhs) {
            return lhs->location < rhs->location;
        });
        for (const ShaderVertexInput* input : vertex_inputs) {
            append_hash_value(hash, input->location);
            append_hash_value(hash, static_cast<std::uint16_t>(input->format));
            append_hash_value(hash, input->semantic_hash);
            append_hash_value(hash, input->semantic_index);
            append_hash_string(hash, input->semantic);
        }
        std::vector<const ShaderConstantRange*> constant_ranges;
        constant_ranges.reserve(program.contract.reflection.constant_ranges.size());
        for (const ShaderConstantRange& range : program.contract.reflection.constant_ranges) {
            constant_ranges.push_back(&range);
        }
        std::sort(constant_ranges.begin(), constant_ranges.end(), [](const ShaderConstantRange* lhs,
                                                                     const ShaderConstantRange* rhs) {
            return std::pair{lhs->offset, lhs->size} < std::pair{rhs->offset, rhs->size};
        });
        for (const ShaderConstantRange* range : constant_ranges) {
            append_hash_value(hash, range->offset);
            append_hash_value(hash, range->size);
            append_hash_value(hash, static_cast<std::uint32_t>(range->shader_stages));
            append_hash_string(hash, range->name);
        }
        return hash;
    }

    Result ShaderManager::validate_layout_owner(const ShaderPipelineLayout& layout) const {
        if (layout.owner_ != this || !layout.is_valid()) {
            return Result::failure(ResultCode::kInvalidState,
                                   "Shader pipeline layout belongs to a different shader manager");
        }
        for (const rhi::BindingLayoutHandle handle : layout.binding_layouts_) {
            if (!handle.is_valid()) {
                return Result::failure(ResultCode::kInvalidState,
                                       "Shader pipeline layout contains an invalid binding layout");
            }
        }
        return Result::success();
    }

    ResultValue<ShaderPipelineLayout> ShaderManager::build_pipeline_layout(
            const ShaderProgram& program, rhi::DebugName name) {
        const Result validation = validate(program);
        if (!validation) {
            return {validation, {}};
        }

        std::map<std::uint16_t, std::vector<const ShaderResourceBinding*>> resources_by_group;
        for (const ShaderResourceBinding& resource : program.contract.reflection.resources) {
            resources_by_group[resource.group].push_back(&resource);
        }

        if (!resources_by_group.empty()) {
            std::uint16_t expected_group = 0;
            for (const auto& [group, resources] : resources_by_group) {
                if (group != expected_group) {
                    return {Result::failure(ResultCode::kInvalidArgument,
                                            "Shader contract binding groups must be contiguous and start at zero"),
                            {}};
                }
                (void) resources;
                if (expected_group == std::numeric_limits<std::uint16_t>::max()) {
                    break;
                }
                ++expected_group;
            }
        }

        ShaderPipelineLayout layout{};
        layout.owner_ = this;
        layout.contract_hash_ = hash_contract(program);
        std::vector<std::string> newly_created_keys;
        newly_created_keys.reserve(resources_by_group.size());

        for (const auto& [group, resource_list] : resources_by_group) {
            std::vector<BindingLayoutEntry> entries;
            entries.reserve(resource_list.size());
            for (const ShaderResourceBinding* resource : resource_list) {
                BindingLayoutEntry entry{};
                entry.location = rhi::BindingLocation{group, resource->binding};
                entry.type = resource->type;
                entry.array_mode = resource->array_mode;
                entry.array_count = resource->array_count;
                entry.shader_stages = resource->shader_stages;
                entry.flags = resource->flags;
                entries.push_back(entry);
            }
            std::sort(entries.begin(), entries.end(), [](const BindingLayoutEntry& lhs,
                                                         const BindingLayoutEntry& rhs) {
                return lhs.location.binding < rhs.location.binding;
            });

            std::string key = make_layout_key(group, entries);
            const auto cached = layout_cache_.find(key);
            if (cached != layout_cache_.end()) {
                layout.binding_layouts_.push_back(cached->second);
                continue;
            }

            const rhi::BindingLayoutDesc binding_desc{entries, name};
            auto binding_result = device_.create_binding_layout(binding_desc);
            if (!binding_result) {
                for (const std::string& created_key : newly_created_keys) {
                    const auto created = layout_cache_.find(created_key);
                    if (created != layout_cache_.end()) {
                        (void) device_.destroy_binding_layout(created->second);
                        layout_cache_.erase(created);
                    }
                }
                return {binding_result.result, {}};
            }

            const auto [created, inserted] =
                    layout_cache_.emplace(std::move(key), binding_result.value);
            if (!inserted) {
                (void) device_.destroy_binding_layout(binding_result.value);
                layout.binding_layouts_.push_back(created->second);
                continue;
            }
            newly_created_keys.push_back(created->first);
            layout.binding_layouts_.push_back(created->second);
        }

        layout.inline_constant_ranges_.reserve(program.contract.reflection.constant_ranges.size());
        for (const ShaderConstantRange& range : program.contract.reflection.constant_ranges) {
            layout.inline_constant_ranges_.push_back(rhi::InlineConstantRange{
                    range.shader_stages, range.offset, range.size
            });
        }
        return {Result::success(), std::move(layout)};
    }

    ResultValue<rhi::BindingLayoutHandle> ShaderManager::binding_layout(
            const ShaderPipelineLayout& layout, std::uint32_t group) const {
        const Result owner_result = validate_layout_owner(layout);
        if (!owner_result) {
            return {owner_result, {}};
        }
        if (group >= layout.binding_layouts_.size()) {
            return {Result::failure(ResultCode::kNotFound,
                                    "Shader pipeline layout group is not present"), {}};
        }
        return {Result::success(), layout.binding_layouts_[group]};
    }

    Result ShaderManager::validate_vertex_inputs(const ShaderProgram& program,
                                                 const rhi::GraphicsPipelineState& graphics) const {
        for (const ShaderVertexInput& input : program.contract.reflection.vertex_inputs) {
            const auto attribute = std::find_if(
                    graphics.vertex_attributes.begin(), graphics.vertex_attributes.end(),
                    [&input](const rhi::VertexAttributeDesc& candidate) {
                        return candidate.location == input.location;
                    });
            if (attribute == graphics.vertex_attributes.end()) {
                return Result::failure(ResultCode::kInvalidArgument,
                                       "Graphics pipeline is missing a reflected vertex input");
            }
            if (attribute->format != input.format) {
                return Result::failure(ResultCode::kInvalidArgument,
                                       "Graphics pipeline vertex input format disagrees with shader reflection");
            }
        }
        return Result::success();
    }

    ResultValue<rhi::PipelineHandle> ShaderManager::create_pipeline(
            const ShaderProgram& program, const ShaderPipelineLayout& layout,
            const ShaderPipelineDesc& desc) {
        const Result validation = validate(program);
        if (!validation) {
            return {validation, {}};
        }
        const Result owner_result = validate_layout_owner(layout);
        if (!owner_result) {
            return {owner_result, {}};
        }
        if (layout.contract_hash_ != hash_contract(program)) {
            return {Result::failure(ResultCode::kInvalidArgument,
                                    "Shader pipeline layout was built from a different shader contract"), {}};
        }
        if (desc.type == rhi::PipelineType::kGraphics) {
            if (const Result vertex_result = validate_vertex_inputs(program, desc.graphics); !vertex_result) {
                return {vertex_result, {}};
            }
        }

        std::vector<rhi::ShaderStageDesc> shader_stages;
        shader_stages.reserve(program.stages.size());
        for (const ShaderStageBinary& stage : program.stages) {
            shader_stages.push_back(rhi::ShaderStageDesc{
                    stage.stage,
                    std::span<const std::byte>{stage.code.data(), stage.code.size()},
                    stage.entry_point,
                    desc.name
            });
        }

        rhi::PipelineDesc pipeline_desc{};
        pipeline_desc.type = desc.type;
        pipeline_desc.layout = layout.desc();
        pipeline_desc.shader_stages = shader_stages;
        pipeline_desc.graphics = desc.graphics;
        pipeline_desc.name = desc.name;
        return device_.create_pipeline(pipeline_desc);
    }

    Result ShaderManager::clear_layout_cache() {
        Result first_error = Result::success();
        for (auto iterator = layout_cache_.begin(); iterator != layout_cache_.end();) {
            const Result result = device_.destroy_binding_layout(iterator->second);
            if (result || result.code == ResultCode::kNotFound) {
                iterator = layout_cache_.erase(iterator);
                continue;
            }
            if (first_error) {
                first_error = result;
            }
            ++iterator;
        }
        return first_error;
    }
} // namespace aeg::shader
