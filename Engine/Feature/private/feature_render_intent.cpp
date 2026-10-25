#include "feature_render_intent.hpp"
#include "feature_contract.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include "serialization.hpp"

namespace aeg::feature {
foundation::ResultValue<std::vector<uint8_t>> encode_render_light_list_reference(const RenderLightListReference& reference) {
    if (!reference.is_valid()) return {foundation::invalid_argument("light-list reference is invalid"), {}};
    serialization::BinaryWriter writer;
    for (const uint64_t value : {reference.id.value(), reference.version, reference.producer_invocation.value(),
                                  reference.producer_generation.value()})
        if (const auto result = writer.write_u64(value); !result) return {result, {}};
    return {foundation::Result::success(), {writer.data().begin(), writer.data().end()}};
}
foundation::ResultValue<RenderLightListReference> decode_render_light_list_reference(std::span<const uint8_t> payload) {
    if (payload.size() != 32) return {foundation::invalid_argument("light-list reference requires exactly32 bytes"), {}};
    serialization::BinaryReader reader(payload);
    const auto id = reader.read_u64(), version = reader.read_u64(), invocation = reader.read_u64(), generation = reader.read_u64();
    if (!id || !version || !invocation || !generation)
        return {foundation::invalid_argument("light-list reference is truncated"), {}};
    const RenderLightListReference reference{RenderResourceId(id.value), version.value,
        InvocationId(invocation.value), RuntimeGeneration(generation.value)};
    if (!reference.is_valid()) return {foundation::invalid_argument("light-list reference is invalid"), {}};
    return {foundation::Result::success(), reference};
}
foundation::Result validate_post_process_settings(const RenderPostProcessSettings& settings) {
    for (const auto& color : {settings.sky_zenith, settings.sky_horizon, settings.sky_ground})
        for (const float value : color)
            if (!std::isfinite(value) || value < 0.f)
                return foundation::invalid_argument("Sky colors must be finite and non-negative");
    if (!std::isfinite(settings.exposure) || settings.exposure < 0.f || !std::isfinite(settings.gamma) ||
        settings.gamma <= 0.f || !std::isfinite(1.f / settings.gamma))
        return foundation::invalid_argument("Postprocess requires non-negative exposure and positive finite gamma");
    return foundation::Result::success();
}
foundation::Result validate_render_intent(const RenderIntent& intent, const FeatureContract& contract) {
    using foundation::invalid_argument;
    if (contract.structure != FeatureStructure::kAtomic || contract.domains != to_mask(FeatureDomain::kRender))
        return invalid_argument("only an Atomic Render Feature can submit a render intent");
    if (std::none_of(contract.side_effects.begin(), contract.side_effects.end(), [](const SideEffectContract& effect) {
            return effect.id == kRenderIntentEffect && effect.version == 1 && effect.kind == SideEffectKind::kIntent &&
                   effect.commit == CommitPolicy::kOwnerBoundary;
        })) return invalid_argument("Render.Intent v1 owner-boundary side effect is not declared");
    if (intent.schema_version != 1 || intent.textures.size() > 256 || intent.operations.size() > 1024)
        return invalid_argument("render intent version or bounded resource/operation budget is invalid");
    const auto allows = [&](RenderIntentCapability capability) {
        return (contract.permissions.capability_mask & to_mask(capability)) != 0;
    };
    std::unordered_map<RenderResourceId, const RenderTextureDeclaration*, StableIdHash<RenderResourceIdTag>> textures;
    for (const auto& texture : intent.textures) {
        if ((texture.lifetime != RenderTextureLifetime::kTransient && texture.lifetime != RenderTextureLifetime::kHistory) ||
            !allows(texture.lifetime == RenderTextureLifetime::kHistory ? RenderIntentCapability::kCreateHistory
                                                                       : RenderIntentCapability::kCreateTransient) ||
            !texture.id.is_valid() || texture.version == 0 ||
            texture.width == 0 || texture.height == 0 || !textures.emplace(texture.id, &texture).second)
            return invalid_argument("render texture declaration or creation permission is invalid");
    }
    const auto validate_reference = [&](const RenderTextureReference& reference, bool write, uint32_t width, uint32_t height) {
        if (!reference.id.is_valid() || reference.version == 0)
            return invalid_argument("render texture reference requires identity and version");
        if (reference.external) {
            if (!allows(write ? RenderIntentCapability::kWriteExternal : RenderIntentCapability::kReadExternal))
                return invalid_argument("external render resource permission is missing");
        } else {
            const auto found = textures.find(reference.id);
            if (found == textures.end() || found->second->version != reference.version ||
                width > found->second->width || height > found->second->height)
                return invalid_argument("local render texture is missing, stale or too small");
        }
        return foundation::Result::success();
    };
    const auto validate_color = [&](const RenderColorAttachment& color) {
        if (!color.target.external ||
            (color.load != RenderAttachmentLoad::kLoad && color.load != RenderAttachmentLoad::kClear) ||
            std::any_of(color.clear_color.begin(), color.clear_color.end(), [](float value) { return !std::isfinite(value); }))
            return invalid_argument("render scene color attachment, load or clear value is invalid");
        if (const auto result = validate_reference(color.target, true, 0, 0); !result) return result;
        if (color.load == RenderAttachmentLoad::kLoad)
            if (const auto result = validate_reference(color.target, false, 0, 0); !result) return result;
        return foundation::Result::success();
    };
    const auto validate_depth = [&](const RenderDepthAttachment& depth) {
        if (!depth.target.external ||
            (depth.load != RenderAttachmentLoad::kLoad && depth.load != RenderAttachmentLoad::kClear) ||
            !std::isfinite(depth.clear_depth) || depth.clear_depth < 0.f || depth.clear_depth > 1.f)
            return invalid_argument("render scene depth attachment, load or clear value is invalid");
        if (const auto result = validate_reference(depth.target, true, 0, 0); !result) return result;
        if (depth.load == RenderAttachmentLoad::kLoad)
            if (const auto result = validate_reference(depth.target, false, 0, 0); !result) return result;
        return foundation::Result::success();
    };
    std::unordered_set<RenderResourceId, StableIdHash<RenderResourceIdTag>> light_lists;
    for (const auto& operation : intent.operations) {
        if (const auto* clear = std::get_if<RenderClearColor>(&operation)) {
            if (!allows(RenderIntentCapability::kClearColor) ||
                std::any_of(clear->color.begin(), clear->color.end(), [](float value) { return !std::isfinite(value); }))
                return invalid_argument("render color clear or permission is invalid");
            if (const auto result = validate_reference(clear->target, true, 0, 0); !result) return result;
        } else if (const auto* draw = std::get_if<RenderDrawScene>(&operation)) {
            if (!allows(RenderIntentCapability::kDrawScene) || !draw->scene.id.is_valid() || draw->scene.version == 0)
                return invalid_argument("render scene draw or permission is invalid");
            if (const auto result = validate_color({draw->target, draw->load, draw->clear_color}); !result) return result;
            if (draw->depth) {
                if (draw->depth->target.id == draw->target.id)
                    return invalid_argument("render scene color and depth attachment identities alias");
                if (const auto result = validate_depth(*draw->depth); !result) return result;
            }
            if (draw->directional_shadow) {
                const auto& shadow = *draw->directional_shadow;
                if (!shadow.external || shadow.id == draw->target.id ||
                    (draw->depth && shadow.id == draw->depth->target.id))
                    return invalid_argument("directional shadow must be an external input distinct from draw attachments");
                if (const auto result = validate_reference(shadow, false, 0, 0); !result) return result;
            }
        } else if (const auto* draw = std::get_if<RenderDrawDepthScene>(&operation)) {
            if (!allows(RenderIntentCapability::kDrawScene) || !draw->scene.id.is_valid() || draw->scene.version == 0 ||
                (draw->projection != RenderDepthSceneProjection::kCamera &&
                 draw->projection != RenderDepthSceneProjection::kDirectionalShadow))
                return invalid_argument("render depth scene draw or permission is invalid");
            if (const auto result = validate_depth(draw->depth); !result) return result;
        } else if (const auto* draw = std::get_if<RenderDrawGBufferScene>(&operation)) {
            if (!allows(RenderIntentCapability::kDrawScene) || !draw->scene.id.is_valid() || draw->scene.version == 0)
                return invalid_argument("render GBuffer scene draw or permission is invalid");
            for (const auto* color : {&draw->albedo, &draw->normal, &draw->material})
                if (const auto result = validate_color(*color); !result) return result;
            if (const auto result = validate_depth(draw->depth); !result) return result;
            const std::array ids{draw->albedo.target.id, draw->normal.target.id, draw->material.target.id, draw->depth.target.id};
            for (size_t first = 0; first < ids.size(); ++first)
                for (size_t second = first + 1; second < ids.size(); ++second)
                    if (ids[first] == ids[second]) return invalid_argument("GBuffer attachment identities alias");
        } else if (const auto* draw = std::get_if<RenderDeferredLightingScene>(&operation)) {
            if (!allows(RenderIntentCapability::kDrawScene) || !draw->scene.id.is_valid() || draw->scene.version == 0)
                return invalid_argument("render DeferredLighting scene or permission is invalid");
            if (const auto result = validate_color(draw->hdr); !result) return result;
            std::array references{draw->albedo, draw->normal, draw->depth, draw->hdr.target,
                                  draw->directional_shadow.value_or(RenderTextureReference{})};
            const size_t count = draw->directional_shadow ? references.size() : references.size() - 1;
            for (size_t index = 0; index < count; ++index) {
                if (index != 3) {
                    if (!references[index].external) return invalid_argument("DeferredLighting inputs must be external");
                    if (const auto result = validate_reference(references[index], false, 0, 0); !result) return result;
                }
                for (size_t previous = 0; previous < index; ++previous)
                    if (references[previous].id == references[index].id)
                        return invalid_argument("DeferredLighting input/output identities alias");
            }
        } else if (const auto* draw = std::get_if<RenderPostProcessScene>(&operation)) {
            if (!allows(RenderIntentCapability::kDrawScene) || !draw->scene.id.is_valid() || draw->scene.version == 0)
                return invalid_argument("render PostProcess scene or permission is invalid");
            if (const auto result = validate_color(draw->output); !result) return result;
            if (draw->settings)
                if (const auto result = validate_post_process_settings(*draw->settings); !result) return result;
            const std::array references{draw->hdr, draw->depth, draw->output.target};
            for (size_t index = 0; index < references.size(); ++index) {
                if (index < 2) {
                    if (!references[index].external) return invalid_argument("PostProcess inputs must be external");
                    if (const auto result = validate_reference(references[index], false, 0, 0); !result) return result;
                }
                for (size_t previous = 0; previous < index; ++previous)
                    if (references[previous].id == references[index].id)
                        return invalid_argument("PostProcess input/output identities alias");
            }
        } else if (const auto* cull = std::get_if<RenderLightCullingScene>(&operation)) {
            if (!allows(RenderIntentCapability::kCullLights) || !cull->scene.id.is_valid() || cull->scene.version == 0 ||
                !cull->camera_depth.external || !cull->output.id.is_valid() || cull->output.version == 0 ||
                !light_lists.insert(cull->output.id).second)
                return invalid_argument("light culling scene, output or permission is invalid");
            if (const auto result = validate_reference(cull->camera_depth, false, 0, 0); !result) return result;
            if (cull->output.export_port.is_valid()) {
                const auto port = std::find_if(contract.ports.begin(), contract.ports.end(), [&](const PortContract& port) {
                    return port.id == cull->output.export_port && port.direction == PortDirection::kOutput &&
                           port.schema == kRenderLightListSchema && port.version == 1;
                });
                if (port == contract.ports.end()) return invalid_argument("light-list export port is undeclared or incompatible");
            }
            if (cull->directional_shadow) {
                if (!cull->directional_shadow->external || cull->directional_shadow->id == cull->camera_depth.id)
                    return invalid_argument("light-list shadow aliases camera depth or is not external");
                if (const auto result = validate_reference(*cull->directional_shadow, false, 0, 0); !result) return result;
            }
        } else if (const auto* draw = std::get_if<RenderDrawClusteredScene>(&operation)) {
            if (!allows(RenderIntentCapability::kDrawScene) || !allows(RenderIntentCapability::kReadLightList) ||
                !draw->scene.id.is_valid() || draw->scene.version == 0 || !draw->light_list.is_valid() ||
                draw->depth.load != RenderAttachmentLoad::kLoad || draw->depth.target.id == draw->hdr.target.id)
                return invalid_argument("clustered scene, light list, preserved depth or permission is invalid");
            if (const auto result = validate_color(draw->hdr); !result) return result;
            if (const auto result = validate_depth(draw->depth); !result) return result;
            if (draw->directional_shadow) {
                if (!draw->directional_shadow->external || draw->directional_shadow->id == draw->depth.target.id ||
                    draw->directional_shadow->id == draw->hdr.target.id)
                    return invalid_argument("clustered shadow input aliases attachments or is not external");
                if (const auto result = validate_reference(*draw->directional_shadow, false, 0, 0); !result) return result;
            }
        } else {
            const auto& copy = std::get<RenderCopyTexture>(operation);
            if (!allows(RenderIntentCapability::kCopyTexture) || copy.width == 0 || copy.height == 0 ||
                (copy.source.id == copy.destination.id && copy.source.external == copy.destination.external))
                return invalid_argument("render texture copy or permission is invalid");
            if (const auto result = validate_reference(copy.source, false, copy.width, copy.height); !result) return result;
            if (const auto result = validate_reference(copy.destination, true, copy.width, copy.height); !result) return result;
        }
    }
    if (intent.final_output) {
        if (!allows(RenderIntentCapability::kExportOutput)) return invalid_argument("render output export permission is missing");
        if (const auto result = validate_reference(*intent.final_output, true, 0, 0); !result) return result;
    }
    if (intent.depth_output) {
        if (!intent.depth_output->external || !allows(RenderIntentCapability::kExportOutput))
            return invalid_argument("render depth output must be external and declare export permission");
        if (const auto result = validate_reference(*intent.depth_output, true, 0, 0); !result) return result;
    }
    return foundation::Result::success();
}
}
