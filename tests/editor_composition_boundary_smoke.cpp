/**
 * @file editor_composition_boundary_smoke.cpp
 * @brief Verifies Editor composition implementation headers are not propagated.
 */

#if __has_include("editor_feature_graph_model.hpp")
#error "Editor composition model must remain behind the Editor private include boundary"
#endif

#if __has_include("editor_feature_graph_document.hpp")
#error "Editor composition document must remain behind the Editor private include boundary"
#endif

#if __has_include("editor_feature_graph_panel.hpp")
#error "Editor composition panels must remain behind the Editor private include boundary"
#endif

#if __has_include("editor_node_definition.hpp")
#error "Editor node-definition contracts must remain behind the Editor private include boundary"
#endif

#if __has_include("editor_parameter_widget.hpp")
#error "Editor parameter widgets must remain behind the Editor private include boundary"
#endif

int main() { return 0; }
