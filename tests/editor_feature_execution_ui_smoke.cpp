/** @brief Actual published plan inspection through the FeatureGraph ImGui panel. */
#include "feature_clock_test_fixture.hpp"
#include "editor_feature_graph_panel.hpp"
#include "editor_render_graph_inspection.hpp"
#include "../Engine/Renderer/private/renderer_feature_profiles.hpp"
#include "../Engine/RenderGraph/private/render_feature_projector.hpp"
#include <imgui_internal.h>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <span>

namespace {
using namespace aeg;
void require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
feature::FeatureDefinition definition(uint64_t id) {
    feature::FeatureDefinition definition;
    definition.id = feature::FeatureDefinitionId{id}; definition.version = {1, 0, 0};
    auto& contract = definition.contract;
    contract.definition_id = definition.id; contract.version = definition.version;
    contract.domains = feature::to_mask(feature::FeatureDomain::kLogic);
    contract.scope = {feature::ScopeId{"World"}};
    contract.clock = aeg::feature::make_periodic_clock_contract(feature::ClockId{"LogicTick"}, 60, 4, feature::LateDataPolicy::kReject,
        aeg::feature::DeadlinePolicy::kObserve, contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    contract.permissions.version = 1;
    return definition;
}

struct PublishedRenderSource final {
    application::runtime::FeatureRuntimeExecutionSnapshot source{};
    std::vector<render_graph::RenderWorkItem> work{};
};

PublishedRenderSource capture_render_source(feature::FeatureRuntime& runtime,
    application::runtime::FeatureGraphRuntimeHost& host, std::string_view asset_uuid, bool issue_frame_tick = true) {
    if (issue_frame_tick)
        require(static_cast<bool>(runtime.tick(renderer::kSceneRenderProfileClock)), "Actual Render profile tick failed");
    const auto captured = host.capture_render_outputs();
    const auto inspected = host.inspect_execution(asset_uuid);
    require(captured && inspected && inspected.value.provenance.size() == 1,
            "Actual Render source output/inspection was not captured");
    const auto selected = std::find_if(captured.value.begin(), captured.value.end(), [&](const auto& output) {
        return output.instance_id == inspected.value.provenance.front().runtime.instance_id;
    });
    require(selected != captured.value.end(), "Actual source instance has no captured Render output");
    const auto& output = *selected;
    const auto& attribution = inspected.value.provenance.front().runtime;
    require(attribution.invocation_id == output.invocation.invocation_id && attribution.node_id == output.node_id &&
                attribution.definition_id == output.contract.definition_id && attribution.owner_module == output.owner_module,
            "Published UI source lost its actual invocation, node, definition or module attribution");
    const auto projected = render_graph::RenderFeatureProjector{}.project(
        std::span<const feature::RenderFeatureOutput>{&output, 1}, 42);
    require(projected && !projected.value.empty(), "Actual Render source work was not projected");
    for (const auto& work : projected.value) {
        require(work.invocation == output.invocation && work.instance_id == output.instance_id &&
                    work.node_id == output.node_id && work.feature_definition_id == output.contract.definition_id &&
                    work.owner_module == feature::ModuleIdentity{"com.aeg.renderer"} &&
                    work.invocation.generation == inspected.value.generation &&
                    work.invocation.invocation_id.is_valid() && work.invocation.attempt_id.is_valid(),
                "UI fixture lost actual projector token, instance or module provenance");
    }
    PublishedRenderSource result{inspected.value, projected.value};
    result.source.source_path = std::filesystem::path{"Assets/Features"} / (std::string{asset_uuid} + ".aegfeature");
    return result;
}

std::shared_ptr<const application::EditorRenderGraphSnapshot> diagnostic_snapshot(
    std::initializer_list<const PublishedRenderSource*> sources) {
    auto snapshot = std::make_shared<application::EditorRenderGraphSnapshot>();
    snapshot->frame_index = 42;
    snapshot->plan.revision = 1;
    if (sources.size() == 1) snapshot->generation = (*sources.begin())->source.generation;
    // This table is a UI diagnostic fixture, not a compiled/executed GPU graph.
    // Every feature_work and source observation comes from the real Runtime/projector above.
    for (const auto* source : sources) {
        snapshot->sources.push_back(source->source);
        for (const auto& work : source->work) {
            const auto index = static_cast<uint32_t>(snapshot->plan.pass_plans.size());
            render_graph::RenderGraphPassPlan pass;
            pass.pass = {index, 1, 1};
            pass.batch_index = 0; pass.topological_index = index; pass.live = true;
            pass.feature_work = work;
            snapshot->plan.pass_plans.push_back(std::move(pass));
            snapshot->plan.topological_order.push_back(index);
        }
    }
    snapshot->compilation.pass_count = static_cast<uint32_t>(snapshot->plan.pass_plans.size());
    return snapshot;
}

void exercise_render_source_navigation(const feature::FeatureGraphDocument& authored,
    const feature::FeatureCatalog& catalog, application::runtime::FeatureGraphRuntimeHost& current_host,
    const std::shared_ptr<const application::EditorRenderGraphSnapshot>& snapshot, int pass_index,
    bool expected_navigation, const char* failure_message) {
    application::EditorFeatureGraphDocument document{authored, catalog};
    const auto original = document.snapshot_owner();
    application::EditorFeatureGraphPanel panel;
    auto* context = ImGui::CreateContext(); auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
    io.DisplaySize = {1680, 2600}; io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels{}; int width{}, height{}; io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    unsigned graph_queries = 0, source_navigations = 0;
    application::EditorFeatureGraphPanel::Actions actions;
    actions.inspect_execution = [&](std::string_view asset) { return current_host.inspect_execution(asset); };
    actions.inspect_render_graph = [&]() {
        ++graph_queries;
        return foundation::ResultValue<std::shared_ptr<const application::EditorRenderGraphSnapshot>>{
            foundation::Result::success(), snapshot};
    };
    actions.inspect_node = [&](std::string_view asset, feature::FeatureNodeId node) {
        ++source_navigations;
        require(asset == authored.asset_uuid && node == feature::FeatureNodeId{17},
                "RenderGraph source selected a foreign asset or runtime node identity");
        return current_host.inspect_node(asset, node);
    };
    const auto draw = [&](ImGuiID activate = 0) {
        ImGui::NewFrame();
        if (activate) {
            auto* window = ImGui::FindWindowByName("Flow Graph###FlowGraph");
            context->NavWindow = window; context->NavId = activate; context->NavInputSource = ImGuiInputSource_Keyboard;
            context->NavActivateId = context->NavActivateDownId = activate;
        }
        ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(io.DisplaySize);
        panel.draw(document, "CurrentRender.aegfeature", actions); ImGui::Render();
    };
    draw();
    auto* window = ImGui::FindWindowByName("Flow Graph###FlowGraph");
    draw(window->GetID("Show RenderGraph"));
    require(graph_queries != 0 && source_navigations == 0, "RenderGraph view did not open without selecting a source");
    const auto row = ImHashData(&pass_index, sizeof(pass_index), window->GetID("FeatureGraphRenderGraph"));
    draw(ImHashStr("Node 17", 0, row));
    if ((source_navigations != 0) != expected_navigation) {
        std::cerr << failure_message << " (source navigation calls=" << source_navigations << ")\n";
        std::exit(1);
    }
    require(document.snapshot_owner() == original && !document.dirty() && !document.can_undo(),
            "RenderGraph source navigation changed the owning author draft");
    ImGui::DestroyContext(context);
}

void render_graph_sources_use_actual_generations() {
    feature::FeatureRegistry first_registry{aeg::feature::test::clock_test_host()}, second_registry{aeg::feature::test::clock_test_host()};
    require(static_cast<bool>(renderer::register_scene_render_profiles(first_registry)) &&
                static_cast<bool>(renderer::register_scene_render_profiles(second_registry)),
            "Actual engine Render definitions were not registered");
    const auto catalog = renderer::scene_render_profile_catalog();
    auto first_graph = renderer::scene_render_profile_graph(renderer::SceneRenderProfile::kOpaque);
    auto second_graph = renderer::scene_render_profile_graph(renderer::SceneRenderProfile::kForward);
    require(catalog && first_graph && second_graph, "Actual engine Render source graph/catalog generation failed");
    first_graph.value.asset_uuid = "render-ui-current";
    second_graph.value.asset_uuid = "render-ui-other";
    // Equal authored node ids ensure the foreign-asset guard is exercised independently.
    first_graph.value.nodes.front().node_id = feature::FeatureNodeId{17};
    second_graph.value.nodes.front().node_id = feature::FeatureNodeId{17};
    feature::FeatureRuntime first_runtime{first_registry}, second_runtime{second_registry};
    application::runtime::FeatureGraphRuntimeHost first_host{first_runtime, first_registry},
        second_host{second_runtime, second_registry};
    require(static_cast<bool>(first_host.stage(first_graph.value)) && static_cast<bool>(first_host.apply_pending()) &&
                static_cast<bool>(second_host.stage(second_graph.value)) && static_cast<bool>(second_host.apply_pending()),
            "Actual Render source publication failed");
    const auto historical = capture_render_source(first_runtime, first_host, first_graph.value.asset_uuid);
    const auto foreign = capture_render_source(second_runtime, second_host, second_graph.value.asset_uuid);
    require(historical.work.size() == 1 && foreign.work.size() == 5 &&
                historical.source.generation != foreign.source.generation,
            "UI sources did not originate from distinct actual Opaque/Forward Runtimes");
    ++first_graph.value.graph_version;
    require(static_cast<bool>(first_host.stage(first_graph.value)) && static_cast<bool>(first_host.apply_pending()),
            "Actual same-asset new generation publication failed");
    const auto current = capture_render_source(first_runtime, first_host, first_graph.value.asset_uuid);
    require(current.source.generation != historical.source.generation &&
                current.source.generation != foreign.source.generation,
            "UI fixture stamped generations instead of observing real replacement");
    const auto single_current = diagnostic_snapshot({&current});
    const auto single_historical = diagnostic_snapshot({&historical});
    const auto mixed = diagnostic_snapshot({&current, &foreign});
    require(!mixed->generation.is_valid() && mixed->sources.size() == 2 &&
                mixed->sources[0].generation != mixed->sources[1].generation,
            "Mixed diagnostic snapshot did not preserve the actual per-source generations");
    feature::FeatureRegistry shared_registry{aeg::feature::test::clock_test_host()};
    require(static_cast<bool>(renderer::register_scene_render_profiles(shared_registry)), "Shared Runtime definitions failed");
    feature::FeatureRuntime shared_runtime{shared_registry};
    application::runtime::FeatureGraphRuntimeHost shared_host{shared_runtime, shared_registry};
    auto same_generation_other = first_graph.value;
    same_generation_other.asset_uuid = "render-ui-same-generation-other";
    require(static_cast<bool>(shared_host.stage(first_graph.value)) &&
                static_cast<bool>(shared_host.stage(same_generation_other)) && static_cast<bool>(shared_host.apply_pending()),
            "Same-generation real multi-asset publication failed");
    const auto shared_current = capture_render_source(shared_runtime, shared_host, first_graph.value.asset_uuid);
    const auto shared_foreign = capture_render_source(shared_runtime, shared_host, same_generation_other.asset_uuid, false);
    require(shared_current.source.generation == shared_foreign.source.generation &&
                shared_current.source.asset_uuid != shared_foreign.source.asset_uuid &&
                shared_current.work.front().node_id != shared_foreign.work.front().node_id,
            "Same-generation source negative control did not preserve actual separate asset ownership");
    const auto same_generation_assets = diagnostic_snapshot({&shared_current, &shared_foreign});
    std::cout << "RenderGraph current source positive control\n";
    exercise_render_source_navigation(first_graph.value, catalog.value, first_host, single_current, 0, true,
        "Current RenderGraph source did not navigate to its authored owner");
    std::cout << "RenderGraph same-Runtime same-generation foreign-asset negative control\n";
    exercise_render_source_navigation(first_graph.value, catalog.value, shared_host, same_generation_assets, 1, false,
        "Same-generation foreign-asset RenderGraph source navigated into the active author draft");
    std::cout << "RenderGraph foreign-asset source negative control\n";
    exercise_render_source_navigation(first_graph.value, catalog.value, first_host, mixed, 1, false,
        "Foreign-asset RenderGraph source navigated into the active author draft");
    std::cout << "RenderGraph historical same-asset source negative control\n";
    exercise_render_source_navigation(first_graph.value, catalog.value, first_host, single_historical, 0, false,
        "Historical same-asset RenderGraph source navigated into the current generation");
    std::cout << "RenderGraph two-Runtime mixed generation current source\n";
    exercise_render_source_navigation(first_graph.value, catalog.value, first_host, mixed, 0, true,
        "Mixed actual Runtime generations incorrectly disabled current RenderGraph source navigation");
}
}

int main() {
    feature::FeatureRegistry registry{aeg::feature::test::clock_test_host()}; const auto definition = ::definition(1);
    require(static_cast<bool>(aeg::feature::test::register_clock_test_definition(registry, definition)), "Definition failed");
    feature::FeatureCatalog catalog; require(static_cast<bool>(catalog.export_definition(definition)), "Catalog failed");
    feature::FeatureGraphDocument a; a.asset_uuid = "asset://execution-ui-a";
    a.schema_version = 1; a.graph_version = 1; a.draft_revision = 1; a.contract_digest = registry.contract_digest();
    a.nodes = {{feature::FeatureNodeId{17}, definition.id, definition.version}};
    auto b = a; b.asset_uuid = "asset://execution-ui-b";
    feature::FeatureRuntime runtime{registry}; application::runtime::FeatureGraphRuntimeHost host{runtime, registry};
    require(static_cast<bool>(host.stage(a)) && static_cast<bool>(host.stage(b)) && static_cast<bool>(host.apply_pending()), "Publish failed");
    application::EditorFeatureGraphDocument document{a, catalog};
    application::EditorFeatureGraphPanel panel;
    auto* context = ImGui::CreateContext(); auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
    io.DisplaySize = {1680, 2600}; io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels{}; int width{}, height{}; io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    unsigned inspections = 0, nodes = 0;
    application::runtime::FeatureRuntimeExecutionSnapshot observed;
    application::EditorFeatureGraphPanel::Actions actions;
    actions.inspect_execution = [&](std::string_view asset) {
        ++inspections; auto result = host.inspect_execution(asset);
        if (result) observed = result.value;
        return result;
    };
    actions.inspect_node = [&](std::string_view asset, feature::FeatureNodeId node) {
        ++nodes;
        require(asset == observed.asset_uuid && node == feature::FeatureNodeId{17}, "Derived view selected runtime ID/foreign root as authored node");
        return host.inspect_node(asset, node);
    };
    const auto draw = [&](application::EditorFeatureGraphDocument& doc, ImGuiID activate = 0) {
        ImGui::NewFrame();
        if (activate) {
            auto* window = ImGui::FindWindowByName("Flow Graph###FlowGraph");
            context->NavWindow = window; context->NavId = activate; context->NavInputSource = ImGuiInputSource_Keyboard;
            context->NavActivateId = context->NavActivateDownId = activate;
        }
        ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(io.DisplaySize);
        actions.publication_pending = host.has_pending(); panel.draw(doc, "Execution.aegfeature", actions); ImGui::Render();
    };
    draw(document); require(inspections == 0, "Closed derived view queried runtime");
    const auto original = document.snapshot_owner();
    auto* window = ImGui::FindWindowByName("Flow Graph###FlowGraph");
    draw(document, window->GetID("Show ExecutionPlan"));
    require(inspections != 0 && observed.asset_uuid == a.asset_uuid && observed.generation == host.generation(), "Published plan was not opened from actual panel");
    require(document.snapshot_owner() == original && !document.dirty(), "Opening derived plan mutated draft");
    const auto select_node = [&](application::EditorFeatureGraphDocument& doc) {
        const auto instance = std::to_string(observed.provenance.front().runtime.instance_id.value());
        const auto row = ImHashStr(instance.c_str(), 0, window->GetID("FeatureGraphExecution"));
        const auto before = nodes; draw(doc, ImHashStr("Node 17", 0, row));
        require(nodes > before, "Derived node did not navigate to authored source selection");
    };
    select_node(document);
    require(static_cast<bool>(document.execute("Dirty draft", [](auto& next) { ++next.graph_version; return foundation::Result::success(); })), "Draft edit failed");
    const auto dirty = document.snapshot_owner(); require(static_cast<bool>(host.stage(document.snapshot())), "Pending stage failed");
    draw(document); require(observed.graph_version == a.graph_version && document.snapshot_owner() == dirty, "Pending draft replaced published plan");
    auto invalid = document.snapshot(); invalid.contract_digest ^= 1;
    require(!host.stage(invalid), "Bad digest accepted"); draw(document);
    require(observed.graph_version == a.graph_version && document.snapshot_owner() == dirty, "Rejected Apply replaced published plan");
    application::EditorFeatureGraphDocument other{b, catalog}; const auto other_original = other.snapshot_owner();
    draw(other); require(observed.asset_uuid == b.asset_uuid, "Root switch retained another root's derived plan");
    select_node(other); require(other.snapshot_owner() == other_original && !other.dirty(), "Derived source selection mutated another root");
    ImGui::DestroyContext(context);
    render_graph_sources_use_actual_generations();
    std::cout << "Published ExecutionPlan ImGui inspection passed\n";
}
