/**
 * @file runtime_feature_inspect_boundary_smoke.cpp
 * @brief Verifies the Runtime Inspect host is not propagated as a public include.
 */
#if __has_include("feature_graph_runtime_host.hpp")
#error "Runtime Inspect host must remain behind the Runtime private include boundary"
#endif
#if __has_include("runtime_feature_control.hpp")
#error "Runtime Feature control must remain behind the Runtime private include boundary"
#endif

int main() {
    return 0;
}
