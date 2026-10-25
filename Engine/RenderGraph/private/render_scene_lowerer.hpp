/**
 * @file render_scene_lowerer.hpp
 * @brief RenderGraph-private bridge types for Renderer scene lowering.
 */
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <utility>

#include "../public/render_feature_runtime.hpp"
#include "frame_context.hpp"

namespace aeg::pipeline { class RenderGraphBuilder; }

namespace aeg::render_graph {
/** One strongly owned identity per build attempt; rollback never reuses it. Engine only. */
struct RenderFeatureBuildIdentity final {};
struct RenderFeatureBuildScope final {
    std::shared_ptr<const RenderFeatureBuildIdentity> identity{};
    const rhi::Device* device{nullptr};
    const FrameContext* frame{nullptr};
    uint64_t frame_index{0}, graph_id{0};
    friend bool operator==(const RenderFeatureBuildScope&, const RenderFeatureBuildScope&) = default;
};
/** Fixed operation variants from the actual committed RenderIntent. Engine only. */
enum class RenderSceneOperationKind : uint8_t {
    kClearColor, kCopyTexture, kDrawScene, kDrawDepthScene, kDrawGBufferScene,
    kDeferredLightingScene, kPostProcessScene, kLightCullingScene, kDrawClusteredScene,
};
/** Owning provenance values copied from the projector's current operation. No author Pass identity. */
struct RenderSceneOperationSource final {
    RenderWorkItem work{};
    feature::RenderResourceOwner owner{};
    uint64_t view_key{0};
    RenderSceneOperationKind kind{RenderSceneOperationKind::kClearColor};
    feature::RenderDepthSceneProjection depth_projection{feature::RenderDepthSceneProjection::kCamera};
};
/** Evidence for the latest depth contents, obtained from a successful scene lowering. */
struct RenderSceneDepthProduction final {
    RenderFeatureBuildScope scope{};
    feature::RenderSceneReference scene{};
    feature::RenderTextureReference reference{};
    RenderGraphTexture texture{};
    feature::RenderDepthSceneProjection projection{feature::RenderDepthSceneProjection::kCamera};
    std::shared_ptr<const void> selected_snapshot{};
    /** Unique within the strongly owned build scope; preserving Load retains it. */
    uint64_t contents_revision{0};
    [[nodiscard]] bool same_contents(const RenderSceneDepthProduction& other) const noexcept {
        return contents_revision != 0 && scope == other.scope && contents_revision == other.contents_revision;
    }
    [[nodiscard]] bool matches(const RenderFeatureBuildScope& expected_scope,
        const feature::RenderSceneReference& expected_scene, const feature::RenderTextureReference& expected_reference,
        RenderGraphTexture expected_texture, feature::RenderDepthSceneProjection expected_projection,
        const void* expected_snapshot) const noexcept {
        return scope == expected_scope && scene.id == expected_scene.id && scene.version == expected_scene.version &&
            reference.id == expected_reference.id && reference.version == expected_reference.version &&
            reference.external == expected_reference.external && texture == expected_texture &&
            projection == expected_projection && selected_snapshot.get() == expected_snapshot;
    }
};
/** Typed engine lease. Concrete resources and callbacks remain owned by Renderer. */
struct RenderLightListProduction final {
    feature::RenderResourceOwner owner{};
    feature::InvocationToken invocation{};
    size_t output_index{0};
    uint32_t operation_index{0};
    friend bool operator==(const RenderLightListProduction&, const RenderLightListProduction&) = default;
};
class RenderLightListLease {
public:
    virtual ~RenderLightListLease() = default;
    [[nodiscard]] const RenderFeatureBuildScope& scope() const noexcept { return scope_; }
    [[nodiscard]] const RenderLightListProduction& production() const noexcept { return production_; }
protected:
    RenderLightListLease(RenderFeatureBuildScope scope, RenderLightListProduction production)
        : scope_(std::move(scope)), production_(std::move(production)) {}
private:
    const RenderFeatureBuildScope scope_;
    const RenderLightListProduction production_;
};
/** Resolved engine-only attachments; their owning graph outlives scene lowering. */
struct RenderSceneDrawTargets final {
    RenderGraphTexture color{};
    rhi::TextureDesc color_description{};
    RenderGraphTexture depth{};
    rhi::TextureDesc depth_description{};
    RenderGraphTexture shadow{};
    rhi::TextureDesc shadow_description{};
    /** GBuffer MRT1/2, DeferredLighting sampled albedo/normal, or PostProcess sampled HDR at index0. */
    std::array<RenderGraphTexture, 2> additional_colors{};
    std::array<rhi::TextureDesc, 2> additional_color_descriptions{};
    /** Borrowed only for the duration of lowering; a retained bundle copies the evidence. */
    const RenderSceneDepthProduction* depth_production{nullptr};
    const RenderSceneDepthProduction* shadow_production{nullptr};
    /** Borrowed during lowering; registered native commands retain their own value copy. */
    const RenderSceneOperationSource* source{nullptr};
};
/** Engine-only seam implemented by Renderer; author intents carry no builders or callbacks. */
class RenderSceneDrawLowerer {
public:
    virtual ~RenderSceneDrawLowerer() = default;
    /** Returned owning lease is retained by the graph until native frame completion. */
    [[nodiscard]] virtual foundation::ResultValue<std::shared_ptr<const void>> lower(pipeline::RenderGraphBuilder& builder,
        const feature::RenderDrawScene& draw, const RenderSceneDrawTargets& targets,
        const feature::RenderResourceOwner& owner,
        uint64_t frame_index) const = 0;
    [[nodiscard]] virtual foundation::ResultValue<std::shared_ptr<const void>> lower(pipeline::RenderGraphBuilder& builder,
        const feature::RenderDrawDepthScene& draw, const RenderSceneDrawTargets& targets,
        const feature::RenderResourceOwner& owner, uint64_t frame_index) const = 0;
    [[nodiscard]] virtual foundation::ResultValue<std::shared_ptr<const void>> lower(pipeline::RenderGraphBuilder& builder,
        const feature::RenderDrawGBufferScene& draw, const RenderSceneDrawTargets& targets,
        const feature::RenderResourceOwner& owner, uint64_t frame_index) const = 0;
    [[nodiscard]] virtual foundation::ResultValue<std::shared_ptr<const void>> lower(pipeline::RenderGraphBuilder& builder,
        const feature::RenderDeferredLightingScene& draw, const RenderSceneDrawTargets& targets,
        const feature::RenderResourceOwner& owner, uint64_t frame_index) const = 0;
    [[nodiscard]] virtual foundation::ResultValue<std::shared_ptr<const void>> lower(pipeline::RenderGraphBuilder& builder,
        const feature::RenderPostProcessScene& draw, const RenderSceneDrawTargets& targets,
        const feature::RenderResourceOwner& owner, uint64_t frame_index) const = 0;
    [[nodiscard]] virtual foundation::ResultValue<std::shared_ptr<const RenderLightListLease>> lower(
        pipeline::RenderGraphBuilder& builder, const feature::RenderLightCullingScene& draw,
        const RenderSceneDrawTargets& targets, const feature::RenderResourceOwner& owner,
        const RenderFeatureBuildScope& scope, const RenderLightListProduction& production) const = 0;
    [[nodiscard]] virtual foundation::Result lower(pipeline::RenderGraphBuilder& builder,
        const feature::RenderDrawClusteredScene& draw, const RenderSceneDrawTargets& targets,
        const feature::RenderResourceOwner& owner, const RenderFeatureBuildScope& scope,
        const RenderLightListLease& light_list) const = 0;
};
}  // namespace aeg::render_graph
