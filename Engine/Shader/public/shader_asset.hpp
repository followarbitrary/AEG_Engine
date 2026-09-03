#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace aeg::shader::asset {
    inline constexpr std::uint32_t kMagic = 0x31474541u;
    inline constexpr std::uint16_t kVersion = 2u;

    struct FileHeader {
        std::uint32_t magic{0};
        std::uint16_t version{0};
        std::uint16_t flags{0};
        std::uint32_t header_size{0};
        std::uint64_t total_size{0};
        std::uint32_t program_name_offset{0};
        std::uint32_t program_name_size{0};
        std::uint32_t stage_offset{0};
        std::uint32_t stage_count{0};
        std::uint32_t resource_offset{0};
        std::uint32_t resource_count{0};
        std::uint32_t vertex_input_offset{0};
        std::uint32_t vertex_input_count{0};
        std::uint32_t constant_range_offset{0};
        std::uint32_t constant_range_count{0};
        std::uint32_t string_offset{0};
        std::uint32_t string_size{0};
    };

    struct StageRecord {
        std::uint32_t stage{0};
        std::uint32_t entry_point_offset{0};
        std::uint32_t entry_point_size{0};
        std::uint32_t code_offset{0};
        std::uint32_t code_size{0};
        std::uint32_t reserved{0};
    };

    struct ResourceRecord {
        std::uint16_t group{0};
        std::uint16_t binding{0};
        std::uint8_t type{0};
        std::uint8_t array_mode{0};
        std::uint32_t flags{0};
        std::uint32_t array_count{0};
        std::uint32_t shader_stages{0};
        std::uint32_t name_offset{0};
        std::uint32_t name_size{0};
        std::uint32_t reserved{0};
    };

    struct VertexInputRecord {
        std::uint32_t location{0};
        std::uint16_t format{0};
        std::uint16_t reserved{0};
        std::uint32_t semantic_hash{0};
        std::uint32_t semantic_index{0};
        std::uint32_t semantic_offset{0};
        std::uint32_t semantic_size{0};
    };

    struct ConstantRangeRecord {
        std::uint32_t offset{0};
        std::uint32_t size{0};
        std::uint32_t shader_stages{0};
        std::uint32_t name_offset{0};
        std::uint32_t name_size{0};
        std::uint32_t reserved{0};
    };

    static_assert(std::is_standard_layout_v<FileHeader>);
    static_assert(std::is_standard_layout_v<StageRecord>);
    static_assert(std::is_standard_layout_v<ResourceRecord>);
    static_assert(std::is_standard_layout_v<VertexInputRecord>);
    static_assert(std::is_standard_layout_v<ConstantRangeRecord>);
    static_assert(sizeof(FileHeader) == 72u);
    static_assert(sizeof(StageRecord) == 24u);
    static_assert(sizeof(ResourceRecord) == 32u);
    static_assert(sizeof(VertexInputRecord) == 24u);
    static_assert(sizeof(ConstantRangeRecord) == 24u);
} // namespace aeg::shader::asset
