/**
 * @file editor_scene_runtime_smoke.cpp
 * @brief Verifies the minimum editable Scene document and offscreen preview loop.
 */
#include "feature_clock_test_fixture.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "Components/component_ids.hpp"
#include "Components/render_components.hpp"
#include "editor_scene_runtime.hpp"
#include "feature_graph_io.hpp"
#include "module_bundle_test_fixture.hpp"
#include "module_dynamic_feature_fixture.hpp"
#include "module_dynamic_feature_probe.hpp"
#include "module_world_motion_fixture.hpp"
#include "module_world_motion_probe.h"
#include "GameModule.hpp"
#include "module.hpp"
#include "concurrency.hpp"
#include "composition/editor_feature_graph_panel.hpp"
#include "composition/editor_render_graph_inspection.hpp"
#include <imgui_internal.h>
#include "rhi.hpp"
#include "renderer_feature_profiles.hpp"
#include "serialization.hpp"
#include "asset_metadata.hpp"
#include "project_cooker.hpp"
#include "game/game_runtime_bootstrap.hpp"
#include "runtime_feature_control.hpp"
#include "world_scope.hpp"

namespace {
template <typename Value>
bool require(const Value& condition, const char* message) {
    if (!static_cast<bool>(condition)) {
        std::cerr << message;
        if constexpr (requires { condition.message; }) std::cerr << ": " << condition.message;
        std::cerr << '\n';
    }
    return static_cast<bool>(condition);
}

bool same_tint(const aeg::engine::components::MeshRenderer& left, const aeg::engine::components::MeshRenderer& right) {
    return left.tint.x == right.tint.x && left.tint.y == right.tint.y && left.tint.z == right.tint.z &&
           left.tint_alpha == right.tint_alpha;
}

bool default_preview_has_feature_source(const aeg::application::EditorSceneRuntime& scene,
    aeg::feature::FeatureDefinitionId definition = aeg::feature::FeatureDefinitionId{"Renderer.Scene.Opaque"},
    uint32_t operation_count = 1) {
    using namespace aeg;
    const auto captured = scene.inspect_render_graph();
    if (!require(captured && captured.value->plan.revision != 0,
                 "Default Preview did not capture its successful RenderGraph")) return false;
    const auto source = std::find_if(captured.value->sources.begin(), captured.value->sources.end(), [&](const auto& value) {
        return std::any_of(value.provenance.begin(), value.provenance.end(), [&](const auto& origin) {
            return origin.runtime.definition_id == definition;
        });
    });
    if (!require(source != captured.value->sources.end() && source->generation.is_valid() && !source->plans.empty(),
                 "Default Preview has no real Renderer Scene Feature ExecutionPlan source")) return false;
    bool drew = false;
    std::vector<bool> operations(operation_count, false);
    for (const auto& pass : captured.value->plan.pass_plans) {
        if (!pass.live || !pass.feature_work) continue;
        const auto& work = *pass.feature_work;
        const auto origin = std::find_if(source->provenance.begin(), source->provenance.end(), [&](const auto& value) {
            return value.runtime.node_id == work.node_id && value.runtime.instance_id == work.instance_id;
        });
        if (!require(work.feature_definition_id == definition && origin != source->provenance.end() &&
                     work.node_id.is_valid() && work.instance_id.is_valid() && work.id.is_valid() &&
                     work.frame_index == captured.value->frame_index && work.operation_index < operation_count &&
                     work.owner_module == feature::ModuleIdentity{"com.aeg.renderer"} && work.owner_module == origin->runtime.owner_module &&
                     work.invocation.invocation_id.is_valid() && work.invocation.generation == source->generation &&
                     std::any_of(source->plans.begin(), source->plans.end(), [&](const auto& plan) {
                         return std::find(plan.instances.begin(), plan.instances.end(), work.instance_id) != plan.instances.end();
                     }),
                     "Default Preview Scene draw lost real Feature/Node/Instance/invocation/module provenance")) return false;
        drew = true;
        operations[work.operation_index] = true;
    }
    return require(drew && std::ranges::all_of(operations, [](bool covered) { return covered; }),
                   "Preview live Feature work does not cover the complete Scene profile intent");
}

bool preview_geometry_pixels(aeg::application::EditorSceneRuntime& scene, aeg::scene::SceneEntityId mesh,
                             uint64_t frame, uint32_t width, uint32_t height, const char* profile = "Opaque",
                             uint8_t expected_alpha = 255) {
    if (!require(scene.select(mesh), "Preview geometry pixel selection failed")) return false;
    const auto projection = scene.selected_gizmo_projection(width, height);
    if (!require(projection && projection.value.valid, "Preview geometry has no viewport projection") ||
        !require(scene.request_viewport_readback(), "Preview geometry readback request failed") ||
        !require(scene.render_frame(frame, 0.0), "Preview geometry readback frame failed")) return false;
    const auto pixels = scene.read_viewport_pixels();
    if (!require(pixels && pixels.value.size() == width * height * 4, "Preview geometry pixels unavailable")) return false;
    const auto x = static_cast<uint32_t>(projection.value.center.x);
    const auto y = static_cast<uint32_t>(projection.value.center.y);
    if (!require(x < width && y < height, "Preview geometry projection escaped the viewport")) return false;
    const auto center = (static_cast<size_t>(y) * width + x) * 4;
    const auto background = static_cast<size_t>(y) * width * 4;
    // The background/sky has the same vertical coordinate; mesh coverage changes its color.
    bool geometry = false;
    for (size_t channel = 0; channel < 3; ++channel)
        geometry = geometry || pixels.value[center + channel] != pixels.value[background + channel];
    std::cout << "Preview " << profile << " center(" << x << ',' << y << ") RGBA=";
    for (size_t channel = 0; channel < 4; ++channel)
        std::cout << unsigned(std::to_integer<uint8_t>(pixels.value[center + channel])) << ' ';
    std::cout << "background=";
    for (size_t channel = 0; channel < 4; ++channel)
        std::cout << unsigned(std::to_integer<uint8_t>(pixels.value[background + channel])) << ' ';
    std::cout << '\n';
    return require(geometry && std::to_integer<uint8_t>(pixels.value[center + 3]) == expected_alpha,
                   "Preview profile did not contribute actual geometry pixels");
}

bool has_default_preview_definition(const aeg::application::EditorSceneRuntime& scene, const char* message) {
    const auto source = scene.inspect_renderer_feature_execution();
    return require(source && source.value.provenance.size() == 1 &&
                   source.value.provenance.front().runtime.definition_id == aeg::feature::FeatureDefinitionId{"Renderer.Scene.Opaque"},
                   message);
}

void feature_panel_frame(aeg::application::EditorSceneRuntime& scene,
                         aeg::application::EditorFeatureGraphPanel& panel,
                         aeg::application::EditorFeatureGraphPanel::Actions& actions, ImGuiContext* context,
                         aeg::application::EditorFeatureGraphDocument& document, const char* activate = nullptr,
                         bool table_node = false, ImGuiID explicit_id = 0) {
    ImGui::SetCurrentContext(context);
    ImGui::NewFrame();
    if (activate != nullptr || explicit_id != 0) {
        auto* window = ImGui::FindWindowByName("Flow Graph###FlowGraph");
        context->NavWindow = window;
        context->NavId = explicit_id ? explicit_id : table_node
            ? ImGui::GetIDWithSeed(activate, nullptr, window->GetID("FeatureGraphNodes")) : window->GetID(activate);
        context->NavInputSource = ImGuiInputSource_Keyboard;
        context->NavActivateId = context->NavActivateDownId = context->NavId;
    }
    ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    actions.publication_pending = scene.feature_publication_pending();
    panel.draw(document, {}, actions); ImGui::Render();
}

bool feature_panel_text(aeg::application::EditorSceneRuntime& scene,
                        aeg::application::EditorFeatureGraphPanel& panel,
                        aeg::application::EditorFeatureGraphPanel::Actions& actions, ImGuiContext* context,
                        aeg::application::EditorFeatureGraphDocument& document, const char* label, const std::string& value) {
    feature_panel_frame(scene, panel, actions, context, document, label);
    auto* window = ImGui::FindWindowByName("Flow Graph###FlowGraph");
    auto* input = ImGui::GetInputTextState(window->GetID(label));
    if (!require(input != nullptr, "real module InputText activation failed")) return false;
    input->SelectAll(); ImGui::GetIO().AddInputCharactersUTF8(value.c_str());
    feature_panel_frame(scene, panel, actions, context, document);
    return true;
}

// Actual DLL callbacks execute only through this same Scene owner and its frame boundary.
bool feature_session(aeg::rhi::Device& device, const std::filesystem::path& binary) {
    using namespace aeg;
    using namespace feature;
    const auto root = std::filesystem::temp_directory_path() / "aeg_editor_scene_feature_session";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    application::EditorSceneRuntime scene;
    if (!require(scene.initialize(device), "Feature Scene initialize failed") ||
        !require(scene.new_project(root, "Feature Session"), "Feature Scene project creation failed") ||
        !require(scene.request_viewport_extent(32, 32), "Feature Scene extent failed")) return false;
    application::EditorFeatureGraphPanel panel;
    application::EditorFeatureGraphPanel::Actions actions;
    unsigned selections = 0, applies = 0, inspections = 0;
    foundation::Result ui_result = foundation::not_ready("No UI action");
    actions.select_module = [&](const std::filesystem::path& manifest, std::string_view configuration) {
        ++selections;
        return ui_result = scene.select_feature_module(manifest, configuration);
    };
    actions.select_saved_module = [&](const std::filesystem::path& manifest, std::string_view configuration) {
        return ui_result = scene.select_feature_module(manifest, configuration,
            application::EditorSceneRuntime::FeatureGraphLoadPolicy::kSavedStartup);
    };
    actions.apply_graph = [&](const FeatureGraphDocument& graph) {
        ++applies;
        return ui_result = scene.queue_feature_graph(graph);
    };
    FeatureNodeId inspected_node{};
    std::string inspected_asset;
    actions.inspect_node = [&](std::string_view asset, FeatureNodeId node) {
        ++inspections;
        inspected_node = node; inspected_asset = asset;
        return scene.inspect_feature_node(asset, node);
    };
    unsigned render_inspections = 0;
    actions.inspect_execution = [&](std::string_view asset) { return scene.inspect_feature_execution(asset); };
    actions.inspect_render_graph = [&] { ++render_inspections; return scene.inspect_render_graph(); };
    actions.authored_feature_documents = {};
    auto* context = ImGui::CreateContext();
    struct UiGuard final { ImGuiContext* context; ~UiGuard() { ImGui::DestroyContext(context); } } ui_guard{context};
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.DisplaySize = {1400, 1800}; io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels{}; int width{}, height{}; io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    const auto ui_frame = [&](application::EditorFeatureGraphDocument& document, const char* activate = nullptr,
                              bool table_node = false, ImGuiID explicit_id = 0) {
        feature_panel_frame(scene, panel, actions, context, document, activate, table_node, explicit_id);
    };
    const auto ui_text = [&](application::EditorFeatureGraphDocument& document, const char* label, const std::string& value) {
        return feature_panel_text(scene, panel, actions, context, document, label, value);
    };
    auto source = dynamic_feature_test::definition();
    const auto bundle = module_bundle_test::write_bundle(binary, root / "Binaries" / "Explicit",
                                                        module_bundle_test::metadata_for(source));
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    module::ModuleRegistry modules(registry);
    if (!require(module::register_static_module(modules, std::move(source)), "fixture module registration failed"))
        return false;
    FeatureGraphDocument first{};
    first.asset_uuid = "asset://scene-first"; first.schema_version = 1; first.graph_version = 1;
    first.contract_digest = registry.contract_digest();
    const auto* logic = registry.find_definition(dynamic_feature_test::kLogic);
    const auto parameters = encode_feature_parameters({{{ParameterId("Channel"), 1, {1}}}}, logic->contract.parameters);
    if (!require(parameters, "Feature Scene parameter encoding failed")) return false;
    const auto node = FeatureNodeId("Logic");
    first.nodes = {{node, logic->id, logic->version, {}, parameters.value}};
    auto second = first; second.asset_uuid = "asset://scene-second";
    const auto first_file = root / "Assets" / "Features" / "First.aegfeature";
    const auto second_file = root / "Assets" / "Features" / "Second.aegfeature";
    if (!require(application::authoring::save_feature_graph_document(first_file, first), "first graph save failed") ||
        !require(application::authoring::save_feature_graph_document(second_file, second), "second graph save failed"))
        return false;
    auto project = application::project::ProjectFileCodec{}.load(root / "Project.aegproj");
    if (!require(project, "Feature Scene project load failed")) return false;
    project.value.config.module.id = "com.aeg.dynamic.fixture";
    project.value.config.module.target = "IndependentCmakeTarget";
    project.value.config.target = "Fixture";
    project.value.config.startup_feature_graphs = {"Assets/Features/First.aegfeature", "Assets/Features/Second.aegfeature"};
    application::EditorFeatureGraphDocument ui_document{first};
    actions.feature_catalog = &bundle.metadata.feature_catalog;
    actions.project_root = root;
    ui_frame(ui_document);
    if (!require(scene.update_project_settings(project.value.config), "Feature Scene initial project settings failed") ||
        !ui_text(ui_document, "Module manifest", bundle.manifest_path.generic_string()) ||
        !ui_text(ui_document, "Configuration", "Debug")) return false;
    if (!require(!scene.inspect_feature_node(first.asset_uuid, node), "metadata-only Scene fabricated live owner") ||
        !require(!scene.select_feature_module(bundle.manifest_path, "Release"), "wrong module configuration accepted")) return false;
    auto broken_startup = second;
    broken_startup.nodes[0].definition_id = FeatureDefinitionId("Absent.Startup.Definition");
    if (!require(application::authoring::save_feature_graph_document(second_file, broken_startup), "broken startup save failed") ||
        !require(!scene.select_feature_module(bundle.manifest_path, "Debug"), "partial valid startup batch was queued") ||
        !require(!scene.feature_publication_pending(), "failed startup batch retained candidate") ||
        !require(!scene.inspect_feature_node(first.asset_uuid, node), "failed startup batch partially published first root") ||
        !require(application::authoring::save_feature_graph_document(second_file, second), "startup restore failed")) return false;
    ui_frame(ui_document, "Load Module");
    if (!require(ui_result && selections == 1, "real module selection UI did not reach actual Scene")) return false;
    if (!require(scene.update_project_settings(project.value.config), "Feature Scene project settings failed") ||
        !require(!scene.inspect_feature_node(first.asset_uuid, node), "metadata-only Scene fabricated live owner") ||

        !require(!scene.select_feature_module(bundle.manifest_path, "Debug"), "duplicate pending selection accepted") ||
        !require(!scene.inspect_feature_node(first.asset_uuid, node), "queued module published before frame") ||
        !require(!scene.render_frame(0, std::numeric_limits<double>::quiet_NaN()), "NaN delta accepted") ||
        !require(!scene.render_frame(0, -1.0), "negative delta accepted") ||
        !require(!scene.inspect_feature_node(first.asset_uuid, node), "invalid delta published pending module") ||
        !require(scene.render_frame(0, 1.0), "Feature Scene queued module frame failed")) return false;
    const auto node_label = std::to_string(node.value());
    ui_frame(ui_document, node_label.c_str(), true);
    if (!require(inspections != 0, "real selected-node UI did not inspect actual Scene owner")) return false;
    const auto edit_first = scene.inspect_feature_node(first.asset_uuid, node);
    const auto edit_second = scene.inspect_feature_node(second.asset_uuid, node);
    const auto execution_first = scene.inspect_feature_execution(first.asset_uuid);
    const auto execution_second = scene.inspect_feature_execution(second.asset_uuid);
    if (!require(execution_first && execution_second && execution_first.value.source_path == first_file &&
                 execution_second.value.source_path == second_file &&
                 execution_first.value.generation == edit_first.value.generation && !execution_first.value.provenance.empty() &&
                 execution_first.value.provenance.front().authored_node_id == node,
                 "Scene derived ExecutionPlan lost source path/generation/author attribution") ||
        !require(scene.inspect_render_graph(), "Scene successful frame has no owning RenderGraph inspection")) return false;
    if (!require(scene.request_viewport_readback(), "Failed New Project transaction readback request failed") ||
        !require(scene.render_frame(0, 0.0), "Failed New Project transaction baseline frame failed")) return false;
    const auto new_project_source = scene.inspect_feature_execution(first.asset_uuid);
    const auto new_project_owner = scene.inspect_feature_node(first.asset_uuid, node);
    const auto new_project_preview = scene.inspect_renderer_feature_execution();
    const auto new_project_capture = scene.inspect_render_graph();
    const auto new_project_pixels = scene.read_viewport_pixels();
    const auto new_project_scene = scene.scene_path();
    const auto new_project_root = scene.project_root();
    const auto new_project_count = scene.entities().size();
    if (!require(new_project_source && new_project_owner && new_project_owner.value.owners.size() == 1 &&
                 new_project_preview && new_project_capture && new_project_capture.value->sources.size() == 3 &&
                 new_project_pixels && new_project_pixels.value.size() == 32 * 32 * 4,
                 "Failed New Project regression has no actual DLL session and GPU frame")) return false;
    for (unsigned rejected_case = 0; rejected_case != 2; ++rejected_case) {
        const auto destination = rejected_case == 0 ? root / "FailedNew" : root;
        const auto result = scene.new_project(destination, rejected_case == 0 ? "" : "Existing project");
        const auto source_after = scene.inspect_feature_execution(first.asset_uuid);
        const auto owner_after = scene.inspect_feature_node(first.asset_uuid, node);
        const auto preview_after = scene.inspect_renderer_feature_execution();
        const auto capture_after = scene.inspect_render_graph();
        const auto pixels_after = scene.read_viewport_pixels();
        if (!require(!result && source_after && owner_after && preview_after &&
                     source_after.value.generation == new_project_source.value.generation &&
                     owner_after.value.generation == new_project_owner.value.generation &&
                     owner_after.value.owners[0].owner == new_project_owner.value.owners[0].owner &&
                     preview_after.value.generation == new_project_preview.value.generation &&
                     scene.scene_path() == new_project_scene && scene.project_root() == new_project_root &&
                     scene.entities().size() == new_project_count && capture_after &&
                     capture_after.value == new_project_capture.value && pixels_after &&
                     pixels_after.value == new_project_pixels.value && !scene.feature_publication_pending(),
                     "Rejected New Project retired the actual DLL Feature session or changed last-good Scene/GPU source")) return false;
    }
    const auto last_frame = scene.inspect_render_graph();
    if (!require(!scene.render_frame(0, std::numeric_limits<double>::quiet_NaN()) &&
                 scene.inspect_render_graph().value == last_frame.value,
                 "Rejected frame replaced captured last-good RenderGraph")) return false;
    if (!require(edit_first && edit_second, "complete startup root batch was not published") ||
        !require(edit_first.value.owners.size() == 1 && edit_second.value.owners.size() == 1,
                 "actual DLL root owner count is wrong") ||
        !require(edit_first.value.source_path.filename() == "First.aegfeature" &&
                     edit_second.value.source_path.filename() == "Second.aegfeature",
                 "Runtime Inspect did not preserve the authored FeatureGraph source paths") ||
        !require(edit_first.value.owners[0].owner.instance_id != edit_second.value.owners[0].owner.instance_id,
                 "same authored Node ID collided across actual roots") ||
        !require(edit_first.value.owners[0].state.payload.empty(), "Edit advanced gameplay Feature clock") ||
        !require(scene.play(), "Feature Scene Play failed") ||
        !require(scene.render_frame(1, 0.5 / 60.0), "Feature Scene subperiod frame failed") ||
        !require(scene.inspect_feature_node(first.asset_uuid, node).value.owners[0].state.payload.empty(),
                 "subperiod delta was replaced by a frame-rate tick") ||
        !require(scene.render_frame(2, 0.6 / 60.0), "Feature Scene accumulated delta frame failed")) return false;
    uint64_t frame = 3;
    auto committed = scene.inspect_feature_node(first.asset_uuid, node);
    for (unsigned attempt = 0; committed && committed.value.owners[0].state.payload.empty() && attempt != 100; ++attempt) {
        static_cast<void>(concurrency::sleep_for(1'000'000));
        if (!require(scene.render_frame(frame++, 0.0), "Feature Scene zero-delta completion pump failed")) return false;
        committed = scene.inspect_feature_node(first.asset_uuid, node);
    }
    if (!require(committed && committed.value.owners[0].state.payload == std::vector<uint8_t>{1},
                 "actual DLL callback did not commit exactly one accumulated tick") ||
        !require(committed.value.owners[0].invocations.last_committed.has_value(), "owning Inspect omitted actual commit") ||
        !require(scene.pause(), "Feature Scene Pause failed") ||
        !require(scene.render_frame(frame++, 0.5), "Feature Scene Pause frame failed") ||
        !require(scene.inspect_feature_node(first.asset_uuid, node).value.owners[0].state.payload == std::vector<uint8_t>{1},
                 "Pause advanced gameplay clock") ||
        !require(scene.play(), "Feature Scene Resume failed") ||
        !require(scene.render_frame(frame++, 0.7 / 60.0), "Feature Scene resumed subperiod frame failed") ||
        !require(scene.inspect_feature_node(first.asset_uuid, node).value.owners[0].state.payload == std::vector<uint8_t>{1},
                 "Pause accumulated time or Resume fabricated a tick") ||
        !require(scene.render_frame(frame++, 0.3 / 60.0), "Feature Scene resumed accumulated frame failed")) return false;
    auto resumed = scene.inspect_feature_node(first.asset_uuid, node);
    for (unsigned attempt = 0; resumed && resumed.value.owners[0].state.payload == std::vector<uint8_t>{1} && attempt != 100; ++attempt) {
        static_cast<void>(concurrency::sleep_for(1'000'000));
        if (!require(scene.render_frame(frame++, 0.0), "resumed completion pump failed")) return false;
        resumed = scene.inspect_feature_node(first.asset_uuid, node);
    }
    if (!require(resumed && resumed.value.owners[0].state.payload == std::vector<uint8_t>{2},
                 "Resume lost clock fraction or advanced paused time") ||
        !require(scene.stop(), "Feature Scene Stop failed")) return false;
    const auto stopped = scene.inspect_feature_node(first.asset_uuid, node);
    if (!require(stopped && stopped.value.generation != committed.value.generation &&
                 stopped.value.owners[0].state.payload.empty(), "Stop did not retire and reset actual Runtime") ||
        !require(committed.value.owners[0].state.payload == std::vector<uint8_t>{1}, "retained Inspect snapshot borrowed state"))
        return false;
    application::runtime::RuntimeProcessCommand stale{};
    stale.generation = committed.value.generation; stale.asset_uuid = first.asset_uuid; stale.node_id = node;
    stale.owner = committed.value.owners[0].owner;
    if (!require(!scene.control_feature_process(stale), "retired Scene command was accepted")) return false;
    auto invalid = second; invalid.nodes[0].definition_id = FeatureDefinitionId("Absent.Definition");
    auto changed = first;
    const std::vector<FeatureParameterRecord> changed_records{{ParameterId("Channel"), 1, {2}}};
    changed.nodes[0].parameter_blob = encode_feature_parameters(changed_records, logic->contract.parameters).value;
    if (!require(!scene.queue_feature_graph(invalid), "invalid Apply accepted")) return false;
    application::EditorFeatureGraphDocument changed_document{changed};
    ui_frame(changed_document, "Apply");
    if (!require(ui_result && applies == 1, "real Apply UI did not queue complete actual batch")) return false;
    ui_frame(changed_document, "Apply");
    if (!require(applies == 1, "pending Apply UI issued a duplicate staging call")) return false;
    if (
        !require(scene.inspect_feature_node(first.asset_uuid, node).value.generation == stopped.value.generation,
                 "failed candidate changed last-good generation") ||

        !require(!scene.queue_feature_graph(changed), "duplicate queued Apply accepted") ||
        !require(scene.inspect_feature_node(first.asset_uuid, node).value.owners[0].parameters == first.nodes[0].parameter_blob,
                 "Apply published before actual frame") ||
        !require(scene.render_frame(frame++, 0.0), "actual frame Apply failed")) return false;
    const auto applied = scene.inspect_feature_node(first.asset_uuid, node);
    const auto other = scene.inspect_feature_node(second.asset_uuid, node);
    if (!require(applied && other && applied.value.generation != stopped.value.generation &&
                 applied.value.generation == other.value.generation, "Apply dropped another active root") ||
        !require(applied.value.owners[0].parameters == changed.nodes[0].parameter_blob, "Apply did not replace selected graph") ||
        !require(!scene.select_feature_module(root / "Absent.manifest.json", "Debug"), "missing module selection accepted") ||
        !require(scene.inspect_feature_node(first.asset_uuid, node).value.generation == applied.value.generation,
                 "invalid module selection changed last-good Runtime") ||
        !require(!scene.feature_publication_pending(), "invalid module selection retained pending publication")) return false;
    // Keep one actual DLL owner so the existing probe counters are written by a single invocation.
    auto empty_second = second; empty_second.nodes.clear();
    if (!require(scene.queue_feature_graph(empty_second), "single-owner gate batch queue failed") ||
        !require(scene.render_frame(frame++, 0.0), "single-owner gate batch Apply failed")) return false;
    {
        auto library = module::detail::DynamicLibrary::open(bundle.binary_path);
        if (!require(library, "actual Scene DLL probe open failed")) return false;
        const auto bind = dynamic_feature_test::bind_probe(library.value);
        dynamic_feature_test::Probe probe; probe.gated = true;
        bind(probe.view());
        struct RetireProbe final {
            application::EditorSceneRuntime& scene; dynamic_feature_test::Probe& probe; AegDynamicBindProbe bind;
            ~RetireProbe() {
                static_cast<void>(probe.release.complete());
                static_cast<void>(scene.stop());
                bind({});
            }
        } retire_probe{scene, probe, bind};
        if (!require(scene.play(), "gated Scene Play failed") ||
            !require(scene.render_frame(frame++, 1.0 / 60.0), "gated actual DLL invocation frame failed") ||
            !require(probe.entered.completion().wait(2'000'000'000), "actual Scene DLL callback did not enter gate") ||
            !require(probe.thread != concurrency::current_thread_id(), "Scene bypassed Concurrency worker execution")) return false;
        const auto in_flight = scene.inspect_feature_node(first.asset_uuid, node);
        if (!require(in_flight && in_flight.value.owners[0].invocations.open_attempts.size() == 1 &&
                     in_flight.value.owners[0].invocations.open_attempts[0].committable,
                     "Scene Inspect did not expose its real in-flight token")) return false;
        concurrency::WorkerExecutor release_executor{{concurrency::ExecutorDomain::kCpuWorker, 1, false}};
        concurrency::CompletionSource stop_started;
        const auto release_job = release_executor.submit({[&](const concurrency::TaskContext&) {
            if (const auto result = stop_started.completion().wait(2'000'000'000); !result) return result;
            if (const auto result = concurrency::sleep_for(20'000'000); !result) return result;
            return probe.release.complete();
        }, concurrency::ExecutorDomain::kCpuWorker});
        if (!require(release_job, "Concurrency release helper submission failed")) return false;
        static_cast<void>(stop_started.complete());
        if (!require(scene.stop(), "Scene Stop failed to join actual blocked DLL callback") ||
            !require(release_job.value.wait(2'000'000'000), "Scene Stop release helper did not finish")) return false;
        const auto fresh = scene.inspect_feature_node(first.asset_uuid, node);
        if (!require(fresh && fresh.value.generation != in_flight.value.generation &&
                     fresh.value.owners[0].state.payload.empty() && fresh.value.owners[0].invocations.open_attempts.empty(),
                     "retired DLL callback committed into fresh Scene Runtime") ||
            !require(scene.render_frame(frame++, 0.0), "fresh stopped Scene frame failed") ||
            !require(scene.inspect_feature_node(first.asset_uuid, node).value.owners[0].state.payload.empty(),
                     "late DLL completion mutated stopped Scene state")) return false;
    }
    // The same Scene and real DLL must project both roots into the actual viewport.
    const auto* render = registry.find_definition(FeatureDefinitionId("Render.IntentSmoke"));
    const auto render_node = FeatureNodeId("Render");
    const auto viewport_parameters = encode_feature_parameters(
        std::vector<FeatureParameterRecord>{{ParameterId("Channel"), 1, {3}}}, logic->contract.parameters);
    if (!require(render && viewport_parameters, "Scene GPU fixture contract unavailable")) return false;
    auto gpu_first = first;
    gpu_first.nodes[0].parameter_blob = viewport_parameters.value;
    gpu_first.nodes.push_back({render_node, render->id, render->version, {}, viewport_parameters.value});
    gpu_first.edges.push_back({node, dynamic_feature_test::kColor, render_node, dynamic_feature_test::kColor,
                              EdgeKind::kSnapshot, ClockId("Frame"), ClockId("Frame"), 1, 0, 8});
    auto gpu_second = gpu_first; gpu_second.asset_uuid = second.asset_uuid;
    if (!require(scene.request_viewport_extent(4, 4), "Scene GPU actual extent failed")) return false;
    application::EditorFeatureGraphDocument gpu_first_document{gpu_first};
    ui_frame(gpu_first_document, "Apply");
    if (!require(ui_result && applies == 2, "Scene GPU first graph actual UI Apply failed") ||
        !require(scene.render_frame(frame++, 0.0), "Scene GPU first graph publication failed")) return false;
    application::EditorFeatureGraphDocument gpu_second_document{gpu_second};
    ui_frame(gpu_second_document, "Apply");
    if (!require(ui_result && applies == 3, "Scene GPU second graph actual UI Apply failed") ||
        !require(scene.render_frame(frame++, 0.0), "Scene GPU complete root publication failed") ||
        !require(scene.play(), "Scene GPU Play failed")) return false;
    const auto wait_node_payload = [&](FeatureNodeId requested, uint8_t count) {
        for (unsigned spin = 0; spin != 2000; ++spin) {
            const auto left = scene.inspect_feature_node(first.asset_uuid, requested);
            const auto right = scene.inspect_feature_node(second.asset_uuid, requested);
            if (left && right && left.value.owners[0].state.payload.size() == 1 &&
                right.value.owners[0].state.payload.size() == 1 &&
                left.value.owners[0].state.payload[0] >= count && right.value.owners[0].state.payload[0] >= count &&
                (requested != render_node || (left.value.owners[0].render_output && right.value.owners[0].render_output)))
                return true;
            if (!require(scene.render_frame(frame++, 0.0), "Scene GPU actual completion/projection frame failed")) return false;
            static_cast<void>(concurrency::sleep_for(1'000'000));
        }
        return require(false, "Scene GPU actual DLL Render commits timed out");
    };
    const auto read_red_viewport = [&] {
        if (!require(scene.request_viewport_readback(), "Scene GPU final readback request failed") ||
            !require(scene.render_frame(frame++, 0.0), "Scene GPU final readback frame failed")) return false;
        const auto pixels = scene.read_viewport_pixels();
        if (!require(pixels && pixels.value.size() == 64, "Scene GPU actual 4x4 readback unavailable")) return false;
        const auto captured = scene.inspect_render_graph();
        if (!require(captured && captured.value->plan.revision != 0 && captured.value->sources.size() == 3 &&
                     std::count_if(captured.value->sources.begin(), captured.value->sources.end(), [&](const auto& source) {
                         return source.asset_uuid == first.asset_uuid || source.asset_uuid == second.asset_uuid;
                     }) == 2,
                     "Actual Scene RenderGraph/source snapshot was not captured")) return false;
        for (const auto* asset : {&first, &second}) {
            const auto owner = scene.inspect_feature_node(asset->asset_uuid, render_node);
            if (!require(owner && owner.value.owners[0].render_output, "Rendered source owner missing")) return false;
            const auto& live = owner.value.owners[0];
            bool found = false;
            for (const auto& pass : captured.value->plan.pass_plans) {
                if (!pass.live || !pass.feature_work) continue;
                const auto& work = *pass.feature_work;
                if (work.instance_id != live.owner.instance_id || work.node_id != live.owner.node_id) continue;
                if (!require(work.feature_definition_id == live.definition_id && work.owner_module == live.owner_module &&
                             work.invocation == live.render_output->invocation &&
                             work.operation_index < live.render_output->intent.operations.size(),
                             "Actual RenderGraph Feature pass lost invocation/module/operation provenance")) return false;
                found = true;
            }
            if (!require(found, "Rendered Feature owner has no captured live pass")) return false;
        }
        const auto before = gpu_first_document.snapshot_owner();
        ui_frame(gpu_first_document, "Show RenderGraph"); ui_frame(gpu_first_document);
        if (!require(render_inspections != 0 && gpu_first_document.snapshot_owner() == before,
                     "Actual Scene RenderGraph was not opened read-only through ImGui")) return false;
        const auto source_owner = scene.inspect_feature_node(first.asset_uuid, render_node);
        const auto pass = std::find_if(captured.value->plan.pass_plans.begin(), captured.value->plan.pass_plans.end(), [&](const auto& item) {
            return item.live && item.feature_work && item.feature_work->instance_id == source_owner.value.owners[0].owner.instance_id;
        });
        if (!require(pass != captured.value->plan.pass_plans.end(), "Source pass missing from RenderGraph UI")) return false;
        const int index = static_cast<int>(pass - captured.value->plan.pass_plans.begin());
        const auto row = ImHashData(&index, sizeof(index), ImGui::FindWindowByName("Flow Graph###FlowGraph")->GetID("FeatureGraphRenderGraph"));
        const auto node_id = ImHashStr(("Node " + std::to_string(render_node.value())).c_str(), 0, row);
        ui_frame(gpu_first_document, nullptr, false, node_id);
        if (!require(inspected_asset == first.asset_uuid && inspected_node == render_node,
                     "RenderGraph UI did not navigate to its original authored Feature node")) return false;
        ui_frame(gpu_first_document, "Show RenderGraph");
        for (std::size_t i = 0; i != pixels.value.size(); i += 4) {
            if (!require(std::to_integer<uint8_t>(pixels.value[i]) == 255 &&
                         std::to_integer<uint8_t>(pixels.value[i + 1]) == 0 &&
                         std::to_integer<uint8_t>(pixels.value[i + 2]) == 0 &&
                         std::to_integer<uint8_t>(pixels.value[i + 3]) == 255,
                         "actual DLL Render commits did not reach final SceneColor pixels")) return false;
        }
        return true;
    };
    if (!require(scene.render_frame(frame++, 1.0 / 60.0), "Scene GPU first real tick failed") ||
        !wait_node_payload(node, 1) ||
        !require(scene.render_frame(frame++, 1.0 / 60.0), "Scene GPU Snapshot consumption tick failed") ||
        !wait_node_payload(render_node, 1) || !read_red_viewport() ||
        !require(scene.render_frame(frame++, 1.0 / 60.0), "Scene GPU second real tick failed") ||
        !wait_node_payload(render_node, 2)) return false;
    const auto history_commit = scene.inspect_feature_node(first.asset_uuid, render_node);
    const auto retained_render = scene.inspect_render_graph();
    if (!require(history_commit && history_commit.value.owners[0].render_output->intent.operations.size() == 1 &&
                 std::holds_alternative<RenderCopyTexture>(history_commit.value.owners[0].render_output->intent.operations[0]),
                 "Scene GPU second actual DLL commit did not read preserved history") ||
        !read_red_viewport() || !require(scene.pause(), "Scene GPU Pause failed") || !read_red_viewport() ||
        !require(scene.stop(), "Scene GPU Stop failed")) return false;
    const auto stopped_render = scene.inspect_feature_node(first.asset_uuid, render_node);
    if (!require(stopped_render && stopped_render.value.generation != history_commit.value.generation &&
                 !stopped_render.value.owners[0].render_output, "Scene GPU Stop retained retired Render output") ||
        !require(scene.request_viewport_readback(), "Scene GPU stopped readback request failed") ||
        !require(scene.render_frame(frame++, 0.0), "Scene GPU stopped readback frame failed")) return false;
    const auto stopped_pixels = scene.read_viewport_pixels();
    if (!require(stopped_pixels && stopped_pixels.value.size() == 64 &&
                 (std::to_integer<uint8_t>(stopped_pixels.value[0]) != 255 ||
                  std::to_integer<uint8_t>(stopped_pixels.value[1]) != 0 ||
                  std::to_integer<uint8_t>(stopped_pixels.value[2]) != 0),
                 "Scene GPU stopped frame replayed retired Feature output")) return false;
    const auto before_import = scene.inspect_render_graph();
    const auto previous_project_preview = renderer::scene_render_profile_graph(renderer::SceneRenderProfile::kForward);
    if (!require(before_import, "Project import regression needs a captured frame") ||
        !require(previous_project_preview && scene.publish_renderer_feature_graph(previous_project_preview.value),
                 "Project import Preview selection setup failed") ||
        !require(scene.import_project(root), "Feature Scene project reimport failed") ||
        !has_default_preview_definition(scene, "Project import inherited the preceding Forward Feature graph") ||
        !require(!scene.inspect_render_graph(), "Project import exposed the previous project RenderGraph") ||
        !require(before_import.value->plan.revision != 0 && retained_render.value->sources.size() == 3,
                 "Project import invalidated caller-owned RenderGraph metadata")) return false;
    auto preceding_close = renderer::scene_render_profile_graph(renderer::SceneRenderProfile::kOpaque).value;
    preceding_close.nodes.clear();
    if (!require(scene.publish_renderer_feature_graph(preceding_close), "Project close empty Preview setup failed") ||
        !require(scene.close_project(), "Feature Scene project close failed") ||
        !has_default_preview_definition(scene, "Project close inherited an empty Feature graph") ||
        !require(!scene.inspect_render_graph() && retained_render && !retained_render.value->plan.pass_plans.empty(),
                 "Closed Scene retained active inspector or invalidated owning captured values") ||
        !require(!scene.inspect_feature_node(first.asset_uuid, node), "closed project retained live Feature owner") ||
        !require(scene.shutdown(), "Feature Scene shutdown failed")) return false;
    std::filesystem::remove_all(root, error);
    return require(!error, "Feature Scene retained loaded DLL after shutdown");
}

int saved_startup_feature_session(aeg::rhi::Device& device, const std::filesystem::path& binary_v1,
                                 const std::filesystem::path& binary_v2) {
    using namespace aeg;
    using namespace feature;
    using Scene = application::EditorSceneRuntime;
    const auto root = std::filesystem::temp_directory_path() / "aeg_editor_scene_saved_startup";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    Scene scene;
    if (!require(scene.initialize(device), "Saved startup Scene initialize failed") ||
        !require(scene.new_project(root, "Saved Startup"), "Saved startup project creation failed") ||
        !require(scene.request_viewport_extent(32, 32), "Saved startup viewport extent failed")) return 1;
    const auto bundle_v1 = module_bundle_test::write_bundle(binary_v1, root / "Bundles" / "V1",
        module_bundle_test::metadata_for(dynamic_feature_test::definition(1)));
    const auto bundle_v2 = module_bundle_test::write_bundle(binary_v2, root / "Bundles" / "V2",
        module_bundle_test::metadata_for(dynamic_feature_test::definition(2)));
    FeatureRegistry registry_v1{aeg::feature::test::clock_test_host()}, registry_v2{aeg::feature::test::clock_test_host()};
    module::ModuleRegistry modules_v1(registry_v1), modules_v2(registry_v2);
    if (!require(module::register_static_module(modules_v1, dynamic_feature_test::definition(1)),
                 "Saved startup V1 contract registration failed") ||
        !require(module::register_static_module(modules_v2, dynamic_feature_test::definition(2)),
                 "Saved startup V2 contract registration failed")) return 1;
    const auto node = FeatureNodeId("Saved.Logic");
    const auto graph_for = [&](const FeatureRegistry& registry, std::string asset, uint8_t channel) {
        FeatureGraphDocument graph{};
        const auto* logic = registry.find_definition(dynamic_feature_test::kLogic);
        const auto parameters = encode_feature_parameters(
            {{{ParameterId("Channel"), 1, {channel}}}}, logic->contract.parameters);
        module_bundle_test::require(bool(parameters), "Saved startup parameters failed");
        graph.asset_uuid = std::move(asset); graph.schema_version = 1; graph.graph_version = logic->version.major;
        graph.contract_digest = registry.contract_digest();
        graph.nodes = {{node, logic->id, logic->version, {}, parameters.value}};
        return graph;
    };
    const auto first_v1 = graph_for(registry_v1, "asset://saved-startup-first", 1);
    const auto second_v1 = graph_for(registry_v1, "asset://saved-startup-second", 2);
    const auto first_v2 = graph_for(registry_v2, first_v1.asset_uuid, 3);
    const auto third_v2 = graph_for(registry_v2, "asset://saved-startup-third", 2);
    const auto first_path = root / "Assets" / "Features" / "First.aegfeature";
    const auto second_path = root / "Assets" / "Features" / "Second.aegfeature";
    const auto third_path = root / "Assets" / "Features" / "Third.aegfeature";
    const auto write_authoring = [&](const module::ModuleBundle& bundle,
                                     const FeatureGraphDocument& first, const FeatureGraphDocument& second,
                                     const std::filesystem::path& second_saved_path,
                                     const std::filesystem::path& source_directory) {
        std::filesystem::create_directories(source_directory, error);
        if (!require(!error, "Saved startup official metadata directory creation failed")) return false;
        const auto source_metadata = source_directory / "GameModule.metadata.json";
        std::filesystem::copy_file(bundle.metadata_path, source_metadata,
                                  std::filesystem::copy_options::overwrite_existing, error);
        return require(!error, "Saved startup official source metadata copy failed") &&
            require(application::authoring::save_feature_graph_document(first_path, first),
                    "Saved startup first root save failed") &&
            require(application::authoring::save_feature_graph_document(second_saved_path, second),
                    "Saved startup second root save failed");
    };
    if (!write_authoring(bundle_v1, first_v1, second_v1, second_path, root / "Source")) return 1;
    auto project = application::project::ProjectFileCodec{}.load(root / "Project.aegproj");
    if (!require(project, "Saved startup exact project read failed")) return 1;
    project.value.config.module.id = "com.aeg.dynamic.fixture";
    project.value.config.module.target = "IndependentCmakeTarget";
    project.value.config.target = "Fixture";
    project.value.config.startup_feature_graphs = {"Assets/Features/First.aegfeature", "Assets/Features/Second.aegfeature"};
    uint64_t frame = 0;
    if (!require(scene.update_project_settings(project.value.config), "Saved startup project roots update failed") ||
        !require(scene.select_feature_module(bundle_v1.manifest_path, "Debug"), "Saved startup initial V1 selection failed") ||
        !require(scene.feature_publication_pending(), "Saved startup initial module was not pending") ||
        !require(scene.render_frame(frame++, 0.0), "Saved startup initial V1 frame failed")) return 1;
    const auto active_v1_first = scene.inspect_feature_node(first_v1.asset_uuid, node);
    const auto active_v1_second = scene.inspect_feature_node(second_v1.asset_uuid, node);
    const auto captured_v1 = scene.inspect_render_graph();
    const auto is_version = [&](const FeatureGraphDocument& graph, uint16_t version) {
        const auto actual = scene.inspect_feature_node(graph.asset_uuid, node);
        return actual && actual.value.owners.size() == 1 && actual.value.owners.front().contract.version == Version{version, 0, 0};
    };
    if (!require(active_v1_first && active_v1_second && captured_v1 && is_version(first_v1, 1) && is_version(second_v1, 1),
                 "Saved startup initial actual V1 batch or capture is missing")) return 1;
    const auto authored_v2 = [&](const FeatureGraphDocument& graph, const std::filesystem::path& path) {
        const auto authored = scene.authored_feature_graphs();
        const auto found = std::find_if(authored.begin(), authored.end(), [&](const auto& value) {
            return value.path == path && value.graph.asset_uuid == graph.asset_uuid;
        });
        return found != authored.end() && found->load_result && found->graph.nodes.size() == 1 &&
               found->graph.nodes.front().definition_version == Version{2, 0, 0} &&
               found->graph.contract_digest == registry_v2.contract_digest();
    };
    const auto remains_v1 = [&] {
        const auto first = scene.inspect_feature_node(first_v1.asset_uuid, node);
        const auto second = scene.inspect_feature_node(second_v1.asset_uuid, node);
        const auto captured = scene.inspect_render_graph();
        return first && second && is_version(first_v1, 1) && is_version(second_v1, 1) &&
            first.value.owners.front().owner == active_v1_first.value.owners.front().owner &&
            second.value.owners.front().owner == active_v1_second.value.owners.front().owner &&
            first.value.generation == active_v1_first.value.generation &&
            second.value.generation == active_v1_second.value.generation &&
            captured && captured.value == captured_v1.value;
    };
    auto empty_project = project.value;
    empty_project.config.startup_feature_graphs.clear();
    if (!require(application::project::ProjectFileCodec{}.save(scene.project_file(), empty_project),
                 "Saved startup empty root list setup failed")) return 1;
    const auto empty_selection = scene.select_feature_module(bundle_v1.manifest_path, "Debug", Scene::FeatureGraphLoadPolicy::kSavedStartup);
    if (empty_selection) {
        std::cerr << "SAVED_STARTUP_EMPTY accepted=1 active_roots=2 saved_roots=0\n";
        if (!require(scene.shutdown(), "Saved startup empty policy failure shutdown failed")) return 1;
        return 73;
    }
    if (!require(empty_selection.code == foundation::ResultCode::kInvalidArgument &&
                 !scene.feature_publication_pending() && remains_v1(),
                 "Saved startup empty list did not explicitly reject without changing the active batch")) return 1;
    auto saved_project_v2 = project.value;
    saved_project_v2.config.startup_feature_graphs = {"Assets/Features/First.aegfeature", "Assets/Features/Third.aegfeature"};
    saved_project_v2.config.paths.source = "Native";
    if (!write_authoring(bundle_v2, first_v2, third_v2, third_path, root / "Native") ||
        !require(scene.update_project_settings(saved_project_v2.config),
                 "Configured Native source project settings failed") ||
        !require(application::project::ProjectFileCodec{}.save(scene.project_file(), saved_project_v2),
                 "Saved startup new project root list save failed") ||
        !require(scene.refresh_assets(), "Saved startup actual authoring refresh failed")) return 1;
    if (!require(scene.authored_feature_catalog().contract_digest() == bundle_v2.metadata.feature_catalog.contract_digest(),
                 "Configured Native authoring catalog included obsolete source contracts") ||
        !require(authored_v2(first_v2, first_path) && authored_v2(third_v2, third_path),
                 "Configured Native authored V2 roots were not refreshed") ||
        !require(remains_v1(), "Native authoring refresh changed the active V1 batch or capture") ||
        !require(!scene.feature_publication_pending(), "Native authoring refresh queued an executable publication")) return 1;
    const auto native_metadata = root / "Native" / "GameModule.metadata.json";
    const auto authoring_v2_digest = scene.authored_feature_catalog().contract_digest();
    {
        std::ofstream malformed(native_metadata, std::ios::binary | std::ios::trunc);
        malformed << "{invalid metadata";
        if (!require(bool(malformed), "Native malformed metadata setup failed")) return 1;
    }
    if (!require(!scene.refresh_assets() && remains_v1() && !scene.feature_publication_pending() &&
                 scene.authored_feature_catalog().contract_digest() == authoring_v2_digest,
                 "Malformed Native metadata replaced authoring or active last-good state")) return 1;
    std::filesystem::copy_file(bundle_v2.metadata_path, native_metadata,
                              std::filesystem::copy_options::overwrite_existing, error);
    if (!require(!error && scene.refresh_assets(), "Native metadata recovery failed")) return 1;
    const auto hidden_metadata = root / "Native" / "GameModule.metadata.hold";
    std::filesystem::rename(native_metadata, hidden_metadata, error);
    if (!require(!error, "Native missing metadata setup failed") ||
        !require(scene.refresh_assets() && remains_v1() && !scene.feature_publication_pending() &&
                 std::ranges::none_of(scene.authored_feature_catalog().entries(), [](const auto& entry) {
                     return entry.definition_id == dynamic_feature_test::kLogic;
                 }), "Missing Native metadata fell back to stale Source or changed active V1")) return 1;
    std::filesystem::rename(hidden_metadata, native_metadata, error);
    if (!require(!error && scene.refresh_assets(), "Native missing metadata recovery failed")) return 1;
    const auto readable_project_backup = root / "Project.aegproj.read-test";
    std::filesystem::copy_file(scene.project_file(), readable_project_backup,
                              std::filesystem::copy_options::overwrite_existing, error);
    if (!require(!error, "Selected project read failure backup failed")) return 1;
    {
        std::ofstream malformed(scene.project_file(), std::ios::binary | std::ios::trunc);
        malformed << "{invalid project";
        if (!require(bool(malformed), "Selected project read failure setup failed")) return 1;
    }
    if (!require(!scene.refresh_assets() && remains_v1() && !scene.feature_publication_pending() &&
                 scene.authored_feature_catalog().contract_digest() == authoring_v2_digest,
                 "Unreadable selected project replaced authoring or active last-good state")) return 1;
    // The formal writer deliberately refuses to overwrite a malformed file.
    // Restore the exact fixture bytes that this negative case corrupted.
    std::filesystem::copy_file(readable_project_backup, scene.project_file(),
                              std::filesystem::copy_options::overwrite_existing, error);
    if (!require(!error && scene.refresh_assets(), "Selected project read failure recovery failed")) return 1;
    const auto preserve = scene.select_feature_module(bundle_v2.manifest_path, "Debug");
    if (!require(!preserve && !scene.feature_publication_pending() && remains_v1(),
                 "Default module selection stopped preserving the strict active graph contracts")) return 1;

    auto broken_second = third_v2;
    broken_second.nodes.front().definition_id = FeatureDefinitionId("Absent.Saved.SecondRoot");
    if (!require(application::authoring::save_feature_graph_document(third_path, broken_second),
                 "Saved startup malformed second root setup failed") ||
        !require(!scene.select_feature_module(bundle_v2.manifest_path, "Debug", Scene::FeatureGraphLoadPolicy::kSavedStartup) &&
                 !scene.feature_publication_pending() && remains_v1(),
                 "Saved startup malformed second root partially queued or retired active V1") ||
        !require(application::authoring::save_feature_graph_document(third_path, third_v2),
                 "Saved startup valid second root restore failed") ||
        !require(scene.refresh_assets(), "Saved startup restored authoring refresh failed")) return 1;
    application::EditorFeatureGraphPanel panel;
    application::EditorFeatureGraphPanel::Actions actions;
    unsigned saved_selections = 0, default_selections = 0;
    unsigned catalog_reviews = 0;
    foundation::Result selected = foundation::not_ready("Load Saved Project UI did not run");
    actions.select_saved_module = [&](const std::filesystem::path& manifest, std::string_view configuration) {
        ++saved_selections;
        return selected = scene.select_feature_module(manifest, configuration, Scene::FeatureGraphLoadPolicy::kSavedStartup);
    };
    actions.active_module_catalog = &bundle_v1.metadata.feature_catalog;
    auto candidate_catalog = bundle_v2.metadata.feature_catalog;
    actions.inspect_module_catalog = [&](const std::filesystem::path&) {
        ++catalog_reviews;
        return foundation::ResultValue<FeatureCatalog>{foundation::Result::success(), candidate_catalog};
    };
    actions.select_module = [&](const std::filesystem::path& manifest, std::string_view configuration) {
        ++default_selections;
        return scene.select_feature_module(manifest, configuration);
    };
    application::EditorFeatureGraphDocument ui_document{first_v1, bundle_v1.metadata.feature_catalog};
    const std::array unsaved_parameters{FeatureParameterRecord{ParameterId("Channel"), 1, {4}}};
    if (!require(ui_document.set_parameters(node, unsaved_parameters, ui_document.catalog()),
                 "Saved startup unsaved document parameter setup failed")) return 1;
    const auto unsaved_document = ui_document.snapshot_owner();
    auto* context = ImGui::CreateContext();
    struct SavedUiGuard final { ImGuiContext* context; ~SavedUiGuard() { ImGui::DestroyContext(context); } } ui_guard{context};
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.DisplaySize = {1400, 1800}; io.DeltaTime = 1.0f / 60.0f;
    unsigned char* font_pixels{}; int font_width{}, font_height{};
    io.Fonts->GetTexDataAsRGBA32(&font_pixels, &font_width, &font_height);
    feature_panel_frame(scene, panel, actions, context, ui_document);
    if (!feature_panel_text(scene, panel, actions, context, ui_document, "Module manifest", bundle_v2.manifest_path.generic_string()) ||
        !feature_panel_text(scene, panel, actions, context, ui_document, "Configuration", "Debug")) return 1;
    feature_panel_frame(scene, panel, actions, context, ui_document, "Load Saved Project");
    if (!require(saved_selections == 0 && catalog_reviews == 0 && !scene.feature_publication_pending() &&
                 ui_document.snapshot_owner() == unsaved_document && ui_document.dirty(),
                 "Changed module catalog loaded before explicit contract review")) return 1;
    feature_panel_frame(scene, panel, actions, context, ui_document, "Review Contract Changes");
    if (!require(saved_selections == 0 && catalog_reviews == 1 && !scene.feature_publication_pending(),
                 "Contract review did not expose the candidate without publishing it")) return 1;
    candidate_catalog = bundle_v1.metadata.feature_catalog;
    feature_panel_frame(scene, panel, actions, context, ui_document, "Accept & Load Saved Project");
    if (!require(saved_selections == 0 && catalog_reviews == 2 && !scene.feature_publication_pending(),
                 "Changed catalog was loaded after its reviewed candidate became stale")) return 1;
    candidate_catalog = bundle_v2.metadata.feature_catalog;
    feature_panel_frame(scene, panel, actions, context, ui_document, "Review Contract Changes");
    feature_panel_frame(scene, panel, actions, context, ui_document, "Accept & Load Saved Project");
    if (!require(saved_selections == 1 && default_selections == 0 && catalog_reviews == 4 &&
                 ui_document.snapshot_owner() == unsaved_document && ui_document.dirty(),
                 "Explicit contract acceptance changed the unsaved graph draft")) return 1;
    if (!selected) {
        std::cerr << "SAVED_STARTUP_POLICY selected=0 code=" << static_cast<unsigned>(selected.code)
                  << " message=" << selected.message << " author_revision=2 active_revision=1 roots=2\n";
        if (!require(remains_v1() && !scene.feature_publication_pending() && scene.shutdown(),
                     "Saved startup failure did not preserve/retire the actual last-good batch")) return 1;
        return 73;
    }
    if (!require(scene.feature_publication_pending() && remains_v1(),
                 "Saved startup V2 selection published before the actual frame boundary") ||
        !require(!scene.select_feature_module(bundle_v2.manifest_path, "Debug", Scene::FeatureGraphLoadPolicy::kSavedStartup),
                 "Saved startup duplicate pending module selection succeeded") ||
        !require((feature_panel_frame(scene, panel, actions, context, ui_document, "Load Saved Project"), saved_selections == 1),
                 "Pending Load Saved Project button issued another selection") ||
        !require(scene.render_frame(frame++, 0.0), "Saved startup V2 publication frame failed") ||
        !require(!scene.feature_publication_pending() && is_version(first_v2, 2) && is_version(third_v2, 2) &&
                 !scene.inspect_feature_node(second_v1.asset_uuid, node),
                 "Saved startup publication omitted a root or retained V1")) return 1;
    const auto published = scene.inspect_feature_node(first_v2.asset_uuid, node);
    const auto published_third = scene.inspect_feature_node(third_v2.asset_uuid, node);
    if (!require(published && published.value.source_path == first_path &&
                 published_third && published_third.value.source_path == third_path &&
                 published.value.generation == published_third.value.generation &&
                 published.value.generation != active_v1_first.value.generation &&
                 published.value.owners.front().owner.instance_id != published_third.value.owners.front().owner.instance_id &&
                 published.value.owners.front().parameters == first_v2.nodes.front().parameter_blob &&
                 published_third.value.owners.front().parameters == third_v2.nodes.front().parameter_blob &&
                 published.value.owners.front().owner.definition_digest != active_v1_first.value.owners.front().owner.definition_digest &&
                 active_v1_first.value.owners.front().contract.version == Version{1, 0, 0},
                 "Saved startup V2 lost actual source/definition provenance or invalidated the owning V1 snapshot") ||
        !require(scene.play(), "Saved startup V2 Play failed")) return 1;
    const auto wait_for_payload = [&](uint8_t expected) {
        for (unsigned attempt = 0; attempt != 100; ++attempt) {
            const auto first = scene.inspect_feature_node(first_v2.asset_uuid, node);
            const auto second = scene.inspect_feature_node(third_v2.asset_uuid, node);
            if (first && second && first.value.owners.front().state.payload == std::vector<uint8_t>{expected} &&
                second.value.owners.front().state.payload == std::vector<uint8_t>{expected})
                return first.value.owners.front().invocations.last_committed.has_value() &&
                    second.value.owners.front().invocations.last_committed.has_value() &&
                    first.value.owners.front().invocations.last_committed->attempt.token.generation == first.value.generation &&
                    second.value.owners.front().invocations.last_committed->attempt.token.generation == second.value.generation;
            static_cast<void>(concurrency::sleep_for(1'000'000));
            if (!scene.render_frame(frame++, 0.0)) return false;
        }
        return false;
    };
    if (!require(scene.render_frame(frame++, 1.0 / 60.0) && wait_for_payload(1), "Saved startup first actual V2 DLL commit failed") ||
        !require(scene.render_frame(frame++, 1.0 / 60.0) && wait_for_payload(11),
                 "Saved startup selected V1 code instead of the actual V2 DLL callback")) return 1;
    const auto retained_v2 = scene.inspect_feature_node(first_v2.asset_uuid, node);
    if (!require(scene.stop() && scene.shutdown(), "Saved startup V2 Stop/shutdown failed") ||
        !require(retained_v2 && retained_v2.value.owners.front().state.payload == std::vector<uint8_t>{11} &&
                 active_v1_first.value.owners.front().contract.version == Version{1, 0, 0},
                 "Saved startup lifecycle invalidated owning V1/V2 Inspect snapshots")) return 1;
    const auto selected_project_file = root / "Selected.aegproj";
    if (!require(application::project::ProjectFileCodec{}.save(selected_project_file, saved_project_v2) &&
                 application::project::ProjectFileCodec{}.save(root / "Project.aegproj", project.value),
                 "Fresh Native import selected/decoy project setup failed")) return 1;
    Scene fresh;
    if (!require(fresh.initialize(device) && fresh.import_project(selected_project_file),
                 "Fresh runtime exact selected Native project import failed") ||
        !require(fresh.project_file() == selected_project_file &&
                 fresh.authored_feature_catalog().contract_digest() == authoring_v2_digest &&
                 std::ranges::any_of(fresh.authored_feature_graphs(), [&](const auto& entry) {
                     return entry.path == third_path && entry.load_result &&
                         entry.graph.contract_digest == registry_v2.contract_digest();
                 }) && !fresh.feature_publication_pending(),
                 "Fresh runtime ignored exact selected Native source or staged an implicit publication") ||
        !require(fresh.shutdown(), "Fresh Native import shutdown failed")) return 1;
    std::filesystem::remove_all(root, error);
    return require(!error, "Saved startup retained a loaded DLL after shutdown") ? 0 : 1;
}

bool shipping_world_motion(const std::filesystem::path& root, const std::filesystem::path& project_file,
    const std::filesystem::path& scene_path, const aeg::module::ModuleBundle& bundle,
    const std::string& graph_uuid) {
    using namespace aeg;
    using namespace editor_world_motion_test;
    auto project = application::project::ProjectFileCodec{}.load(project_file);
    if (!require(project, "Shipping World route project could not be read")) return false;
    project.value.config.startup_scene = scene_path.lexically_relative(root);
    if (!require(application::project::ProjectFileCodec{}.save(project_file, project.value),
                 "Shipping World route startup Scene could not be selected")) return false;
    tooling::AssetMeta scene_meta{};
    scene_meta.source = tooling::SourceId::parse("00000000-0000-4000-8000-000000000601").value;
    scene_meta.importer = "aeg.scene";
    const auto scene_product = foundation::AssetId::parse("A1234567-89AB-4CDE-8FED-000000000601");
    scene_meta.products = {{scene_product.value, foundation::AssetKind::kScene, "Shipping Scene"}};
    tooling::AssetMeta graph_meta{};
    graph_meta.source = tooling::SourceId::parse("00000000-0000-4000-8000-000000000602").value;
    graph_meta.importer = "aeg.feature_graph";
    graph_meta.products = {{foundation::AssetId::parse(graph_uuid).value, foundation::AssetKind::kFeatureGraph, "Motion"}};
    if (!require(tooling::AssetMetaCodec{}.save(scene_path.string() + ".meta", scene_meta),
                 "Shipping World route Scene product metadata failed") ||
        !require(tooling::AssetMetaCodec{}.save((root / project.value.config.startup_feature_graphs.front()).string() + ".meta", graph_meta),
                 "Shipping World route graph metadata failed")) return false;
    const auto cooked = tooling::cook_project({project_file, bundle.manifest_path, root / "CookedWorldRoute", "Debug"});
    if (!cooked) {
        for (const auto& diagnostic : cooked.value.diagnostics) std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        return require(cooked, "Actual Editor World route Cook failed before Shipping execution");
    }
    const auto delivery = std::filesystem::temp_directory_path() / "aeg_shipping_world_routes_delivery";
    std::error_code error;
    std::filesystem::remove_all(delivery, error);
    if (!require(!error, "Shipping World route isolated delivery cleanup failed")) return false;
    std::filesystem::copy(root / "CookedWorldRoute", delivery, std::filesystem::copy_options::recursive, error);
    if (!require(!error, "Shipping World route isolated delivery copy failed")) return false;
    auto hidden = root;
    hidden += ".shipping-hidden-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const bool parent_is_temporary = std::filesystem::equivalent(
        root.parent_path(), std::filesystem::temp_directory_path(), error);
    if (!require(!error && root.filename() == "aeg_editor_world_motion_lifecycle" && parent_is_temporary &&
                 hidden.parent_path() == root.parent_path() && !std::filesystem::exists(hidden),
                 "Shipping source isolation requires its exact owned temporary directory")) return false;
    std::filesystem::rename(root, hidden, error);
    if (!require(!error, "Shipping source tree isolation failed")) return false;
    struct RestoreSource final {
        std::filesystem::path hidden, original;
        ~RestoreSource() { std::error_code error; std::filesystem::rename(hidden, original, error); }
    } restore_source{hidden, root};
    application::runtime::RuntimeBootstrap bootstrap;
    application::runtime::RuntimeBootRequest request{};
    request.manifest_path = delivery / "current.toml";
    request.headless = true; request.offscreen = true;
    double now = 0.0;
    request.clock = {[](void* data) noexcept { return *static_cast<double*>(data); }, &now};
    if (!require(request.world_command_bindings.empty() && !std::filesystem::exists(root),
                 "Shipping oracle supplied host routes or left source files available") ||
        !require(bootstrap.initialize(request, std::nullopt,
                    {bundle.metadata.build_id, project.value.config.target, "Debug"}),
                 "Shipping World route delivery-only dynamic DLL initialization failed")) return false;
    auto* world = bootstrap.startup_world_scope();
    if (!require(world != nullptr, "Shipping World route has no startup World")) return false;
    ecs::EntityId target{}, bystander{};
    unsigned targets = 0, bystanders = 0;
    if (!require(world->state().for_each<engine::components::Transform>([&](ecs::EntityId entity, const auto& value) {
            if (value.translation.x == 5.0f) { target = entity; ++targets; }
            if (value.translation.x == 23.0f) { bystander = entity; ++bystanders; }
        }), "Shipping startup World entity scan failed") ||
        !require(targets == 1 && bystanders == 1 && target != bystander,
                 "Shipping fixture requires two distinct exact Transform targets")) return false;
    const auto before_revision = world->state().revision();
    now = 0.02; // One due quantum; later pumps keep this clock frozen.
    std::vector<uint8_t> trace;
    float actual_x = 5.0f, other_x = 23.0f;
    for (unsigned attempt = 0; attempt != 500; ++attempt) {
        const auto ticked = bootstrap.tick();
        if (!ticked && ticked.code != foundation::ResultCode::kNotReady)
            return require(ticked, "Shipping World route frame failed");
        const auto snapshot = world->latest_snapshot();
        if (snapshot) {
            const auto* value = snapshot.value->get<engine::components::Transform>(target);
            const auto* other = snapshot.value->get<engine::components::Transform>(bystander);
            if (value) actual_x = value->translation.x;
            if (other) other_x = other->translation.x;
        }
        const auto node = application::runtime::RuntimeFeatureControl::inspect_node(bootstrap, graph_uuid, kNode);
        if (node && node.value.owners.size() == 1 && !node.value.owners.front().processes.empty() &&
            node.value.owners.front().processes.front().state.commit_count != 0) {
            trace = node.value.owners.front().processes.front().state.payload;
            if (actual_x == 7.0f || trace.size() == 1) break;
        }
        static_cast<void>(concurrency::sleep_for(1'000'000));
    }
    uint64_t observed_revision = 0;
    uint32_t before_bits = 0;
    if (trace.size() == 29) {
        for (unsigned byte = 0; byte != 8; ++byte) observed_revision |= uint64_t(trace[1 + byte]) << (8u * byte);
        for (unsigned byte = 0; byte != 4; ++byte) before_bits |= uint32_t(trace[9 + byte]) << (8u * byte);
    }
    const bool consumed = actual_x == 7.0f && other_x == 23.0f && trace.size() == 29 && trace.front() == 1 &&
        observed_revision == before_revision && std::bit_cast<float>(before_bits) == 5.0f &&
        world->state().revision() == before_revision + 1;
    if (!consumed) std::cerr << "SHIPPING_WORLD_ROUTE target=" << actual_x << " bystander=" << other_x
                            << " trace_bytes=" << trace.size() << " request_bindings=0 source_hidden=1\n";
    if (!require(bootstrap.shutdown(), "Shipping World route shutdown failed") ||
        !require(consumed, "Saved Editor route was not delivered to the actual Shipping DLL consumer")) return false;
    std::filesystem::remove_all(delivery, error);
    return require(!error, "Shipping isolated delivery retained a loaded DLL");
}

bool editor_world_motion(aeg::rhi::Device& device, const std::filesystem::path& binary) {
    using namespace aeg;
    using namespace feature;
    using namespace editor_world_motion_test;
    const auto root = std::filesystem::temp_directory_path() / "aeg_editor_world_motion_lifecycle";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    if (!require(!error, "Editor World Motion temporary root cleanup failed")) return false;

    application::EditorSceneRuntime editor_runtime;
    if (!require(editor_runtime.initialize(device), "Editor World Motion initialization failed")) return false;
    const auto precommit_snapshot = editor_runtime.inspect_active_world_snapshot();
    if (!require(!precommit_snapshot && precommit_snapshot.result.code == foundation::ResultCode::kNotReady,
                 "Editor fabricated an active World snapshot before its first World commit") ||
        !require(editor_runtime.request_viewport_extent(48, 32), "Editor World Motion viewport request failed") ||
        !require(editor_runtime.new_project(root, "Editor World Motion"), "Editor World Motion project creation failed") ||
        !require(editor_runtime.create_scene_entity({}, "Motion Target"), "Editor World Motion target creation failed"))
        return false;
    const auto target = editor_runtime.entities().back().id;
    const ecs::ComponentTypeId transform_type(engine::components::kTransformName);
    const auto* current_transform = static_cast<const engine::components::Transform*>(editor_runtime.component_value(target, transform_type));
    if (!require(current_transform != nullptr, "Editor World Motion target has no Transform")) return false;
    auto initial_transform = *current_transform;
    initial_transform.translation.x = 5.0f;
    if (!require(editor_runtime.queue_component_update(target, transform_type, std::as_bytes(std::span{&initial_transform, 1})),
                 "Editor World Motion initial Transform edit failed") ||
        !require(editor_runtime.save_scene(), "Editor World Motion scene save failed")) return false;

    const auto asset_source = [&]() -> foundation::ResultValue<scene::SceneAsset> {
        std::ifstream input(editor_runtime.scene_path(), std::ios::binary);
        if (!input) return {foundation::not_found("Editor World Motion scene archive is missing"), {}};
        const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>{input}, {}};
        serialization::BinaryReader reader(bytes);
        return scene::SceneSerializer::deserialize(reader);
    }();
    if (!require(asset_source, "Editor World Motion scene identity could not be read")) return false;
    const auto scene_asset_id = asset_source.value.id();

    const auto source_definition = definition();
    const auto metadata = module_bundle_test::metadata_for(source_definition);
    const auto bundle = module_bundle_test::write_bundle(binary, root / "Binaries" / "WorldMotion", metadata,
                                                        "editor-world-motion-v1");
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    module::ModuleRegistry modules(registry);
    auto registered_definition = definition();
    if (!require(module::register_static_module(modules, std::move(registered_definition)),
                 "Editor World Motion fixture contract registration failed")) return false;

    FeatureGraphDocument graph{};
    graph.asset_uuid = "00000000-0000-4000-8000-000000000501";
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.contract_digest = registry.contract_digest();
    graph.nodes = {{kNode, kMotion, {1, 0, 0}, OwnerPath{{kAuthorRoot}}, {}}};
    const auto graph_path = root / "Assets" / "Features" / "Motion.aegfeature";
    if (!require(application::authoring::save_feature_graph_document(graph_path, graph),
                 "Editor World Motion graph save failed")) return false;
    auto project = application::project::ProjectFileCodec{}.load(editor_runtime.project_file());
    if (!require(project, "Editor World Motion project config could not be loaded")) return false;
    project.value.config.module.id = "com.aeg.editor.world.motion";
    project.value.config.module.target = "EditorWorldMotionFixture";
    project.value.config.target = "Fixture";
    project.value.config.startup_feature_graphs = {"Assets/Features/Motion.aegfeature"};
    if (!require(editor_runtime.update_project_settings(project.value.config),
                 "Editor World Motion project settings update failed") ||
        !require(editor_runtime.select_feature_module(bundle.manifest_path, "Debug",
                 application::EditorSceneRuntime::FeatureGraphLoadPolicy::kSavedStartup),
                 "Editor World Motion DLL selection failed")) return false;

    uint64_t frame = 1;
    if (!require(editor_runtime.render_frame(frame++, 0.0), "Editor World Motion Edit publication frame failed")) return false;
    const auto edit_snapshot = editor_runtime.inspect_active_world_snapshot();
    const auto world_x = [&](const application::EditorWorldSnapshot& snapshot) -> std::optional<float> {
        if (!snapshot.snapshot) return std::nullopt;
        const auto found = std::find_if(snapshot.entities.begin(), snapshot.entities.end(),
            [&](const scene::SceneEntityBinding& binding) { return binding.scene_entity == target; });
        if (found == snapshot.entities.end()) return std::nullopt;
        const auto* value = snapshot.snapshot->get<engine::components::Transform>(found->runtime_entity);
        return value == nullptr ? std::nullopt : std::optional<float>{value->translation.x};
    };
    if (!require(edit_snapshot && world_x(edit_snapshot.value) == 5.0f,
                 "Editor World Motion Edit snapshot is not the committed authoring World")) return false;
    const auto published = editor_runtime.inspect_feature_execution(graph.asset_uuid);
    if (!require(published && published.value.generation.is_valid(),
                 "Editor World Motion DLL graph was not published")) return false;

    application::EditorFeatureWorldBinding binding{};
    binding.graph_asset_uuid = graph.asset_uuid;
    binding.node_id = kNode;
    binding.owner_path = OwnerPath{{kAuthorRoot}};
    binding.scene_asset = scene_asset_id;
    binding.scene_entity = target;
    binding.effect = kWorldTransformCommandEffect;
    binding.version = kWorldTransformCommandVersion;
    binding.observation_port = kObservationPort;
    auto invalid_binding = binding;
    invalid_binding.scene_entity = scene::SceneEntityId{scene_asset_id.high(), target.low() + 100u};
    if (!require(!editor_runtime.set_feature_world_bindings(std::span{&invalid_binding, 1}),
                 "Editor accepted a route to a missing stable Scene entity")) return false;

    application::EditorFeatureGraphPanel route_panel;
    application::EditorFeatureGraphPanel::Actions route_actions;
    route_actions.selected_world_target_label = [] { return std::string{"Motion Target"}; };
    route_actions.world_binding_editable = true;
    route_actions.bind_world_command = [&](std::string_view asset_uuid, FeatureNodeId node_id) {
        return editor_runtime.bind_feature_world_node(asset_uuid, node_id, target);
    };
    route_actions.clear_world_command = [&](std::string_view asset_uuid, FeatureNodeId node_id) {
        return editor_runtime.clear_feature_world_node_binding(asset_uuid, node_id);
    };
    route_actions.world_binding_status = [&](std::string_view asset_uuid, FeatureNodeId node_id) {
        const auto routes = editor_runtime.feature_world_bindings();
        const auto found = std::find_if(routes.begin(), routes.end(), [&](const auto& route) {
            return route.graph_asset_uuid == asset_uuid && route.node_id == node_id;
        });
        return found == routes.end() ? std::string{} : std::string{"Routed to Motion Target"};
    };
    auto route_document = application::EditorFeatureGraphDocument{graph, bundle.metadata.feature_catalog};
    auto* route_context = ImGui::CreateContext();
    struct RouteUiGuard final { ImGuiContext* context; ~RouteUiGuard() { ImGui::DestroyContext(context); } } route_ui_guard{route_context};
    auto& route_io = ImGui::GetIO();
    route_io.IniFilename = nullptr;
    route_io.DisplaySize = {1400, 1800};
    route_io.DeltaTime = 1.0f / 60.0f;
    unsigned char* route_font_pixels{};
    int route_font_width{}, route_font_height{};
    route_io.Fonts->GetTexDataAsRGBA32(&route_font_pixels, &route_font_width, &route_font_height);
    feature_panel_frame(editor_runtime, route_panel, route_actions, route_context, route_document);
    feature_panel_frame(editor_runtime, route_panel, route_actions, route_context, route_document,
                        std::to_string(kNode.value()).c_str(), true);
    feature_panel_frame(editor_runtime, route_panel, route_actions, route_context, route_document,
                        "Bind selected node to selected Scene entity");
    if (!require(editor_runtime.feature_world_bindings().size() == 1 &&
                 editor_runtime.feature_world_bindings().front().scene_entity == target,
                 "FeatureGraph panel did not install the explicit selected Scene route")) return false;
    feature_panel_frame(editor_runtime, route_panel, route_actions, route_context, route_document,
                        "Remove selected node World route");
    if (!require(editor_runtime.feature_world_bindings().empty(),
                 "FeatureGraph panel did not remove the selected node World route")) return false;
    route_actions.world_binding_editable = false;
    feature_panel_frame(editor_runtime, route_panel, route_actions, route_context, route_document,
                        "Bind selected node to selected Scene entity");
    if (!require(editor_runtime.feature_world_bindings().empty(),
                 "FeatureGraph panel allowed route mutation outside Edit mode")) return false;
    route_actions.world_binding_editable = true;
    feature_panel_frame(editor_runtime, route_panel, route_actions, route_context, route_document,
                        "Bind selected node to selected Scene entity");

    auto library = module::detail::DynamicLibrary::open(bundle.binary_path);
    if (!require(library, "Editor World Motion DLL probe open failed")) return false;
    const auto gate_symbol = library.value.symbol("aeg_world_motion_test_set_gate");
    const auto count_symbol = library.value.symbol("aeg_world_motion_test_get_call_count");
    if (!require(gate_symbol && count_symbol, "Editor World Motion DLL lifecycle probes are missing")) return false;
    const auto set_gate = reinterpret_cast<AegWorldMotionSetGate>(gate_symbol.value);
    const auto get_call_count = reinterpret_cast<AegWorldMotionGetCallCount>(count_symbol.value);
    struct GateReset final {
        AegWorldMotionSetGate set_gate{};
        ~GateReset() { if (set_gate != nullptr) set_gate(1); }
    } gate_reset{set_gate};
    set_gate(1);

    const auto pump_until = [&](float expected_x, uint8_t expected_count, double initial_delta,
                                unsigned max_attempts = 200) {
        for (unsigned attempt = 0; attempt != max_attempts; ++attempt) {
            const auto rendered = editor_runtime.render_frame(frame++, attempt == 0 ? initial_delta : 0.0);
            if (!rendered && rendered.code != foundation::ResultCode::kNotReady)
                return require(rendered, "Editor World Motion frame failed while awaiting its World command");
            const auto snapshot = editor_runtime.inspect_active_world_snapshot();
            const auto actual_x = snapshot ? world_x(snapshot.value) : std::optional<float>{};
            if (actual_x == expected_x) {
                const auto feature_snapshot = editor_runtime.inspect_feature_node(graph.asset_uuid, kNode);
                if (!require(feature_snapshot && feature_snapshot.value.owners.size() == 1,
                             "Editor World Motion owner inspection failed after World commit")) return false;
                const auto& owner = feature_snapshot.value.owners.front();
                if (!require(owner.processes.size() == 1,
                             "Editor World Motion ProcessState owner has no Process snapshot")) return false;
                const auto& payload = owner.processes.front().state.payload;
                if (expected_count == 0) {
                    if (payload == std::vector<uint8_t>{1}) return true;
                    static_cast<void>(concurrency::sleep_for(1'000'000));
                    continue;
                }
                if (!require(payload.size() == 29 && payload.front() == expected_count,
                             "Editor World Motion did not retain its exact observed input trace")) return false;
                const auto read_u64 = [&](std::size_t offset) {
                    uint64_t value = 0;
                    for (unsigned byte = 0; byte != 8; ++byte)
                        value |= uint64_t(payload[offset + byte]) << (byte * 8u);
                    return value;
                };
                const uint32_t before_bits = uint32_t(payload[9]) | (uint32_t(payload[10]) << 8u) |
                    (uint32_t(payload[11]) << 16u) | (uint32_t(payload[12]) << 24u);
                const float before_x = std::bit_cast<float>(before_bits);
                const double elapsed = std::bit_cast<double>(read_u64(13));
                const double frame_delta = std::bit_cast<double>(read_u64(21));
                const float required_before = expected_count == 1 ? 5.0f : expected_count == 2 ? 7.0f :
                    expected_count == 3 ? 41.0f : 5.0f;
                const uint64_t revision = read_u64(1);
                return require(before_x == required_before && revision != 0 &&
                               elapsed == 1.0 / 60.0 && frame_delta == 1.0 / 60.0,
                               "Editor World Motion observation lost exact before/revision/logical/host timing");
            }
            static_cast<void>(concurrency::sleep_for(1'000'000));
        }
        const auto final_world = editor_runtime.inspect_active_world_snapshot();
        const auto final_feature = editor_runtime.inspect_feature_node(graph.asset_uuid, kNode);
        std::cerr << "editor_world_motion_timeout expected_x=" << expected_x
                  << " expected_count=" << unsigned(expected_count)
                  << " dll_calls=" << get_call_count()
                  << " mode=" << static_cast<unsigned>(editor_runtime.mode());
        if (final_world) {
            const auto value = world_x(final_world.value);
            if (value) std::cerr << " actual_x=" << *value << " revision=" << final_world.value.snapshot->revision();
        }
        if (final_feature && !final_feature.value.owners.empty()) {
            const auto& owner = final_feature.value.owners.front();
            const auto& payload = owner.processes.empty() ? owner.state.payload : owner.processes.front().state.payload;
            std::cerr << " payload_bytes=" << payload.size()
                      << " open_attempts=" << owner.invocations.open_attempts.size()
                      << " failed=" << owner.invocations.last_failed.has_value();
            if (!owner.processes.empty())
                std::cerr << " process_status=" << static_cast<unsigned>(owner.processes.front().status)
                          << " process_revision=" << owner.processes.front().revision.value();
        }
        std::cerr << '\n';
        return require(false, "Editor World Motion command did not reach the active committed World");
    };

    if (!require(editor_runtime.set_feature_world_bindings({}), "Editor World Motion unbound baseline setup failed") ||
        !require(editor_runtime.play(), "Editor World Motion unbound Play failed") ||
        !pump_until(5.0f, 0, 1.0 / 60.0) ||
        !require(editor_runtime.pause(), "Editor World Motion unbound Pause failed") ||
        !require(editor_runtime.stop(), "Editor World Motion unbound Stop failed")) return false;
    const uint32_t unbound_calls = get_call_count();
    if (!require(unbound_calls == 1 && world_x(edit_snapshot.value) == 5.0f,
                 "Unbound Editor Feature wrote the Scene World without an explicit route")) return false;

    if (!require(editor_runtime.set_feature_world_bindings(std::span{&binding, 1}),
                 "Editor World Motion explicit route installation failed") ||
        !require(!editor_runtime.set_feature_world_bindings(std::span{&invalid_binding, 1}),
                 "Editor replaced a valid World binding after rejecting a missing-entity route")) return false;
    const auto binding_scene_path = editor_runtime.scene_path();
    if (!require(editor_runtime.save_scene(), "Editor World Motion binding scene save failed") ||
        !require(editor_runtime.new_scene(), "Editor World Motion route round-trip blank Scene creation failed") ||
        !require(editor_runtime.save_scene_as(binding_scene_path.parent_path() / "RouteRoundTrip.aegscene"),
                 "Editor World Motion route round-trip blank Scene save failed") ||
        !require(editor_runtime.open_scene(binding_scene_path), "Editor World Motion binding scene reopen failed"))
        return false;
    const auto restored_bindings = editor_runtime.feature_world_bindings();
    if (!require(restored_bindings.size() == 1 &&
                 restored_bindings.front().graph_asset_uuid == binding.graph_asset_uuid &&
                 restored_bindings.front().node_id == binding.node_id &&
                 restored_bindings.front().owner_path.nodes == binding.owner_path.nodes &&
                 restored_bindings.front().scene_asset == binding.scene_asset &&
                 restored_bindings.front().scene_entity == binding.scene_entity &&
                 restored_bindings.front().effect == binding.effect &&
                 restored_bindings.front().version == binding.version &&
                 restored_bindings.front().observation_port == binding.observation_port,
                 "Editor World Motion saved Scene did not restore its explicit binding")) return false;
    const auto route_backup_path = binding_scene_path.parent_path() / "MotionWithRoute.aegscene";
    if (!require(editor_runtime.save_scene_as(route_backup_path),
                 "Editor World Motion route-bearing Scene Save As failed") ||
        !require(editor_runtime.open_scene(route_backup_path),
                 "Editor World Motion route-bearing Scene Save As reopen failed")) return false;
    if (!require(editor_runtime.feature_world_bindings().size() == 1 &&
                 editor_runtime.feature_world_bindings().front().scene_entity == target,
                 "Scene Save As did not preserve its explicit World route")) return false;
    if (!require(editor_runtime.open_scene(binding_scene_path),
                 "Editor World Motion original route Scene reopen failed") ||
        !require(editor_runtime.new_scene(), "Editor World Motion route replacement Scene creation failed") ||
        !require(editor_runtime.save_scene(), "Editor World Motion route replacement Scene save failed") ||
        !require(editor_runtime.open_scene(binding_scene_path),
                 "Editor World Motion route replacement Scene reopen failed")) return false;
    if (!require(editor_runtime.feature_world_bindings().empty(),
                 "Saving a replacement Scene retained stale World routes from the prior Scene")) return false;
    if (!require(editor_runtime.open_scene(route_backup_path),
                 "Editor World Motion preserved route Scene reopen failed") ||
        !require(editor_runtime.feature_world_bindings().size() == 1 &&
                 editor_runtime.feature_world_bindings().front().scene_entity == target,
                 "Preserved Scene route was lost after replacement Scene save")) return false;
    if (!require(editor_runtime.play(), "Editor World Motion Play failed") ||
        !require(!editor_runtime.set_feature_world_bindings({}),
                 "Editor allowed route mutation while the Runtime World was active") ||
        !pump_until(7.0f, 1, 1.0 / 60.0)) return false;
    const auto first_runtime = editor_runtime.inspect_active_world_snapshot();
    if (!require(first_runtime && world_x(first_runtime.value) == 7.0f &&
                 world_x(edit_snapshot.value) == 5.0f && world_x(edit_snapshot.value) == 5.0f,
                 "Editor Play mutated the Edit World or failed to retain its first committed Runtime snapshot")) return false;

    const uint32_t after_first = get_call_count();
    if (!require(editor_runtime.pause(), "Editor World Motion Pause failed") ||
        !require(editor_runtime.render_frame(frame++, 1.0), "Editor World Motion paused completion drain failed") ||
        !require(get_call_count() == after_first &&
                 world_x(editor_runtime.inspect_active_world_snapshot().value) == 7.0f,
                 "Pause issued backlog work or changed the active Runtime World") ||
        !require(editor_runtime.play(), "Editor World Motion Resume failed") ||
        !pump_until(41.0f, 2, 1.0 / 60.0)) return false;
    const auto second_runtime = editor_runtime.inspect_active_world_snapshot();
    if (!require(second_runtime && world_x(second_runtime.value) == 41.0f,
                 "Editor World Motion second observed command did not apply")) return false;

    if (!require(editor_runtime.request_viewport_extent(72, 40), "Editor World Motion resize request failed") ||
        !require(editor_runtime.render_frame(frame++, 0.0), "Editor World Motion resize frame failed") ||
        !require(world_x(editor_runtime.inspect_active_world_snapshot().value) == 41.0f,
                 "Editor viewport resize replaced the active World snapshot") ||
        !pump_until(43.0f, 3, 1.0 / 60.0)) return false;
    const auto old_runtime_snapshot = editor_runtime.inspect_active_world_snapshot();
    const auto old_feature = editor_runtime.inspect_feature_node(graph.asset_uuid, kNode);
    if (!require(old_runtime_snapshot && old_feature && world_x(old_runtime_snapshot.value) == 43.0f,
                 "Editor World Motion retained snapshot capture failed")) return false;
    if (!require(editor_runtime.stop(), "Editor World Motion Stop failed") ||
        !require(world_x(editor_runtime.inspect_active_world_snapshot().value) == 5.0f,
                 "Stop did not restore the authored Edit World") ||
        !require(editor_runtime.play(), "Editor World Motion replay failed") ||
        !pump_until(7.0f, 1, 1.0 / 60.0)) return false;

    const auto generation_before_apply = editor_runtime.inspect_feature_execution(graph.asset_uuid);
    if (!require(generation_before_apply, "Editor World Motion pre-Apply generation inspection failed")) return false;
    set_gate(0);
    const uint32_t gated_call_count = get_call_count();
    const auto gated_frame = editor_runtime.render_frame(frame++, 1.0 / 60.0);
    if (!gated_frame && gated_frame.code != foundation::ResultCode::kNotReady)
        return require(gated_frame, "Editor World Motion gated frame failed unexpectedly");
    bool entered = false;
    for (unsigned attempt = 0; attempt != 200; ++attempt) {
        if (get_call_count() > gated_call_count) { entered = true; break; }
        static_cast<void>(concurrency::sleep_for(1'000'000));
    }
    if (!require(entered, "Editor World Motion gated DLL callback was not issued")) {
        set_gate(1);
        return false;
    }
    auto invalid_candidate = graph;
    invalid_candidate.nodes.clear();
    if (!require(!editor_runtime.queue_feature_graph(invalid_candidate) && !editor_runtime.feature_publication_pending(),
                 "Editor staged an Apply candidate that removed a bound World command owner")) {
        set_gate(1);
        return false;
    }
    auto apply_candidate = graph;
    ++apply_candidate.draft_revision;
    if (!require(editor_runtime.queue_feature_graph(apply_candidate) && editor_runtime.feature_publication_pending(),
                 "Editor World Motion valid Apply candidate was not queued")) {
        set_gate(1);
        return false;
    }
    const auto waiting = editor_runtime.render_frame(frame++, 0.0);
    if (!waiting && waiting.code != foundation::ResultCode::kNotReady) {
        set_gate(1);
        return require(waiting, "Editor World Motion gated Apply frame failed unexpectedly");
    }
    if (!require(world_x(editor_runtime.inspect_active_world_snapshot().value) == 7.0f &&
                 editor_runtime.feature_publication_pending(),
                 "World-bound Apply published before its old command reached the World boundary")) {
        set_gate(1);
        return false;
    }
    set_gate(1);
    bool applied = false;
    for (unsigned attempt = 0; attempt != 200; ++attempt) {
        const auto rendered = editor_runtime.render_frame(frame++, 0.0);
        if (!rendered && rendered.code != foundation::ResultCode::kNotReady) {
            set_gate(1);
            return require(rendered, "Editor World Motion old-generation Apply drain failed");
        }
        const auto actual = editor_runtime.inspect_active_world_snapshot();
        const auto generation_after = editor_runtime.inspect_feature_execution(graph.asset_uuid);
        if (actual && world_x(actual.value) == 41.0f && generation_after &&
            generation_after.value.generation != generation_before_apply.value.generation &&
            !editor_runtime.feature_publication_pending()) {
            applied = true;
            break;
        }
        static_cast<void>(concurrency::sleep_for(1'000'000));
    }
    set_gate(1);
    if (!require(applied, "World-bound Apply did not commit old output then rebind the new Feature generation"))
        return false;

    if (!require(editor_runtime.stop(), "Editor World Motion final Stop failed") ||
        !require(editor_runtime.create_scene_entity({}, "Shipping Bystander"), "Shipping bystander authoring failed")) return false;
    const auto bystander = editor_runtime.entities().back().id;
    const auto* bystander_value = static_cast<const engine::components::Transform*>(editor_runtime.component_value(bystander, transform_type));
    if (!require(bystander_value != nullptr, "Shipping bystander has no Transform")) return false;
    auto bystander_transform = *bystander_value;
    bystander_transform.translation.x = 23.0f;
    if (!require(editor_runtime.queue_component_update(bystander, transform_type, std::as_bytes(std::span{&bystander_transform, 1})),
                 "Shipping bystander value authoring failed")) return false;
    feature_panel_frame(editor_runtime, route_panel, route_actions, route_context, route_document,
                        "Remove selected node World route");
    feature_panel_frame(editor_runtime, route_panel, route_actions, route_context, route_document,
                        "Bind selected node to selected Scene entity");
    if (!require(editor_runtime.feature_world_bindings().size() == 1 && editor_runtime.save_scene(),
                 "Shipping route actual UI Bind/Save failed")) return false;
    const auto shipping_project = editor_runtime.project_file();
    const auto shipping_scene = editor_runtime.scene_path();
    if (!require(editor_runtime.shutdown(), "Editor World Motion shutdown failed")) return false;
    bool retained_feature_trace = false;
    if (old_feature && !old_feature.value.owners.empty() && !old_feature.value.owners.front().processes.empty()) {
        const auto& payload = old_feature.value.owners.front().processes.front().state.payload;
        retained_feature_trace = payload.size() == 29 && payload.front() == 3;
    }
    const auto retained_x = old_runtime_snapshot ? world_x(old_runtime_snapshot.value) : std::optional<float>{};
    if (!require(old_runtime_snapshot && retained_x == 43.0f &&
                 old_runtime_snapshot.value.snapshot->revision() != 0 && retained_feature_trace,
                 "Editor retained World/Feature snapshots borrowed state past Stop or shutdown")) return false;
    gate_reset.set_gate = nullptr;
    const auto probe_close = library.value.close();
    if (!require(probe_close, "Editor World Motion probe handle remained loaded after shutdown")) return false;
    if (!shipping_world_motion(root, shipping_project, shipping_scene, bundle, graph.asset_uuid)) return false;
    std::filesystem::remove_all(root, error);
    return require(!error, "Editor World Motion fixture retained its loaded DLL after shutdown");
}

bool editor_demo_motion(aeg::rhi::Device& device, const std::filesystem::path& binary,
                        const std::filesystem::path& project_root) {
    using namespace aeg;
    using namespace feature;
    const auto root = std::filesystem::temp_directory_path() / "aeg_editor_demo_motion_integration";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    if (!require(!error, "Demo Motion temporary root cleanup failed")) return false;

    auto definition = demo::make_module();
    if (!require(definition.status, "Demo Motion module definition is invalid")) return false;
    module::ModuleBundle bundle{};
    try {
        bundle = module_bundle_test::write_bundle(binary, root / "Bundle",
            module_bundle_test::metadata_for(definition), "demo-motion-integration", "windows-x64-vulkan");
    } catch (const std::exception& exception) {
        std::cerr << "demo_motion: bundle-error " << exception.what() << '\n';
        return false;
    }
    application::EditorSceneRuntime runtime;
    if (!require(runtime.initialize(device), "Demo Motion Editor initialization failed")) return false;
    if (!require(runtime.request_viewport_extent(64, 48), "Demo Motion viewport request failed")) return false;
    if (!require(runtime.import_project(project_root / "Project.aegproj"), "Demo Motion project import failed")) return false;
    if (!require(runtime.open_scene(project_root / "Scenes" / "RenderingShowcase.aegscene"),
                 "Demo Motion showcase scene open failed")) return false;

    const auto project = application::project::ProjectFileCodec{}.load(project_root / "Project.aegproj");
    const auto graph = application::authoring::load_feature_graph_document(
        project_root / "Assets" / "Features" / "Motion.aegfeature");
    if (!require(project && project.value.config.module.id == "com.aeg.demo" &&
                 project.value.config.startup_feature_graphs.size() == 1 &&
                 project.value.config.startup_feature_graphs.front().filename() == "Motion.aegfeature",
                 "Demo Motion module identity or saved startup root did not round-trip") ||
        !require(graph && graph.value.nodes.size() == 1 &&
                 graph.value.nodes.front().definition_id == FeatureDefinitionId{"Demo.Logic.Motion"},
                 "Demo Motion author graph does not contain its executable Process")) return false;
    if (!require(runtime.select_feature_module(bundle.manifest_path, "Debug",
            application::EditorSceneRuntime::FeatureGraphLoadPolicy::kSavedStartup),
            "Demo Motion dynamic module and saved startup roots did not publish")) return false;

    uint64_t frame = 1;
    if (!require(runtime.render_frame(frame++, 0.0), "Demo Motion Edit publication frame failed")) return false;
    const auto target = std::find_if(runtime.entities().begin(), runtime.entities().end(),
        [](const scene::SceneEntityRecord& entity) { return entity.name == "Orange Cube"; });
    if (!require(target != runtime.entities().end(), "Demo Motion showcase target is missing")) return false;
    const scene::SceneEntityId target_id = target->id;
    const auto scene_source = [&]() -> foundation::ResultValue<scene::SceneAsset> {
        std::ifstream input(runtime.scene_path(), std::ios::binary);
        if (!input) return {foundation::not_found("Demo Motion scene archive is missing"), {}};
        const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>{input}, {}};
        serialization::BinaryReader reader(bytes);
        return scene::SceneSerializer::deserialize(reader);
    }();
    if (!require(scene_source, "Demo Motion scene identity could not be read")) return false;
    const auto scene_asset = scene_source.value.id();

    application::EditorFeatureWorldBinding binding{};
    binding.graph_asset_uuid = graph.value.asset_uuid;
    binding.node_id = FeatureNodeId{"Demo.Motion.Node"};
    binding.owner_path = graph.value.nodes.front().owner_path;
    binding.scene_asset = scene_asset;
    binding.scene_entity = target_id;
    binding.effect = kWorldTransformCommandEffect;
    binding.version = kWorldTransformCommandVersion;
    binding.observation_port = PortId{"Demo.WorldObservation"};
    if (!require(runtime.set_feature_world_bindings(std::span{&binding, 1}),
                 "Demo Motion explicit Scene-to-World route was rejected")) return false;
    const auto edit_snapshot = runtime.inspect_active_world_snapshot();
    const auto read_rotation = [&](const application::EditorWorldSnapshot& snapshot) -> std::optional<math::Quat> {
        if (!snapshot.snapshot) return std::nullopt;
        const auto mapped = std::find_if(snapshot.entities.begin(), snapshot.entities.end(),
            [&](const scene::SceneEntityBinding& value) { return value.scene_entity == target_id; });
        if (mapped == snapshot.entities.end()) return std::nullopt;
        const auto* transform = snapshot.snapshot->get<engine::components::Transform>(mapped->runtime_entity);
        return transform == nullptr ? std::nullopt : std::optional<math::Quat>{transform->rotation};
    };
    if (!require(edit_snapshot && read_rotation(edit_snapshot.value), "Demo Motion Edit Transform snapshot is unavailable"))
        return false;
    const auto before = *read_rotation(edit_snapshot.value);

    if (!require(runtime.play(), "Demo Motion Play failed")) return false;
    std::optional<math::Quat> after;
    for (unsigned attempt = 0; attempt != 300 && !after; ++attempt) {
        const auto rendered = runtime.render_frame(frame++, attempt == 0 ? 1.0 / 60.0 : 0.0);
        if (!rendered && rendered.code != foundation::ResultCode::kNotReady)
            return require(rendered, "Demo Motion frame failed before its World command committed");
        const auto active = runtime.inspect_active_world_snapshot();
        if (active) {
            const auto candidate = read_rotation(active.value);
            if (candidate && (*candidate != before)) after = candidate;
        }
        if (!after) static_cast<void>(concurrency::sleep_for(1'000'000));
    }
    const auto process = runtime.inspect_feature_node(graph.value.asset_uuid, FeatureNodeId{"Demo.Motion.Node"});
    if (!require(after.has_value(), "Demo Motion DLL did not produce a World.Transform command from observation") ||
        !require(process && process.value.owners.size() == 1 && process.value.owners.front().processes.size() == 1 &&
                 process.value.owners.front().processes.front().state.payload.size() == sizeof(uint32_t),
                 "Demo Motion callback state did not persist in its Process owner") ||
        !require(runtime.pause(), "Demo Motion Pause failed") || !require(runtime.play(), "Demo Motion Resume failed") ||
        !require(runtime.stop(), "Demo Motion Stop failed")) return false;
    const auto stopped = runtime.inspect_active_world_snapshot();
    if (!require(stopped && read_rotation(stopped.value) == read_rotation(edit_snapshot.value),
                 "Demo Motion changed the Edit World or failed to retain its pre-Play value")) return false;
    if (!require(runtime.shutdown(), "Demo Motion Editor shutdown failed")) return false;
    std::filesystem::remove_all(root, error);
    return require(!error, "Demo Motion temporary root retained the loaded DLL or bundle");
}
}  // namespace

int main(int argc, char** argv) {
    const aeg::rhi::DeviceCreateDesc device_desc{
        aeg::rhi::BackendType::kVulkan,
        "AEG Editor Scene Runtime Smoke",
        std::numeric_limits<uint32_t>::max(),
        true,
        true,
    };
    auto device_result = aeg::rhi::create_device(device_desc);
    if (!require(device_result, "editor scene smoke device creation failed")) {
        return 1;
    }
    std::unique_ptr<aeg::rhi::Device> device = std::move(device_result.value);

    aeg::application::EditorSceneRuntime runtime;
    if (!require(!runtime.save_scene_as("uninitialized.aegscene"),
                 "Save As on an uninitialized runtime must return a failure")) {
        return 13;
    }
    if (!require(runtime.initialize(*device), "editor scene smoke runtime initialization failed") ||
        !require(runtime.request_viewport_extent(96, 64), "editor scene smoke viewport request failed") ||
        !require(runtime.entities().size() == 4, "editor scene smoke default scene entity count is invalid")) {
        return 2;
    }

    if (argc == 2 && std::string_view(argv[1]) == "--world-api") {
        const auto initial = runtime.inspect_active_world_snapshot();
        if (!require(!initial && initial.result.code == aeg::foundation::ResultCode::kNotReady,
                     "Editor fabricated an active World snapshot before its first commit") ||
            !require(runtime.set_feature_world_bindings(
                         std::span<const aeg::application::EditorFeatureWorldBinding>{}),
                     "Edit mode could not clear explicit Feature World bindings")) {
            return 31;
        }
        return require(runtime.shutdown(), "Editor world API smoke shutdown failed") ? 0 : 32;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--world-feature") {
        const bool result = editor_world_motion(*device, argv[2]);
        if (!require(runtime.shutdown(), "Editor outer World Motion smoke shutdown failed")) return 34;
        return result ? 0 : 33;
    }
    if (argc == 4 && std::string_view(argv[1]) == "--demo-motion") {
        const bool result = editor_demo_motion(*device, argv[2], argv[3]);
        if (!require(runtime.shutdown(), "Editor outer Demo Motion smoke shutdown failed")) return 36;
        return result ? 0 : 35;
    }

    const auto light_entity =
        std::find_if(runtime.entities().begin(), runtime.entities().end(),
                     [](const aeg::scene::SceneEntityRecord& entity) { return entity.name == "Directional Light"; });
    const aeg::ecs::ComponentTypeId light_type(aeg::engine::components::kDirectionalLightName);
    const auto* source_light = light_entity == runtime.entities().end()
                                   ? nullptr
                                   : static_cast<const aeg::engine::components::DirectionalLight*>(
                                         runtime.component_value(light_entity->id, light_type));
    if (!require(light_entity != runtime.entities().end(), "editor scene smoke directional light is missing") ||
        !require(source_light != nullptr, "editor scene smoke directional light value is missing")) {
        return 3;
    }
    aeg::engine::components::DirectionalLight invalid_light = *source_light;
    invalid_light.color = {444.0f, 444.0f, 444.0f};
    const bool rejected_light_update = !runtime.queue_component_update(
        light_entity->id, light_type, std::as_bytes(std::span{&invalid_light, 1}));
    const auto rejected_light_frame = runtime.render_frame(0, 1.0 / 60.0);
    if (!require(
            rejected_light_update, "editor scene smoke accepted an out-of-range light color") ||
        !require(rejected_light_frame, rejected_light_frame.message.empty()
                                          ? "editor scene smoke rejected-light frame failed"
                                          : rejected_light_frame.message.c_str())) {
        return 4;
    }
    const auto* preserved_light = static_cast<const aeg::engine::components::DirectionalLight*>(
        runtime.component_value(light_entity->id, light_type));
    if (!require(preserved_light != nullptr && preserved_light->color.x <= 1.0f && preserved_light->color.y <= 1.0f &&
                     preserved_light->color.z <= 1.0f,
                 "editor scene smoke invalid light edit polluted the World")) {
        return 5;
    }

    const auto left_entity =
        std::find_if(runtime.entities().begin(), runtime.entities().end(),
                     [](const aeg::scene::SceneEntityRecord& entity) { return entity.name == "Left Mesh"; });
    if (!require(left_entity != runtime.entities().end(), "editor scene smoke left mesh is missing") ||
        !require(runtime.select(left_entity->id), "editor scene smoke entity selection failed")) {
        return 6;
    }

    const aeg::ecs::ComponentTypeId mesh_type(aeg::engine::components::kMeshRendererName);
    const auto descriptor = runtime.component_descriptor(mesh_type);
    const auto* source =
        static_cast<const aeg::engine::components::MeshRenderer*>(runtime.component_value(left_entity->id, mesh_type));
    if (!require(descriptor, "editor scene smoke MeshRenderer descriptor is missing") ||
        !require(source != nullptr, "editor scene smoke MeshRenderer value is missing") ||
        !require(descriptor.value.size == sizeof(aeg::engine::components::MeshRenderer),
                 "editor scene smoke MeshRenderer storage size is invalid")) {
        return 7;
    }

    aeg::engine::components::MeshRenderer edited = *source;
    edited.tint = {0.2f, 0.7f, 0.95f};
    edited.tint_alpha = 0.8f;
    const auto edited_bytes = std::as_bytes(std::span{&edited, 1});
    if (!require(runtime.queue_component_update(left_entity->id, mesh_type, edited_bytes),
                 "editor scene smoke MeshRenderer edit queue failed") ||
        !require(runtime.render_frame(0, 1.0 / 60.0), "editor scene smoke first RenderGraph frame failed") ||
        !require(runtime.render_frame(1, 1.0 / 60.0), "editor scene smoke second RenderGraph frame failed") ||
        !require(runtime.viewport_ready(), "editor scene smoke viewport target is not ready")) {
        return 8;
    }
    if (!default_preview_has_feature_source(runtime)) return 26;

    const auto* committed =
        static_cast<const aeg::engine::components::MeshRenderer*>(runtime.component_value(left_entity->id, mesh_type));
    if (!require(committed != nullptr && same_tint(*committed, edited),
                 "editor scene smoke CommandBuffer edit did not commit")) {
        return 9;
    }

    const std::filesystem::path scene_path =
        std::filesystem::temp_directory_path() / "aeg_editor_scene_runtime_smoke.aegscene";
    std::error_code remove_error;
    std::filesystem::remove(scene_path, remove_error);
    if (!require(runtime.save_scene_as(scene_path), "editor scene smoke Save As failed") ||
        !require(!runtime.dirty(), "editor scene smoke scene remained dirty after save") ||
        !require(runtime.open_scene(scene_path), "editor scene smoke reopen failed") ||
        !require(runtime.entities().size() == 4, "editor scene smoke reopened entity count is invalid")) {
        std::filesystem::remove(scene_path, remove_error);
        return 10;
    }

    const auto reopened_left =
        std::find_if(runtime.entities().begin(), runtime.entities().end(),
                     [](const aeg::scene::SceneEntityRecord& entity) { return entity.name == "Left Mesh"; });
    const aeg::scene::SceneEntityId reopened_left_id =
        reopened_left == runtime.entities().end() ? aeg::scene::SceneEntityId{} : reopened_left->id;
    const auto* reopened_value = reopened_left == runtime.entities().end()
                                     ? nullptr
                                     : static_cast<const aeg::engine::components::MeshRenderer*>(
                                           runtime.component_value(reopened_left_id, mesh_type));
    if (!require(reopened_left != runtime.entities().end(), "editor scene smoke reopened left mesh is missing") ||
        !require(reopened_value != nullptr && same_tint(*reopened_value, edited),
                 "editor scene smoke MeshRenderer values did not round-trip")) {
        std::filesystem::remove(scene_path, remove_error);
        return 11;
    }

    if (!require(runtime.play(), "editor scene smoke Play failed") ||
        !require(runtime.render_frame(2, 1.0 / 60.0), "editor scene smoke Play frame failed") ||
        !require(runtime.pause(), "editor scene smoke Pause failed") ||
        !require(runtime.render_frame(3, 1.0 / 60.0), "editor scene smoke Pause frame failed") ||
        !require(runtime.play(), "editor scene smoke Resume failed") ||
        !require(runtime.mode() == aeg::application::EditorSceneRuntime::Mode::kPlay,
                 "editor scene smoke Resume did not restore Play mode") ||
        !require(runtime.render_frame(4, 1.0 / 60.0), "editor scene smoke resumed Play frame failed") ||
        !require(runtime.stop(), "editor scene smoke Stop failed") ||
        !require(runtime.mode() == aeg::application::EditorSceneRuntime::Mode::kEdit,
                 "editor scene smoke Stop did not return to Edit mode") ||
        !require(runtime.request_viewport_extent(72, 48), "editor scene smoke resize request failed") ||
        !require(runtime.render_frame(5, 1.0 / 60.0), "editor scene smoke resized frame failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 12;
    }

    if (!default_preview_has_feature_source(runtime) ||
        !preview_geometry_pixels(runtime, reopened_left_id, 6, 72, 48)) return 19;
    const auto retained_pixels = runtime.read_viewport_pixels();
    const auto original_renderer = aeg::renderer::scene_render_profile_graph(aeg::renderer::SceneRenderProfile::kOpaque);
    if (!require(retained_pixels && original_renderer, "Actual Preview source/readback is unavailable")) return 20;
    auto empty_renderer = original_renderer.value;
    empty_renderer.graph_version += 1;
    empty_renderer.nodes.clear();
    const auto* after_stop = static_cast<const aeg::engine::components::MeshRenderer*>(runtime.component_value(reopened_left_id, mesh_type));
    const bool preserved = after_stop != nullptr && same_tint(*after_stop, edited);
    if (!require(runtime.publish_renderer_feature_graph(empty_renderer), "Preview empty Feature publication failed")) return 21;
    if (!require(runtime.render_frame(6, 1.0 / 60.0), "editor scene smoke empty Feature frame failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 22;
    }
    if (!require(runtime.request_viewport_readback(), "editor scene smoke empty Feature readback request failed") ||
        !require(runtime.render_frame(7, 1.0 / 60.0), "editor scene smoke empty Feature readback frame failed")) {
        std::filesystem::remove(scene_path, remove_error);
        return 23;
    }
    const auto disconnected_pixels = runtime.read_viewport_pixels();
    const auto empty_capture = runtime.inspect_render_graph();
    const auto empty_execution = runtime.inspect_renderer_feature_execution();
    if (!require(disconnected_pixels && disconnected_pixels.value == retained_pixels.value && runtime.draw_call_count() == 0,
                 "Empty Preview graph performed hidden rendering or fallback clearing") ||
        !require(empty_capture && empty_execution && empty_execution.value.plans.empty() &&
                 std::ranges::none_of(empty_capture.value->plan.pass_plans, [](const auto& pass) { return pass.feature_work.has_value(); }),
                 "Empty Preview graph retained business Feature work")) {
        std::filesystem::remove(scene_path, remove_error);
        return 24;
    }
    if (!require(runtime.restore_default_renderer_feature(), "Actual Restore Default Preview failed") ||
        !require(runtime.render_frame(8, 0.0), "Preview Opaque restored frame failed") ||
        !default_preview_has_feature_source(runtime)) return 25;
    // Black pixels come from the actual Opaque Feature with no visible geometry.
    for (const auto& entity : runtime.entities()) {
        const auto* mesh = static_cast<const aeg::engine::components::MeshRenderer*>(runtime.component_value(entity.id, mesh_type));
        if (mesh == nullptr) continue;
        auto hidden = *mesh;
        hidden.visible = false;
        if (!require(runtime.queue_component_update(entity.id, mesh_type, std::as_bytes(std::span{&hidden, 1})),
                     "Preview empty-geometry setup failed")) return 25;
    }
    if (!require(runtime.request_viewport_readback(), "Preview empty-geometry readback request failed") ||
        !require(runtime.render_frame(9, 0.0), "Preview empty-geometry frame failed") ||
        !default_preview_has_feature_source(runtime)) return 25;
    const auto black_pixels = runtime.read_viewport_pixels();
    if (!require(black_pixels && black_pixels.value.size() == 72 * 48 * 4, "Preview empty-geometry pixels unavailable")) return 25;
    for (std::size_t index = 0; index + 3 < black_pixels.value.size(); index += 4) {
        if (!require(std::to_integer<uint8_t>(black_pixels.value[index]) < 10 &&
                         std::to_integer<uint8_t>(black_pixels.value[index + 1]) < 10 &&
                         std::to_integer<uint8_t>(black_pixels.value[index + 2]) < 10,
                     "Preview empty-geometry Opaque Feature did not produce black pixels")) {
            std::filesystem::remove(scene_path, remove_error);
            return 25;
        }
    }
    if (!require(runtime.open_scene(scene_path), "Preview geometry restore failed")) return 25;
    for (const auto profile : {aeg::renderer::SceneRenderProfile::kForward, aeg::renderer::SceneRenderProfile::kDeferred}) {
        const auto graph = aeg::renderer::scene_render_profile_graph(profile);
        if (!require(graph && runtime.publish_renderer_feature_graph(graph.value), "Actual Preview profile publication failed") ||
            !require(runtime.render_frame(10, 0.0), "Actual Preview profile GPU frame failed") ||
            !default_preview_has_feature_source(runtime, aeg::feature::FeatureDefinitionId{
                profile == aeg::renderer::SceneRenderProfile::kForward ? "Renderer.Scene.Forward" : "Renderer.Scene.Deferred"},
                profile == aeg::renderer::SceneRenderProfile::kForward ? 5 : 4) ||
            !preview_geometry_pixels(runtime, reopened_left_id, 10, 72, 48,
                profile == aeg::renderer::SceneRenderProfile::kForward ? "Forward" : "Deferred",
                profile == aeg::renderer::SceneRenderProfile::kForward ? 255 : 204)) return 25;
    }
    if (!require(runtime.publish_renderer_feature_graph(original_renderer.value), "Preview final Opaque restore failed")) return 25;
    std::filesystem::remove(scene_path, remove_error);
    if (!require(preserved, "editor scene smoke Play/Stop polluted edit World")) {
        return 13;
    }
    aeg::engine::components::MeshRenderer failed_save_edit = edited;
    failed_save_edit.tint.x = 0.15f;
    if (!require(
            runtime.queue_component_update(reopened_left_id, mesh_type, std::as_bytes(std::span{&failed_save_edit, 1})),
            "editor scene smoke dirty setup failed") ||
        !require(runtime.dirty(), "editor scene smoke dirty setup did not mark scene dirty")) {
        std::filesystem::remove(scene_path, remove_error);
        return 14;
    }
    const auto failed_save_directory = std::filesystem::temp_directory_path() / "aeg_editor_save_failure_directory";
    std::filesystem::create_directories(failed_save_directory, remove_error);
    if (!require(!runtime.save_scene_as(failed_save_directory),
                 "editor scene smoke directory Save As unexpectedly passed") ||
        !require(runtime.dirty(), "editor scene smoke failed save cleared dirty state")) {
        std::filesystem::remove_all(failed_save_directory, remove_error);
        std::filesystem::remove(scene_path, remove_error);
        return 15;
    }
    std::filesystem::remove_all(failed_save_directory, remove_error);
    const auto corrupt_scene = std::filesystem::temp_directory_path() / "aeg_editor_corrupt_scene.aegscene";
    std::ofstream(corrupt_scene, std::ios::binary) << "corrupt";
    const auto scene_before_failed_open = runtime.scene_path();
    const auto entity_count_before_failed_open = runtime.entities().size();
    if (!require(!runtime.open_scene(corrupt_scene), "editor scene smoke corrupt scene unexpectedly loaded") ||
        !require(runtime.scene_path() == scene_before_failed_open,
                 "editor scene smoke failed open changed the current scene path") ||
        !require(runtime.entities().size() == entity_count_before_failed_open,
                 "editor scene smoke failed open changed the current scene")) {
        std::filesystem::remove(corrupt_scene, remove_error);
        std::filesystem::remove(scene_path, remove_error);
        return 16;
    }
    std::filesystem::remove(corrupt_scene, remove_error);
    const auto project_root = std::filesystem::temp_directory_path() / "aeg_editor_scene_runtime_project";
    std::filesystem::remove_all(project_root, remove_error);
    const auto before_new_project = runtime.inspect_render_graph();
    if (!require(before_new_project, "New Project regression needs a captured frame")) return 17;
    const auto preceding_preview = aeg::renderer::scene_render_profile_graph(aeg::renderer::SceneRenderProfile::kForward);
    if (!require(preceding_preview && runtime.publish_renderer_feature_graph(preceding_preview.value),
                 "Cross-project Forward Preview setup failed")) return 17;
    const auto new_project_result = runtime.new_project(project_root, "Runtime Smoke Game");
    if (!new_project_result) {
        std::cerr << "editor scene smoke new game project diagnostic: " << new_project_result.message << '\n';
    }
    if (!require(new_project_result, "editor scene smoke new game project failed") ||
        !require(!runtime.inspect_render_graph(), "New Project exposed the previous scene RenderGraph") ||
        !require(before_new_project.value->plan.revision != 0 && !before_new_project.value->plan.pass_plans.empty(),
                 "New Project invalidated caller-owned RenderGraph metadata") ||
        !require(runtime.project_loaded(), "editor scene smoke new game project is not active") ||
        !has_default_preview_definition(runtime, "New Project inherited the previous Renderer Feature selection") ||
        !require(runtime.project_root() == std::filesystem::weakly_canonical(project_root),
                 "editor scene smoke active project root is incorrect") ||
        !require(runtime.project_file() == std::filesystem::weakly_canonical(project_root / "Project.aegproj"),
                 "editor scene smoke lost the exact active project file") ||
        !require(runtime.entities().empty(), "editor scene smoke new project inherited editor sample entities") ||
        !require(std::ranges::none_of(runtime.resource_descriptors(),
                                     [](const auto& resource) { return resource.path.empty(); }),
                 "editor scene smoke new project exposed editor sample resources") ||
        !require(runtime.render_frame(100, 1.0 / 60.0), "editor scene smoke empty project viewport frame failed") ||
        !require(runtime.play(), "editor scene smoke empty project Play failed") ||
        !require(runtime.render_frame(101, 1.0 / 60.0), "editor scene smoke empty project Play frame failed") ||
        !require(runtime.stop(), "editor scene smoke empty project Stop failed") ||
        !require(runtime.save_scene(), "editor scene smoke project scene save failed") ||
        !require(runtime.import_project(project_root), "editor scene smoke project reimport failed") ||
        !require(runtime.entities().empty(), "editor scene smoke imported project restored editor sample entities") ||
        !require(std::ranges::none_of(runtime.resource_descriptors(),
                                     [](const auto& resource) { return resource.path.empty(); }),
                 "editor scene smoke imported project exposed editor sample resources") ||
        !require(runtime.create_scene_entity({}, "Project Entity"),
                 "editor scene smoke project entity creation failed") ||
        !require(runtime.save_scene(), "editor scene smoke populated project scene save failed") ||
        !require(runtime.import_project(project_root), "editor scene smoke populated project reimport failed") ||
        !require(runtime.entities().size() == 1 && runtime.entities().front().name == "Project Entity",
                 "editor scene smoke project reimport did not preserve project content")) {
        std::filesystem::remove(project_root / "Scenes" / "Main.aegscene", remove_error);
        std::filesystem::remove_all(project_root, remove_error);
        return 17;
    }
    const auto default_project_scene = runtime.scene_path();
    const auto missing_recent_scene = project_root / "Scenes" / "MissingRecent.aegscene";
    if (!require(runtime.save_scene_as(missing_recent_scene),
                 "editor scene smoke alternate recent scene save failed")) {
        std::filesystem::remove_all(project_root, remove_error);
        return 17;
    }
    std::filesystem::remove(missing_recent_scene, remove_error);
    if (!require(!remove_error, "editor scene smoke alternate recent scene removal failed") ||
        !require(runtime.import_project(project_root),
                 "editor scene smoke missing recent scene fallback import failed") ||
        !require(runtime.scene_path() == default_project_scene,
                 "editor scene smoke import did not fall back to the default scene") ||
        !require(runtime.entities().size() == 1 && runtime.entities().front().name == "Project Entity",
                 "editor scene smoke default scene fallback lost project content") ||
        !require(runtime.new_scene(), "editor scene smoke project New Scene failed") ||
        !require(runtime.entities().empty(), "editor scene smoke project New Scene restored editor sample entities")) {
        std::filesystem::remove_all(project_root, remove_error);
        return 17;
    }
    std::filesystem::remove(default_project_scene, remove_error);
    if (!require(!remove_error, "editor scene smoke default scene removal failed") ||
        !require(runtime.import_project(project_root), "editor scene smoke scene-less project import failed") ||
        !require(runtime.entities().empty(), "editor scene smoke scene-less project import restored sample entities") ||
        !require(std::ranges::none_of(runtime.resource_descriptors(),
                                     [](const auto& resource) { return resource.path.empty(); }),
                 "editor scene smoke scene-less project import exposed sample resources")) {
        std::filesystem::remove_all(project_root, remove_error);
        return 17;
    }
    if (!require(runtime.shutdown(), "editor scene smoke shutdown failed")) {
        std::filesystem::remove_all(project_root, remove_error);
        return 18;
    }
    aeg::application::EditorSceneRuntime imported_runtime;
    if (!require(imported_runtime.initialize(*device), "editor scene smoke imported runtime initialization failed") ||
        !require(imported_runtime.import_project(project_root), "editor scene smoke fresh project import failed") ||
        !has_default_preview_definition(imported_runtime, "Fresh project import lost its actual default Renderer Feature") ||
        !require(imported_runtime.shutdown(), "editor scene smoke imported runtime shutdown failed")) {
        std::filesystem::remove_all(project_root, remove_error);
        return 18;
    }
    if (argc != 3 || !feature_session(*device, argv[1])) return 19;
    if (const int saved = saved_startup_feature_session(*device, argv[1], argv[2]); saved != 0) return saved;
    std::filesystem::remove_all(project_root, remove_error);
    std::cout << "Editor scene runtime smoke passed\n";
    return 0;
}
