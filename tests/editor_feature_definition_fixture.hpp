#pragma once
#include "editor_node_definition.hpp"
#include "feature_definition_fixture.hpp"
#include "editor_feature_graph_document.hpp"
#include <filesystem>
#include <fstream>

namespace aeg::tests {
inline application::NodeDefinitionDraft feature_definition_draft() {
    application::NodeDefinitionDraft draft{};
    draft.name = "Game.CustomFeature"; draft.display_name = "Custom Feature";
    draft.entry = feature_definition();
    draft.port_labels = {{feature::PortId{11}, "Input"}, {feature::PortId{12}, "Output"}};
    return draft;
}
inline std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {(std::istreambuf_iterator<char>(input)), {}};
}
}  // namespace aeg::tests
