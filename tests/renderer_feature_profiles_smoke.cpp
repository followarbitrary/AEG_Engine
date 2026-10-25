/** @brief Exercises engine Scene profiles through the real Feature compiler and Runtime. */
#include "feature_clock_test_fixture.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>

#include "../Engine/Renderer/private/renderer_feature_profiles.hpp"
#include "../Engine/RenderGraph/private/render_feature_projector.hpp"

namespace {
using namespace aeg;
using namespace feature;
using renderer::SceneRenderProfile;
constexpr std::array kProfiles{SceneRenderProfile::kOpaque, SceneRenderProfile::kForward,
                              SceneRenderProfile::kDeferred};
constexpr std::array kIds{FeatureDefinitionId("Renderer.Scene.Opaque"),
                         FeatureDefinitionId("Renderer.Scene.Forward"),
                         FeatureDefinitionId("Renderer.Scene.Deferred")};
constexpr std::array kAssets{"renderer-scene-opaque", "renderer-scene-forward", "renderer-scene-deferred"};
constexpr ModuleIdentity kRendererModule{"com.aeg.renderer"};

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void require_external(const RenderTextureReference& value, const char* id) {
    require(value.id == RenderResourceId(id) && value.version == 1 && value.external,
            std::string("profile external slot differs: ") + id);
}

void check_intent(SceneRenderProfile profile, const RenderFeatureOutput& output) {
    const auto& intent = output.intent;
    require(intent.schema_version == 1 && intent.textures.empty(), "profile creates author texture declarations");
    require(intent.final_output && intent.depth_output, "profile terminal outputs are missing");
    require_external(*intent.final_output, "Renderer.SceneColor");
    require_external(*intent.depth_output, "Renderer.SceneDepth");
    for (const auto& operation : intent.operations) {
        std::visit([](const auto& value) {
            if constexpr (requires { value.scene; })
                require(value.scene.id == RenderResourceId("Runtime.Scene") && value.scene.version == 1,
                        "profile does not use the canonical versioned Scene slot");
        }, operation);
    }
    if (profile == SceneRenderProfile::kOpaque) {
        require(intent.operations.size() == 1 && std::holds_alternative<RenderDrawScene>(intent.operations[0]),
                "Opaque profile does not emit one typed scene draw");
        const auto& draw = std::get<RenderDrawScene>(intent.operations[0]);
        require_external(draw.target, "Renderer.SceneColor");
        require(draw.depth && draw.load == RenderAttachmentLoad::kClear && !draw.directional_shadow,
                "Opaque profile attachment contract differs");
        require_external(draw.depth->target, "Renderer.SceneDepth");
        require(draw.depth->load == RenderAttachmentLoad::kClear && draw.depth->clear_depth == 1.f,
                "Opaque profile does not initialize camera depth");
        return;
    }
    const auto shadow_index = profile == SceneRenderProfile::kForward ? 1u : 0u;
    const auto& shadow = std::get<RenderDrawDepthScene>(intent.operations.at(shadow_index));
    require(shadow.projection == RenderDepthSceneProjection::kDirectionalShadow &&
                shadow.depth.load == RenderAttachmentLoad::kClear && shadow.depth.clear_depth == 1.f,
            "profile ShadowDepth is not an independent initialized projection");
    require_external(shadow.depth.target, "Renderer.ShadowDepth");
    if (profile == SceneRenderProfile::kForward) {
        require(intent.operations.size() == 5, "Forward profile operation count differs");
        const auto& depth = std::get<RenderDrawDepthScene>(intent.operations[0]);
        require(depth.projection == RenderDepthSceneProjection::kCamera &&
                    depth.depth.load == RenderAttachmentLoad::kClear && depth.depth.clear_depth == 1.f,
                "Forward profile does not produce initialized camera depth");
        require_external(depth.depth.target, "Renderer.SceneDepth");
        const auto& cull = std::get<RenderLightCullingScene>(intent.operations[2]);
        const auto& draw = std::get<RenderDrawClusteredScene>(intent.operations[3]);
        require_external(cull.camera_depth, "Renderer.SceneDepth");
        require(cull.output.id == RenderResourceId("Renderer.Scene.LightList") && cull.output.version == 1 &&
                    !cull.output.export_port.is_valid(), "Forward profile does not keep its LightList private");
        require(draw.light_list == RenderLightListReference{cull.output.id, cull.output.version,
                    output.invocation.invocation_id, output.invocation.generation},
                "Forward profile LightList reference does not come from its actual invocation");
        require_external(draw.hdr.target, "Renderer.SceneLighting");
        require_external(draw.depth.target, "Renderer.SceneDepth");
        require(draw.depth.load == RenderAttachmentLoad::kLoad && cull.directional_shadow && draw.directional_shadow,
                "Forward profile does not preserve its Cull depth/shadow inputs");
        require_external(*cull.directional_shadow, "Renderer.ShadowDepth");
        require_external(*draw.directional_shadow, "Renderer.ShadowDepth");
    } else {
        require(intent.operations.size() == 4, "Deferred profile operation count differs");
        const auto& gbuffer = std::get<RenderDrawGBufferScene>(intent.operations[1]);
        require_external(gbuffer.albedo.target, "Renderer.GBuffer.Albedo");
        require_external(gbuffer.normal.target, "Renderer.GBuffer.Normal");
        require_external(gbuffer.material.target, "Renderer.GBuffer.Material");
        require_external(gbuffer.depth.target, "Renderer.SceneDepth");
        require(gbuffer.albedo.load == RenderAttachmentLoad::kClear && gbuffer.normal.load == RenderAttachmentLoad::kClear &&
                    gbuffer.material.load == RenderAttachmentLoad::kClear && gbuffer.depth.load == RenderAttachmentLoad::kClear &&
                    gbuffer.depth.clear_depth == 1.f, "Deferred profile does not initialize all ordered MRT/depth slots");
        const auto& lighting = std::get<RenderDeferredLightingScene>(intent.operations[2]);
        require_external(lighting.albedo, "Renderer.GBuffer.Albedo");
        require_external(lighting.normal, "Renderer.GBuffer.Normal");
        require_external(lighting.depth, "Renderer.SceneDepth");
        require_external(lighting.hdr.target, "Renderer.SceneLighting");
        require(lighting.directional_shadow.has_value(), "Deferred profile does not consume its ShadowDepth");
        require_external(*lighting.directional_shadow, "Renderer.ShadowDepth");
    }
    const auto& post = std::get<RenderPostProcessScene>(intent.operations.back());
    require_external(post.hdr, "Renderer.SceneLighting");
    require_external(post.depth, "Renderer.SceneDepth");
    require_external(post.output.target, "Renderer.SceneColor");
    require(!post.settings && post.output.load == RenderAttachmentLoad::kClear,
            "profile PostProcess does not use immutable View defaults");
}

void definitions_catalog_and_presets_share_contracts() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    require(bool(renderer::register_scene_render_profiles(registry)), "profile registration failed");
    const auto catalog = renderer::scene_render_profile_catalog();
    require(catalog && catalog.value.entries().size() == 3 &&
                catalog.value.contract_digest() == registry.contract_digest(), "profile catalog differs from executable registry");
    for (size_t index = 0; index < kProfiles.size(); ++index) {
        const auto source = renderer::scene_render_profile_definition(kProfiles[index]);
        require(bool(source), "profile definition factory failed");
        const auto& definition = source.value;
        const auto& contract = definition.contract;
        require(definition.id == kIds[index] && definition.owner_module == kRendererModule && definition.version == Version{1, 0, 0},
                "profile executable identity differs");
        require(contract.definition_id == definition.id && contract.version == definition.version &&
                    contract.structure == FeatureStructure::kAtomic && contract.domains == to_mask(FeatureDomain::kRender) &&
                    contract.execution == ExecutionKind::kContinuous && contract.state == StatePolicy::kStateless &&
                    contract.children.empty() && contract.child_port_mappings.empty(), "profile is not a pure Atomic Render Feature");
        require(contract.scope.id == ScopeId("Frame") && contract.clock.id == renderer::kSceneRenderProfileClock &&
                    contract.clock.frequency_hz == 60 && contract.clock.max_backlog == 4 &&
                    contract.clock.late_data_policy == LateDataPolicy::kDrop, "profile Frame clock/scope differs");
        require(contract.clock.capability_version == Version{1, 0, 0} && contract.clock.timing &&
                    contract.clock.timing->phase_ns == 0 && contract.clock.timing->relative_deadline_ns == 16'666'667 &&
                    contract.clock.timing->deadline_policy == DeadlinePolicy::kObserve && contract.clock.work_budget_ns == 250000,
                "profile Clock timing or declared work budget differs");
        require(contract.ports.empty() && contract.parameters.empty() && !contract.has_continuation &&
                    !contract.allows_child_process && bool(definition.execute), "profile depends on author ports, state or child processes");
        require(contract.side_effects.size() == 1 && contract.side_effects[0].id == kRenderIntentEffect &&
                    contract.side_effects[0].version == 1 && contract.side_effects[0].kind == SideEffectKind::kIntent &&
                    contract.side_effects[0].commit == CommitPolicy::kOwnerBoundary, "profile RenderIntent commit effect differs");
        auto capabilities = to_mask(RenderIntentCapability::kDrawScene) | to_mask(RenderIntentCapability::kWriteExternal) |
                            to_mask(RenderIntentCapability::kExportOutput);
        if (kProfiles[index] != SceneRenderProfile::kOpaque) capabilities |= to_mask(RenderIntentCapability::kReadExternal);
        if (kProfiles[index] == SceneRenderProfile::kForward)
            capabilities |= to_mask(RenderIntentCapability::kCullLights) | to_mask(RenderIntentCapability::kReadLightList);
        require(contract.permissions.version == 1 && contract.permissions.capability_mask == capabilities,
                "profile permissions do not match its actual typed operations");
        const auto* registered = registry.find_definition(definition.id);
        const auto entry = std::find_if(catalog.value.entries().begin(), catalog.value.entries().end(),
            [&](const auto& value) { return value.definition_id == definition.id; });
        require(registered && entry != catalog.value.entries().end() && entry->owner_module == definition.owner_module &&
                    entry->contract_digest == feature_contract_digest(definition) &&
                    feature_contract_digest(*registered) == entry->contract_digest, "catalog metadata is not from the executable definition");
        const auto preset = renderer::scene_render_profile_graph(kProfiles[index]);
        require(preset && preset.value.asset_uuid == kAssets[index] && preset.value.schema_version == 1 &&
                    preset.value.graph_version == 1 && preset.value.contract_digest == registry.contract_digest() &&
                    preset.value.nodes.size() == 1 && preset.value.edges.empty() && preset.value.process_subgraphs.empty(),
                "profile preset is not a real single-node versioned FeatureGraph");
        require(preset.value.nodes[0].node_id == FeatureNodeId("Renderer.Scene.Root") &&
                    preset.value.nodes[0].definition_id == definition.id && preset.value.nodes[0].definition_version == definition.version,
                "profile preset does not reference its executable definition");
        for (size_t invalid = 0; invalid != 3; ++invalid) {
            auto bad = preset.value;
            if (invalid == 0) bad.nodes[0].definition_id = FeatureDefinitionId("Renderer.Pipeline.Forward");
            if (invalid == 1) bad.nodes[0].definition_version.major = 2;
            if (invalid == 2) bad.contract_digest ^= 1;
            require(!FeatureGraphCompiler{}.compile(bad, registry), "profile preset accepted a foreign identity/version/digest");
        }
    }
    const auto invalid = static_cast<SceneRenderProfile>(255);
    require(!renderer::scene_render_profile_definition(invalid) && !renderer::scene_render_profile_graph(invalid),
            "profile factory accepted an unknown enum");
}

void duplicate_registration_is_transactional() {
    for (size_t collision = 0; collision < kProfiles.size(); ++collision) {
        FeatureRegistry registry{aeg::feature::test::clock_test_host()};
        auto definition = renderer::scene_render_profile_definition(kProfiles[collision]);
        require(definition && aeg::feature::test::register_clock_test_definition(registry, std::move(definition.value)), "collision fixture registration failed");
        const auto digest = registry.contract_digest();
        require(!renderer::register_scene_render_profiles(registry) && registry.contract_digest() == digest,
                "duplicate profile registration partially changed the registry");
        for (size_t index = 0; index < kIds.size(); ++index)
            require(bool(registry.find_definition(kIds[index])) == (index == collision), "duplicate profile registration added another profile");
    }
    FeatureRegistry conflicting{aeg::feature::test::clock_test_host()};
    auto unrelated = renderer::scene_render_profile_definition(SceneRenderProfile::kOpaque);
    unrelated.value.id = unrelated.value.contract.definition_id = FeatureDefinitionId("Game.ConflictingFrame");
    unrelated.value.contract.clock.frequency_hz = 120;
    require(bool(aeg::feature::test::register_clock_test_definition(conflicting, std::move(unrelated.value))), "conflicting Clock fixture registration failed");
    const auto digest = conflicting.contract_digest();
    require(!renderer::register_scene_render_profiles(conflicting) && conflicting.contract_digest() == digest,
            "conflicting Frame contract registration changed the registry");
    for (auto id : kIds) require(!conflicting.find_definition(id), "conflicting Clock registration left a profile");
}

void real_runtime_capture_and_private_light_list() {
    for (auto profile : kProfiles) {
        FeatureRegistry registry{aeg::feature::test::clock_test_host()};
        require(bool(renderer::register_scene_render_profiles(registry)), "Runtime profile registration failed");
        const auto preset = renderer::scene_render_profile_graph(profile);
        auto compiled = FeatureGraphCompiler{}.compile(preset.value, registry);
        require(bool(compiled), "production profile preset did not compile");
        FeatureRuntime runtime(registry);
        require(bool(runtime.publish_graph(preset.value, compiled.value)) && compiled.value.provenance.size() == 1 &&
                    compiled.value.plans.size() == 1 && compiled.value.plans[0].clock == renderer::kSceneRenderProfileClock,
                "production profile did not publish a real Frame execution plan");
        require(bool(runtime.tick(renderer::kSceneRenderProfileClock)), "production profile did not commit on its Frame clock");
        auto captured = runtime.capture_render_outputs(compiled.value);
        require(captured && captured.value.size() == 1, "production profile has no actual captured RenderIntent");
        const auto output = captured.value.front();
        const auto& provenance = compiled.value.provenance.front();
        const auto* owner = runtime.find_instance(provenance.instance_id);
        require(owner && output.node_id == provenance.node_id && output.instance_id == provenance.instance_id &&
                    output.contract.definition_id == provenance.definition_id && output.owner_module == kRendererModule &&
                    owner->scope == ScopeId("Frame") && owner->state.payload.empty() && owner->state.commit_count == 1 &&
                    output.input_messages.empty() && output.resource_owner.instance_id == owner->id &&
                    output.resource_owner.node_id == output.node_id && output.resource_owner.definition_digest == owner->definition_digest &&
                    output.resource_owner.resource_epoch == owner->render_resource_epoch && owner->last_invocation_committed &&
                    output.invocation == owner->last_invocation_committed->attempt.token,
                "profile output source does not come from its real Runtime owner and committed invocation");
        check_intent(profile, output);
        const auto plan = render_graph::RenderFeatureProjector{}.plan(captured.value);
        const auto work = render_graph::RenderFeatureProjector{}.project(captured.value, 42);
        require(plan && plan.value.output_order == std::vector<size_t>{0} && work &&
                    work.value.size() == output.intent.operations.size(), "profile cannot project its captured operation sequence");
        std::unordered_set<uint64_t> ids;
        for (size_t index = 0; index < work.value.size(); ++index) {
            const auto& item = work.value[index];
            require(item.feature_definition_id == provenance.definition_id && item.owner_module == kRendererModule &&
                        item.node_id == provenance.node_id && item.instance_id == provenance.instance_id &&
                        item.invocation == output.invocation && item.operation_index == index && item.frame_index == 42 &&
                        item.kind == render_graph::RenderWorkItemKind::kExecute && ids.insert(item.id.value()).second,
                    "profile work has fabricated or duplicate operation provenance");
        }
        for (uint64_t bit = 1; bit <= to_mask(RenderIntentCapability::kReadLightList); bit <<= 1) {
            if (!(output.contract.permissions.capability_mask & bit)) continue;
            auto denied = output.contract;
            denied.permissions.capability_mask &= ~bit;
            require(!validate_render_intent(output.intent, denied), "profile used a capability it did not require");
        }
        if (profile == SceneRenderProfile::kForward) {
            require(plan.value.light_list_uses.size() == 1 && plan.value.light_list_uses[0].producer_output == 0 &&
                        plan.value.light_list_uses[0].consumer_output == 0 && plan.value.light_list_uses[0].producer_operation == 2 &&
                        plan.value.light_list_uses[0].consumer_operation == 3, "private LightList does not select its real earlier Cull operation");
            for (size_t invalid = 0; invalid != 13; ++invalid) {
                auto bad = captured.value;
                auto& cull = std::get<RenderLightCullingScene>(bad[0].intent.operations[2]);
                auto& draw = std::get<RenderDrawClusteredScene>(bad[0].intent.operations[3]);
                switch (invalid) {
                case 0: cull.output.export_port = PortId("Unpublished"); break;
                case 1: draw.light_list.producer_invocation = InvocationId(output.invocation.invocation_id.value() + 100); break;
                case 2: draw.light_list.producer_generation = RuntimeGeneration(output.invocation.generation.value() + 1); break;
                case 3: draw.light_list.id = RenderResourceId("Foreign.LightList"); break;
                case 4: ++draw.light_list.version; break;
                case 5: draw.scene.id = RenderResourceId("Foreign.Scene"); break;
                case 6: draw.depth.target.id = RenderResourceId("Foreign.CameraDepth"); break;
                case 7: draw.directional_shadow.reset(); break;
                case 8: std::swap(bad[0].intent.operations[2], bad[0].intent.operations[3]); break;
                case 9: bad[0].resource_owner.definition_digest = 0; break;
                case 10: bad[0].resource_owner.resource_epoch = 0; break;
                case 11: bad[0].resource_owner.instance_id = FeatureInstanceId(output.instance_id.value() + 1); break;
                case 12: draw.depth.load = RenderAttachmentLoad::kClear; break;
                }
                require(!render_graph::RenderFeatureProjector{}.plan(bad),
                        "private LightList accepted malformed source/ordering/scene/depth case " + std::to_string(invalid));
            }
            const auto token = runtime.invoke(output.instance_id);
            const auto* definition = registry.find_definition(output.contract.definition_id);
            require(token && definition, "profile rejection invocation failed");
            auto delta = definition->execute(token.value, StateSnapshot{owner->state.version}, {}, {});
            require(bool(delta), "profile rejection fixture could not obtain a pure valid delta");
            auto rejected = delta.value;
            std::get<RenderDrawClusteredScene>(rejected.render_intent->operations[3]).depth.load = RenderAttachmentLoad::kClear;
            const auto version = owner->state.version;
            require(!runtime.complete(token.value, rejected) && owner->state.version == version && owner->state.commit_count == 1,
                    "invalid profile intent changed its owner state");
            const auto preserved = runtime.capture_render_outputs(compiled.value);
            require(preserved && preserved.value.front().invocation == output.invocation,
                    "invalid profile intent replaced the last accepted presentation");
            require(bool(runtime.complete(token.value, delta.value)), "invalid profile intent consumed the valid commit token");
        }
        require(bool(runtime.tick(renderer::kSceneRenderProfileClock)), "second profile Frame tick failed");
        const auto next = runtime.capture_render_outputs(compiled.value);
        require(next && next.value.size() == 1 && next.value.front().invocation != output.invocation &&
                    next.value.front().instance_id == output.instance_id && next.value.front().resource_owner == output.resource_owner,
                "profile did not retain its owner while issuing a fresh invocation");
        check_intent(profile, next.value.front());
    }
}

void private_light_lists_do_not_authorize_another_owner() {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    require(bool(renderer::register_scene_render_profiles(registry)), "private ownership registry failed");
    auto graph = renderer::scene_render_profile_graph(SceneRenderProfile::kForward);
    require(bool(graph), "private ownership profile graph failed");
    auto second = graph.value.nodes.front();
    second.node_id = FeatureNodeId("Renderer.Scene.SecondOwner");
    graph.value.nodes.push_back(second);
    auto compiled = FeatureGraphCompiler{}.compile(graph.value, registry);
    FeatureRuntime runtime(registry);
    require(compiled && runtime.publish_graph(graph.value, compiled.value) && runtime.tick(renderer::kSceneRenderProfileClock),
            "private ownership graph did not execute");
    const auto captured = runtime.capture_render_outputs(compiled.value);
    require(captured && captured.value.size() == 2 && captured.value[0].instance_id != captured.value[1].instance_id &&
                captured.value[0].input_messages.empty() && captured.value[1].input_messages.empty(),
            "private ownership graph did not capture two independent owners");
    const auto plan = render_graph::RenderFeatureProjector{}.plan(captured.value);
    require(plan && plan.value.light_list_uses.size() == 2, "identical private LightList names alias between owners");
    for (const auto& use : plan.value.light_list_uses)
        require(use.producer_output == use.consumer_output && use.producer_operation == 2 && use.consumer_operation == 3,
                "private LightList resolves to another owner's producer");
    auto borrowed = captured.value;
    std::get<RenderDrawClusteredScene>(borrowed[1].intent.operations[3]).light_list =
        std::get<RenderDrawClusteredScene>(borrowed[0].intent.operations[3]).light_list;
    require(!render_graph::RenderFeatureProjector{}.plan(borrowed),
            "another owner's private LightList became authorized without an actual DataEdge receipt");
}
} // namespace

int main() {
    try {
        definitions_catalog_and_presets_share_contracts();
        duplicate_registration_is_transactional();
        real_runtime_capture_and_private_light_list();
        private_light_lists_do_not_authorize_another_owner();
        std::cout << "Engine Scene Feature definitions/catalog/presets and actual private LightList provenance passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
