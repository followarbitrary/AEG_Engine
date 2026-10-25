#include "editor_renderer_feature_host.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {
using namespace aeg;

bool require(bool condition, std::string_view message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

template <typename T>
bool require(const foundation::ResultValue<T>& result, std::string_view message) {
    if (!result) std::cerr << message << ": " << result.result.message << '\n';
    return static_cast<bool>(result);
}

bool require(const foundation::Result& result, std::string_view message) {
    if (!result) std::cerr << message << ": " << result.message << '\n';
    return static_cast<bool>(result);
}

feature::FeatureDefinitionId definition_id(renderer::SceneRenderProfile profile) {
    switch (profile) {
    case renderer::SceneRenderProfile::kOpaque: return feature::FeatureDefinitionId{"Renderer.Scene.Opaque"};
    case renderer::SceneRenderProfile::kForward: return feature::FeatureDefinitionId{"Renderer.Scene.Forward"};
    case renderer::SceneRenderProfile::kDeferred: return feature::FeatureDefinitionId{"Renderer.Scene.Deferred"};
    }
    return {};
}

bool output_is_actual_profile(const feature::RenderFeatureOutput& output,
                              renderer::SceneRenderProfile profile,
                              const application::runtime::FeatureRuntimeExecutionSnapshot& snapshot) {
    if (!require(output.contract.definition_id == definition_id(profile) &&
                     output.contract.domains == feature::to_mask(feature::FeatureDomain::kRender) &&
                     output.contract.scope.id == feature::ScopeId{"Frame"} &&
                     output.contract.clock.id == feature::ClockId{"Frame"} &&
                     output.owner_module == feature::ModuleIdentity{"com.aeg.renderer"} &&
                     output.invocation.invocation_id.is_valid() && output.invocation.attempt_id.is_valid() &&
                     output.invocation.generation == snapshot.generation &&
                     output.resource_owner.instance_id == output.instance_id && output.instance_id.is_valid() &&
                     output.resource_owner.node_id == output.node_id && output.node_id.is_valid() &&
                     output.resource_owner.definition_digest != 0 && output.resource_owner.resource_epoch != 0,
                 "Scene output lost actual definition, clock, invocation or resource ownership")) return false;
    if (!require(snapshot.provenance.size() == 1 && snapshot.plans.size() == 1 &&
                     snapshot.plans.front().clock == feature::ClockId{"Frame"} &&
                     snapshot.plans.front().instances.size() == 1 &&
                     snapshot.plans.front().instances.front() == output.instance_id &&
                     snapshot.provenance.front().authored_node_id == feature::FeatureNodeId{"Renderer.Scene.Root"} &&
                     snapshot.provenance.front().runtime.definition_id == output.contract.definition_id &&
                     snapshot.provenance.front().runtime.instance_id == output.instance_id &&
                     snapshot.provenance.front().runtime.node_id == output.node_id &&
                     snapshot.provenance.front().runtime.invocation_id == output.invocation.invocation_id &&
                     !output.owner_path.empty(),
                 "Published plan did not trace its actual Scene invocation")) return false;

    const auto& operations = output.intent.operations;
    if (profile == renderer::SceneRenderProfile::kOpaque) {
        const auto* draw = operations.size() == 1 ? std::get_if<feature::RenderDrawScene>(&operations[0]) : nullptr;
        return require(draw && draw->depth && output.intent.final_output &&
                           output.intent.final_output->id == draw->target.id,
                       "Opaque profile did not emit its real color/depth draw");
    }
    if (profile == renderer::SceneRenderProfile::kForward) {
        const auto* camera = operations.size() == 5 ? std::get_if<feature::RenderDrawDepthScene>(&operations[0]) : nullptr;
        const auto* shadow = operations.size() == 5 ? std::get_if<feature::RenderDrawDepthScene>(&operations[1]) : nullptr;
        const auto* cull = operations.size() == 5 ? std::get_if<feature::RenderLightCullingScene>(&operations[2]) : nullptr;
        const auto* draw = operations.size() == 5 ? std::get_if<feature::RenderDrawClusteredScene>(&operations[3]) : nullptr;
        const auto* post = operations.size() == 5 ? std::get_if<feature::RenderPostProcessScene>(&operations[4]) : nullptr;
        return require(camera && shadow && cull && draw && post &&
                           camera->projection == feature::RenderDepthSceneProjection::kCamera &&
                           shadow->projection == feature::RenderDepthSceneProjection::kDirectionalShadow &&
                           cull->camera_depth.id == camera->depth.target.id &&
                           draw->depth.target.id == camera->depth.target.id &&
                           draw->depth.load == feature::RenderAttachmentLoad::kLoad &&
                           !cull->output.export_port.is_valid() && cull->output.id == draw->light_list.id &&
                           draw->light_list.producer_invocation == output.invocation.invocation_id &&
                           draw->light_list.producer_generation == output.invocation.generation &&
                           cull->directional_shadow && draw->directional_shadow &&
                           cull->directional_shadow->id == shadow->depth.target.id &&
                           draw->directional_shadow->id == shadow->depth.target.id &&
                           post->hdr.id == draw->hdr.target.id && output.intent.final_output &&
                           output.intent.final_output->id == post->output.target.id,
                       "Forward profile did not emit the actual depth/cull/clustered/postprocess chain");
    }
    const auto* shadow = operations.size() == 4 ? std::get_if<feature::RenderDrawDepthScene>(&operations[0]) : nullptr;
    const auto* gbuffer = operations.size() == 4 ? std::get_if<feature::RenderDrawGBufferScene>(&operations[1]) : nullptr;
    const auto* lighting = operations.size() == 4 ? std::get_if<feature::RenderDeferredLightingScene>(&operations[2]) : nullptr;
    const auto* post = operations.size() == 4 ? std::get_if<feature::RenderPostProcessScene>(&operations[3]) : nullptr;
    return require(shadow && gbuffer && lighting && post &&
                       shadow->projection == feature::RenderDepthSceneProjection::kDirectionalShadow &&
                       lighting->albedo.id == gbuffer->albedo.target.id &&
                       lighting->normal.id == gbuffer->normal.target.id &&
                       lighting->depth.id == gbuffer->depth.target.id && lighting->directional_shadow &&
                       lighting->directional_shadow->id == shadow->depth.target.id &&
                       post->hdr.id == lighting->hdr.target.id && output.intent.final_output &&
                       output.intent.final_output->id == post->output.target.id,
                   "Deferred profile did not emit the actual shadow/GBuffer/lighting/postprocess chain");
}

bool profile_publication_and_retirement() {
    auto created = application::EditorRendererFeatureHost::create();
    if (!require(created, "Default Scene Feature host creation failed")) return false;
    auto& host = *created.value;
    const auto initial = host.capture();
    const auto initial_owners = host.capture_owners();
    if (!require(initial && initial.value && initial.value->empty(), "Host drew before its first real tick") ||
        !require(initial_owners && initial_owners.value.size() == 1, "Default graph has no actual live owner")) return false;
    feature::RuntimeGeneration previous_generation{};
    std::shared_ptr<const std::vector<feature::RenderFeatureOutput>> retained_outputs;
    application::runtime::FeatureRuntimeExecutionSnapshot retained_inspection;
    for (const auto profile : {renderer::SceneRenderProfile::kOpaque, renderer::SceneRenderProfile::kForward,
                               renderer::SceneRenderProfile::kDeferred, renderer::SceneRenderProfile::kForward}) {
        auto graph = renderer::scene_render_profile_graph(profile);
        if (!require(graph, "Scene Feature graph generation failed")) return false;
        graph.value.graph_version = 7;
        graph.value.draft_revision = 12;
        const auto source_uuid = graph.value.asset_uuid;
        if (!require(host.publish(graph.value), "Scene graph replacement failed")) return false;
        graph.value.nodes.clear();
        const auto unticked = host.capture();
        auto snapshot = host.inspect();
        if (!require(unticked && unticked.value && unticked.value->empty(), "Replacement exposed retired output") ||
            !require(snapshot && snapshot.value.generation.value() > previous_generation.value() &&
                         snapshot.value.asset_uuid == source_uuid && snapshot.value.graph_version == 7 &&
                         snapshot.value.draft_revision == 12 && snapshot.value.source_path.empty() &&
                         snapshot.value.contract_digest == host.catalog().contract_digest() &&
                         host.document().nodes.size() == 1,
                     "Publication lost its immutable source metadata or monotonic generation")) return false;
        previous_generation = snapshot.value.generation;
        if (!require(host.tick(), "Scene Frame clock tick failed")) return false;
        const auto outputs = host.capture();
        const auto owners = host.capture_owners();
        snapshot = host.inspect();
        if (!require(outputs && outputs.value && outputs.value->size() == 1 && owners && owners.value.size() == 1 && snapshot,
                     "Scene replacement accumulated old profiles or lost owners") ||
            !output_is_actual_profile(outputs.value->front(), profile, snapshot.value) ||
            !require(owners.value.front() == outputs.value->front().resource_owner, "Capture did not use its actual owner"))
            return false;
        retained_outputs = outputs.value;
        retained_inspection = snapshot.value;
        const auto issued = outputs.value->front().invocation.invocation_id;
        if (!require(host.tick(), "Second Scene Frame tick failed")) return false;
        const auto next = host.capture();
        if (!require(next && next.value->size() == 1 && next.value->front().invocation.invocation_id != issued &&
                         retained_outputs->front().invocation.invocation_id == issued,
                     "Ticks reused an invocation or mutated a caller-owned capture")) return false;
    }
    auto empty = host.document();
    empty.nodes.clear(); empty.edges.clear(); empty.process_subgraphs.clear();
    ++empty.graph_version;
    if (!require(host.publish(empty), "Empty Feature graph publication failed") || !require(host.tick(), "Empty graph tick failed"))
        return false;
    const auto outputs = host.capture();
    const auto owners = host.capture_owners();
    const auto empty_snapshot = host.inspect();
    if (!require(outputs && outputs.value && outputs.value->empty() && owners && owners.value.empty() &&
                     empty_snapshot && empty_snapshot.value.plans.empty() && empty_snapshot.value.provenance.empty() &&
                     empty_snapshot.value.generation.value() > previous_generation.value(),
                 "Empty graph revived hidden default work or retained old owners")) return false;
    created.value.reset();
    return require(retained_outputs && retained_outputs->size() == 1 &&
                       retained_inspection.asset_uuid == "renderer-scene-forward" && retained_inspection.provenance.size() == 1 &&
                       retained_outputs->front().invocation.generation == retained_inspection.generation,
                   "Host destruction invalidated caller-owned output/inspection metadata");
}

bool invalid_graphs_keep_last_good() {
    auto created = application::EditorRendererFeatureHost::create(renderer::SceneRenderProfile::kForward);
    if (!require(created, "Forward host creation failed") || !require(created.value->tick(), "Initial Forward tick failed")) return false;
    auto& host = *created.value;
    const auto accepted = host.document();
    const auto before = host.inspect();
    const auto captured = host.capture();
    if (!require(before && captured && captured.value->size() == 1, "Initial last-good output unavailable")) return false;
    constexpr std::array<std::string_view, 13> cases{
        "missing digest", "stale digest", "unknown definition", "wrong version", "missing node identity", "missing source identity",
        "missing schema", "unsupported schema", "missing graph version", "duplicate owner", "invalid parameters", "unsupported profile edge",
        "multiple complete Scene profiles"};
    for (std::size_t index = 0; index < cases.size(); ++index) {
        auto invalid = accepted;
        switch (index) {
        case 0: invalid.contract_digest = 0; break;
        case 1: invalid.contract_digest ^= 1; break;
        case 2: invalid.nodes.front().definition_id = feature::FeatureDefinitionId{"Game.Logic.NonProfile"}; break;
        case 3: ++invalid.nodes.front().definition_version.major; break;
        case 4: invalid.nodes.front().node_id = {}; break;
        case 5: invalid.asset_uuid.clear(); break;
        case 6: invalid.schema_version = 0; break;
        case 7: invalid.schema_version = 2; break;
        case 8: invalid.graph_version = 0; break;
        case 9: invalid.nodes.push_back(invalid.nodes.front()); break;
        case 10: invalid.nodes.front().parameter_blob = {1}; break;
        case 11:
            invalid.edges.push_back({invalid.nodes.front().node_id, feature::PortId{"Out"},
                                     invalid.nodes.front().node_id, feature::PortId{"In"}, feature::EdgeKind::kData,
                                     feature::ClockId{"Gameplay"}, feature::ClockId{"Frame"}, 1});
            break;
        case 12:
            invalid.nodes.push_back(invalid.nodes.front());
            invalid.nodes.back().node_id = feature::FeatureNodeId{"Renderer.Scene.Second"};
            break;
        }
        if (host.publish(invalid)) {
            std::cerr << "Invalid Scene graph accepted: " << cases[index] << '\n';
            return false;
        }
        const auto after = host.inspect();
        const auto output = host.capture();
        if (!require(after && output && output.value->size() == 1 && after.value.generation == before.value.generation &&
                         after.value.asset_uuid == before.value.asset_uuid && host.document().nodes.size() == 1 &&
                         output.value->front().invocation == captured.value->front().invocation &&
                         output.value->front().resource_owner == captured.value->front().resource_owner,
                     "Rejected publication changed the last-good runtime/source/output")) return false;
    }
    return require(host.tick(), "Bad publication left host staging poisoned");
}

bool independent_hosts_and_contract_scope() {
    auto first = application::EditorRendererFeatureHost::create(renderer::SceneRenderProfile::kDeferred);
    auto second = application::EditorRendererFeatureHost::create(renderer::SceneRenderProfile::kDeferred);
    if (!require(first && second, "Independent Scene hosts failed to initialize") ||
        !require(first.value->tick(), "First host tick failed") || !require(second.value->tick(), "Second host tick failed")) return false;
    const auto left = first.value->capture(); const auto right = second.value->capture();
    if (!require(left && right && left.value->size() == 1 && right.value->size() == 1 &&
                     left.value->front().invocation.generation != right.value->front().invocation.generation,
                 "Independent Preview hosts shared an actual runtime generation")) return false;
    const auto right_token = right.value->front().invocation;
    auto opaque = renderer::scene_render_profile_graph(renderer::SceneRenderProfile::kOpaque);
    if (!require(opaque, "Opaque replacement source unavailable") || !require(first.value->publish(opaque.value), "First replacement failed") ||
        !require(first.value->tick(), "First replacement tick failed")) return false;
    const auto unchanged = second.value->capture();
    if (!require(unchanged && unchanged.value->front().invocation == right_token &&
                     unchanged.value->front().contract.definition_id == feature::FeatureDefinitionId{"Renderer.Scene.Deferred"},
                 "Replacing one Preview host changed another host")) return false;
    for (const auto& entry : second.value->catalog().entries()) {
        if (!require(entry.domains == feature::to_mask(feature::FeatureDomain::kRender) &&
                         entry.contract.scope.id == feature::ScopeId{"Frame"} && entry.clock == feature::ClockId{"Frame"} &&
                         entry.contract.clock.id == entry.clock && entry.execution == feature::ExecutionKind::kContinuous &&
                         entry.owner_module == feature::ModuleIdentity{"com.aeg.renderer"} &&
                         entry.definition_id == entry.contract.definition_id && entry.version == entry.contract.version,
                     "Preview catalog exposed a non-engine Render/Frame contract")) return false;
    }
    return require(!application::EditorRendererFeatureHost::create(static_cast<renderer::SceneRenderProfile>(255)),
                   "Invalid default profile enum was accepted");
}
}  // namespace

int main() {
    static_assert(std::is_same_v<decltype(std::declval<const application::EditorRendererFeatureHost&>().document()),
                                 const feature::FeatureGraphDocument&>);
    if (!profile_publication_and_retirement()) return 1;
    if (!invalid_graphs_keep_last_good()) return 2;
    if (!independent_hosts_and_contract_scope()) return 3;
    std::cout << "Editor Renderer Feature host actual publication/clock/ownership checks passed\n";
    return 0;
}
