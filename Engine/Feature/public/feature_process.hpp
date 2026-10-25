/**
 * @file feature_process.hpp
 * @brief Runtime-neutral Process state machine model and validation.
 *
 * This module contains process semantics only. Canvas coordinates, colors and
 * automatic layout belong to Application/Editor.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "feature_types.hpp"
#include "result.hpp"

namespace aeg::feature {

/**
 * @brief State machine state node with metadata.
 */
struct ProcessStateNode final {
    /** State name (must be unique within Process). */
    std::string name{};

    /** Optional user-facing state name. */
    std::string display_name{};

    /** Description of state semantics. */
    std::string description{};

    /** Whether this is an entry state (Process starts here). */
    bool is_entry{false};

    /** Whether this is a terminal state (Process ends here). */
    bool is_terminal{false};

    /** Whether this state can be retried on failure. */
    bool retryable{false};

    /** Maximum time in ticks the Process can stay in this state (0 = unlimited). */
    uint32_t max_duration_ticks{0};

};

/**
 * @brief Transition condition type.
 */
enum class TransitionConditionType : uint8_t {
    /** Unconditional transition (always taken). */
    kAlways,

    /** Transition on successful completion. */
    kOnSuccess,

    /** Transition on failure/error. */
    kOnFailure,

    /** Transition on timeout. */
    kOnTimeout,

    /** Transition when specific port receives data. */
    kOnPortData,

    /** Transition when custom condition evaluates to true. */
    kCustomCondition,
};

/**
 * @brief Transition condition with evaluation parameters.
 */
struct TransitionCondition final {
    /** Condition type. */
    TransitionConditionType type{TransitionConditionType::kAlways};

    /** Port ID for kOnPortData conditions. */
    PortId port{};

    /** Custom condition expression (format TBD, e.g., simple boolean expression). */
    std::string expression{};

    /** Timeout duration in ticks for kOnTimeout conditions. */
    uint32_t timeout_ticks{0};
};

/**
 * @brief State machine transition between states.
 */
struct ProcessTransition final {
    /** Source state name. */
    std::string from_state{};

    /** Target state name. */
    std::string to_state{};

    /** Optional user-facing transition label. */
    std::string label{};

    /** Condition that triggers this transition. */
    TransitionCondition condition{};

    /** Priority when multiple transitions are enabled (higher = higher priority). */
    int32_t priority{0};

};

/**
 * @brief Runtime-neutral Process flow definition.
 */
struct ProcessStateMachine final {
    /** All states in the state machine. */
    std::vector<ProcessStateNode> states{};

    /** All transitions between states. */
    std::vector<ProcessTransition> transitions{};

    /** Initial state name (must match a state in states[]). */
    std::string initial_state{};

    /** Error handling state (transitioned to on unhandled errors). */
    std::string error_state{};

    /** Compensation state (transitioned to for rollback). */
    std::string compensation_state{};

    /** Process-level timeout in ticks (0 = no timeout). */
    uint32_t process_timeout_ticks{0};

    /** Maximum retry attempts for retryable states. */
    uint32_t max_retry_attempts{3};
};

/**
 * @brief Validate Process state machine for correctness.
 *
 * Checks:
 * - Initial state exists
 * - All transitions reference valid states
 * - No unreachable states (except error/compensation)
 * - At least one terminal state or cycle exists
 *
 * @param machine State machine to validate
 * @return Success if valid, error with diagnostic message otherwise
 */
[[nodiscard]] foundation::Result validate_process_state_machine(const ProcessStateMachine& machine);

/**
 * @brief Find unreachable states in state machine.
 *
 * Performs reachability analysis from initial state.
 *
 * @param machine State machine to analyze
 * @return List of unreachable state names
 */
[[nodiscard]] std::vector<std::string> find_unreachable_states(const ProcessStateMachine& machine);

/**
 * @brief Find cycles in state machine.
 *
 * Detects cycles that might indicate infinite loops.
 *
 * @param machine State machine to analyze
 * @return List of cycles, each cycle is a list of state names
 */
[[nodiscard]] std::vector<std::vector<std::string>> find_cycles(const ProcessStateMachine& machine);

/**
 * @brief Convert Process state machine to FeatureContract transitions.
 *
 * Extracts transition strings in "FromState->ToState" format for FeatureContract.
 *
 * @param machine State machine definition
 * @return List of transition strings
 */
[[nodiscard]] std::vector<std::string> extract_transition_strings(const ProcessStateMachine& machine);

/**
 * @brief Validate transition condition for semantic correctness.
 *
 * @param condition Condition to validate
 * @param ports Available ports (for kOnPortData validation)
 * @return Success if valid, error otherwise
 */
[[nodiscard]] foundation::Result validate_transition_condition(
    const TransitionCondition& condition,
    const std::vector<PortContract>& ports);

}  // namespace aeg::feature
