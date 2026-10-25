#include "renderer_feature_profiles.hpp"
#include "../../Feature/private/feature_clock_admission.hpp"

#include <array>
#include <new>
#include <utility>

namespace aeg::renderer {
namespace {
using namespace feature;
constexpr std::array kProfiles{SceneRenderProfile::kOpaque, SceneRenderProfile::kForward,
                              SceneRenderProfile::kDeferred};
constexpr std::array kDefinitionIds{FeatureDefinitionId("Renderer.Scene.Opaque"),
                                  FeatureDefinitionId("Renderer.Scene.Forward"),
                                  FeatureDefinitionId("Renderer.Scene.Deferred")};
constexpr std::array kGraphIds{"renderer-scene-opaque", "renderer-scene-forward", "renderer-scene-deferred"};
constexpr RenderSceneReference kScene{RenderResourceId("Runtime.Scene"), 1};
constexpr RenderTextureReference kColor{RenderResourceId("Renderer.SceneColor"), 1, true};
constexpr RenderTextureReference kDepth{RenderResourceId("Renderer.SceneDepth"), 1, true};
constexpr RenderTextureReference kShadow{RenderResourceId("Renderer.ShadowDepth"), 1, true};
constexpr RenderTextureReference kLighting{RenderResourceId("Renderer.SceneLighting"), 1, true};
constexpr RenderTextureReference kAlbedo{RenderResourceId("Renderer.GBuffer.Albedo"), 1, true};
constexpr RenderTextureReference kNormal{RenderResourceId("Renderer.GBuffer.Normal"), 1, true};
constexpr RenderTextureReference kMaterial{RenderResourceId("Renderer.GBuffer.Material"), 1, true};
constexpr RenderResourceId kLightList{"Renderer.Scene.LightList"};
constexpr std::array kClearColor{0.f, 0.f, 0.f, 1.f};

foundation::ResultValue<size_t> profile_index(SceneRenderProfile profile) {
    switch (profile) {
    case SceneRenderProfile::kOpaque: return {foundation::Result::success(), 0};
    case SceneRenderProfile::kForward: return {foundation::Result::success(), 1};
    case SceneRenderProfile::kDeferred: return {foundation::Result::success(), 2};
    }
    return {foundation::invalid_argument("Scene Render profile is unknown"), {}};
}

RenderIntent profile_intent(SceneRenderProfile profile, const InvocationToken& token) {
    RenderIntent intent;
    intent.final_output = kColor;
    intent.depth_output = kDepth;
    if (profile == SceneRenderProfile::kOpaque) {
        RenderDrawScene draw{kScene, kColor};
        draw.depth = RenderDepthAttachment{kDepth};
        intent.operations = {draw};
        return intent;
    }
    RenderDrawDepthScene shadow{kScene, {kShadow}};
    shadow.projection = RenderDepthSceneProjection::kDirectionalShadow;
    if (profile == SceneRenderProfile::kForward) {
        RenderDrawDepthScene depth{kScene, {kDepth}};
        RenderLightCullingScene cull{kScene, kDepth, {kLightList, 1, {}}};
        cull.directional_shadow = kShadow;
        RenderDrawClusteredScene draw;
        draw.scene = kScene;
        draw.light_list = {kLightList, 1, token.invocation_id, token.generation};
        draw.hdr = {kLighting, RenderAttachmentLoad::kClear, kClearColor};
        draw.depth = {kDepth, RenderAttachmentLoad::kLoad, 1.f};
        draw.directional_shadow = kShadow;
        intent.operations = {depth, shadow, cull, draw};
    } else {
        RenderDrawGBufferScene gbuffer{kScene, {kAlbedo}, {kNormal}, {kMaterial}, {kDepth}};
        RenderDeferredLightingScene lighting;
        lighting.scene = kScene;
        lighting.albedo = kAlbedo;
        lighting.normal = kNormal;
        lighting.depth = kDepth;
        lighting.hdr = {kLighting, RenderAttachmentLoad::kClear, kClearColor};
        lighting.directional_shadow = kShadow;
        intent.operations = {shadow, gbuffer, lighting};
    }
    RenderPostProcessScene post{kScene, kLighting, kDepth, {kColor, RenderAttachmentLoad::kClear, kClearColor}};
    intent.operations.push_back(post);
    return intent;
}
} // namespace

foundation::ResultValue<feature::FeatureDefinition> scene_render_profile_definition(SceneRenderProfile profile) try {
    const auto index = profile_index(profile);
    if (!index) return {index.result, {}};
    FeatureDefinition definition;
    definition.id = kDefinitionIds[index.value];
    definition.version = {1, 0, 0};
    definition.owner_module = ModuleIdentity("com.aeg.renderer");
    auto& contract = definition.contract;
    contract.definition_id = definition.id;
    contract.version = definition.version;
    contract.structure = FeatureStructure::kAtomic;
    contract.domains = to_mask(FeatureDomain::kRender);
    contract.execution = ExecutionKind::kContinuous;
    contract.state = StatePolicy::kStateless;
    contract.scope = {ScopeId("Frame")};
    contract.clock = make_periodic_clock_contract(kSceneRenderProfileClock, 60, 4, LateDataPolicy::kDrop,
        DeadlinePolicy::kObserve, 250'000, 0);
    contract.permissions = {to_mask(RenderIntentCapability::kDrawScene) |
        to_mask(RenderIntentCapability::kWriteExternal) | to_mask(RenderIntentCapability::kExportOutput), 1};
    if (profile != SceneRenderProfile::kOpaque)
        contract.permissions.capability_mask |= to_mask(RenderIntentCapability::kReadExternal);
    if (profile == SceneRenderProfile::kForward)
        contract.permissions.capability_mask |= to_mask(RenderIntentCapability::kCullLights) | to_mask(RenderIntentCapability::kReadLightList);
    contract.side_effects = {{kRenderIntentEffect, 1, SideEffectKind::kIntent, CommitPolicy::kOwnerBoundary}};
    definition.execute = [profile](const InvocationToken& token, const StateSnapshot&,
                                  std::span<const uint8_t>, std::span<const uint8_t>) -> foundation::ResultValue<StateDelta> {
        try {
            StateDelta delta{token.state_version};
            delta.render_intent = profile_intent(profile, token);
            return {foundation::Result::success(), std::move(delta)};
        } catch (const std::bad_alloc&) {
            return {foundation::out_of_memory("Scene Render profile intent allocation failed"), {}};
        }
    };
    if (const auto valid = validate_feature_contract(contract); !valid) return {valid, {}};
    return {foundation::Result::success(), std::move(definition)};
} catch (const std::bad_alloc&) {
    return {foundation::out_of_memory("Scene Render profile definition allocation failed"), {}};
}

foundation::Result register_scene_render_profiles(feature::FeatureRegistry& registry) {
    for (const auto id : kDefinitionIds)
        if (registry.find_definition(id))
            return foundation::invalid_state("Scene Render profile identity is already registered");
    std::array<FeatureDefinition, kProfiles.size()> definitions;
    for (size_t index = 0; index < kProfiles.size(); ++index) {
        auto definition = scene_render_profile_definition(kProfiles[index]);
        if (!definition) return definition.result;
        definitions[index] = std::move(definition.value);
    }
    // All candidates share one validated Frame contract, so an existing Clock
    // conflict is rejected by the first registration before any candidate is added.
    std::array<feature::detail::ClockBindingKey, kProfiles.size()> new_bindings{};
    size_t binding_count = 0;
    size_t registered_count = 0;
    const auto rollback = [&] {
        while (registered_count != 0)
            static_cast<void>(registry.unregister_definition(kDefinitionIds[--registered_count]));
        static_cast<void>(feature::detail::rollback_new_clock_bindings(registry,
            std::span<const feature::detail::ClockBindingKey>(new_bindings.data(), binding_count)));
    };
    for (auto& definition : definitions) {
        const auto approved = feature::detail::admit_clock_request(registry, definition.contract);
        if (!approved) { rollback(); return approved.result; }
        if (approved.value.inserted) new_bindings[binding_count++] = approved.value.key;
        try {
            const auto registered = registry.register_definition(std::move(definition));
            if (!registered) { rollback(); return registered; }
            ++registered_count;
        } catch (const std::bad_alloc&) {
            rollback();
            return foundation::out_of_memory("Scene Render profile registration allocation failed");
        }
    }
    return foundation::Result::success();
}

foundation::ResultValue<feature::FeatureCatalog> scene_render_profile_catalog() try {
    FeatureCatalog catalog;
    for (const auto profile : kProfiles) {
        const auto definition = scene_render_profile_definition(profile);
        if (!definition) return {definition.result, {}};
        if (const auto exported = catalog.export_definition(definition.value); !exported) return {exported, {}};
    }
    return {foundation::Result::success(), std::move(catalog)};
} catch (const std::bad_alloc&) {
    return {foundation::out_of_memory("Scene Render profile catalog allocation failed"), {}};
}

foundation::ResultValue<feature::FeatureGraphDocument> scene_render_profile_graph(SceneRenderProfile profile) try {
    const auto index = profile_index(profile);
    if (!index) return {index.result, {}};
    const auto catalog = scene_render_profile_catalog();
    if (!catalog) return {catalog.result, {}};
    FeatureGraphDocument graph;
    graph.asset_uuid = kGraphIds[index.value];
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.contract_digest = catalog.value.contract_digest();
    graph.nodes = {{FeatureNodeId("Renderer.Scene.Root"), kDefinitionIds[index.value], {1, 0, 0}}};
    return {foundation::Result::success(), std::move(graph)};
} catch (const std::bad_alloc&) {
    return {foundation::out_of_memory("Scene Render profile graph allocation failed"), {}};
}

} // namespace aeg::renderer
