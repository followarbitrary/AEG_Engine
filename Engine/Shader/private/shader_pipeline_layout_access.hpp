/**
 * @file shader_pipeline_layout_access.hpp
 * @brief Shader-private access bridge for ShaderPipelineLayout storage.
 */
#pragma once

#include "../public/shader_contract.hpp"

#include <memory>
#include <vector>

namespace aeg::shader::detail {

struct ShaderPipelineLayoutAccess final {
    static void initialize(ShaderPipelineLayout& layout, const ShaderManager* owner,
                           std::uint64_t cache_epoch, std::uint64_t contract_hash,
                           std::weak_ptr<const std::uint8_t> lifetime) noexcept;
    [[nodiscard]] static bool belongs_to(const ShaderPipelineLayout& layout,
                                         const ShaderManager* owner) noexcept;
    [[nodiscard]] static bool is_valid(const ShaderPipelineLayout& layout) noexcept;
    [[nodiscard]] static std::uint64_t cache_epoch(const ShaderPipelineLayout& layout) noexcept;
    [[nodiscard]] static std::uint64_t contract_hash(const ShaderPipelineLayout& layout) noexcept;
    [[nodiscard]] static std::vector<rhi::BindingLayoutHandle>& binding_layouts(ShaderPipelineLayout& layout) noexcept;
    [[nodiscard]] static const std::vector<rhi::BindingLayoutHandle>& binding_layouts(
        const ShaderPipelineLayout& layout) noexcept;
    [[nodiscard]] static std::vector<rhi::InlineConstantRange>& inline_constant_ranges(
        ShaderPipelineLayout& layout) noexcept;
};

}  // namespace aeg::shader::detail
