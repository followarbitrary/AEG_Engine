/**
 * @file shader_contract.hpp
 * @brief Runtime shader packages, reflection contracts, layout caching, and pipeline creation.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "parameter_value.hpp"
#include "result.hpp"
#include "rhi.hpp"

namespace aeg::shader {
/** @brief Reflected declaration of one shader resource binding. */
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

/** @brief Reflected declaration of one vertex shader input. */
struct ShaderVertexInput {
    std::uint32_t location{0};
    rhi::Format format{rhi::Format::kUnknown};
    std::uint32_t semantic_hash{0};
    std::uint32_t semantic_index{0};
    std::string semantic{};
};

/** @brief Reflected inline constant range shared by one or more shader stages. */
struct ShaderConstantRange {
    std::uint32_t offset{0};
    std::uint32_t size{0};
    rhi::ShaderStageMask shader_stages{rhi::ShaderStage::kNone};
    std::string name{};
};

/** @brief Scalar representation used by a shader-owned material parameter block. */
enum class ShaderParameterType : std::uint8_t { kBool, kInt32, kUInt32, kFloat32, kFloat32x2, kFloat32x3, kFloat32x4 };

/** @brief Reflected field in the material parameter constant buffer. */
struct ShaderParameterField {
    std::uint64_t id{0};
    ShaderParameterType type{ShaderParameterType::kFloat32};
    std::uint32_t offset{0};
    std::uint32_t size{0};
    std::vector<std::byte> default_value{};
    std::string name{};
};

/** @brief Complete normalized reflection data for a shader program. */
struct ShaderReflection {
    std::vector<ShaderResourceBinding> resources{};
    std::vector<ShaderVertexInput> vertex_inputs{};
    std::vector<ShaderConstantRange> constant_ranges{};
    std::vector<ShaderParameterField> parameters{};

    /** @brief Reports whether the program declares no reflected interface. */
    [[nodiscard]] bool empty() const noexcept {
        return resources.empty() && vertex_inputs.empty() && constant_ranges.empty() && parameters.empty();
    }
};

/**
 * @brief Backend-neutral interface contract consumed by layout generation.
 *
 * The contract is derived from reflection and is validated before pipeline
 * layouts or pipelines are created.
 */
struct ShaderContract {
    ShaderReflection reflection{};
};

/** @brief Owned compiled bytecode and entry point for one shader stage. */
struct ShaderStageBinary {
    rhi::ShaderStage stage{rhi::ShaderStage::kNone};
    std::string entry_point{"main"};
    std::vector<std::byte> code{};
};

/** @brief Loaded shader program containing stage binaries and a reflection contract. */
struct ShaderProgram {
    std::string name{};
    std::vector<ShaderStageBinary> stages{};
    ShaderContract contract{};

    /** @brief Reports whether the program has a name and at least one stage. */
    [[nodiscard]] bool is_valid() const noexcept { return !name.empty() && !stages.empty(); }
};

/** @brief Runtime pipeline options not encoded in shader reflection. */
struct ShaderPipelineDesc {
    rhi::PipelineType type{rhi::PipelineType::kGraphics};
    rhi::GraphicsPipelineState graphics{};
    rhi::DebugName name{};
};

/** @brief Value metadata for publishing a shader layout's contract and cache generation. */
struct ShaderPipelineLayoutIdentity {
    std::uint64_t contract_hash{0};
    std::uint64_t cache_epoch{0};
};

namespace detail {
struct ShaderPipelineLayoutAccess;
}  // namespace detail

/**
 * @brief Pipeline-layout data generated from a validated shader contract.
 *
 * Binding layouts are owned by the ShaderManager cache. The layout remains
 * usable only while its owner exists and has not cleared the referenced
 * cached layouts.
 */
class ShaderPipelineLayout {
public:
    ShaderPipelineLayout() = default;

    [[nodiscard]] bool is_valid() const noexcept;

    /** @brief Returns binding layout handles ordered by binding group. */
    [[nodiscard]] std::span<const rhi::BindingLayoutHandle> binding_layouts() const noexcept;

    /** @brief Returns merged inline constant ranges for the contract. */
    [[nodiscard]] std::span<const rhi::InlineConstantRange> inline_constant_ranges() const noexcept;

    /** @brief Builds a non-owning RHI pipeline-layout description. */
    [[nodiscard]] rhi::PipelineLayoutDesc desc() const noexcept;

private:
    friend struct detail::ShaderPipelineLayoutAccess;

    const void* owner_{nullptr};
    std::weak_ptr<const std::uint8_t> owner_lifetime_{};
    std::uint64_t cache_epoch_{0};
    std::uint64_t contract_hash_{0};
    std::vector<rhi::BindingLayoutHandle> binding_layouts_{};
    std::vector<rhi::InlineConstantRange> inline_constant_ranges_{};
};

/** @brief Loads and validates a compiled `.aegshader` package. */
[[nodiscard]] foundation::ResultValue<ShaderProgram> load_shader_program(const std::filesystem::path& path);
/** @brief Loads and validates a compiled shader package already resident in memory. */
[[nodiscard]] foundation::ResultValue<ShaderProgram> load_shader_program(std::span<const std::byte> bytes);

/** @brief Validates authored material values against reflected shader fields. */
[[nodiscard]] foundation::Result validate_parameter_block(const ShaderProgram& program,
                                                          const foundation::ParameterBlock& values);

/** @brief Packs reflected numeric material values into the shader constant-buffer ABI. */
[[nodiscard]] foundation::ResultValue<std::vector<std::byte>> pack_parameter_block(
    const ShaderProgram& program, const foundation::ParameterBlock& values);

/**
 * @brief Validates shader contracts, caches layouts, and creates RHI pipelines.
 *
 * The manager borrows its RHI device and must not outlive it. Cached binding
 * layouts are destroyed by clear_layout_cache() or the manager destructor.
 */
class ShaderManager {
public:
    explicit ShaderManager(rhi::Device& device);
    ~ShaderManager();

    ShaderManager(const ShaderManager&) = delete;
    ShaderManager& operator=(const ShaderManager&) = delete;
    ShaderManager(ShaderManager&&) = delete;
    ShaderManager& operator=(ShaderManager&&) = delete;

    /** @brief Validates stage composition and the normalized reflection contract. */
    [[nodiscard]] foundation::Result validate(const ShaderProgram& program) const;

    /** @brief Builds or reuses binding layouts derived from program reflection. */
    [[nodiscard]] foundation::ResultValue<ShaderPipelineLayout> build_pipeline_layout(const ShaderProgram& program,
                                                                                      rhi::DebugName name = {});

    /**
     * @brief Returns identity metadata only for a live layout owned by this manager.
     *
     * The returned value is a snapshot for host-published contracts. A cache reset
     * retires that epoch; querying a foreign, empty, or retired layout fails.
     */
    [[nodiscard]] foundation::ResultValue<ShaderPipelineLayoutIdentity> pipeline_layout_identity(
        const ShaderPipelineLayout& layout) const;

    /** @brief Returns the binding layout for one contract group. */
    [[nodiscard]] foundation::ResultValue<rhi::BindingLayoutHandle> binding_layout(const ShaderPipelineLayout& layout,
                                                                                   std::uint32_t group) const;

    /** @brief Creates an RHI pipeline from bytecode, reflection layout, and runtime state. */
    [[nodiscard]] foundation::ResultValue<rhi::PipelineHandle> create_pipeline(const ShaderProgram& program,
                                                                               const ShaderPipelineLayout& layout,
                                                                               const ShaderPipelineDesc& desc);

    /** @brief Destroys cached binding layouts and invalidates their views. */
    [[nodiscard]] foundation::Result clear_layout_cache();

    /** @brief Returns the current epoch for layouts and Shader/PSO cache entries. */
    [[nodiscard]] std::uint64_t cache_epoch() const noexcept { return cache_epoch_; }

private:
    /** @brief Verifies that a layout belongs to this manager and its cache is live. */
    [[nodiscard]] foundation::Result validate_layout_owner(const ShaderPipelineLayout& layout) const;

    /** @brief Matches reflected vertex inputs against graphics vertex state. */
    [[nodiscard]] foundation::Result validate_vertex_inputs(const ShaderProgram& program,
                                                            const rhi::GraphicsPipelineState& graphics) const;

    /** @brief Computes the stable normalized hash used for layout-cache lookup. */
    [[nodiscard]] std::uint64_t hash_contract(const ShaderProgram& program) const;

    rhi::Device& device_;
    std::shared_ptr<const std::uint8_t> lifetime_token_{};
    std::uint64_t cache_epoch_{1};
    std::unordered_map<std::string, rhi::BindingLayoutHandle> layout_cache_{};
    // Layouts that could not be destroyed during a cache reset remain owned by
    // the manager until their dependent pipelines/sets have been retired. They
    // are deliberately kept out of layout_cache_ so a new epoch can never
    // re-wrap a live old handle.
    std::vector<rhi::BindingLayoutHandle> pending_layout_retirement_{};
};
}  // namespace aeg::shader
