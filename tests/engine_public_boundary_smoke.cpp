#include <iostream>
#include <type_traits>

#include <engine_frame_orchestrator.hpp>
#include <runtime_session.hpp>
#include <renderer.hpp>
#include <render_graph.hpp>
#include <project_operation.hpp>
#ifdef AEG_PUBLIC_EDITOR_CONSUMER
#include <imgui_renderer.hpp>
#endif

template <class Inputs>
constexpr bool exposes_legacy_inputs = requires(Inputs value) { value.buffers; } ||
    requires(Inputs value) { value.state; } || requires(Inputs value) { value.configuration; } ||
    requires(Inputs value) { value.user_data; } || requires(Inputs value) { value.feature_data; };
static_assert(!exposes_legacy_inputs<aeg::render_graph::RenderGraphFeatureRuntimeDesc>);

int main() {
    int failures = 0;
#if __has_include(<pipeline.hpp>) || __has_include(<pipeline_types.hpp>) || \
    __has_include(<pipeline_configuration.hpp>) || __has_include(<pipeline_builder_backend.hpp>) || \
    __has_include(<pipeline_execution.hpp>) || __has_include(<render_pass.hpp>) || __has_include(<render_program.hpp>)
    std::cerr << "Engine consumers inherit the retired Pipeline hierarchy or its declaration backend\n";
    ++failures;
#endif
#if __has_include(<render_state_flow.hpp>)
    std::cerr << "Engine consumers inherit internal compiled Pipeline state flow\n";
    ++failures;
#endif
    if constexpr (!std::is_same_v<decltype(aeg::renderer::RenderMeshResource{}.vertex_resource),
                                  aeg::render_graph::RenderGraphBuffer>) {
        std::cerr << "Renderer resources require a second Pipeline-owned graph handle contract\n";
        ++failures;
    }
    const auto view = aeg::renderer::make_render_view_id(7);
    const auto resource = aeg::renderer::make_view_scoped_render_resource_id(
        aeg::feature::RenderResourceId{"SceneColor"}, view);
    const aeg::engine::EngineFrameDesc frame{};
    const aeg::render_graph::RenderGraphTexture texture{3, 4, 5};
    if (!view.is_valid() || !resource.is_valid() || !texture.is_valid() ||
        frame.swapchain_resource_id != aeg::engine::kSwapchainImageResourceId) ++failures;
    return failures == 0 ? 0 : 1;
}
