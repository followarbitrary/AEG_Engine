#include <array>
#include <cstdint>
#include <iostream>

#include "renderer_material_contract.hpp"
#include "renderer_profile_contract.hpp"

namespace {
using aeg::renderer::RendererProfile;
using aeg::renderer::RendererProfilePsoContract;
using aeg::renderer::RendererProfileShaderContract;
using aeg::renderer::RenderMaterialPipelineVariant;

bool require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}
}  // namespace

int main() {
    using aeg::rhi::Format;
    using aeg::rhi::SampleCount;

    RendererProfilePsoContract shadow{};
    shadow.shader = {aeg::renderer::kRendererProfileContractVersion, RendererProfile::kShadow,
                     0x123456789abcdef0ull, 19};
    shadow.cache_epoch = 19;
    shadow.attachment = {Format::kUnknown, SampleCount::kX1, Format::kD32Float};
    for (const auto profile : {RendererProfile::kUnknown, static_cast<RendererProfile>(0xff)}) {
        auto invalid = shadow;
        invalid.shader.profile = profile;
        if (!require(!invalid.shader.is_valid() && !invalid.is_valid(profile),
                     "Shadow shader/PSO contract accepted an unknown or invalid profile"))
            return 29;
    }
    if (!require(shadow.attachment.is_valid() && shadow.is_valid(RendererProfile::kShadow),
                 "published depth-only Shadow PSO contract must be valid"))
        return 20;

    for (const auto depth : {Format::kD24UnormS8Uint, Format::kD32FloatS8Uint}) {
        auto depth_variant = shadow;
        depth_variant.attachment.depth_stencil_format = depth;
        if (!require(depth_variant.is_valid(RendererProfile::kShadow),
                     "backend-neutral Shadow contract must preserve supported depth formats"))
            return 21;
    }
    for (const auto samples : {SampleCount::kX1, SampleCount::kX2, SampleCount::kX4, SampleCount::kX8,
                               SampleCount::kX16, SampleCount::kX32, SampleCount::kX64}) {
        auto sampled_shadow = shadow;
        sampled_shadow.attachment.samples = samples;
        if (!require(sampled_shadow.is_valid(RendererProfile::kShadow),
                     "backend-neutral Shadow contract must preserve supported sample counts"))
            return 22;
    }
    for (const auto samples : {0u, 3u, 255u}) {
        auto invalid = shadow;
        invalid.attachment.samples = static_cast<SampleCount>(samples);
        if (!require(!invalid.is_valid(RendererProfile::kShadow),
                     "Shadow PSO contract accepted an invalid sample count"))
            return 23;
    }
    for (const auto depth : {Format::kUnknown, Format::kRgba8Unorm, static_cast<Format>(0xffff)}) {
        auto invalid = shadow;
        invalid.attachment.depth_stencil_format = depth;
        if (!require(!invalid.attachment.is_valid() && !invalid.is_valid(RendererProfile::kShadow),
                     "depth-only attachment accepted a missing or non-depth format"))
            return 24;
    }
    if (!require(!aeg::renderer::RenderMaterialAttachmentContract{}.is_valid(),
                 "an empty graphics attachment contract must be invalid"))
        return 25;
    for (const auto colors : {std::array{Format::kRgba16Float, Format::kUnknown, Format::kUnknown},
                              std::array{Format::kUnknown, Format::kRgba16Float, Format::kUnknown},
                              std::array{Format::kUnknown, Format::kUnknown, Format::kRgba16Float},
                              std::array{Format::kRgba16Float, Format::kRgba16Float, Format::kRgba16Float}}) {
        auto invalid = shadow;
        invalid.attachment.color_format = colors[0];
        invalid.attachment.additional_color_formats = {colors[1], colors[2]};
        if (!require(!invalid.is_valid(RendererProfile::kShadow),
                     "Shadow PSO contract accepted a primary color or additional MRT attachment"))
            return 26;
    }
    for (const auto profile : {RendererProfile::kGBuffer, RendererProfile::kDeferredLighting,
                               RendererProfile::kForwardClustered, RendererProfile::kPostProcess,
                               RendererProfile::kLightCulling}) {
        auto other_profile = shadow;
        other_profile.shader.profile = profile;
        if (!require(!other_profile.is_valid(profile) && !shadow.is_valid(profile),
                     "non-Shadow profile accepted a depth-only or mismatched Shadow PSO"))
            return 27;
        other_profile.attachment.color_format = Format::kRgba16Float;
        if (profile == RendererProfile::kGBuffer)
            other_profile.attachment.additional_color_formats = {Format::kRgba16Float, Format::kRgba8Unorm};
        if (!require(other_profile.is_valid(profile),
                     "matching color profile attachment contract must remain valid"))
            return 28;
    }

    RendererProfilePsoContract gbuffer{};
    gbuffer.shader = {aeg::renderer::kRendererProfileContractVersion, RendererProfile::kGBuffer,
                      0xaabbccddeeff0123ull, 23};
    gbuffer.cache_epoch = 23;
    gbuffer.attachment = {Format::kRgba16Float, SampleCount::kX1, Format::kD32Float,
                          {Format::kRgba16Float, Format::kRgba8Unorm}};
    if (!require(gbuffer.is_valid(RendererProfile::kGBuffer),
                 "complete ordered three-color/depth GBuffer PSO contract must be valid"))
        return 40;
    auto single_color_gbuffer = gbuffer;
    single_color_gbuffer.attachment.additional_color_formats = {};
    if (!require(!single_color_gbuffer.is_valid(RendererProfile::kGBuffer),
                 "GBuffer PSO contract accepted a single-primary-color shape"))
        return 41;
    for (size_t slot = 0; slot < 3; ++slot) {
        for (const auto color : {Format::kUnknown, Format::kD24UnormS8Uint, Format::kD32Float,
                                 Format::kD32FloatS8Uint, Format::kBc1RgbaUnorm, Format::kBc3RgbaUnorm,
                                 Format::kBc5Unorm, Format::kBc7RgbaUnorm, static_cast<Format>(0xffff)}) {
            auto invalid = gbuffer;
            if (slot == 0)
                invalid.attachment.color_format = color;
            else
                invalid.attachment.additional_color_formats[slot - 1] = color;
            if (!require(!invalid.is_valid(RendererProfile::kGBuffer),
                         "GBuffer PSO contract accepted a missing or non-renderable ordered color slot"))
                return 42;
        }
    }
    for (const auto depth : {Format::kUnknown, Format::kRgba16Float, static_cast<Format>(0xffff)}) {
        auto invalid = gbuffer;
        invalid.attachment.depth_stencil_format = depth;
        if (!require(!invalid.is_valid(RendererProfile::kGBuffer),
                     "GBuffer PSO contract accepted a missing or invalid depth attachment"))
            return 43;
    }
    for (const auto samples : {0u, 3u, 255u}) {
        auto invalid = gbuffer;
        invalid.attachment.samples = static_cast<SampleCount>(samples);
        if (!require(!invalid.is_valid(RendererProfile::kGBuffer),
                     "GBuffer PSO contract accepted an invalid sample count"))
            return 44;
    }
    for (const auto depth : {Format::kD24UnormS8Uint, Format::kD32FloatS8Uint}) {
        auto alternate = gbuffer;
        alternate.attachment = {Format::kRgba32Float, SampleCount::kX4, depth,
                                {Format::kBgra8Unorm, Format::kR32Uint}};
        if (!require(alternate.is_valid(RendererProfile::kGBuffer),
                     "neutral GBuffer shape must preserve valid noncanonical ordered formats/depth/samples"))
            return 45;
    }
    for (const auto color : {Format::kR8Unorm, Format::kRg8Unorm, Format::kRgba8Unorm, Format::kBgra8Unorm,
                             Format::kRgba8Srgb, Format::kRgba16Float, Format::kRgba32Float, Format::kR32Uint,
                             Format::kR32Float, Format::kRg32Float, Format::kRgb32Float}) {
        auto alternate = gbuffer;
        alternate.attachment.color_format = color;
        if (!require(alternate.is_valid(RendererProfile::kGBuffer),
                     "neutral GBuffer shape rejected a known uncompressed RHI color format"))
            return 46;
    }
    for (unsigned field = 0; field < 7; ++field) {
        auto invalid = gbuffer;
        switch (field) {
            case 0: invalid.shader.profile = RendererProfile::kDeferredLighting; break;
            case 1: ++invalid.shader.version; break;
            case 2: invalid.shader.layout_hash = 0; break;
            case 3: invalid.shader.cache_epoch = 0; break;
            case 4: invalid.cache_epoch = 0; break;
            case 5: ++invalid.shader.cache_epoch; break;
            case 6: ++invalid.cache_epoch; break;
        }
        if (!require(!invalid.is_valid(RendererProfile::kGBuffer),
                     "GBuffer PSO contract accepted an invalid profile/version/layout/cache epoch"))
            return 47;
    }

    auto invalid_shadow = shadow;
    ++invalid_shadow.shader.version;
    if (!require(!invalid_shadow.is_valid(RendererProfile::kShadow),
                 "Shadow PSO contract accepted an unsupported version"))
        return 30;
    invalid_shadow = shadow;
    invalid_shadow.shader.layout_hash = 0;
    if (!require(!invalid_shadow.is_valid(RendererProfile::kShadow),
                 "Shadow PSO contract accepted an absent shader layout"))
        return 31;
    invalid_shadow = shadow;
    invalid_shadow.shader.cache_epoch = 0;
    if (!require(!invalid_shadow.is_valid(RendererProfile::kShadow),
                 "Shadow PSO contract accepted an absent shader epoch"))
        return 32;
    invalid_shadow = shadow;
    invalid_shadow.cache_epoch = 0;
    if (!require(!invalid_shadow.is_valid(RendererProfile::kShadow),
                 "Shadow PSO contract accepted an absent PSO epoch"))
        return 33;
    invalid_shadow = shadow;
    ++invalid_shadow.shader.cache_epoch;
    if (!require(!invalid_shadow.is_valid(RendererProfile::kShadow),
                 "Shadow PSO contract accepted mismatched shader and PSO epochs"))
        return 34;

    RendererProfileShaderContract shader{};
    if (!require(!shader.is_valid(), "default profile shader contract must be invalid"))
        return 1;

    shader.profile = RendererProfile::kDeferredLighting;
    shader.layout_hash = 0x1020304050607080ull;
    if (!require(shader.is_valid(RendererProfile::kDeferredLighting),
                 "matching deferred shader contract must be valid"))
        return 2;
    if (!require(!shader.is_valid(RendererProfile::kPostProcess),
                 "shader contract must reject a profile mismatch"))
        return 3;

    RendererProfilePsoContract pso{};
    pso.shader = shader;
    pso.attachment = {aeg::rhi::Format::kRgba16Float, aeg::rhi::SampleCount::kX1,
                      aeg::rhi::Format::kUnknown};
    if (!require(pso.is_valid(RendererProfile::kDeferredLighting),
                 "matching deferred PSO contract must be valid"))
        return 4;

    RenderMaterialPipelineVariant clustered_variant{};
    clustered_variant.pipeline = aeg::rhi::PipelineHandle{1};
    clustered_variant.cache_epoch = 7;
    clustered_variant.profile_contract = {
        {aeg::renderer::kRendererProfileContractVersion, RendererProfile::kForwardClustered,
         0x8877665544332211ull, 7},
        pso.attachment};
    clustered_variant.profile_contract.cache_epoch = 7;
    clustered_variant.attachment_contract = pso.attachment;
    clustered_variant.shader_contract = {
        aeg::renderer::kRenderMaterialShaderContractVersion,
        0x1122334455667788ull,
        7,
    };
    if (!require(clustered_variant.profile_contract.is_valid(RendererProfile::kForwardClustered),
                 "clustered material variant must carry a valid profile PSO contract"))
        return 5;
    if (!require(clustered_variant.cache_epoch == 7 && clustered_variant.is_valid(RendererProfile::kForwardClustered),
                 "clustered material variant cache epoch was not preserved"))
        return 6;
    clustered_variant.shader_contract.cache_epoch = 0;
    if (!require(!clustered_variant.is_valid(RendererProfile::kForwardClustered),
                 "clustered material variant accepted a retired material shader contract"))
        return 12;
    clustered_variant.shader_contract.cache_epoch = 7;
    clustered_variant.cache_epoch = 0;
    if (!require(!clustered_variant.is_valid(RendererProfile::kForwardClustered),
                 "zero clustered material variant cache epoch was accepted"))
        return 7;
    pso.shader.version += 1u;
    if (!require(!pso.is_valid(RendererProfile::kDeferredLighting),
                 "PSO contract must reject an unsupported version"))
        return 8;

    pso.shader = shader;
    pso.cache_epoch = 7;
    pso.shader.cache_epoch = 2;
    if (!require(!pso.is_valid(RendererProfile::kDeferredLighting),
                 "PSO contract must reject a shader/cache epoch mismatch"))
        return 9;
    pso.shader.cache_epoch = 7;
    if (!require(pso.is_valid(RendererProfile::kDeferredLighting),
                 "PSO contract must accept a matching shader/cache epoch"))
        return 10;
    pso.attachment.samples = static_cast<aeg::rhi::SampleCount>(0);
    if (!require(!pso.is_valid(RendererProfile::kDeferredLighting),
                 "PSO contract must reject an invalid sample-count attachment"))
        return 13;
    pso.attachment.samples = aeg::rhi::SampleCount::kX1;
    pso.attachment.color_format = aeg::rhi::Format::kUnknown;
    if (!require(!pso.is_valid(RendererProfile::kDeferredLighting),
                 "PSO contract must reject an unknown color attachment"))
        return 11;
    return 0;
}
