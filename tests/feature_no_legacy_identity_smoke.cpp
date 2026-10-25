/**
 * @file feature_no_legacy_identity_smoke.cpp
 * @brief Static boundary checks for the FeatureGraph-only authoring surface.
 */
#include <cassert>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <string>

namespace {
bool contains(const std::filesystem::path& path, std::string_view needle) {
    std::ifstream input(path);
    if (!input) {
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return text.find(needle) != std::string::npos;
}

bool contains_between(const std::filesystem::path& path, std::string_view begin, std::string_view end,
                      std::string_view needle) {
    std::ifstream input(path);
    if (!input) {
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const std::size_t begin_offset = text.find(begin);
    if (begin_offset == std::string::npos) {
        return false;
    }
    const std::size_t end_offset = text.find(end, begin_offset + begin.size());
    if (end_offset == std::string::npos || end_offset <= begin_offset) {
        return false;
    }
    return text.substr(begin_offset, end_offset - begin_offset).find(needle) != std::string::npos;
}

bool removal_boundary(const std::filesystem::path& path,
                      std::initializer_list<std::string_view> forbidden,
                      std::initializer_list<std::string_view> required = {}) {
    // These implementation files must survive this incremental removal. A
    // missing/unreadable source cannot count as proof that a symbol is gone.
    if (!std::filesystem::exists(path)) {
        std::cerr << "Removal preflight required source is missing: " << path.string() << '\n';
        return false;
    }
    std::ifstream input(path);
    if (!input) {
        std::cerr << "Removal preflight could not read source: " << path.string() << '\n';
        return false;
    }
    const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    bool valid = true;
    for (const auto symbol : forbidden) {
        if (source.find(symbol) == std::string::npos) continue;
        std::cerr << "Removal preflight retained authoring surface: " << path.string() << " -> " << symbol << '\n';
        valid = false;
    }
    for (const auto symbol : required) {
        if (source.find(symbol) != std::string::npos) continue;
        std::cerr << "Removal preflight missing neutral contract: " << path.string() << " -> " << symbol << '\n';
        valid = false;
    }
    return valid;
}
}

int main() {
    // Required live consumers must be readable; retired authoring files must
    // actually be absent. Missing sources cannot make this boundary green.
    bool removal_preflight_valid = true;
    // Application consumers use public contracts, never relative imports of
    // another module's execution or admission implementation.
    if (!removal_boundary("Application/Editor/private/editor_application.cpp",
                          {"Engine/Pipeline/private/pipeline_scope_host_execution.hpp"}) ||
        !removal_boundary("Application/Editor/private/editor_scene_runtime.cpp",
                          {"Engine/Shader/private/shader_pipeline_layout_access.hpp",
                           "Engine/Feature/private/feature_clock_admission.hpp",
                           "ShaderPipelineLayoutAccess", "feature::detail::make_clock_host_context"}) ||
        !removal_boundary("Application/Editor/private/editor_renderer_feature_host.cpp",
                          {"Engine/Feature/private/feature_clock_admission.hpp",
                           "feature::detail::make_clock_host_context"}) ||
        !removal_boundary("Application/Editor/private/composition/editor_render_graph_inspection.cpp",
                          {"Engine/RenderGraph/private/render_graph_execution.hpp", "RenderGraphExecution::"}) ||
        !removal_boundary("Application/Tooling/private/project_cooker.cpp",
                          {"Engine/Feature/private/feature_clock_admission.hpp",
                           "feature::detail::make_clock_host_context", "detail::admit_clock_request"}))
        removal_preflight_valid = false;
    for (const auto path : {"tests/renderer_smoke.cpp", "tests/runtime_component_orchestrator_smoke.cpp"})
        if (!removal_boundary(path,
            {": public aeg::pipeline::RenderFeature", "PipelineFeatureDeclaration", "registry.register_feature("},
            {"RenderSceneSnapshot", "renderer"}))
            removal_preflight_valid = false;
    if (!removal_boundary("Engine/Pipeline/private/pipeline.hpp",
        {"PipelineTaskContext", "TaskDescriptor", "TaskExecutionFunction", "PipelineTaskHandle", "add_task(",
         "bind_task(", "PipelineSchedulerDesc", "CancellationToken", "set_executor(", "worker_count(", "concurrency.hpp"},
        {"visited_nodes()", "PipelineExecutionContext", "ArtifactScopeBridge", "PipelineScopeHost"}))
        removal_preflight_valid = false;
    if (!removal_boundary("Engine/Pipeline/private/pipeline_types.hpp",
        {"TaskDescriptor", "TaskExecutionFunction", "TaskFlags", "TaskId", "PipelineTaskRecord", "synthetic", "user_data"},
        {"struct PipelineNode", "StageId stage", "artifact_bindings", "definition_stage_index"}))
        removal_preflight_valid = false;
    if (!removal_boundary("Engine/Pipeline/private/pipeline.cpp",
        {"PipelineTaskContext", "TaskDescriptor", "add_task(", "bind_task(", "execute_node(", "node.execute",
         "node.user_data", "CancellationToken", "WorkerExecutor", "TaskGroup"},
        {"visited_nodes_", "PipelineScheduler::execute_internal", "ArtifactScopeBridge::read"}))
        removal_preflight_valid = false;
    for (const auto source : {"Engine/Pipeline/private/pipeline.hpp", "Engine/Pipeline/private/pipeline.cpp"}) {
        if (!removal_boundary(source,
            {"class Pipeline {", "PipelineFactory", "FeatureFactory", "FeatureBuildContext", "DomainGraphBuilder",
             "LogicFeature", "PhysicsFeature", "AudioFeature", "StreamFeature", "CompositeFeature", "collect_contributions",
             "PipelineFeatureDeclaration", "register_feature(", "bind_feature(", "find_feature_", "add_feature_point("},
            {"register_pipeline(PipelineDefinition definition)"}))
            removal_preflight_valid = false;
    }
    // These are structural guards beside the actual native execution and
    // geometry provenance tests. A missing source cannot prove retirement.
    if (!removal_boundary("Application/EditorFramework/public/imgui_render_execution.hpp",
                          {"class ImGuiRenderFeature", "register_imgui_render_feature", "unregister_imgui_render_feature",
                           "pipeline.hpp", "PipelineRegistry", "FeatureId"},
                          {"struct ImGuiRenderExecution", "render_graph::RenderGraph&", "release_prepared_frames"}) ||
        !removal_boundary("Application/EditorFramework/private/imgui/imgui_renderer.cpp",
                          {"ImGuiRenderFeature::", "register_imgui_render_feature", "unregister_imgui_render_feature",
                           "PipelineFeatureDeclaration", "kImGuiRenderFeatureId", "kImGuiFeaturePointId"},
                          {"NativeGraphBuildSession::begin", "ImGuiRenderExecution::build_render_graph",
                           "ImGuiRenderExecution::release_prepared_frames", "retain_until_complete(frame_owner)"}) ||
        !removal_boundary("Application/Editor/private/editor_application.cpp",
                          {"register_imgui_render_feature", "unregister_imgui_render_feature"},
                          {"ImGuiRenderExecution::build_render_graph", "release_shared_ui_texture_frames",
                           "ImGuiRenderExecution::release_prepared_frames("}))
        removal_preflight_valid = false;
    if (!removal_boundary("Engine/Renderer/private/render_pipeline_variants.cpp",
                          {"class VariantFeature", "PipelineFeatureDeclaration", "register_deferred_render_feature",
                           "register_forward_render_feature", "bind_deferred_render_feature", "bind_forward_render_feature"},
                          {"register_builtin_rendering", "register_deferred_render_pipeline", "register_forward_render_pipeline"}) ||
        !removal_boundary("Engine/Renderer/private/renderer_variants_internal.hpp",
                          {"kDeferredRenderFeatureId", "kForwardRenderFeatureId", "register_deferred_render_feature",
                           "register_forward_render_feature", "bind_deferred_render_feature", "bind_forward_render_feature"},
                          {"kDeferredRenderPipelineId", "kForwardRenderPipelineId", "register_builtin_rendering"}) ||
        !removal_boundary("Engine/Renderer/private/native_pass_sequence.hpp",
                          {"FeatureId", "FeaturePointId", "FeaturePartId", "PipelineInstanceConfiguration"},
                          {"struct NativePassStep", "struct NativePassSequence", "ordered_after"}) ||
        !removal_boundary("Engine/Renderer/private/render_pipeline_graph.cpp",
                          {"authored_feature_configuration", "FeatureInstanceConfiguration"},
                          {"context.native_sequence", "validate_native_sequence", "apply_native_ordering"}))
        removal_preflight_valid = false;
    if (!removal_boundary("Engine/Renderer/private/opaque_render_feature.cpp",
                          {"OpaqueRenderFeature feature;", "class OpaqueRenderFeature", "register_opaque_render_feature",
                           "bind_opaque_render_feature", "build_render_graph_with_options", "RenderPassTypeId type(pass_name)"},
                          {"foundation::Result build_native_opaque_draw(", "return build_native_opaque_draw(builder, context,",
                           "context.registered_native", "source->depth_projection"}))
        removal_preflight_valid = false;
    if (!contains_between("Engine/Renderer/private/opaque_render_feature.cpp",
                          "foundation::Result build_opaque_render_graph(",
                          "foundation::Result build_gbuffer_render_graph(",
                          "return build_native_opaque_draw(builder, context,"))
        removal_preflight_valid = false;
    if (!removal_boundary("Engine/Pipeline/private/pipeline.hpp",
                          {"RenderGraphBuilder::configuration()", "feature_configuration()", "feature_data()",
                           "RenderGraphFeatureContext", "FeaturePointFlags"},
                          {"RenderGraphBuildContext", "RenderGraphBuilder::context_data()",
                           "return build_context_.context_data;", "RenderGraphBuilder::build_flags()"}) ||
        contains_between("Engine/Pipeline/private/pipeline.hpp", "class RenderGraphBuilder final",
                         "class RenderPassBuilder final", " configuration() const"))
        removal_preflight_valid = false;
    if (!removal_boundary("Engine/Renderer/private/native_scene_pass_source.hpp", {},
                          {"struct NativeScenePassInspection", "ResultValue<render_graph::RenderSceneOperationSource>",
                           "const pipeline::RenderPassConfiguration&"}) ||
        !removal_boundary("Engine/Renderer/private/builtin_render_passes.cpp", {},
                          {"NativeScenePassInspection::source", "context.operation_source",
                           "commands->operation = *context.operation_source", "prepared_identity_matches"}) ||
        !removal_boundary("Engine/RenderGraph/private/render_scene_lowerer.hpp", {},
                          {"enum class RenderSceneOperationKind", "struct RenderSceneOperationSource", "RenderWorkItem work",
                           "feature::RenderResourceOwner owner", "RenderDepthSceneProjection depth_projection"}) ||
        !removal_boundary("Engine/Renderer/private/render_feature_context.hpp", {},
                          {"bool registered_native", "const render_graph::RenderSceneOperationSource* operation_source"}) ||
        !removal_boundary("Engine/RenderGraph/private/render_feature_runtime.cpp", {},
                          {"RenderSceneOperationSource source{work, output.resource_owner, output.view_key}",
                           "sourced_targets.source = &source;", "attribute_feature_passes_since"}))
        removal_preflight_valid = false;
    if (!removal_boundary("Engine/RenderGraph/private/render_graph_execution.hpp", {},
                          {"struct RenderGraphPassInspection", "std::string name", "inspect_pass("}) ||
        !removal_boundary("Engine/RenderGraph/private/render_graph.cpp", {},
                          {"RenderGraphExecution::inspect_pass", "graph.impl_->get_pass(pass)", ".name = record->name"}))
        removal_preflight_valid = false;
    for (const auto path : {"Engine/Renderer/public/native_scene_pass_source.hpp",
                           "Engine/RenderGraph/public/render_scene_lowerer.hpp",
                           "Engine/RenderGraph/public/render_graph_execution.hpp"}) {
        if (!std::filesystem::exists(path)) continue;
        std::cerr << "Native Scene inspection header must remain private: " << path << '\n';
        removal_preflight_valid = false;
    }
    for (const auto root : {"Engine", "Application"}) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".hpp") continue;
            bool public_header = false;
            for (const auto& component : entry.path())
                if (component == "public") public_header = true;
            if (public_header && !removal_boundary(entry.path(),
                    {"RenderSceneOperationSource", "RenderSceneOperationKind", "NativeScenePassInspection",
                     "native_scene_pass_source.hpp", "RenderGraphPassInspection", "registered_native", "operation_source"}))
                removal_preflight_valid = false;
        }
    }
    if (!removal_boundary("Application/Runtime/private/runtime_bootstrap.cpp", {},
                          {"pass_host = render_graph::RenderPassExecutionHost{", "execution_epoch(*impl_->passes)",
                           "*history, 0, pass_host", "scene->view.id.value(), pass_host"}) ||
        !removal_boundary("Application/Editor/private/editor_scene_runtime.cpp", {},
                          {"RenderPassExecutionHost pass_host{&impl->scene_passes", "execution_epoch(impl->scene_passes)",
                           "history, 0, pass_host", "scene->view.id.value(), pass_host"}) ||
        !removal_boundary("Engine/Renderer/private/render_feature_draw.cpp", {},
                          {"pass_host ? RenderFeatureDrawLowerer(scenes, *pass_host) : RenderFeatureDrawLowerer(scenes)",
                           "registered_native_ ? targets.source : nullptr"}))
        removal_preflight_valid = false;
    if (!removal_boundary("Engine/Runtime/private/engine_frame_inspection.hpp",
        {"std::function", "execute", "callback;"}, {"EngineFrameStageObservation", "WorldSnapshot", "ArtifactView"}))
        removal_preflight_valid = false;
    for (const auto retired : {"Engine/RenderGraph/public/render_state_flow.hpp",
        "Engine/RenderGraph/private/render_state_flow.hpp", "Engine/RenderGraph/private/render_state_flow.cpp",
        "Engine/RenderGraph/private/render_state_flow_pipeline.hpp",
        "Engine/RenderGraph/private/render_feature_state_composer.hpp",
        "Engine/RenderGraph/private/render_feature_runtime_pipeline.hpp",
        "Engine/RenderGraph/private/render_graph_pass_host.hpp",
        "Engine/RenderGraph/private/render_feature_flow.hpp", "Engine/Pipeline/private/pipeline_configuration.hpp"})
        if (std::filesystem::exists(retired)) removal_preflight_valid = false;
    if (!removal_boundary("Engine/RenderGraph/private/render_feature_runtime.cpp",
        {"RenderGraphFeatureRuntime::", "PipelineFeature", "set_feature_owner", "build_render_state_flow",
         "expand_authored_render_passes", "render_state_output_id", "build_render_graph_pipeline_features"},
        {"RenderWorkItem", "feature_work", "NativeGraphBuildSession::begin", "retain_execution_owner",
         "foundation::Result build_render_graph_features(", "RenderFeatureProjector{}.project(*outputs, frame_index)"}))
        removal_preflight_valid = false;
    if (!removal_boundary("Engine/Pipeline/private/pipeline.hpp",
        {"class PipelineFeature", "class RenderFeature", "find_feature(FeatureId", "RenderStateBlendMode",
         "state_input()", "state_inputs()", "state_output()", "state_layer_count()", "PipelineInstanceConfiguration",
         "FeatureInstanceConfiguration", "FeaturePointDescriptor", "set_configuration("},
        {"class RenderGraphBuilder final", "class PipelineRegistry final", "RenderGraphBuildFlags"}) ||
        !removal_boundary("Engine/Pipeline/private/pipeline_types.hpp",
        {"class PipelineFeature", "PipelineFeatureImplementationRecord", "feature_implementations", "implementation{}",
         "PipelineFeatureDeclaration", "FeaturePoint", "FeatureBinding", "feature_contributions", "feature_owners",
         "FeatureIdTag", "FeatureInstanceIdTag", "FeaturePartIdTag", "FeaturePriority", "FeatureDomain",
         "kMayPublishSnapshot", "kMayCreateArtifacts"},
        {"struct PipelineDefinition", "definition_owner", "RenderGraphBuildFlags", "kMayCreateResources"}) ||
        !removal_boundary("Engine/Pipeline/private/pipeline.cpp",
        {"find_feature(FeatureId", "feature_implementations", "declaration.implementation", "feature_contributions",
         "feature_owners", "PipelineInstanceConfiguration", "FeaturePoint", "configuration_"},
        {"plan.definition_owner = registration.value.definition", "PipelineScopeHost::rebuild"}) ||
        !removal_boundary("Engine/RenderGraph/public/render_feature_runtime.hpp",
        {"RenderGraphFeatureBufferBinding", "RenderGraphFeatureStateDesc", "compose_terminal_states",
         "user_data", "feature_data", "configuration{}"},
        {"RenderGraphFeatureTextureBinding", "build_render_graph_features", "draw_lowerer", "current_frame_owner", "history_key"}) ||
        !removal_boundary("Engine/Runtime/private/engine_frame_orchestrator.cpp",
        {"build_render_graph_pipeline_features", "RenderGraphPassHost", "Impl::build_render_features("},
        {"render_feature_post_build", "render_feature_preparation"}))
        removal_preflight_valid = false;
    for (const auto path : {"pipeline.hpp", "pipeline_types.hpp", "pipeline_configuration.hpp",
                           "pipeline_builder_backend.hpp", "pipeline_execution.hpp", "render_pass.hpp", "render_program.hpp"}) {
        const auto retired = std::filesystem::path("Engine/Pipeline/public") / path;
        if (std::filesystem::exists(retired)) {
            std::cerr << "Retired public execution header still exists: " << retired << '\n';
            removal_preflight_valid = false;
        }
    }
    for (const auto path : {"pipeline.hpp", "pipeline_types.hpp", "render_pass.hpp", "render_program.hpp"})
        if (!removal_boundary(std::filesystem::path("Engine/Pipeline/private") / path,
                              {"PipelineDeclarationBackend", "FeatureDeclarationBackend", "PipelineBuilderBackend"}))
            removal_preflight_valid = false;
    if (!removal_boundary("Engine/Runtime/public/engine_frame_orchestrator.hpp",
                          {"pipeline::", "pipeline.hpp", "PinnedPipeline", "ArtifactScopeBridge", "PipelinePlanHandle"},
                          {"struct Impl;", "std::unique_ptr<Impl>", "feature::RenderResourceId"}))
        removal_preflight_valid = false;
    for (const auto path : {"Engine/Renderer/public/renderer.hpp", "Engine/Renderer/public/renderer_material_resource_contract.hpp",
                           "Engine/Renderer/public/renderer_profile_resource_contract.hpp", "Engine/RenderGraph/public/render_feature_runtime.hpp"})
        if (!removal_boundary(path, {"pipeline::", "pipeline.hpp", "pipeline_types.hpp", "RenderStateLinkOverride", "RenderStateBlendOverride"}))
            removal_preflight_valid = false;
    for (const auto path : {"Application/Authoring/public/pipeline_document_io.hpp",
                            "Application/Authoring/private/pipeline_document_io.cpp",
                            "Engine/Pipeline/public/pipeline_authoring_types.hpp",
                            "Engine/Pipeline/public/pipeline_document.hpp",
                            "Engine/Pipeline/public/pipeline_authoring.hpp",
                            "Engine/Pipeline/private/pipeline_authoring.cpp",
                            "Engine/Pipeline/private/pipeline_document.cpp",
                            "Engine/Pipeline/private/flowgraph_input.cpp",
                            "tests/pipeline_document_validation_smoke.cpp",
                            "DemoProject/project.aegproject"}) {
        if (!std::filesystem::exists(path)) continue;
        std::cerr << "Retired global author/project file still exists: " << path << '\n';
        removal_preflight_valid = false;
    }
    if (!removal_boundary("CMakeLists.txt", {"pipeline_document_io", "pipeline_authoring.cpp",
        "pipeline_document.cpp", "flowgraph_input.cpp", "aeg_pipeline_document_validation_smoke",
        "AEG_EDITOR_ENABLE_BUILTIN_PIPELINE_DEBUG"}))
        removal_preflight_valid = false;
    if (!removal_boundary("Application/Editor/private/editor_ui_interaction.hpp",
        {"PipelineOrderingDraft", "PipelineNodeIdentity", "PipelineSavedOrdering", "PipelineLayoutSaveState",
         "PipelineSavedBlend", "required_pipeline_present_consumer"}, {"graph_link_hit", "GraphPanGesture"}))
        removal_preflight_valid = false;
    if (!removal_boundary("Application/Editor/private/editor_project.cpp",
        {"kLegacyProjectHeader", "kProjectHeader", "write_project_file", "project.aegproject"},
        {"ProjectFileCodec", "discover_project_file", "project_file_"})) removal_preflight_valid = false;
    if (!removal_boundary("Application/Project/public/project_context.hpp", {"bool legacy"}))
        removal_preflight_valid = false;
    if (!removal_boundary("Application/Project/private/project_context.cpp",
        {"format_version >= 2", "format_version == 1"}, {"Project.aegproj", "validate_source_format"}))
        removal_preflight_valid = false;
    if (!removal_boundary("Engine/World/public/world_scope.hpp",
        {"pipeline.hpp", "PipelineRegistry", "PipelineScopeHost", "WorldScopeDesc", "ArtifactScopeBridge",
         "pipeline_host", "bind_pipeline", "has_pipeline", "is_pipeline_active"},
        {"explicit WorldScope", "ResultValue<ecs::WorldSnapshot>", "create_frame_scope"}))
        removal_preflight_valid = false;
    if (!removal_boundary("Engine/Runtime/private/engine_frame_orchestrator.cpp",
        {"world_scope.pipeline_host", "world_scope->pipeline_host", "find_world_for_host"},
        {"Raw World Pipeline execution is retired", "apply_world_commands", "commit_worlds"}))
        removal_preflight_valid = false;
    if (!removal_boundary("Engine/World/private/world_snapshot_execution.hpp", {},
        {"WorldSnapshotExecution", "ArtifactScopeBridge", "kWorldSnapshotArtifactId"}))
        removal_preflight_valid = false;
    if (!removal_boundary("Application/Runtime/private/runtime_bootstrap.cpp",
        {"pipeline_hosts", "scope.pipeline_host", "WorldScopeDesc"}, {"scope.apply_commands", "scope.commit"}))
        removal_preflight_valid = false;
    for (const auto path : {"Engine/Renderer/public/renderer_variants.hpp",
                            "Engine/Renderer/private/renderer_authoring.cpp",
                            "tests/renderer_variant_document_fixture.hpp",
                            "DemoProject/Assets/Pipelines/Forward.aegpipe",
                            "DemoProject/Assets/Pipelines/Deferred.aegpipe",
                            "DemoProject/Scenes/Main.aegscene.pipeline",
                            "DemoProject/Scenes/RenderingShowcase.aegscene.pipeline"}) {
        if (!std::filesystem::exists(path)) continue;
        std::cerr << "Retired renderer authoring file still exists: " << path << '\n';
        removal_preflight_valid = false;
    }
    if (!removal_boundary("CMakeLists.txt", {"renderer_authoring.cpp"})) removal_preflight_valid = false;
    if (!removal_boundary("Application/Editor/private/editor_project_feature_authoring.hpp",
        {"EditorProjectFrameProjection", "frame_catalog", "frame_projections", "PipelineDocument", "pipeline_authoring.hpp"},
        {"feature::FeatureCatalog", "feature::FeatureGraphDocument"})) removal_preflight_valid = false;
    if (!removal_boundary("Application/Editor/private/editor_project_feature_authoring.cpp",
        {"register_renderer_authoring_types", "load_pipeline_document", "frame_projections", "pipeline_document_io.hpp"},
        {"load_feature_graph_document"})) removal_preflight_valid = false;
    if (!removal_boundary("Application/Editor/private/editor_scene_runtime.hpp",
        {"activate_frame_projection", "active_frame_projection", "authored_frame_projections", "create_project_frame_renderer",
         "add_render_state_link", "set_render_state_blend", "add_pipeline_runtime_ordering", "pipeline_snapshots", "PipelineTarget"},
        {"publish_renderer_feature_graph", "inspect_renderer_feature_execution"})) removal_preflight_valid = false;
    if (!removal_boundary("Application/Editor/private/editor_scene_runtime.cpp",
        {"PipelineDocument", "prepare_pipeline_document", "switch_renderer", "frame_host", "feature_preview_enabled",
         "render_state_link_overrides", "render_blend_overrides"},
        {"preview_renderer", "capture_render_outputs"})) removal_preflight_valid = false;
    if (!removal_boundary("Application/Editor/private/editor_scene_runtime.cpp",
        {"advance_runtime_demo", "runtime_demo_time"},
        {"FeatureWorldCommandConsumer", "feature_world_bindings"})) removal_preflight_valid = false;
    if (!removal_boundary("Application/Editor/private/editor_application.cpp",
        {"Apply to Viewport", "activate_frame_projection", "authored_frame_projections", "AEG_PIPELINE_EDITS",
         "create_project_frame_renderer", "renderer_variants.hpp", "Startup Renderer", "project_hub_pipeline_index_",
         "PipelineOrderingDraftQueue", "save_scene_and_apply_pipeline_drafts", "pipeline_document_path"},
        {"Select FeatureGraph", "queue_feature_graph", "save_feature_graph_document",
         "inspect_renderer_feature_execution", "inspect_render_graph"})) removal_preflight_valid = false;
    if (!removal_boundary("Application/Project/public/project_context.hpp", {"startup_frame_projections"},
        {"startup_feature_graphs"})) removal_preflight_valid = false;
    for (const auto path : {"DemoProject/Project.aegproj", "Application/Project/templates/game/Project.aegproj"})
        if (!removal_boundary(path, {"startup_pipelines", "startup_frame_projections"}, {"startup_feature_graphs"}))
            removal_preflight_valid = false;
    if (!contains("DemoProject/Project.aegproj", "id = \"com.aeg.demo\"") ||
        !contains("DemoProject/Project.aegproj", "startup_feature_graphs = [\"Assets/Features/Motion.aegfeature\"]")) {
        std::cerr << "Demo project does not identify its Feature module and saved Motion FeatureGraph root\n";
        removal_preflight_valid = false;
    }
    if (!std::filesystem::exists("DemoProject/Assets/Features/Motion.aegfeature") ||
        !std::filesystem::exists("DemoProject/Source/GameModule.cpp")) {
        std::cerr << "Demo Motion author graph or dynamic Feature module source is missing\n";
        removal_preflight_valid = false;
    }
    if (!removal_boundary("Application/Runtime/private/runtime_bootstrap.cpp",
                          {"pipeline::TypeCatalog", "register_renderer_authoring_types", "impl_->catalog"}))
        removal_preflight_valid = false;
    if (!removal_boundary("Engine/Pipeline/private/render_pass.hpp",
                          {"global_render_pass_registry", "StaticRenderPassRegistrar", "REGISTER_RENDER_PASS",
                           "pipeline_authoring_types.hpp"}))
        removal_preflight_valid = false;
    if (!removal_boundary("Engine/Pipeline/private/render_pass.cpp",
                          {"global_render_pass_registry", "StaticRenderPassRegistrar", "REGISTER_RENDER_PASS"}))
        removal_preflight_valid = false;
    if (!removal_boundary("Engine/Renderer/private/builtin_render_passes.cpp",
                          {"TypeCatalogEntry", "PassAuthoringDeclaration", "make_pass_entry",
                           "validate_pass_configuration"}))
        removal_preflight_valid = false;
    if (!removal_boundary("Engine/Renderer/private/render_pipeline_contract.hpp",
                          {"TypeCatalogEntry", "PassAuthoringDeclaration", "make_pass_entry",
                           "validate_pass_configuration", "pipeline_authoring.hpp"}))
        removal_preflight_valid = false;
    if (!removal_boundary("Engine/Pipeline/private/render_program.hpp",
                          {"pipeline_authoring_types.hpp", "pipeline_authoring.hpp"},
                          {"parameter_value.hpp", "pipeline_types.hpp", "foundation::AssetReference",
                           "foundation::ParameterBlock"}))
        removal_preflight_valid = false;
    if (!removal_boundary("Engine/Renderer/private/renderer_variants_internal.hpp",
                          {"pipeline_authoring.hpp", "pipeline_authoring_types.hpp"}))
        removal_preflight_valid = false;
    if (!removal_preflight_valid) return 1;

    assert(!std::filesystem::exists("Engine/Module/abi/module_pipeline_api.h"));
    assert(!std::filesystem::exists("Engine/Asset/public/pipeline_asset.hpp"));
    assert(!std::filesystem::exists("Engine/Asset/private/pipeline_asset.cpp"));
    assert(!std::filesystem::exists("Engine/Asset/schema/pipeline_asset.fbs"));
    assert(!std::filesystem::exists("Application/Tooling/public/pipeline_asset_importer.hpp"));
    assert(!std::filesystem::exists("Application/Tooling/public/pipeline_asset_encoder.hpp"));
    assert(std::filesystem::exists("Engine/Module/abi/feature_module_api.h"));
    assert(!contains("Engine/Module/abi/module_api.h", "AEG_CAP_PIPELINE_DECLARATION"));
    assert(!contains("Engine/Module/abi/module_api.h", "declaration"));
    assert(!contains("Engine/Module/abi/module_api.h", "task"));
    assert(contains("Engine/Module/abi/module_api.h", "AEG_CAP_FEATURES"));
    assert(!std::filesystem::exists("Engine/Asset/public/asset_identity.hpp"));
    assert(std::filesystem::exists("Engine/Foundation/public/asset_identity.hpp"));
    assert(contains("Engine/Foundation/public/asset_identity.hpp", "kFeatureGraph"));
    assert(std::filesystem::exists("Application/Project/templates/game/Assets/Features/Default.aegfeature"));
    assert(std::filesystem::exists("Application/Project/templates/game/Assets/Features/Default.aegfeature.meta"));
    assert(!std::filesystem::exists("Application/Project/templates/game/Assets/Pipelines/Default.aegpipe"));
    // Editor's ImGui owner is public; its Pipeline Feature and registry assembly
    // belong to the private frame adapter, not the authoring include surface.
    const auto imgui_public = "Application/EditorFramework/public/imgui_renderer.hpp";
    assert(!contains(imgui_public, "#include \"pipeline.hpp\""));
    assert(!contains(imgui_public, "pipeline::"));
    assert(!contains(imgui_public, "class ImGuiRenderFeature"));
    assert(!contains(imgui_public, "register_imgui_render_feature("));
    assert(!contains(imgui_public, "unregister_imgui_render_feature("));
    assert(contains(imgui_public, "feature::RenderResourceId"));
    assert(std::filesystem::exists("Application/EditorFramework/public/imgui_render_execution.hpp"));
    // The main FlowGraph tab must not fall back to the obsolete editable
    // PipelineDocument authoring model when no FeatureGraph is present.
    assert(!contains("Application/Editor/private/editor_application.cpp", "EditorCompositionDocument*"));
    assert(!contains("Application/Editor/private/editor_application.cpp", "save_editor_composition"));
    assert(contains("Application/Editor/private/editor_project_feature_authoring.hpp",
                    "EditorProjectFeatureAuthoring"));
    assert(!std::filesystem::exists("Application/Editor/private/editor_project_pipeline_authoring.hpp"));
    assert(!std::filesystem::exists("Application/Editor/private/editor_project_pipeline_authoring.cpp"));
    // Malformed authored FeatureGraph files remain visible in the project
    // authoring catalog and must surface their source path/diagnostic in the
    // main FlowGraph panel instead of disappearing behind an empty state.
    assert(contains("Application/Editor/private/editor_application.cpp", "FeatureGraph diagnostics"));
    assert(contains("Application/Editor/private/editor_application.cpp", "load_result.message"));
    // Project Settings must expose startup FeatureGraph authoring identity;
    // the old Pipeline label/field is no longer a valid editor entry point.
    assert(!contains("Application/Editor/private/editor_application.cpp", "Startup Pipeline"));
    assert(!contains("Application/Editor/private/editor_application.cpp", "project_settings_pipeline_"));
    // Project settings must preserve the ordered startup FeatureGraph list;
    // reducing the vector to one text field silently drops additional roots.
    assert(contains("Application/Editor/private/editor_application.cpp",
                    "project_settings_feature_graphs_"));
    assert(contains("Application/Editor/private/editor_application.cpp",
                    "Add Startup FeatureGraph"));
    assert(contains("Application/Editor/private/editor_application.cpp",
                    "for (const auto& path : project_settings_draft_.startup_feature_graphs)"));
    assert(contains("Application/Editor/private/editor_application.cpp",
                    "for (const auto& buffer : project_settings_feature_graphs_)"));
    assert(!contains("Application/Editor/private/editor_application.cpp",
                     "startup_feature_graphs.front()"));
    assert(!contains("Application/Editor/private/editor_application.cpp",
                     "project_settings_feature_graph_"));
    // Multiple valid authored graphs must be selectable from the FlowGraph
    // workspace instead of silently editing only the first scanned document.
    assert(contains("Application/Editor/private/editor_application.cpp",
                    "Select FeatureGraph"));
    assert(contains("Application/Editor/private/editor_application.cpp",
                    "select_feature_graph"));
    assert(!contains("Application/Editor/private/editor_application.cpp",
                     "selected_feature = std::find_if"));
    assert(!contains("Application/Editor/private/editor_scene_runtime.hpp",
                     "set_renderer_projection_present_connection"));
    assert(!contains("Application/Editor/private/editor_scene_runtime.hpp",
                     "set_authored_present_connection"));
    assert(!contains("Application/Editor/private/editor_scene_runtime.hpp",
                     "authored_frame_catalog"));
    for (const auto stem : {"editor_composition_document", "editor_composition_catalog",
                            "editor_composition_view", "editor_composition_commands",
                            "editor_composition_storage", "editor_composition_operations",
                            "editor_composition_preview", "editor_flowgraph_panel"}) {
        const auto base = std::filesystem::path{"Application/Editor/private/composition"} / stem;
        assert(!std::filesystem::exists(base.string() + ".hpp"));
        assert(!std::filesystem::exists(base.string() + ".cpp"));
    }
    assert(!contains("CMakeLists.txt", "aeg_editor_composition_model"));
    assert(contains("CMakeLists.txt", "aeg_editor_feature_model"));
    // Native registry assembly and identities are Renderer-private technical facilities.
    assert(std::filesystem::exists("Engine/Renderer/private/renderer_variants_internal.hpp"));
    assert(contains("Engine/Renderer/private/renderer_variants_internal.hpp", "register_builtin_rendering"));
    assert(contains("Engine/Renderer/private/renderer_variants_internal.hpp", "kShadowPassTypeId"));
    assert(contains("Engine/Renderer/private/renderer_variants_internal.hpp", "kForwardOpaquePassTypeId"));
    // RenderFeatureDrawLowerer is the Renderer-owned adapter from neutral
    // Feature intents to native RenderGraph builders. It is consumed only by
    // Engine Runtime/Editor integration and focused Renderer smokes, so it
    // must not remain in the public Renderer include surface.
    assert(!std::filesystem::exists("Engine/Renderer/public/render_feature_draw.hpp"));
    assert(std::filesystem::exists("Engine/Renderer/private/render_feature_draw.hpp"));
    // The old Opaque registration adapter is retired. Modern Scene Feature
    // intents reach the fixed private recorder through the actual projector.
    assert(!std::filesystem::exists("Engine/Renderer/public/opaque_render_feature.hpp"));
    assert(!std::filesystem::exists("Engine/Renderer/private/opaque_render_feature.hpp"));
    // Standard RenderPipeline registration is a Renderer-internal assembly
    // seam; the public renderer scene contract must not expose it.
    assert(!contains("Engine/Renderer/public/renderer.hpp", "register_render_pipeline"));
    assert(std::filesystem::exists("Engine/Renderer/private/renderer_variants_internal.hpp"));
    assert(contains("Engine/Renderer/private/renderer_variants_internal.hpp",
                    "register_render_pipeline"));
    // Renderer collects fixed owning Stage/Artifact values. The Registry must
    // not call arbitrary factories, build methods or structural collectors.
    assert(!contains("Engine/Renderer/public/renderer.hpp", "class RenderPipeline final"));
    assert(std::filesystem::exists("Engine/Renderer/private/renderer_pipeline.hpp"));
    assert(contains("Engine/Renderer/private/renderer_pipeline.hpp", "make_render_pipeline_definition"));
    assert(!contains("Engine/Renderer/private/renderer_pipeline.hpp", "class RenderPipeline final"));
    for (const auto source : {"Engine/Pipeline/private/pipeline.hpp", "Engine/Pipeline/private/pipeline.cpp"}) {
        assert(std::filesystem::exists(source));
        assert(contains(source, "register_pipeline(PipelineDefinition definition)"));
        assert(!contains(source, "register_feature(PipelineFeatureDeclaration declaration)"));
        for (const auto retired : {"class Pipeline {", "PipelineFactory", "FeatureFactory", "FeatureBuildContext",
                                  "DomainGraphBuilder", "LogicFeature", "PhysicsFeature", "AudioFeature",
                                  "StreamFeature", "CompositeFeature", "collect_contributions"}) {
            assert(!contains(source, retired));
        }
    }
    assert(!contains("Engine/Pipeline/private/pipeline_types.hpp", "struct PipelineFeatureDeclaration"));
    assert(contains("Engine/Pipeline/private/pipeline_types.hpp", "std::shared_ptr<const PipelineDefinition> definition_owner"));
    assert(contains("Engine/Pipeline/private/pipeline.cpp", "plan.definition_owner = registration.value.definition"));
    // Renderer profile resource owners and graph assembly helpers are engine
    // implementation seams. Stable shadow view/value contracts remain in
    // renderer.hpp for RenderResourceResolver, while profile helpers stay
    // behind the Renderer private include surface.
    for (const auto stem : {"clustered_forward", "deferred_lighting_resources", "light_culling", "post_process",
                            "shadow_rendering"}) {
        assert(!std::filesystem::exists(std::filesystem::path{"Engine/Renderer/public"} / (std::string{stem} + ".hpp")));
        assert(std::filesystem::exists(std::filesystem::path{"Engine/Renderer/private"} / (std::string{stem} + ".hpp")));
    }
    assert(contains("Engine/Renderer/public/renderer.hpp", "struct RenderShadowView final"));
    // Renderer profile PSO metadata is a stable host/value contract. Keep its
    // definitions in a dedicated public boundary while renderer.hpp aggregates
    // that boundary for the existing resolver ABI; backend shader/PSO factories
    // must not leak through the aggregate header.
    assert(std::filesystem::exists("Engine/Renderer/public/renderer_material_contract.hpp"));
    assert(std::filesystem::exists("Engine/Renderer/public/renderer_attachment_contract.hpp"));
    assert(contains("Engine/Renderer/public/renderer.hpp",
                    "renderer_material_contract.hpp"));
    assert(contains("Engine/Renderer/public/renderer_attachment_contract.hpp",
                    "struct RenderMaterialAttachmentContract final"));
    assert(contains("Engine/Renderer/public/renderer_material_contract.hpp",
                    "struct RenderMaterialPipelineVariant final"));
    assert(!contains("Engine/Renderer/public/renderer.hpp",
                     "struct RenderMaterialAttachmentContract final"));
    assert(!contains("Engine/Renderer/public/renderer.hpp",
                     "struct RenderMaterialPipelineVariant final"));
    assert(!contains("Engine/Renderer/public/renderer.hpp", "ShaderManager"));
    assert(!contains("Engine/Renderer/public/renderer.hpp", "GraphicsPipelineState"));
    assert(!contains("Engine/Renderer/public/renderer.hpp", "ShaderProgram"));
    assert(contains("Engine/Renderer/public/renderer_material_contract.hpp",
                    "struct RenderMaterialShaderContract final"));
    assert(contains("Engine/Renderer/public/renderer_material_contract.hpp", "layout_hash"));
    // ShaderPipelineLayout is a public value/API, but its ShaderManager owner
    // and storage mutation path are Shader-private implementation details.
    assert(!contains("Engine/Shader/public/shader_contract.hpp", "class ShaderManager;"));
    assert(!contains("Engine/Shader/public/shader_contract.hpp", "friend class ShaderManager;"));
    assert(std::filesystem::exists("Engine/Shader/private/shader_pipeline_layout_access.hpp"));
    assert(contains("Engine/Shader/private/shader_pipeline_layout_access.hpp",
                    "ShaderPipelineLayoutAccess"));
    assert(!contains_between("Engine/Shader/public/shader_contract.hpp", "class ShaderPipelineLayout",
                             "/** @brief Loads and validates a compiled `.aegshader` package.",
                             "cache_epoch() const noexcept"));
    assert(!contains_between("Engine/Shader/public/shader_contract.hpp", "class ShaderPipelineLayout",
                             "/** @brief Loads and validates a compiled `.aegshader` package.",
                             "contract_hash() const noexcept"));
    assert(contains("Engine/Shader/private/shader_pipeline_layout_access.hpp",
                    "static std::uint64_t cache_epoch("));
    assert(contains("Engine/Shader/private/shader_pipeline_layout_access.hpp",
                    "static std::uint64_t contract_hash("));
    // Shader-facing value ABI belongs to its own narrow Renderer contract.
    // Keep the aggregate scene/extraction header from becoming the shader ABI
    // owner while preserving the include for existing resolver consumers.
    assert(std::filesystem::exists("Engine/Renderer/public/renderer_shader_contract.hpp"));
    assert(contains("Engine/Renderer/public/renderer.hpp", "renderer_shader_contract.hpp"));
    for (const auto stem : {"RenderShadowConstants", "RenderMeshVertexLayout", "RenderObjectConstants",
                            "RenderLightingConstants"}) {
        assert(contains("Engine/Renderer/public/renderer_shader_contract.hpp",
                        std::string{"struct "} + stem + " final"));
        assert(!contains("Engine/Renderer/public/renderer.hpp",
                         std::string{"struct "} + stem + " final"));
    }
    // Per-frame Renderer profile resources are a host/value contract of their
    // own. Keep the profile-specific declarations out of renderer.hpp so the
    // aggregate scene/extraction header does not become the profile ABI.
    assert(std::filesystem::exists("Engine/Renderer/public/renderer_profile_resource_contract.hpp"));
    assert(contains("Engine/Renderer/public/renderer.hpp", "renderer_profile_resource_contract.hpp"));
    assert(contains("Engine/Renderer/public/renderer_profile_resource_contract.hpp",
                    "struct RenderDeferredLightingInputs final"));
    assert(contains("Engine/Renderer/public/renderer_profile_resource_contract.hpp",
                    "struct RenderDeferredLightingResource final"));
    assert(contains("Engine/Renderer/public/renderer_profile_resource_contract.hpp",
                    "struct RenderShadowResource final"));
    assert(contains("Engine/Renderer/public/renderer_profile_resource_contract.hpp",
                    "struct RenderLightCullingResource final"));
    assert(contains("Engine/Renderer/public/renderer_profile_resource_contract.hpp",
                    "struct RenderPostProcessResource final"));
    assert(std::filesystem::exists("Engine/Renderer/public/renderer_profile_contract.hpp"));
    assert(contains("Engine/Renderer/public/renderer_profile_resource_contract.hpp",
                    "renderer_profile_contract.hpp"));
    assert(contains("Engine/Renderer/public/renderer_profile_contract.hpp",
                    "struct RendererProfilePsoContract final"));
    // Light-culling frame provenance is a Renderer-private validation detail;
    // the public profile resource contract must carry only an opaque token.
    assert(!contains("Engine/Renderer/public/renderer_profile_resource_contract.hpp", "has_origin("));
    assert(!contains("Engine/Renderer/public/renderer_profile_resource_contract.hpp",
                     "friend class LightCullingResources"));
    assert(!contains("Engine/Renderer/public/renderer_profile_resource_contract.hpp", "struct Origin final"));
    assert(std::filesystem::exists("Engine/Renderer/private/light_culling_origin.hpp"));
    assert(contains("Engine/Renderer/private/light_culling_origin.hpp", "struct LightCullingOrigin final"));
    // Frame artifact publication is an Engine-owned bridge. Keep the
    // PipelineScopeHost seam out of the stable Renderer scene contract.
    assert(!contains("Engine/Renderer/public/renderer.hpp", "install_frame_contract"));
    assert(!contains("Engine/Renderer/public/renderer.hpp", "publish_frame_snapshot"));
    assert(std::filesystem::exists("Engine/Renderer/private/renderer_frame_contract.hpp"));
    assert(contains("Engine/Renderer/private/renderer_frame_contract.hpp",
                    "publish_frame_snapshot"));
    assert(!contains("Engine/Renderer/public/renderer.hpp",
                     "struct RenderDeferredLightingInputs final"));
    assert(!contains("Engine/Renderer/public/renderer.hpp",
                     "struct RenderDeferredLightingResource final"));
    assert(!contains("Engine/Renderer/public/renderer.hpp",
                     "struct RenderShadowResource final"));
    assert(!contains("Engine/Renderer/public/renderer.hpp",
                     "struct RenderLightCullingResource final"));
    assert(!contains("Engine/Renderer/public/renderer.hpp",
                     "struct RenderPostProcessResource final"));
    // Material GPU resources are a stable resolver value contract of their
    // own; keep the large material/PSO payload out of renderer.hpp's scene
    // aggregate and expose it through a narrow dedicated header.
    assert(std::filesystem::exists("Engine/Renderer/public/renderer_material_resource_contract.hpp"));
    assert(contains("Engine/Renderer/public/renderer_material_resource_contract.hpp",
                    "struct RenderMaterialResource final"));
    assert(contains("Engine/Renderer/public/renderer.hpp",
                    "renderer_material_resource_contract.hpp"));
    assert(!contains("Engine/Renderer/public/renderer.hpp",
                     "struct RenderMaterialResource final"));
    // RenderFeatureContext carries the engine-owned pass execution host. Keep
    // the complete context and its lookup helper out of the public Renderer
    // aggregate so public Feature authors cannot depend on registry internals.
    assert(!contains("Engine/Renderer/public/renderer.hpp",
                     "struct RenderFeatureContext final"));
    assert(!contains("Engine/Renderer/public/renderer.hpp",
                     "const pipeline::RenderPassRegistry* render_passes"));
    assert(!contains("Engine/Renderer/public/renderer.hpp",
                     "render_feature_context(const pipeline::RenderGraphBuilder"));
    assert(std::filesystem::exists("Engine/Renderer/private/render_feature_context.hpp"));
    assert(contains("Engine/Renderer/private/render_feature_context.hpp",
                    "struct RenderFeatureContext final"));
    assert(contains("Engine/Renderer/private/render_feature_context.hpp",
                    "render_graph::RenderPassExecutionHost pass_execution"));
    assert(contains("Engine/Renderer/private/render_feature_context.hpp",
                    "render_pass_execution_host.hpp"));
    assert(!contains("Engine/Renderer/private/render_feature_context.hpp",
                     "const pipeline::RenderPassRegistry* render_passes"));
    // The public Pipeline runtime context carries feature metadata only. The
    // engine-owned RenderPassRegistry is supplied by RenderGraph runtime when
    // authored passes are expanded, so it must not ride in this public value.
    assert(!contains("Engine/Pipeline/private/pipeline.hpp",
                     "const RenderPassRegistry* render_passes"));
    // Registered authored-pass expansion is an execution helper used only by
    // Engine-private RenderGraph/Renderer consumers, not a public Pipeline
    // authoring contract.
    assert(!contains("Engine/Pipeline/private/render_pass.hpp",
                     "add_registered_render_pass("));
    assert(std::filesystem::exists("Engine/Pipeline/private/render_pass_execution.hpp"));
    assert(contains("Engine/Pipeline/private/render_pass_execution.hpp",
                    "add_registered_render_pass("));
    // Render-pass resource declaration dispatch is an Engine-owned adapter;
    // public Pipeline authors consume only the checked RenderPassBuilder
    // facade and must not inherit the RHI-facing backend directly.
    assert(!contains("Engine/Pipeline/private/pipeline.hpp",
                     "class RenderPassBuilderBackend {"));
    assert(std::filesystem::exists("Engine/Pipeline/private/render_pass_builder_backend.hpp"));
    assert(contains("Engine/Pipeline/private/render_pass_builder_backend.hpp",
                    "class RenderPassBuilderBackend {"));
    // Render-pass command recording is also an Engine-owned adapter. Keep the
    // RHI-facing implementation behind Pipeline-private while retaining the
    // stable RenderPassExecutionContext facade for callbacks.
    assert(!contains("Engine/Pipeline/private/pipeline.hpp",
                     "class RenderPassExecutionContextBackend {"));
    assert(std::filesystem::exists("Engine/Pipeline/private/render_pass_execution_context_backend.hpp"));
    assert(contains("Engine/Pipeline/private/render_pass_execution_context_backend.hpp",
                    "class RenderPassExecutionContextBackend {"));
    // Logical graph/resource dispatch is an Engine-owned adapter as well;
    // keep RenderGraphBuilder's public facade free of the RHI-facing backend.
    assert(!contains("Engine/Pipeline/private/pipeline.hpp",
                     "class RenderGraphBuilderBackend {"));
    assert(std::filesystem::exists("Engine/Pipeline/private/render_graph_builder_backend.hpp"));
    assert(contains("Engine/Pipeline/private/render_graph_builder_backend.hpp",
                    "class RenderGraphBuilderBackend {"));
    // Registry mutation epochs guard only the Engine-owned authored-pass
    // execution host; keep that lifecycle detail out of public authoring.
    assert(!contains("Engine/Pipeline/private/render_pass.hpp",
                     "execution_epoch(const RenderPassRegistry&"));
    assert(std::filesystem::exists("Engine/Pipeline/private/render_pass_registry_execution.hpp"));
    assert(contains("Engine/Pipeline/private/render_pass_registry_execution.hpp",
                    "struct RenderPassRegistryExecution"));
    assert(contains("Engine/Pipeline/private/render_pass_registry_execution.hpp", "execution_epoch("));
    assert(!contains("Engine/Pipeline/private/render_pass.hpp", "find_pass("));
    assert(contains("Engine/Pipeline/private/render_pass_registry_execution.hpp", "find_pass("));
    // Phase ordering/revision are execution metadata owned by Pipeline. Keep
    // the authoring registry contract public while resolving frame/compiler
    // snapshots through a Pipeline-private bridge.
    assert(!contains_between("Engine/Pipeline/private/pipeline.hpp", "class PhaseRegistry final",
                             "/** @brief Thread-safe registry of immutable pipeline definitions.",
                             "snapshot() const"));
    assert(!contains_between("Engine/Pipeline/private/pipeline.hpp", "class PhaseRegistry final",
                             "/** @brief Thread-safe registry of immutable pipeline definitions.",
                             "revision() const noexcept"));
    assert(std::filesystem::exists("Engine/Pipeline/private/phase_registry_execution.hpp"));
    assert(contains("Engine/Pipeline/private/phase_registry_execution.hpp",
                    "struct PhaseRegistryExecution"));
    assert(contains("Engine/Pipeline/private/phase_registry_execution.hpp", "snapshot("));
    assert(contains("Engine/Pipeline/private/phase_registry_execution.hpp", "revision("));
    // PipelineScopeHost plan state is an Engine execution lifecycle detail.
    // Keep the public host contract focused on binding/rebuild/control while
    // resolving plan snapshots and retirement metadata through a private bridge.
    for (const auto helper : {"active_plan() const", "active_plan_snapshot() const", "has_pending_plan() const",
                              "retired_plan_count() const"}) {
        assert(!contains_between("Engine/Pipeline/private/pipeline.hpp", "class PipelineScopeHost : public PipelineInstance",
                                 "inline foundation::Result render_graph_builder_invalid_state", helper));
    }
    assert(std::filesystem::exists("Engine/Pipeline/private/pipeline_scope_host_execution.hpp"));
    assert(contains("Engine/Pipeline/private/pipeline_scope_host_execution.hpp",
                    "struct PipelineScopeHostExecution"));
    assert(contains("Engine/Pipeline/private/pipeline_scope_host_execution.hpp", "active_plan("));
    assert(contains("Engine/Pipeline/private/pipeline_scope_host_execution.hpp", "active_plan_snapshot("));
    assert(contains("Engine/Pipeline/private/pipeline_scope_host_execution.hpp", "has_pending_plan("));
    assert(contains("Engine/Pipeline/private/pipeline_scope_host_execution.hpp", "retired_plan_count("));
    assert(!contains_between("Engine/Pipeline/private/pipeline.hpp", "class PipelineScopeHost : public PipelineInstance",
                             "inline foundation::Result render_graph_builder_invalid_state", "scope_bridge() const"));
    assert(!contains_between("Engine/Pipeline/private/pipeline.hpp", "class PipelineScopeHost : public PipelineInstance",
                             "inline foundation::Result render_graph_builder_invalid_state", "set_scope_bridge("));
    assert(contains("Engine/Pipeline/private/pipeline_scope_host_execution.hpp", "scope_bridge("));
    assert(contains("Engine/Pipeline/private/pipeline_scope_host_execution.hpp", "set_scope_bridge("));
    // Pipeline compilation is an Engine-owned execution helper. Keep the
    // public input value available while resolving the compiler through a
    // Pipeline-private bridge.
    assert(!contains_between("Engine/Pipeline/private/pipeline.hpp", "struct PipelineCompileInput",
                             "/** @brief Configures worker lanes used by PipelineScheduler.", "compile("));
    assert(std::filesystem::exists("Engine/Pipeline/private/pipeline_compiler_execution.hpp"));
    assert(contains("Engine/Pipeline/private/pipeline_compiler_execution.hpp",
                    "struct PipelineCompilerExecution"));
    assert(contains("Engine/Pipeline/private/pipeline_compiler_execution.hpp", "compile("));
    // RenderGraphFeatureRuntimeDesc is a public value descriptor. The
    // engine-owned RenderPassRegistry is supplied explicitly to the runtime
    // build boundary instead of riding in that descriptor.
    assert(!contains("Engine/RenderGraph/public/render_feature_runtime.hpp",
                     "const pipeline::RenderPassRegistry* render_passes{nullptr}"));
    assert(!contains("Engine/RenderGraph/public/render_feature_runtime.hpp",
                     "const pipeline::RenderPassRegistry* render_passes"));
    assert(std::filesystem::exists("Engine/RenderGraph/private/render_pass_execution_host.hpp"));
    assert(!contains("Engine/RenderGraph/public/render_feature_runtime.hpp",
                     "RenderPassExecutionHost"));
    assert(!contains("Engine/RenderGraph/public/render_feature_runtime.hpp",
                     "PipelineInstanceConfiguration"));
    assert(!contains("Engine/RenderGraph/public/render_feature_runtime.hpp",
                     "RenderSceneDrawLowerer* draw_lowerer"));
    // Renderer scene lowering types are implementation-only bridge values. The
    // public RenderGraph descriptor carries only an opaque lowerer transport.
    assert(!contains("Engine/RenderGraph/public/render_feature_runtime.hpp",
                     "class RenderSceneDrawLowerer"));
    assert(!contains("Engine/RenderGraph/public/render_feature_runtime.hpp",
                     "struct RenderSceneDrawTargets"));
    assert(!contains("Engine/RenderGraph/public/render_feature_runtime.hpp",
                     "struct RenderFeatureBuildScope"));
    assert(!contains("Engine/RenderGraph/public/render_feature_runtime.hpp",
                     "class RenderLightListLease"));
    assert(std::filesystem::exists("Engine/RenderGraph/private/render_scene_lowerer.hpp"));
    assert(contains("Engine/RenderGraph/private/render_scene_lowerer.hpp",
                    "class RenderSceneDrawLowerer"));
    // Legacy Pipeline execution is retired. RenderGraph builds committed
    // Feature outputs through the existing neutral projection entry point.
    assert(!contains("Engine/RenderGraph/public/render_feature_runtime.hpp",
                     "const pipeline::PipelinePlan& plan"));
    assert(!contains("Engine/RenderGraph/public/render_feature_runtime.hpp",
                     "pipeline::PipelinePlanHandle plan"));
    assert(!contains("Engine/RenderGraph/public/render_feature_runtime.hpp",
                     "const pipeline::PipelineRegistry& registry"));
    assert(!std::filesystem::exists("Engine/RenderGraph/private/render_feature_runtime_pipeline.hpp"));
    // The old terminal state composer and its opaque public transport are gone.
    assert(!contains("Engine/RenderGraph/public/render_feature_runtime.hpp",
                     "pipeline::RenderGraphBuilder& builder"));
    assert(!contains("Engine/RenderGraph/public/render_feature_runtime.hpp",
                     "RenderStateCompositeFunction"));
    assert(!std::filesystem::exists("Engine/RenderGraph/private/render_feature_state_composer.hpp"));
    assert(!std::filesystem::exists("Engine/RenderGraph/private/render_state_flow.hpp"));
    assert(!std::filesystem::exists("Engine/RenderGraph/private/render_state_flow_pipeline.hpp"));
    // Upload submission receipts are RenderGraph/Runtime implementation
    // details. Public graph/resource contracts transport only an opaque owner;
    // receipt inspection remains on the private RenderGraph seam.
    assert(!contains("Engine/RenderGraph/public/render_graph.hpp",
                     "class RenderGraphPassSubmission"));
    assert(!contains("Engine/Resource/public/resource_uploader.hpp",
                     "RenderGraphPassSubmission"));
    assert(!contains("Engine/Resource/public/texture_uploader.hpp",
                     "RenderGraphPassSubmission"));
    assert(std::filesystem::exists("Engine/RenderGraph/private/render_graph_upload_submission.hpp"));
    assert(contains("Engine/RenderGraph/private/render_graph_upload_submission.hpp",
                    "class RenderGraphPassSubmission final"));
    // Upload receipt tracking is a Resource-private graph operation. Keep the
    // pass submission helper behind a RenderGraph-private access bridge rather
    // than exposing upload lifecycle details on the public graph contract.
    assert(!contains("Engine/RenderGraph/public/render_graph.hpp",
                     "track_pass_submission("));
    assert(std::filesystem::exists("Engine/RenderGraph/private/render_graph_upload_access.hpp"));
    assert(contains("Engine/RenderGraph/private/render_graph_upload_access.hpp",
                    "RenderGraphUploadAccess"));
    // Submission observers are RenderGraph transaction participants owned by
    // the private history/runtime implementation. Keep the observer type and
    // typed retention helper off the public RenderGraph contract.
    assert(!contains("Engine/RenderGraph/public/render_graph.hpp",
                     "class RenderGraphSubmissionObserver;"));
    assert(!contains("Engine/RenderGraph/public/render_graph.hpp",
                     "retain_submission_observer"));
    assert(std::filesystem::exists("Engine/RenderGraph/private/render_graph_submission.hpp"));
    assert(contains("Engine/RenderGraph/private/render_graph_submission.hpp",
                    "class RenderGraphSubmissionObserver"));
    // Feature history storage is an Engine-owned temporal host. Public
    // RenderGraph build calls transport only a borrowed opaque owner; the
    // typed history store is resolved in RenderGraph-private code.
    assert(!contains("Engine/RenderGraph/public/render_feature_runtime.hpp",
                     "class RenderFeatureHistory;"));
    assert(!contains("Engine/RenderGraph/public/render_feature_runtime.hpp",
                     "RenderFeatureHistory* history"));
    // FrameContext is a concrete runtime owner. Keep it out of the public
    // RenderGraph feature descriptor and resolve it only in private code.
    assert(!contains("Engine/RenderGraph/public/render_feature_runtime.hpp",
                     "class FrameContext;"));
    assert(!contains("Engine/RenderGraph/public/render_feature_runtime.hpp",
                     "const FrameContext* current_frame"));
    // Renderer public values do not use these internal runtime types; avoid
    // retaining misleading forward declarations in the exported header.
    assert(!contains("Engine/Renderer/public/renderer.hpp",
                     "namespace aeg::render_graph { class FrameContext; }"));
    assert(!contains("Engine/RenderGraph/public/render_feature_runtime.hpp",
                     "class RenderGraphPassHost;"));
    // EngineFrameOrchestrator owns a Pipeline registry/scheduler bridge, but
    // the stable Runtime constructor should receive only the opaque private
    // host. Pipeline registry and worker scheduling stay Engine-internal.
    assert(!contains("Engine/Runtime/public/engine_frame_orchestrator.hpp",
                     "EngineFrameOrchestrator(pipeline::PipelineRegistry& registry"));
    assert(!contains("Engine/Runtime/public/engine_frame_orchestrator.hpp",
                     "pipeline::PipelineScheduler& scheduler"));
    assert(std::filesystem::exists("Engine/Runtime/private/engine_frame_pipeline_host.hpp"));
    assert(contains("Engine/Runtime/private/engine_frame_pipeline_host.hpp",
                    "class EngineFramePipelineHost final"));
    // PipelineScopeHost registration is an Engine-private adapter seam. The
    // stable Runtime orchestrator API should not require callers to expose
    // Pipeline execution-host types just to register a frame/world host.
    assert(!contains("Engine/Runtime/public/engine_frame_orchestrator.hpp",
                     "register_pipeline(pipeline::PipelineScopeHost& host"));
    assert(!contains("Engine/Runtime/public/engine_frame_orchestrator.hpp",
                     "pipeline::PipelineScopeHost& frame_host"));
    assert(std::filesystem::exists("Engine/Runtime/private/engine_frame_pipeline_registration.hpp"));
    assert(contains("Engine/Runtime/private/engine_frame_pipeline_registration.hpp",
                    "class EngineFramePipelineRegistration final"));
    // The contribution reachability walk retired with the old authoring chain.
    assert(!std::filesystem::exists("Engine/RenderGraph/public/render_feature_flow.hpp"));
    assert(!std::filesystem::exists("Engine/RenderGraph/private/render_feature_flow.hpp"));
    // RenderFeatureProjector is the typed RenderGraph-private lowering
    // adapter. Public projection headers retain only frame work-item values
    // consumed by the graph contract.
    assert(!contains("Engine/RenderGraph/public/render_feature_projection.hpp",
                     "class RenderFeatureProjector final"));
    assert(std::filesystem::exists("Engine/RenderGraph/private/render_feature_projector.hpp"));
    assert(contains("Engine/RenderGraph/private/render_feature_projector.hpp",
                    "class RenderFeatureProjector final"));
    // RenderIntentPlanner is the Feature-private validation/planning adapter;
    // public intent values remain available to the projection contract.
    assert(!contains("Engine/Feature/public/render_intent_plan.hpp",
                     "class RenderIntentPlanner final"));
    assert(std::filesystem::exists("Engine/Feature/private/render_intent_planner.hpp"));
    assert(contains("Engine/Feature/private/render_intent_planner.hpp",
                    "class RenderIntentPlanner final"));
    assert(!std::filesystem::exists("Engine/RenderGraph/private/render_graph_pass_host.hpp"));
    // RenderGraph's Feature build helpers are private implementation access;
    // the public graph header must not name the typed runtime itself.
    assert(!contains("Engine/RenderGraph/public/render_graph.hpp",
                     "class RenderGraphFeatureRuntime;"));
    assert(!contains("Engine/RenderGraph/public/render_graph.hpp",
                     "friend class RenderGraphFeatureRuntime;"));
    assert(std::filesystem::exists("Engine/RenderGraph/private/render_graph_feature_access.hpp"));
    assert(contains("Engine/RenderGraph/private/render_graph_feature_access.hpp",
                    "RenderGraphFeatureGraphAccess"));
    // Compile statistics belong to RenderGraph execution state. Keep the
    // public graph façade neutral while exposing them through a private bridge.
    assert(!contains_between("Engine/RenderGraph/public/render_graph.hpp", "class RenderGraph final",
                             "friend class PassBuilder", "compile_info() const"));
    assert(std::filesystem::exists("Engine/RenderGraph/private/render_graph_execution.hpp"));
    assert(contains("Engine/RenderGraph/private/render_graph_execution.hpp", "struct RenderGraphExecution"));
    assert(contains("Engine/RenderGraph/private/render_graph_execution.hpp", "compile_info("));
    assert(contains("Engine/RenderGraph/private/render_graph_execution.hpp", "plan("));
    assert(!contains_between("Engine/RenderGraph/public/render_graph.hpp", "class RenderGraph final",
                             "friend class PassBuilder", "plan() const"));
    // Native device access belongs to RenderGraph execution internals. Keep
    // the public graph façade neutral and expose it only through the bridge.
    assert(!contains_between("Engine/RenderGraph/public/render_graph.hpp", "class RenderGraph final",
                             "friend class PassBuilder", "device() const noexcept"));
    assert(contains("Engine/RenderGraph/private/render_graph_execution.hpp", "device("));
    // Graph-owned handle membership is execution validation, not authoring API.
    assert(!contains_between("Engine/RenderGraph/public/render_graph.hpp", "class RenderGraph final",
                             "friend class PassBuilder", "contains_buffer(RenderGraphBuffer"));
    assert(!contains_between("Engine/RenderGraph/public/render_graph.hpp", "class RenderGraph final",
                             "friend class PassBuilder", "contains_texture(RenderGraphTexture"));
    assert(std::filesystem::exists("Engine/RenderGraph/private/render_graph_resource_access.hpp"));
    assert(contains("Engine/RenderGraph/private/render_graph_resource_access.hpp", "RenderGraphResourceAccess"));
    assert(contains("Engine/RenderGraph/private/render_graph_resource_access.hpp", "contains_buffer("));
    assert(contains("Engine/RenderGraph/private/render_graph_resource_access.hpp", "contains_texture("));
    // Feature graph transaction/provenance helpers are implementation access
    // and must live entirely behind the existing private bridge.
    for (const auto helper : {"begin_feature_build_transaction(", "retain_execution_owner(",
                              "retain_submission_owner(", "set_feature_work(", "feature_pass_checkpoint(",
                              "attribute_feature_passes_since(", "texture_description(", "textures_alias(",
                              "commit_feature_build_transaction(", "rollback_feature_build_transaction("}) {
        assert(!contains("Engine/RenderGraph/public/render_graph.hpp", helper));
    }
    // EngineFrameDesc is a public per-frame value. The engine-owned pass
    // registry is fixed on the orchestrator host and must not travel in each
    // frame input descriptor.
    assert(!contains("Engine/Runtime/public/engine_frame_orchestrator.hpp",
                     "const pipeline::RenderPassRegistry* render_passes{nullptr}"));
    assert(!contains("Engine/Runtime/public/engine_frame_orchestrator.hpp",
                     "const pipeline::RenderPassRegistry* render_passes"));
    assert(std::filesystem::exists("Engine/Runtime/private/engine_frame_pass_registry.hpp"));
    assert(contains("Engine/Runtime/private/engine_frame_pass_registry.hpp",
                    "class EngineFramePassRegistry final"));
    // Authored RenderPass expansion is fixed on the private Pipeline host;
    // the public orchestrator constructor and state must not name its adapter.
    assert(!contains("Engine/Runtime/public/engine_frame_orchestrator.hpp",
                     "class EngineFramePassRegistry;"));
    assert(!contains("Engine/Runtime/public/engine_frame_orchestrator.hpp",
                     "const EngineFramePassRegistry* render_passes"));
    // RenderFeatureHistory is an engine-owned temporal host. Keep the public
    // runtime descriptor neutral and pass the host explicitly at the build seam.
    assert(!contains("Engine/RenderGraph/public/render_feature_runtime.hpp",
                     "RenderFeatureHistory* history{nullptr}"));
    // History storage is RenderGraph-private. Keep the runtime implementation
    // out of the history owner's typed friend surface and use a narrow bridge.
    assert(!contains("Engine/RenderGraph/private/render_feature_history.hpp",
                     "class RenderGraphFeatureRuntime;"));
    assert(!contains("Engine/RenderGraph/private/render_feature_history.hpp",
                     "friend class RenderGraphFeatureRuntime;"));
    assert(std::filesystem::exists("Engine/RenderGraph/private/render_feature_history_access.hpp"));
    assert(contains("Engine/RenderGraph/private/render_feature_history_access.hpp",
                    "RenderFeatureHistoryRuntimeAccess"));
    // Asset upload and shader packing are Runtime host implementation details.
    assert(!std::filesystem::exists("Application/Runtime/public/runtime_asset_resolver.hpp"));
    assert(std::filesystem::exists("Application/Runtime/private/runtime_asset_resolver.hpp"));
    assert(std::filesystem::exists("Application/Runtime/private/runtime_asset_upload_host.hpp"));
    assert(contains("Application/Runtime/private/runtime_asset_upload_host.hpp",
                    "material_contract_epoch"));
    // The resolver's private API transports only an opaque borrowed host.  Its
    // header must not pull ShaderProgram/RHI/uploader implementation types into
    // every Runtime resolver consumer; the complete host belongs to the .cpp
    // and to explicit internal host owners.
    assert(!contains("Application/Runtime/private/runtime_asset_resolver.hpp",
                     "runtime_asset_upload_host.hpp"));
    assert(!contains("Application/Runtime/private/runtime_asset_resolver.hpp",
                     "ShaderProgram"));
    assert(!contains("Application/Runtime/private/runtime_asset_resolver.hpp",
                     "rhi.hpp"));
    assert(contains("Application/Runtime/private/runtime_asset_resolver.hpp",
                    "RuntimeAssetUploadHost*"));
    assert(contains("Application/Runtime/private/runtime_asset_resolver.cpp",
                    "runtime_asset_upload_host.hpp"));
    assert(!contains("Application/Runtime/public/runtime_bootstrap.hpp", "runtime_asset_resolver.hpp"));
    assert(!contains("Application/Runtime/public/runtime_bootstrap.hpp", "runtime_asset_upload_host.hpp"));
    // Runtime Inspect and process-control hosts are engine-owned forwarding
    // services. They carry FeatureGraph runtime implementation values and must
    // not be reachable through the Runtime public include surface.
    assert(!std::filesystem::exists("Application/Runtime/public/feature_graph_runtime_host.hpp"));
    assert(!std::filesystem::exists("Application/Runtime/public/runtime_feature_control.hpp"));
    assert(std::filesystem::exists("Application/Runtime/private/feature_graph_runtime_host.hpp"));
    assert(std::filesystem::exists("Application/Runtime/private/runtime_feature_control.hpp"));
    assert(contains("Application/Runtime/private/feature_graph_runtime_host.hpp",
                    "class FeatureGraphRuntimeHost final"));
    assert(contains("Application/Runtime/private/runtime_feature_control.hpp",
                    "class RuntimeFeatureControl final"));
    // Process editor layout is an Editor-only implementation detail. Keep it
    // out of the public Editor include surface so runtime and SDK consumers
    // cannot depend on viewport layout state.
    assert(!std::filesystem::exists("Application/Editor/public/feature_process_editor.hpp"));
    assert(std::filesystem::exists("Application/Editor/private/feature_process_editor.hpp"));
    assert(contains("Application/Editor/private/feature_process_editor.hpp",
                    "struct ProcessEditorLayout final"));
    // FeatureGraph composition models, panels, and node-definition helpers
    // are Editor implementation details. The model target must not publish
    // their private include directory through its usage requirements.
    assert(!contains("CMakeLists.txt",
                     "PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Application/Editor/private/composition"));
    assert(contains("CMakeLists.txt",
                    "Application/Editor/private/composition/editor_feature_graph_document.cpp"));
    // RenderFeatureHistory owns GPU-backed temporal allocations and transaction
    // leases for the Engine host. Keep its complete declaration out of the
    // public RenderGraph include surface; render_feature_runtime.hpp only
    // carries the opaque pointer transport.
    assert(!std::filesystem::exists("Engine/RenderGraph/public/render_feature_history.hpp"));
    assert(std::filesystem::exists("Engine/RenderGraph/private/render_feature_history.hpp"));
    assert(contains("Engine/RenderGraph/private/render_feature_history.hpp",
                    "class RenderFeatureHistory final"));
    assert(!contains("Engine/RenderGraph/public/render_feature_runtime.hpp",
                     "class RenderFeatureHistory;"));
    std::cout << "feature no legacy identity smoke passed\n";
    return 0;
}
