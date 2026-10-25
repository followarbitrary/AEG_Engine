/**
 * @file editor_process_layout_boundary_smoke.cpp
 * @brief Verifies Editor-only process layout state is not propagated publicly.
 */
#if __has_include("feature_process_editor.hpp")
#error "Editor process layout must remain behind the Editor private include boundary"
#endif

int main() {
    return 0;
}
