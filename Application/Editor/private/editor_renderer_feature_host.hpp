/**
 * @file editor_renderer_feature_host.hpp
 * @brief Owns the executable engine Scene FeatureGraph used by the Editor viewport.
 */
#pragma once

#include <memory>
#include <vector>

#include "renderer_feature_profiles.hpp"
#include "../../Runtime/private/feature_graph_runtime_host.hpp"

namespace aeg::application {

/** Synchronous owner-thread host; author documents never contain native render callbacks. */
class EditorRendererFeatureHost final {
public:
    [[nodiscard]] static foundation::ResultValue<std::unique_ptr<EditorRendererFeatureHost>> create(
        renderer::SceneRenderProfile profile = renderer::SceneRenderProfile::kOpaque);
    ~EditorRendererFeatureHost();

    EditorRendererFeatureHost(const EditorRendererFeatureHost&) = delete;
    EditorRendererFeatureHost& operator=(const EditorRendererFeatureHost&) = delete;

    /** Replaces the complete active Scene graph; failure retains its published generation. */
    [[nodiscard]] foundation::Result publish(const feature::FeatureGraphDocument& graph);
    /** Runs the actual Frame-clock Feature implementation once, including in Edit mode. */
    [[nodiscard]] foundation::Result tick();
    [[nodiscard]] foundation::ResultValue<std::shared_ptr<const std::vector<feature::RenderFeatureOutput>>> capture() const;
    [[nodiscard]] foundation::ResultValue<std::vector<feature::RenderResourceOwner>> capture_owners() const;
    /** The Scene caller adds its actual project source path to this owning inspection value. */
    [[nodiscard]] foundation::ResultValue<runtime::FeatureRuntimeExecutionSnapshot> inspect() const;
    [[nodiscard]] const feature::FeatureCatalog& catalog() const noexcept;
    [[nodiscard]] const feature::FeatureGraphDocument& document() const noexcept;

private:
    struct Impl;
    explicit EditorRendererFeatureHost(const feature::ClockHost& clock_host);
    std::unique_ptr<Impl> impl_;
};

}  // namespace aeg::application
