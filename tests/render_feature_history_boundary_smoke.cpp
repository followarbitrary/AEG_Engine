/**
 * @file render_feature_history_boundary_smoke.cpp
 * @brief Verifies temporal RenderFeature history stays behind the RenderGraph host boundary.
 */

#include "render_feature_runtime.hpp"

#if __has_include("render_feature_history.hpp")
#error "RenderFeatureHistory must remain behind the RenderGraph private include boundary"
#endif

int main() {
    aeg::render_graph::RenderGraphFeatureRuntimeDesc desc{};
    (void)desc;
    return 0;
}
