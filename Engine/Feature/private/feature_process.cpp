/** @file feature_process.cpp */
#include "feature_process.hpp"

#include <algorithm>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace aeg::feature {

namespace {

foundation::Result invalid_argument(std::string message) {
    return foundation::Result::failure(foundation::ResultCode::kInvalidArgument, std::move(message));
}

/**
 * @brief Check if a state name exists in the state machine.
 */
bool state_exists(const ProcessStateMachine& machine, const std::string& state_name) {
    return std::any_of(machine.states.begin(), machine.states.end(),
                       [&](const ProcessStateNode& state) { return state.name == state_name; });
}

/**
 * @brief Build adjacency list for graph traversal.
 */
std::unordered_map<std::string, std::vector<std::string>> build_adjacency_list(
    const ProcessStateMachine& machine) {

    std::unordered_map<std::string, std::vector<std::string>> adjacency;

    for (const auto& transition : machine.transitions) {
        adjacency[transition.from_state].push_back(transition.to_state);
    }

    return adjacency;
}

/**
 * @brief Perform depth-first search for cycle detection.
 */
bool has_cycle_dfs(const std::string& node,
                   const std::unordered_map<std::string, std::vector<std::string>>& adjacency,
                   std::unordered_set<std::string>& visited,
                   std::unordered_set<std::string>& recursion_stack,
                   std::vector<std::string>& current_path,
                   std::vector<std::vector<std::string>>& cycles) {

    visited.insert(node);
    recursion_stack.insert(node);
    current_path.push_back(node);

    const auto it = adjacency.find(node);
    if (it != adjacency.end()) {
        for (const auto& neighbor : it->second) {
            if (recursion_stack.find(neighbor) != recursion_stack.end()) {
                // Found cycle - extract it
                std::vector<std::string> cycle;
                auto cycle_start = std::find(current_path.begin(), current_path.end(), neighbor);
                if (cycle_start != current_path.end()) {
                    cycle.assign(cycle_start, current_path.end());
                    cycle.push_back(neighbor);
                    cycles.push_back(std::move(cycle));
                }
            } else if (visited.find(neighbor) == visited.end()) {
                has_cycle_dfs(neighbor, adjacency, visited, recursion_stack, current_path, cycles);
            }
        }
    }

    current_path.pop_back();
    recursion_stack.erase(node);
    return !cycles.empty();
}

}  // namespace

foundation::Result validate_process_state_machine(const ProcessStateMachine& machine) {
    // Check for empty state machine
    if (machine.states.empty()) {
        return invalid_argument("state machine has no states");
    }

    // Validate initial state exists
    if (machine.initial_state.empty()) {
        return invalid_argument("state machine has no initial state");
    }
    if (!state_exists(machine, machine.initial_state)) {
        return invalid_argument("initial state '" + machine.initial_state + "' does not exist");
    }

    // Validate error state if specified
    if (!machine.error_state.empty() && !state_exists(machine, machine.error_state)) {
        return invalid_argument("error state '" + machine.error_state + "' does not exist");
    }

    // Validate compensation state if specified
    if (!machine.compensation_state.empty() && !state_exists(machine, machine.compensation_state)) {
        return invalid_argument("compensation state '" + machine.compensation_state + "' does not exist");
    }

    // Check for duplicate state names
    std::unordered_set<std::string> state_names;
    for (const auto& state : machine.states) {
        if (state.name.empty()) {
            return invalid_argument("state has empty name");
        }
        if (!state_names.insert(state.name).second) {
            return invalid_argument("duplicate state name '" + state.name + "'");
        }
    }

    // Validate exactly one entry state
    const auto entry_count = std::count_if(machine.states.begin(), machine.states.end(),
                                          [](const ProcessStateNode& s) { return s.is_entry; });
    if (entry_count == 0) {
        return invalid_argument("no entry state marked");
    }
    if (entry_count > 1) {
        return invalid_argument("multiple entry states marked");
    }
    const auto entry = std::find_if(machine.states.begin(), machine.states.end(),
                                    [](const ProcessStateNode& state) { return state.is_entry; });
    if (entry != machine.states.end() && entry->name != machine.initial_state) {
        return invalid_argument("initial state does not match the marked entry state");
    }

    // Validate at least one terminal state (unless cycles exist)
    const auto terminal_count = std::count_if(machine.states.begin(), machine.states.end(),
                                             [](const ProcessStateNode& s) { return s.is_terminal; });
    if (terminal_count == 0 && find_cycles(machine).empty()) {
        return invalid_argument("no terminal state and no cycles (process would deadlock)");
    }

    // Validate all transitions reference valid states
    for (const auto& transition : machine.transitions) {
        if (transition.from_state.empty()) {
            return invalid_argument("transition has empty source state");
        }
        if (transition.to_state.empty()) {
            return invalid_argument("transition has empty target state");
        }
        if (!state_exists(machine, transition.from_state)) {
            return invalid_argument("transition source state '" + transition.from_state + "' does not exist");
        }
        if (!state_exists(machine, transition.to_state)) {
            return invalid_argument("transition target state '" + transition.to_state + "' does not exist");
        }
    }

    // Check for unreachable states (excluding error/compensation)
    const auto unreachable = find_unreachable_states(machine);
    for (const auto& state_name : unreachable) {
        if (state_name != machine.error_state && state_name != machine.compensation_state) {
            return invalid_argument("state '" + state_name + "' is unreachable");
        }
    }

    return foundation::Result::success();
}

std::vector<std::string> find_unreachable_states(const ProcessStateMachine& machine) {
    std::unordered_set<std::string> reachable;
    std::queue<std::string> to_visit;

    // Start from initial state
    to_visit.push(machine.initial_state);
    reachable.insert(machine.initial_state);

    // BFS to find all reachable states
    const auto adjacency = build_adjacency_list(machine);
    while (!to_visit.empty()) {
        const auto current = to_visit.front();
        to_visit.pop();

        const auto it = adjacency.find(current);
        if (it != adjacency.end()) {
            for (const auto& neighbor : it->second) {
                if (reachable.insert(neighbor).second) {
                    to_visit.push(neighbor);
                }
            }
        }
    }

    // Collect unreachable states
    std::vector<std::string> unreachable;
    for (const auto& state : machine.states) {
        if (reachable.find(state.name) == reachable.end()) {
            unreachable.push_back(state.name);
        }
    }

    return unreachable;
}

std::vector<std::vector<std::string>> find_cycles(const ProcessStateMachine& machine) {
    std::unordered_set<std::string> visited;
    std::unordered_set<std::string> recursion_stack;
    std::vector<std::string> current_path;
    std::vector<std::vector<std::string>> cycles;

    const auto adjacency = build_adjacency_list(machine);

    for (const auto& state : machine.states) {
        if (visited.find(state.name) == visited.end()) {
            has_cycle_dfs(state.name, adjacency, visited, recursion_stack, current_path, cycles);
        }
    }

    return cycles;
}

std::vector<std::string> extract_transition_strings(const ProcessStateMachine& machine) {
    std::vector<std::string> result;
    result.reserve(machine.transitions.size());

    for (const auto& transition : machine.transitions) {
        result.push_back(transition.from_state + "->" + transition.to_state);
    }

    return result;
}

foundation::Result validate_transition_condition(
    const TransitionCondition& condition,
    const std::vector<PortContract>& ports) {

    switch (condition.type) {
        case TransitionConditionType::kAlways:
        case TransitionConditionType::kOnSuccess:
        case TransitionConditionType::kOnFailure:
            // No additional validation needed
            break;

        case TransitionConditionType::kOnTimeout:
            if (condition.timeout_ticks == 0) {
                return invalid_argument("timeout condition requires non-zero timeout_ticks");
            }
            break;

        case TransitionConditionType::kOnPortData: {
            if (!condition.port.is_valid()) {
                return invalid_argument("port data condition requires valid port ID");
            }
            // Verify port exists
            const auto port_it = std::find_if(ports.begin(), ports.end(),
                [&](const PortContract& p) { return p.id == condition.port; });
            if (port_it == ports.end()) {
                return invalid_argument("port data condition references non-existent port");
            }
            if (port_it->direction != PortDirection::kInput) {
                return invalid_argument("port data condition must reference input port");
            }
            break;
        }

        case TransitionConditionType::kCustomCondition:
            if (condition.expression.empty()) {
                return invalid_argument("custom condition requires expression");
            }
            // Future: validate expression syntax
            break;
    }

    return foundation::Result::success();
}

}  // namespace aeg::feature
