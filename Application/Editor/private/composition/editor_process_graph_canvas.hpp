/** @brief Editor-only visual subview of an owner Feature's Process graph. */
#pragma once

#include <string_view>

#include "editor_feature_graph_document.hpp"
#include "../feature_process_editor.hpp"

namespace aeg::application {

class EditorProcessGraphCanvas final {
public:
    void draw(EditorFeatureGraphDocument& document, feature::FeatureNodeId owner, std::string& feedback);
    [[nodiscard]] std::string_view selected_state() const noexcept { return selected_state_; }

private:
    std::shared_ptr<const feature::FeatureGraphDocument> snapshot_{};
    std::shared_ptr<const feature::FeatureGraphDocument> selection_snapshot_{};
    feature::FeatureNodeId owner_{};
    ProcessEditorLayout layout_{};
    std::string selected_state_{};
    std::string dragging_state_{};
    std::string linking_from_{};
    std::pair<float, float> drag_start_{};
    std::pair<float, float> pan_start_{};
    bool panning_{false};
};

} // namespace aeg::application
