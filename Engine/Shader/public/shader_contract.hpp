#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "result.hpp"
#include "rhi.hpp"

namespace aeg::shader {
    struct ShaderResourceBinding {
        std::uint16_t group{0};
        std::uint16_t binding{0};
        rhi::BindingType type{rhi::BindingType::kSampledTexture};
        rhi::BindingArrayMode array_mode{rhi::BindingArrayMode::kSingle};
        std::uint32_t array_count{1};
        rhi::ShaderStageMask shader_stages{rhi::ShaderStage::kNone};
        rhi::BindingFlags flags{rhi::BindingFlags::kNone};
        std::string name{};
    };

    struct ShaderVertexInput {
        std::uint32_t location{0};
        rhi::Format format{rhi::Format::kUnknown};
        std::uint32_t semantic_hash{0};
        std::uint32_t semantic_index{0};
        std::string semantic{};
    };

    struct ShaderConstantRange {
        std::uint32_t offset{0};
        std::uint32_t size{0};
        rhi::ShaderStageMask shader_stages{rhi::ShaderStage::kNone};
        std::string name{};
    };

    struct ShaderReflection {
        std::vector<ShaderResourceBinding> resources{};
        std::vector<ShaderVertexInput> vertex_inputs{};
        std::vector<ShaderConstantRange> constant_ranges{};

        [[nodiscard]] bool empty() const noexcept {
            return resources.empty() && vertex_inputs.empty() && constant_ranges.empty();
        }
    };

    struct ShaderContract {
        ShaderReflection reflection{};
    };

    struct ShaderStageBinary {
        rhi::ShaderStage stage{rhi::ShaderStage::kNone};
        std::string entry_point{"main"};
        std::vector<std::byte> code{};
    };

    struct ShaderProgram {
        std::string name{};
        std::vector<ShaderStageBinary> stages{};
        ShaderContract contract{};

        [[nodiscard]] bool is_valid() const noexcept {
            return !name.empty() && !stages.empty();
        }
    };

    struct ShaderPipelineDesc {
        rhi::PipelineType type{rhi::PipelineType::kGraphics};
        rhi::GraphicsPipelineState graphics{};
        rhi::DebugName name{};
    };

    class ShaderManager;

    class ShaderPipelineLayout {
    public:
        ShaderPipelineLayout() = default;

        [[nodiscard]] bool is_valid() const noexcept;

        [[nodiscard]] std::span<const rhi::BindingLayoutHandle> binding_layouts() const noexcept;

        [[nodiscard]] std::span<const rhi::InlineConstantRange> inline_constant_ranges() const noexcept;

        [[nodiscard]] rhi::PipelineLayoutDesc desc() const noexcept;

        [[nodiscard]] std::uint64_t contract_hash() const noexcept { return contract_hash_; }

    private:
        friend class ShaderManager;

        const ShaderManager* owner_{nullptr};
        std::uint64_t contract_hash_{0};
        std::vector<rhi::BindingLayoutHandle> binding_layouts_{};
        std::vector<rhi::InlineConstantRange> inline_constant_ranges_{};
    };

    [[nodiscard]] foundation::ResultValue<ShaderProgram> load_shader_program(
            const std::filesystem::path& path);

    class ShaderManager {
    public:
        explicit ShaderManager(rhi::Device& device);
        ~ShaderManager();

        ShaderManager(const ShaderManager&) = delete;
        ShaderManager& operator=(const ShaderManager&) = delete;
        ShaderManager(ShaderManager&&) = delete;
        ShaderManager& operator=(ShaderManager&&) = delete;

        [[nodiscard]] foundation::Result validate(const ShaderProgram& program) const;

        [[nodiscard]] foundation::ResultValue<ShaderPipelineLayout> build_pipeline_layout(
                const ShaderProgram& program, rhi::DebugName name = {});

        [[nodiscard]] foundation::ResultValue<rhi::BindingLayoutHandle> binding_layout(
                const ShaderPipelineLayout& layout, std::uint32_t group) const;

        [[nodiscard]] foundation::ResultValue<rhi::PipelineHandle> create_pipeline(
                const ShaderProgram& program, const ShaderPipelineLayout& layout,
                const ShaderPipelineDesc& desc);

        [[nodiscard]] foundation::Result clear_layout_cache();

    private:
        [[nodiscard]] foundation::Result validate_layout_owner(const ShaderPipelineLayout& layout) const;

        [[nodiscard]] foundation::Result validate_vertex_inputs(
                const ShaderProgram& program, const rhi::GraphicsPipelineState& graphics) const;

        [[nodiscard]] std::uint64_t hash_contract(const ShaderProgram& program) const;

        rhi::Device& device_;
        std::unordered_map<std::string, rhi::BindingLayoutHandle> layout_cache_{};
    };
} // namespace aeg::shader
