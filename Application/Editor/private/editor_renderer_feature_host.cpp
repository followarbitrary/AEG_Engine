#include "editor_renderer_feature_host.hpp"
#include "feature_clock_host.hpp"

#include <new>
#include <utility>

namespace aeg::application {

struct EditorRendererFeatureHost::Impl final {
    explicit Impl(const feature::ClockHost& clock_host) : registry(clock_host) {}
    feature::FeatureRegistry registry;
    feature::FeatureRuntime execution{registry};
    runtime::FeatureGraphRuntimeHost host{execution, registry};
    feature::FeatureCatalog catalog{};
    feature::FeatureGraphDocument document{};

    [[nodiscard]] foundation::Result validate(const feature::FeatureGraphDocument& graph) const {
        if (graph.asset_uuid.empty() || graph.schema_version != 1 || graph.graph_version == 0)
            return foundation::invalid_argument("Scene FeatureGraph identity, schema and graph version are required");
        if (graph.nodes.size() > 1 || !graph.edges.empty() || !graph.process_subgraphs.empty())
            return foundation::invalid_argument("A Scene profile is one complete Render Feature or an empty graph");
        for (const auto& node : graph.nodes) {
            const auto* definition = registry.find_definition(node.definition_id);
            if (definition == nullptr || definition->version != node.definition_version)
                return foundation::invalid_argument("Scene FeatureGraph definition is missing or stale");
            const auto& contract = definition->contract;
            if (definition->owner_module != feature::ModuleIdentity{"com.aeg.renderer"} ||
                contract.domains != feature::to_mask(feature::FeatureDomain::kRender) ||
                contract.scope.id != feature::ScopeId{"Frame"} ||
                contract.clock.id != renderer::kSceneRenderProfileClock ||
                contract.execution != feature::ExecutionKind::kContinuous)
                return foundation::invalid_argument("Scene profile requires an engine-owned Render/Frame contract");
        }
        return foundation::Result::success();
    }
};

EditorRendererFeatureHost::EditorRendererFeatureHost(const feature::ClockHost& clock_host)
    : impl_(std::make_unique<Impl>(clock_host)) {}
EditorRendererFeatureHost::~EditorRendererFeatureHost() = default;

foundation::ResultValue<std::unique_ptr<EditorRendererFeatureHost>> EditorRendererFeatureHost::create(
    renderer::SceneRenderProfile profile) {
    try {
        const auto clock_host = feature::make_clock_host();
        if (!clock_host) return {clock_host.result, {}};
        auto result = std::unique_ptr<EditorRendererFeatureHost>(new EditorRendererFeatureHost{clock_host.value});
        if (const auto registered = renderer::register_scene_render_profiles(result->impl_->registry); !registered)
            return {registered, {}};
        auto catalog = renderer::scene_render_profile_catalog();
        if (!catalog) return {catalog.result, {}};
        if (catalog.value.contract_digest() != result->impl_->registry.contract_digest())
            return {foundation::invalid_state("Scene Feature catalog and executable definitions disagree"), {}};
        result->impl_->catalog = std::move(catalog.value);
        const auto graph = renderer::scene_render_profile_graph(profile);
        if (!graph) return {graph.result, {}};
        if (const auto published = result->publish(graph.value); !published) return {published, {}};
        return {foundation::Result::success(), std::move(result)};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("Scene Feature host allocation failed"), {}};
    }
}

foundation::Result EditorRendererFeatureHost::publish(const feature::FeatureGraphDocument& graph) {
    if (const auto valid = impl_->validate(graph); !valid) return valid;
    try {
        // Prepare the owning source copy before the Runtime publication commits.
        // apply_pending replaces the whole batch; no preceding Scene graph is restaged.
        auto next_document = graph;
        if (const auto staged = impl_->host.stage(next_document); !staged) return staged;
        if (const auto published = impl_->host.apply_pending(); !published) return published;
        impl_->document = std::move(next_document);
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Scene Feature publication allocation failed");
    }
}

foundation::Result EditorRendererFeatureHost::tick() {
    if (impl_->document.nodes.empty()) return foundation::Result::success();
    return impl_->execution.tick(renderer::kSceneRenderProfileClock);
}

foundation::ResultValue<std::shared_ptr<const std::vector<feature::RenderFeatureOutput>>>
EditorRendererFeatureHost::capture() const {
    auto captured = impl_->host.capture_render_outputs();
    if (!captured) return {captured.result, {}};
    try {
        return {foundation::Result::success(),
                std::make_shared<const std::vector<feature::RenderFeatureOutput>>(std::move(captured.value))};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("Scene Feature output capture allocation failed"), {}};
    }
}

foundation::ResultValue<std::vector<feature::RenderResourceOwner>> EditorRendererFeatureHost::capture_owners() const {
    const auto* graph = impl_->host.active();
    if (graph == nullptr) return {foundation::not_ready("Scene Feature graph has not been published"), {}};
    return impl_->execution.capture_render_resource_owners(*graph);
}

foundation::ResultValue<runtime::FeatureRuntimeExecutionSnapshot> EditorRendererFeatureHost::inspect() const {
    return impl_->host.inspect_execution(impl_->document.asset_uuid);
}

const feature::FeatureCatalog& EditorRendererFeatureHost::catalog() const noexcept { return impl_->catalog; }
const feature::FeatureGraphDocument& EditorRendererFeatureHost::document() const noexcept { return impl_->document; }

}  // namespace aeg::application
