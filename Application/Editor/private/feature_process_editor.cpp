/** @file feature_process_editor.cpp */
#include "feature_process_editor.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace aeg::application {
namespace {
constexpr std::string_view k_layout_namespace = "aeg.editor.process-layout.";
constexpr std::string_view k_layout_prefix = "aeg.editor.process-layout.v1:";
using Json = nlohmann::json;

struct LayoutRecord final {
    std::size_t extension_index{};
    feature::FeatureNodeId owner{};
    ProcessEditorLayout layout{};
};

foundation::ResultValue<const feature::ProcessSubgraph*> find_process(
    const feature::FeatureGraphDocument& graph, feature::FeatureNodeId owner) {
    if (!owner.is_valid())
        return {foundation::invalid_argument("Process canvas owner identity is invalid"), {}};
    const auto node_count = std::count_if(graph.nodes.begin(), graph.nodes.end(),
                                        [owner](const auto& node) { return node.node_id == owner; });
    if (node_count != 1)
        return {foundation::invalid_argument("Process canvas requires one declared owner node"), {}};
    const feature::ProcessSubgraph* process = nullptr;
    for (const auto& value : graph.process_subgraphs) {
        if (value.owner_node != owner) continue;
        if (process)
            return {foundation::invalid_argument("Process canvas owner has duplicate subgraphs"), {}};
        process = &value;
    }
    if (!process)
        return {foundation::not_found("Process canvas subgraph was not found"), {}};
    std::unordered_set<std::string> names;
    for (const auto& name : process->states) {
        if (name.empty() || !names.insert(name).second)
            return {foundation::invalid_argument("Process canvas states must be declared and unique"), {}};
    }
    return {foundation::Result::success(), process};
}

bool finite_point(const std::pair<float, float>& point) {
    return std::isfinite(point.first) && std::isfinite(point.second);
}

foundation::Result validate_layout(const feature::ProcessSubgraph& process,
                                   const ProcessEditorLayout& layout) {
    if (!std::isfinite(layout.zoom) || layout.zoom <= 0.0f ||
        !finite_point(layout.pan_offset) || !std::isfinite(layout.grid_size) || layout.grid_size <= 0.0f)
        return foundation::invalid_argument("Process canvas zoom, pan and grid must be finite and positive");
    const std::unordered_set<std::string> names(process.states.begin(), process.states.end());
    for (const auto& [name, point] : layout.state_positions) {
        if (!names.contains(name) || !finite_point(point))
            return foundation::invalid_argument("Process canvas positions require declared states and finite coordinates");
    }
    for (const auto& [name, color] : layout.state_colors) {
        (void)color;
        if (!names.contains(name))
            return foundation::invalid_argument("Process canvas colors require declared states");
    }
    if (!layout.transition_control_points.empty() &&
        layout.transition_control_points.size() != process.transitions.size())
        return foundation::invalid_argument("Process canvas control points must match the authored transitions");
    for (const auto& controls : layout.transition_control_points) {
        for (const auto& point : controls) {
            if (!finite_point(point))
                return foundation::invalid_argument("Process canvas control points must be finite");
        }
    }
    return foundation::Result::success();
}

float read_float(const Json& value) {
    if (!value.is_number()) throw std::invalid_argument("Process canvas number is malformed");
    const double number = value.get<double>();
    constexpr double limit = static_cast<double>(std::numeric_limits<float>::max());
    if (!std::isfinite(number) || number < -limit || number > limit)
        throw std::invalid_argument("Process canvas number is outside the finite coordinate range");
    return static_cast<float>(number);
}

bool read_bool(const Json& value) {
    if (!value.is_boolean()) throw std::invalid_argument("Process canvas flag is malformed");
    return value.get<bool>();
}

std::pair<float, float> read_point(const Json& value) {
    if (!value.is_array() || value.size() != 2)
        throw std::invalid_argument("Process canvas coordinate pair is malformed");
    return {read_float(value[0]), read_float(value[1])};
}

LayoutRecord decode_record(std::string_view encoded, std::size_t index) {
    if (!encoded.starts_with(k_layout_prefix))
        throw std::invalid_argument("Process canvas metadata version is unsupported");
    encoded.remove_prefix(k_layout_prefix.size());
    std::unordered_set<std::string> keys;
    bool duplicate_key = false;
    const auto root = Json::parse(encoded.begin(), encoded.end(),
        [&keys, &duplicate_key](int, Json::parse_event_t event, Json& parsed) {
            if (event == Json::parse_event_t::key && !keys.insert(parsed.get<std::string>()).second)
                duplicate_key = true;
            return true;
        });
    if (duplicate_key || !root.is_object() || root.size() != 10 || !root.at("owner").is_string())
        throw std::invalid_argument("Process canvas metadata has duplicate or malformed fields");
    const auto owner_text = root.at("owner").get<std::string>();
    uint64_t owner_value = 0;
    const auto parsed_owner = std::from_chars(owner_text.data(), owner_text.data() + owner_text.size(), owner_value);
    if (parsed_owner.ec != std::errc{} || parsed_owner.ptr != owner_text.data() + owner_text.size() || owner_value == 0)
        throw std::invalid_argument("Process canvas metadata owner identity is malformed");
    LayoutRecord record{index, feature::FeatureNodeId(owner_value), {}};
    auto& layout = record.layout;
    layout.zoom = read_float(root.at("zoom"));
    layout.pan_offset = read_point(root.at("pan_offset"));
    layout.grid_snap = read_bool(root.at("grid_snap"));
    layout.grid_size = read_float(root.at("grid_size"));
    layout.show_descriptions = read_bool(root.at("show_descriptions"));
    layout.show_conditions = read_bool(root.at("show_conditions"));
    const auto& positions = root.at("state_positions");
    const auto& colors = root.at("state_colors");
    const auto& controls = root.at("transition_control_points");
    if (!positions.is_array() || !colors.is_array() || !controls.is_array())
        throw std::invalid_argument("Process canvas collections are malformed");
    for (const auto& position : positions) {
        if (!position.is_array() || position.size() != 3 || !position[0].is_string() ||
            !layout.state_positions.emplace(position[0].get<std::string>(),
                std::make_pair(read_float(position[1]), read_float(position[2]))).second)
            throw std::invalid_argument("Process canvas state position is malformed or duplicated");
    }
    for (const auto& color : colors) {
        if (!color.is_array() || color.size() != 2 || !color[0].is_string() || !color[1].is_string() ||
            !layout.state_colors.emplace(color[0].get<std::string>(), color[1].get<std::string>()).second)
            throw std::invalid_argument("Process canvas state color is malformed or duplicated");
    }
    for (const auto& transition : controls) {
        if (!transition.is_array()) throw std::invalid_argument("Process canvas transition controls are malformed");
        auto& points = layout.transition_control_points.emplace_back();
        for (const auto& point : transition) points.push_back(read_point(point));
    }
    return record;
}

foundation::ResultValue<std::vector<LayoutRecord>> read_records(const feature::FeatureGraphDocument& graph) {
    std::vector<LayoutRecord> records;
    std::unordered_set<feature::FeatureNodeId, feature::StableIdHash<feature::FeatureNodeIdTag>> owners;
    for (std::size_t index = 0; index < graph.extensions.size(); ++index) {
        const auto& extension = graph.extensions[index];
        if (!extension.starts_with(k_layout_namespace)) continue;
        auto record = decode_record(extension, index);
        if (!owners.insert(record.owner).second)
            return {foundation::invalid_argument("Process canvas owner metadata is duplicated"), {}};
        const auto process = find_process(graph, record.owner);
        if (!process) return {process.result, {}};
        if (const auto valid = validate_layout(*process.value, record.layout); !valid)
            return {valid, {}};
        records.push_back(std::move(record));
    }
    return {foundation::Result::success(), std::move(records)};
}

std::string encode_record(feature::FeatureNodeId owner, const ProcessEditorLayout& layout) {
    Json root{{"owner", std::to_string(owner.value())}, {"zoom", layout.zoom},
              {"pan_offset", {layout.pan_offset.first, layout.pan_offset.second}},
              {"grid_snap", layout.grid_snap}, {"grid_size", layout.grid_size},
              {"show_descriptions", layout.show_descriptions}, {"show_conditions", layout.show_conditions}};
    auto positions = Json::array();
    auto colors = Json::array();
    auto controls = Json::array();
    std::vector<std::string> names;
    for (const auto& [name, point] : layout.state_positions) { (void)point; names.push_back(name); }
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
        const auto point = layout.state_positions.at(name);
        positions.push_back({name, point.first, point.second});
    }
    names.clear();
    for (const auto& [name, color] : layout.state_colors) { (void)color; names.push_back(name); }
    std::sort(names.begin(), names.end());
    for (const auto& name : names) colors.push_back({name, layout.state_colors.at(name)});
    for (const auto& transition : layout.transition_control_points) {
        auto points = Json::array();
        for (const auto& point : transition) points.push_back({point.first, point.second});
        controls.push_back(std::move(points));
    }
    root["state_positions"] = std::move(positions);
    root["state_colors"] = std::move(colors);
    root["transition_control_points"] = std::move(controls);
    return std::string(k_layout_prefix) + root.dump();
}
}  // namespace

foundation::ResultValue<ProcessEditorLayout> read_process_editor_layout(
    const feature::FeatureGraphDocument& graph, feature::FeatureNodeId owner_node) {
    try {
        const auto process = find_process(graph, owner_node);
        if (!process) return {process.result, {}};
        const auto records = read_records(graph);
        if (!records) return {records.result, {}};
        for (const auto& record : records.value)
            if (record.owner == owner_node) return {foundation::Result::success(), record.layout};
        return {foundation::Result::success(), {}};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("Process canvas metadata allocation failed"), {}};
    } catch (const std::exception& error) {
        return {foundation::invalid_argument(error.what()), {}};
    }
}

foundation::Result write_process_editor_layout(feature::FeatureGraphDocument& graph,
    feature::FeatureNodeId owner_node, const ProcessEditorLayout& layout) {
    try {
        const auto process = find_process(graph, owner_node);
        if (!process) return process.result;
        if (const auto valid = validate_layout(*process.value, layout); !valid) return valid;
        const auto records = read_records(graph);
        if (!records) return records.result;
        const auto existing = std::find_if(records.value.begin(), records.value.end(),
                                          [owner_node](const auto& record) { return record.owner == owner_node; });
        if (existing != records.value.end() && existing->layout == layout)
            return foundation::Result::success();
        if (layout == ProcessEditorLayout{}) {
            if (existing != records.value.end())
                graph.extensions.erase(graph.extensions.begin() + static_cast<std::ptrdiff_t>(existing->extension_index));
        } else {
            const auto encoded = encode_record(owner_node, layout);
            if (existing == records.value.end()) graph.extensions.push_back(encoded);
            else graph.extensions[existing->extension_index] = encoded;
        }
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Process canvas metadata allocation failed");
    } catch (const std::exception& error) {
        return foundation::invalid_argument(error.what());
    }
}

foundation::Result erase_process_editor_layout(feature::FeatureGraphDocument& graph,
                                               feature::FeatureNodeId owner_node) {
    try {
        const auto records = read_records(graph);
        if (!records) return records.result;
        const auto existing = std::find_if(records.value.begin(), records.value.end(),
                                          [owner_node](const auto& record) { return record.owner == owner_node; });
        if (existing != records.value.end())
            graph.extensions.erase(graph.extensions.begin() + static_cast<std::ptrdiff_t>(existing->extension_index));
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Process canvas metadata allocation failed");
    } catch (const std::exception& error) {
        return foundation::invalid_argument(error.what());
    }
}

void auto_layout_state_machine(const aeg::feature::ProcessStateMachine& machine,
                               ProcessEditorLayout& layout,
                               float canvas_width,
                               float canvas_height) {
    if (machine.states.empty() || canvas_width < 100.0f || canvas_height < 100.0f) {
        return;
    }

    constexpr float k_spring = 0.01f;
    constexpr float k_repulsion = 5000.0f;
    constexpr float k_damping = 0.8f;
    constexpr int k_iterations = 100;
    constexpr float k_time_step = 1.0f;

    for (std::size_t index = 0; index < machine.states.size(); ++index) {
        const auto& state = machine.states[index];
        auto& position = layout.state_positions[state.name];
        if (position.first == 0.0f && position.second == 0.0f) {
            const auto column = static_cast<float>(index % 4);
            const auto row = static_cast<float>(index / 4);
            position.first = std::clamp(canvas_width * 0.5f + (column - 1.5f) * 100.0f,
                                        50.0f, canvas_width - 50.0f);
            position.second = std::clamp(canvas_height * 0.5f + (row - 0.5f) * 100.0f,
                                         50.0f, canvas_height - 50.0f);
        }
    }

    std::unordered_map<std::string, std::vector<std::string>> adjacency;
    for (const auto& transition : machine.transitions) {
        adjacency[transition.from_state].push_back(transition.to_state);
    }
    std::vector<std::pair<float, float>> velocities(machine.states.size(), {0.0f, 0.0f});

    for (int iteration = 0; iteration < k_iterations; ++iteration) {
        for (std::size_t i = 0; i < machine.states.size(); ++i) {
            const auto& state = machine.states[i];
            const auto position_i = layout.state_positions.at(state.name);
            float force_x = 0.0f;
            float force_y = 0.0f;

            for (std::size_t j = 0; j < machine.states.size(); ++j) {
                if (i == j) {
                    continue;
                }
                const auto position_j = layout.state_positions.at(machine.states[j].name);
                const float dx = position_i.first - position_j.first;
                const float dy = position_i.second - position_j.second;
                const float distance = std::sqrt(dx * dx + dy * dy);
                if (distance > 1.0f) {
                    const float repulsion = k_repulsion / (distance * distance);
                    force_x += (dx / distance) * repulsion;
                    force_y += (dy / distance) * repulsion;
                }
            }

            const auto adjacent = adjacency.find(state.name);
            if (adjacent != adjacency.end()) {
                for (const auto& neighbor_name : adjacent->second) {
                    const auto neighbor = layout.state_positions.find(neighbor_name);
                    if (neighbor == layout.state_positions.end()) {
                        continue;
                    }
                    const float dx = neighbor->second.first - position_i.first;
                    const float dy = neighbor->second.second - position_i.second;
                    const float distance = std::sqrt(dx * dx + dy * dy);
                    if (distance > 1.0f) {
                        const float spring = k_spring * distance;
                        force_x += (dx / distance) * spring;
                        force_y += (dy / distance) * spring;
                    }
                }
            }
            velocities[i].first = (velocities[i].first + force_x * k_time_step) * k_damping;
            velocities[i].second = (velocities[i].second + force_y * k_time_step) * k_damping;
        }

        for (std::size_t i = 0; i < machine.states.size(); ++i) {
            auto& position = layout.state_positions[machine.states[i].name];
            position.first = std::clamp(position.first + velocities[i].first * k_time_step,
                                        50.0f, canvas_width - 50.0f);
            position.second = std::clamp(position.second + velocities[i].second * k_time_step,
                                         50.0f, canvas_height - 50.0f);
        }
    }

    layout.transition_control_points.resize(machine.transitions.size());
}

}  // namespace aeg::application
