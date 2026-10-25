/** @brief Verifies public overlay execution does not expose retired Feature assembly. */
#include "imgui_renderer.hpp"
#include "imgui_render_execution.hpp"

#include <type_traits>

#if __has_include("imgui/imgui_render_execution.hpp")
#error "EditorFramework must expose the public execution contract without private include directories"
#endif

namespace aeg::pipeline {
class PipelineRegistry;
}
namespace aeg::editor {
class ImGuiRenderFeature;

namespace boundary_test {
template <typename T>
concept Complete = requires { sizeof(T); };

template <typename Registry, typename Renderer>
concept HasRegistration = requires(Registry& registry, Renderer& renderer) {
    register_imgui_render_feature(registry, renderer);
};

template <typename Registry>
concept HasUnregistration = requires(Registry& registry) {
    unregister_imgui_render_feature(registry);
};

static_assert(!Complete<ImGuiRenderFeature>, "retired ImGui Feature identity must remain unavailable");
static_assert(Complete<detail::ImGuiRenderExecution>, "fixed overlay execution uses the public EditorFramework contract");
static_assert(!HasRegistration<pipeline::PipelineRegistry, ImGuiRenderer>,
              "ImGui registry assembly must remain private");
static_assert(!HasUnregistration<pipeline::PipelineRegistry>, "ImGui registry teardown must remain private");
static_assert(std::is_constructible_v<ImGuiRenderer, rhi::Device&, ImGuiTextureRegistry&>);
static_assert(std::is_default_constructible_v<ImGuiRenderTargetDesc>);
}  // namespace boundary_test
}  // namespace aeg::editor

int main() { return 0; }
