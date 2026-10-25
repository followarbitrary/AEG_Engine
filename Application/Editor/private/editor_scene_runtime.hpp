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
#include "editor_project_feature_authoring.hpp"
#include "feature_graph_runtime_host.hpp"
#include "imgui_texture_registry.hpp"
#include "reflection.hpp"
#include "render_graph.hpp"
#include "renderer.hpp"
#include "rhi.hpp"
#include "scene_asset.hpp"

namespace aeg::application {
struct EditorRenderGraphSnapshot;

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

/** @brief Explicit authored source-to-Scene entity route for World commands. */
struct EditorFeatureWorldBinding final {
    std::string graph_asset_uuid{};
    feature::FeatureNodeId node_id{};
    feature::OwnerPath owner_path{};
    scene::SceneAssetId scene_asset{};
    scene::SceneEntityId scene_entity{};
    feature::SideEffectId effect{};
    uint32_t version{0};
    feature::PortId observation_port{};
};

/** @brief Owning immutable view of the Editor's current active World and stable Scene mapping. */
struct EditorWorldSnapshot final {
    std::shared_ptr<const ecs::WorldSnapshot> snapshot{};
    std::vector<scene::SceneEntityBinding> entities{};
};

/** @brief Owns the minimum editor scene lifecycle and its offscreen Renderer preview. */
class EditorSceneRuntime final {
public:
    enum class Mode : uint8_t {
        kEdit,
        kPlay,
        kPause,
    };
    /** Source of the complete graph batch when explicitly selecting an executable module. */
    enum class FeatureGraphLoadPolicy : uint8_t {
        kPreserveActive,
        kSavedStartup,
    };

    EditorSceneRuntime();
    ~EditorSceneRuntime();

    EditorSceneRuntime(const EditorSceneRuntime&) = delete;
    EditorSceneRuntime& operator=(const EditorSceneRuntime&) = delete;

    [[nodiscard]] foundation::Result initialize(rhi::Device& device,
                                                const std::filesystem::path& shader_package_path = {});
    [[nodiscard]] foundation::Result shutdown();

    /** @brief Renders the active World into the current viewport target. */
    [[nodiscard]] foundation::Result render_frame(uint64_t frame_number, double elapsed_seconds);
    /**
     * Explicit executable selection; the complete candidate batch publishes at the next Scene frame.
     * PreserveActive retains the published graphs when present. SavedStartup reads only the current
     * project file's saved startup roots and requires a nonempty list; neither policy changes editor drafts.
     */
    [[nodiscard]] foundation::Result select_feature_module(const std::filesystem::path& manifest,
                                                            std::string_view configuration,
                                                            FeatureGraphLoadPolicy policy = FeatureGraphLoadPolicy::kPreserveActive);
    /** Replaces one graph within the complete active batch at the next Scene frame. */
    [[nodiscard]] foundation::Result queue_feature_graph(const feature::FeatureGraphDocument& graph);
    [[nodiscard]] foundation::ResultValue<runtime::FeatureRuntimeNodeSnapshot> inspect_feature_node(
        std::string_view asset_uuid, feature::FeatureNodeId node) const;
    [[nodiscard]] foundation::ResultValue<runtime::FeatureRuntimeExecutionSnapshot> inspect_feature_execution(
        std::string_view asset_uuid) const;
    [[nodiscard]] foundation::ResultValue<std::shared_ptr<const EditorRenderGraphSnapshot>> inspect_render_graph() const;
    /** Publishes the Engine-owned Preview Feature graph at an Edit frame boundary. */
    [[nodiscard]] foundation::Result publish_renderer_feature_graph(const feature::FeatureGraphDocument& graph);
    [[nodiscard]] foundation::ResultValue<runtime::FeatureRuntimeExecutionSnapshot> inspect_renderer_feature_execution() const;
    [[nodiscard]] foundation::Result control_feature_process(const runtime::RuntimeProcessCommand& command);
    [[nodiscard]] bool feature_publication_pending() const noexcept;
    /** Restores the Engine-owned Opaque Preview Feature at an Edit frame boundary. */
    [[nodiscard]] foundation::Result restore_default_renderer_feature();
    /** Authored project FeatureGraph documents, available before the game module has been built. */
    [[nodiscard]] std::span<const EditorProjectFeatureGraphDocument> authored_feature_graphs() const noexcept;
    /** Feature definitions exported by module metadata for FeatureGraph authoring. */
    [[nodiscard]] const feature::FeatureCatalog& authored_feature_catalog() const noexcept;
    /** Metadata contract for the currently published executable module, if any. */
    [[nodiscard]] const feature::FeatureCatalog* active_feature_module_catalog() const noexcept;
    /** Validates a candidate bundle and returns only its immutable catalog metadata. */
    [[nodiscard]] foundation::ResultValue<feature::FeatureCatalog> inspect_feature_module_catalog(
        const std::filesystem::path& manifest) const;
    [[nodiscard]] std::span<const feature::FeatureDefinitionId> authored_compiled_features() const noexcept;
    [[nodiscard]] std::span<const NodeContractFile> authored_feature_drafts() const noexcept;
    /** Catalog-matched editor display names and port labels; never executable metadata. */
    [[nodiscard]] std::span<const NodeDefinitionDraft> authored_feature_presentations() const noexcept;
    [[nodiscard]] std::span<const EditorProjectFeatureConflict> authored_feature_conflicts() const noexcept;
    /** @brief Schedules a GPU readback of the next rendered viewport frame. */
    [[nodiscard]] foundation::Result request_viewport_readback();
    /** @brief Returns the most recently requested viewport pixels in RGBA8 order. */
    [[nodiscard]] foundation::ResultValue<std::vector<std::byte>> read_viewport_pixels() const;
    /** @brief Requests a target rebuild at the next frame boundary. */
    [[nodiscard]] foundation::Result request_viewport_extent(uint32_t width, uint32_t height);

    [[nodiscard]] editor::ImGuiTextureBinding viewport_binding() const noexcept;
    [[nodiscard]] bool viewport_ready() const noexcept;
    [[nodiscard]] bool viewport_resize_pending() const noexcept;
    /** @brief Actual Scene frame draws, including the default Preview before a project is opened. */
    [[nodiscard]] uint32_t draw_call_count() const noexcept;

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
    [[nodiscard]] std::filesystem::path project_file() const;

    [[nodiscard]] foundation::Result new_scene();
    [[nodiscard]] foundation::Result open_scene(const std::filesystem::path& path);
    [[nodiscard]] foundation::Result save_scene();
    [[nodiscard]] foundation::Result save_scene_as(const std::filesystem::path& path);
    [[nodiscard]] foundation::Result new_project(const std::filesystem::path& root, std::string_view name);
    [[nodiscard]] foundation::Result new_project(const ProjectCreateOptions& options);
    [[nodiscard]] foundation::Result import_project(const std::filesystem::path& root_or_project_file);
    [[nodiscard]] foundation::Result close_project();
    [[nodiscard]] foundation::Result update_project_settings(const project::ProjectConfig& config);

    /** @brief Refreshes the project-local Asset Browser index. */
    [[nodiscard]] foundation::Result refresh_assets();
    [[nodiscard]] foundation::Result import_external_asset(const EditorAssetImportRequest& request);
    [[nodiscard]] foundation::Result reimport_asset(uint64_t id);
    [[nodiscard]] foundation::Result rename_asset(uint64_t id, std::string_view new_name);
    [[nodiscard]] foundation::Result remove_asset(uint64_t id);
    /** @brief Applies an Asset Browser drag to the selected MeshRenderer or creates one entity. */
    [[nodiscard]] foundation::Result drop_resource(EditorResourceKind kind, uint64_t id);

    [[nodiscard]] foundation::Result play();
    [[nodiscard]] foundation::Result pause();
    [[nodiscard]] foundation::Result stop();
    /** Installs an explicit World route batch while in Edit mode; an empty batch clears it. */
    [[nodiscard]] foundation::Result set_feature_world_bindings(
        std::span<const EditorFeatureWorldBinding> bindings);
    /** Returns the current Editor-private Scene routes without exposing mutable storage. */
    [[nodiscard]] std::span<const EditorFeatureWorldBinding> feature_world_bindings() const noexcept;
    /** Binds one uniquely published authored command owner to a stable Scene entity. */
    [[nodiscard]] foundation::Result bind_feature_world_node(
        std::string_view graph_asset_uuid, feature::FeatureNodeId node, scene::SceneEntityId scene_entity);
    /** Removes all World routes associated with one authored graph node. */
    [[nodiscard]] foundation::Result clear_feature_world_node_binding(
        std::string_view graph_asset_uuid, feature::FeatureNodeId node);
    /** Captures the committed active Edit/Play World without exposing its mutable ECS owner. */
    [[nodiscard]] foundation::ResultValue<EditorWorldSnapshot> inspect_active_world_snapshot() const;

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
    [[nodiscard]] std::span<const EditorAssetRecord> asset_records() const noexcept;
    [[nodiscard]] std::vector<EditorResourceDescriptor> resource_descriptors() const;
    [[nodiscard]] bool is_resource_valid(EditorResourceKind kind, uint64_t id) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_{};
};
}  // namespace aeg::application
