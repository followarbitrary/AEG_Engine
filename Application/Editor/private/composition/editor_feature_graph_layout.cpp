#include "editor_feature_graph_layout.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace aeg::application {
namespace {
constexpr std::string_view kPrefix{"aeg.editor.feature-layout.v1:"};
using Json = nlohmann::json;

float number(const Json& value) {
    if (!value.is_number()) throw std::invalid_argument("Feature canvas coordinate is not numeric");
    const auto parsed = value.get<double>();
    if (!std::isfinite(parsed) || std::abs(parsed) > static_cast<double>(std::numeric_limits<float>::max()))
        throw std::invalid_argument("Feature canvas coordinate is outside the finite range");
    return static_cast<float>(parsed);
}

foundation::Result validate(const feature::FeatureGraphDocument& graph, const FeatureEditorLayout& layout) {
    if (!std::isfinite(layout.zoom) || layout.zoom < 0.2f || layout.zoom > 3.0f ||
        !std::isfinite(layout.pan_offset.first) || !std::isfinite(layout.pan_offset.second))
        return foundation::invalid_argument("Feature canvas zoom/pan is outside the supported finite range");
    for (const auto& [id, point] : layout.node_positions) {
        if (id == 0 || !std::isfinite(point.first) || !std::isfinite(point.second) ||
            std::count_if(graph.nodes.begin(), graph.nodes.end(), [id](const auto& node) {
                return node.node_id.value() == id;
            }) != 1)
            return foundation::invalid_argument("Feature canvas positions require unique declared nodes and finite coordinates");
    }
    return foundation::Result::success();
}

FeatureEditorLayout decode(std::string_view text) {
    text.remove_prefix(kPrefix.size());
    std::unordered_set<std::string> fields;
    bool duplicate = false;
    const auto root = Json::parse(text.begin(), text.end(), [&fields, &duplicate](int, Json::parse_event_t event, Json& value) {
        if (event == Json::parse_event_t::key && !fields.insert(value.get<std::string>()).second) duplicate = true;
        return true;
    });
    if (duplicate || !root.is_object() || root.size() != 3 || !root.at("node_positions").is_array() ||
        !root.at("pan_offset").is_array() || root.at("pan_offset").size() != 2)
        throw std::invalid_argument("Feature canvas layout fields are malformed or duplicated");
    FeatureEditorLayout layout;
    layout.zoom = number(root.at("zoom"));
    layout.pan_offset = {number(root.at("pan_offset")[0]), number(root.at("pan_offset")[1])};
    for (const auto& position : root.at("node_positions")) {
        if (!position.is_array() || position.size() != 3 || !position[0].is_string())
            throw std::invalid_argument("Feature canvas node position is malformed");
        const auto id_text = position[0].get<std::string>();
        uint64_t id{};
        const auto parsed = std::from_chars(id_text.data(), id_text.data() + id_text.size(), id);
        if (parsed.ec != std::errc{} || parsed.ptr != id_text.data() + id_text.size() || id == 0 ||
            !layout.node_positions.emplace(id, std::pair{number(position[1]), number(position[2])}).second)
            throw std::invalid_argument("Feature canvas node identity is malformed or duplicated");
    }
    return layout;
}
} // namespace

std::pair<float, float> default_feature_node_position(std::size_t index) noexcept {
    return {40.0f + static_cast<float>(index % 4) * 350.0f,
            60.0f + static_cast<float>(index / 4) * 200.0f};
}

foundation::ResultValue<FeatureEditorLayout> read_feature_editor_layout(const feature::FeatureGraphDocument& graph) {
    try {
        std::optional<FeatureEditorLayout> layout;
        for (const auto& extension : graph.extensions) {
            if (!extension.starts_with(kPrefix)) continue;
            if (layout) return {foundation::invalid_argument("Feature canvas layout record is duplicated"), {}};
            layout = decode(extension);
        }
        auto result = layout.value_or(FeatureEditorLayout{});
        if (const auto valid = validate(graph, result); !valid) return {valid, {}};
        return {foundation::Result::success(), std::move(result)};
    } catch (const std::bad_alloc&) { return {foundation::out_of_memory("Feature canvas layout allocation failed"), {}}; }
      catch (const std::exception& error) { return {foundation::invalid_argument(error.what()), {}}; }
}

foundation::Result write_feature_editor_layout(feature::FeatureGraphDocument& graph, const FeatureEditorLayout& layout) {
    try {
        if (const auto previous = read_feature_editor_layout(graph); !previous) return previous.result;
        if (const auto valid = validate(graph, layout); !valid) return valid;
        Json root{{"zoom", layout.zoom}, {"pan_offset", {layout.pan_offset.first, layout.pan_offset.second}},
                  {"node_positions", Json::array()}};
        for (const auto& [id, point] : layout.node_positions)
            root["node_positions"].push_back({std::to_string(id), point.first, point.second});
        auto encoded = std::string{kPrefix} + root.dump();
        const auto found = std::find_if(graph.extensions.begin(), graph.extensions.end(), [](const auto& extension) {
            return extension.starts_with(kPrefix);
        });
        if (found == graph.extensions.end()) graph.extensions.push_back(std::move(encoded));
        else *found = std::move(encoded);
        return foundation::Result::success();
    } catch (const std::bad_alloc&) { return foundation::out_of_memory("Feature canvas layout allocation failed"); }
      catch (const std::exception& error) { return foundation::invalid_argument(error.what()); }
}
} // namespace aeg::application
