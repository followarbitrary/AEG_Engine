/**
 * @file renderer_feature_context_boundary_smoke.cpp
 * @brief Verifies Renderer feature execution context is not publicly propagated.
 */
#include "renderer.hpp"

#if __has_include("render_feature_context.hpp")
#error "RenderFeatureContext must remain behind the Renderer private include boundary"
#endif

int main() {
    return 0;
}
