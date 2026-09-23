/**
 * @file editor_scene_runtime.hpp
 * @brief Application-owned scene document, edit/runtime World, and viewport renderer.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Components/component_reflection.hpp"
#include "Components/render_components.hpp"
#include "ecs.hpp"
#include "editor_asset_database.hpp"
#include "editor_project.hpp"
#include "editor_project_pipeline_authoring.hpp"
#include "engine_frame_orchestrator.hpp"
#include "imgui_texture_registry.hpp"
#include "pipeline.hpp"
#include "reflection.hpp"
#include "render_graph.hpp"
#include "render_state_flow.hpp"
#include "renderer.hpp"
#include "rhi.hpp"
#include "scene_asset.hpp"
#include "world_scope.hpp"

namespace aeg::application {

/** @brief Pixel position in the Scene Viewport image. */
struct ViewportPoint final {
    float x{0.0f};
    float y{0.0f};
};

/** @brief Result of CPU viewport picking; an invalid entity means no hit. */
struct ViewportPickResult final {
    scene::SceneEntityId entity{};
    float distance{0.0f};
    bool hit{false};
};

/** @brief Transform operation exposed by the editor overlay. */
enum class ViewportGizmoMode : uint8_t {
    kTranslate,
    kRotate,
    kScale,
};

/** @brief World-space axis used by a Transform Gizmo transaction. */
enum class ViewportGizmoAxis : uint8_t {
    kX,
    kY,
    kZ,
};

/** @brief Screen-space projection of the selected entity's world-axis gizmo. */
struct ViewportGizmoProjection final {
    ViewportPoint center{};
    ViewportPoint x_axis{};
    ViewportPoint y_axis{};
    ViewportPoint z_axis{};
    bool valid{false};
};

/** @brief Operation applied when changing the Editor-owned selection set. */
enum class SelectionOperation : uint8_t {
    kReplace,
    kAdd,
    kToggle,
};

/** @brief Transform policy used while changing an entity's hierarchy. */
enum class HierarchyTransformPolicy : uint8_t {
    kKeepWorld,
    kKeepLocal,
};

/** @brief Stable resource category accepted by editor Inspector bindings. */
enum class EditorResourceKind : uint8_t {
    kMesh,
    kMaterial,
    kTexture,
    kScene,
    kFont,
    kUnknown,
};

/** @brief Stable editor-visible resource identity used by drag-and-drop payloads. */
struct EditorResourceDescriptor final {
    EditorResourceKind kind{EditorResourceKind::kMesh};
    uint64_t id{0};
    std::string name{};
    bool valid{false};
    EditorAssetStatus status{EditorAssetStatus::kImported};
    std::string path{};
};

/** @brief Borrowed component payload used by one atomic editor update. */
struct ComponentUpdateView final {
    scene::SceneEntityId entity{};
    ecs::ComponentTypeId component{};
    std::span<const std::byte> bytes{};
};

/** @brief Owns the minimum editor scene lifecycle and its offscreen Renderer preview. */
class EditorSceneRuntime final {
public:
    enum class PipelineTarget : uint8_t {
        kWorld,
        kRenderer,
    };
    enum class Mode : uint8_t {
        kEdit,
        kPlay,
        kPause,
    };

    EditorSceneRuntime();
    ~EditorSceneRuntime();

    EditorSceneRuntime(const EditorSceneRuntime&) = delete;
    EditorSceneRuntime& operator=(const EditorSceneRuntime&) = delete;

    [[nodiscard]] foundation::Result initialize(rhi::Device& device,
                                                const std::filesystem::path& shader_package_path = {});
    [[nodiscard]] foundation::Result shutdown();

    /** @brief Renders the active World into the current viewport target. */
    [[nodiscard]] foundation::Result render_frame(uint64_t frame_number);
    /** @brief Exposes the engine renderer plan as this project's editable Renderer Pipeline. */
    [[nodiscard]] foundation::Result create_project_renderer_pipeline();
    /** Validates, activates and persists a project renderer document at a frame boundary. */
    [[nodiscard]] foundation::Result activate_renderer_pipeline_document(const std::filesystem::path& path);
    [[nodiscard]] const std::filesystem::path& active_renderer_pipeline_document() const noexcept;
    /** Non-blocking project pipeline failure while the Editor default preview remains usable. */
    [[nodiscard]] std::string_view renderer_pipeline_diagnostic() const noexcept;
    /** Restores the compatible built-in renderer and removes the project renderer selection. */
    [[nodiscard]] foundation::Result restore_default_renderer_pipeline();
    [[nodiscard]] std::vector<std::pair<std::string, pipeline::PipelinePlanHandle>> pipeline_snapshots() const;
    /** Authored project Pipeline documents, available before the game module has been built. */
    [[nodiscard]] std::span<const EditorProjectPipelineDocument> authored_pipeline_documents() const noexcept;
    [[nodiscard]] const pipeline::TypeCatalog& authored_pipeline_catalog() const noexcept;
    /** Persists one authored node's optional connection to the editor-owned Present consumer. */
    [[nodiscard]] foundation::Result set_authored_present_connection(const std::filesystem::path& document_path,
                                                                     std::string_view source_node, bool connected);
    /** @brief Stages an Editor-authored ordering override for the selected live game pipeline. */
    [[nodiscard]] foundation::Result add_pipeline_runtime_ordering(PipelineTarget target, uint32_t source_node,
                                                                   uint32_t destination_node);
    /** @brief Removes an Editor-authored ordering override from the selected live game pipeline. */
    [[nodiscard]] foundation::Result remove_pipeline_runtime_ordering(PipelineTarget target, uint32_t source_node,
                                                                      uint32_t destination_node);
    /** Adds an editor-owned State Link without changing the compiled Pipeline schedule. */
    [[nodiscard]] foundation::Result add_render_state_link(uint32_t source_node, uint32_t destination_node);
    /** Masks either an editor-owned or a compiled State Link without changing task scheduling. */
    [[nodiscard]] foundation::Result remove_render_state_link(uint32_t source_node, uint32_t destination_node);
    [[nodiscard]] std::vector<render_graph::RenderStateLinkOverride> render_state_link_overrides() const;
    /** Overrides the terminal blend of the RenderFeature owning this renderer-plan node. */
    [[nodiscard]] foundation::Result set_render_state_blend(uint32_t node, pipeline::RenderStateBlendMode blend_mode);
    [[nodiscard]] std::vector<render_graph::RenderStateBlendOverride> render_state_blend_overrides() const;
    /** @brief Schedules a GPU readback of the next rendered viewport frame. */
    [[nodiscard]] foundation::Result request_viewport_readback();
    /** @brief Returns the most recently requested viewport pixels in RGBA8 order. */
    [[nodiscard]] foundation::ResultValue<std::vector<std::byte>> read_viewport_pixels() const;
    /** @brief Requests a target rebuild at the next frame boundary. */
    [[nodiscard]] foundation::Result request_viewport_extent(uint32_t width, uint32_t height);

    [[nodiscard]] editor::ImGuiTextureBinding viewport_binding() const noexcept;
    [[nodiscard]] bool viewport_ready() const noexcept;
    /** @brief True when a render Feature can reach the active Renderer Pipeline Present stage. */
    [[nodiscard]] bool viewport_present_connected() const noexcept;
    [[nodiscard]] bool viewport_resize_pending() const noexcept;

    /** @brief Intersects the edit World mesh bounds with a camera ray. */
    [[nodiscard]] foundation::ResultValue<ViewportPickResult> pick_viewport(ViewportPoint pixel, uint32_t width,
                                                                            uint32_t height) const;
    /** @brief Projects the selected entity and its world axes into viewport pixels. */
    [[nodiscard]] foundation::ResultValue<ViewportGizmoProjection> selected_gizmo_projection(uint32_t width,
                                                                                             uint32_t height) const;
    /** @brief Begins a world-axis Transform edit transaction. */
    [[nodiscard]] foundation::Result begin_transform_edit(ViewportGizmoMode mode, ViewportGizmoAxis axis,
                                                          ViewportPoint pixel, uint32_t width, uint32_t height);
    /** @brief Queues the current cursor position as the transaction value. */
    [[nodiscard]] foundation::Result update_transform_edit(ViewportPoint pixel, uint32_t width, uint32_t height);
    /** @brief Ends the current transaction and keeps its final queued value. */
    [[nodiscard]] foundation::Result commit_transform_edit();
    /** @brief Cancels the current transaction and queues its original Transform. */
    [[nodiscard]] foundation::Result cancel_transform_edit();
    [[nodiscard]] bool transform_edit_active() const noexcept;

    [[nodiscard]] std::span<const scene::SceneEntityRecord> entities() const noexcept;
    [[nodiscard]] std::span<const scene::SceneEntityId> selected_entities() const noexcept;
    [[nodiscard]] scene::SceneEntityId selected_entity() const noexcept;
    [[nodiscard]] bool is_selected(scene::SceneEntityId entity) const noexcept;
    [[nodiscard]] foundation::Result select(scene::SceneEntityId entity,
                                            SelectionOperation operation = SelectionOperation::kReplace);
    [[nodiscard]] foundation::ResultValue<std::vector<scene::SceneEntityId>> children(
        scene::SceneEntityId parent) const;
    [[nodiscard]] foundation::ResultValue<std::string> hierarchy_path(scene::SceneEntityId entity) const;
    [[nodiscard]] foundation::Result reparent_entities(
        std::span<const scene::SceneEntityId> entities, scene::SceneEntityId parent,
        std::size_t sibling_index = static_cast<std::size_t>(-1),
        HierarchyTransformPolicy transform_policy = HierarchyTransformPolicy::kKeepWorld);
    [[nodiscard]] foundation::Result create_scene_entity(scene::SceneEntityId parent, std::string_view name);
    [[nodiscard]] foundation::Result rename_entity(scene::SceneEntityId entity, std::string_view name);
    [[nodiscard]] foundation::Result set_entity_tag(scene::SceneEntityId entity, std::string_view tag);
    [[nodiscard]] foundation::Result duplicate_entities(std::span<const scene::SceneEntityId> entities);
    [[nodiscard]] foundation::Result add_component(std::span<const scene::SceneEntityId> entities,
                                                   ecs::ComponentTypeId component);
    [[nodiscard]] foundation::Result remove_component(std::span<const scene::SceneEntityId> entities,
                                                      ecs::ComponentTypeId component);
    [[nodiscard]] foundation::Result remove_entities(
        std::span<const scene::SceneEntityId> entities,
        scene::SceneEntityRemovalPolicy policy = scene::SceneEntityRemovalPolicy::kCascade);
    [[nodiscard]] std::string_view scene_name() const noexcept;
    [[nodiscard]] const std::filesystem::path& scene_path() const noexcept;
    [[nodiscard]] bool dirty() const noexcept;
    [[nodiscard]] Mode mode() const noexcept;
    [[nodiscard]] std::string_view diagnostic() const noexcept;
    [[nodiscard]] bool project_loaded() const noexcept;
    [[nodiscard]] std::string_view project_name() const noexcept;
    [[nodiscard]] const std::filesystem::path& project_root() const noexcept;

    [[nodiscard]] foundation::Result new_scene();
    [[nodiscard]] foundation::Result open_scene(const std::filesystem::path& path);
    [[nodiscard]] foundation::Result save_scene();
    [[nodiscard]] foundation::Result save_scene_as(const std::filesystem::path& path);
    [[nodiscard]] foundation::Result new_project(const std::filesystem::path& root, std::string_view name);
    [[nodiscard]] foundation::Result import_project(const std::filesystem::path& root_or_project_file);

    /** @brief Refreshes the project-local Asset Browser index. */
    [[nodiscard]] foundation::Result refresh_assets();
    [[nodiscard]] foundation::Result reimport_asset(uint64_t id);
    [[nodiscard]] foundation::Result rename_asset(uint64_t id, std::string_view new_name);
    [[nodiscard]] foundation::Result remove_asset(uint64_t id);
    /** @brief Applies an Asset Browser drag to the selected MeshRenderer or creates one entity. */
    [[nodiscard]] foundation::Result drop_resource(EditorResourceKind kind, uint64_t id);

    [[nodiscard]] foundation::Result play();
    [[nodiscard]] foundation::Result pause();
    [[nodiscard]] foundation::Result stop();

    /** @brief Undoes the latest committed Edit transaction. */
    [[nodiscard]] foundation::Result undo();
    /** @brief Reapplies the latest transaction removed by undo(). */
    [[nodiscard]] foundation::Result redo();
    [[nodiscard]] bool can_undo() const noexcept;
    [[nodiscard]] bool can_redo() const noexcept;

    /** @brief Queues a reflected component byte update for the next WorldCommit. */
    [[nodiscard]] foundation::Result queue_component_update(scene::SceneEntityId entity, ecs::ComponentTypeId component,
                                                            std::span<const std::byte> bytes);
    /** @brief Queues one atomic multi-entity component transaction. */
    [[nodiscard]] foundation::Result queue_component_updates(std::span<const ComponentUpdateView> updates);

    /** @brief Begins one Inspector transaction and applies its first value. */
    [[nodiscard]] foundation::Result begin_component_edit(std::span<const ComponentUpdateView> updates);
    /** @brief Replaces the current Inspector transaction value without adding history. */
    [[nodiscard]] foundation::Result update_component_edit(std::span<const ComponentUpdateView> updates);
    /** @brief Commits one Inspector transaction as a single history command. */
    [[nodiscard]] foundation::Result commit_component_edit();
    /** @brief Restores the value captured at Inspector transaction start. */
    [[nodiscard]] foundation::Result cancel_component_edit();
    [[nodiscard]] bool component_edit_active() const noexcept;

    [[nodiscard]] const reflection::TypeRegistry& reflection_registry() const noexcept;
    [[nodiscard]] const ecs::World* edit_world() const noexcept;
    [[nodiscard]] foundation::ResultValue<ecs::EntityId> runtime_entity(scene::SceneEntityId entity) const;
    [[nodiscard]] foundation::ResultValue<ecs::ComponentDescriptor> component_descriptor(
        ecs::ComponentTypeId type) const;
    [[nodiscard]] const void* component_value(scene::SceneEntityId entity, ecs::ComponentTypeId type) const;

    /** @brief Returns the stable component identities visible to the selected entity. */
    [[nodiscard]] std::vector<ecs::ComponentTypeId> selected_component_types() const;
    [[nodiscard]] std::vector<EditorResourceDescriptor> resource_descriptors() const;
    [[nodiscard]] bool is_resource_valid(EditorResourceKind kind, uint64_t id) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_{};
};
}  // namespace aeg::application
