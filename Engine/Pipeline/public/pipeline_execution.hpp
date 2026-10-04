/** @file pipeline_execution.hpp @brief Stable identities and result states for FlowGraph execution. */
#pragma once

#include <cstdint>
#include <string>

namespace aeg::pipeline {

/** Identifies one logical graph call. It remains stable across preparation retries. */
struct ExecutionInstanceId final {
    std::string graph_asset_id{};
    std::string graph_version{};
    std::string instance_path{};
    uint64_t frame_id{};
    std::string view_id{};
    std::string world_id{};
    std::string target_variant{};
    uint64_t invocation_ordinal{};

    [[nodiscard]] bool is_valid() const noexcept {
        return !graph_asset_id.empty() && !graph_version.empty() && !instance_path.empty() &&
               !view_id.empty() && !world_id.empty() && !target_variant.empty();
    }
    friend bool operator==(const ExecutionInstanceId&, const ExecutionInstanceId&) = default;
};

struct ExecutionAttemptId final {
    ExecutionInstanceId instance{};
    uint64_t attempt{};

    [[nodiscard]] bool is_valid() const noexcept { return instance.is_valid() && attempt != 0; }
    friend bool operator==(const ExecutionAttemptId&, const ExecutionAttemptId&) = default;
};

enum class ExecutionResultStatus : uint8_t { kReady, kInvalid, kCancelled, kSkipped, kPartial };
enum class CompletionDomain : uint8_t { kCpu, kGpu, kReadback, kExternal };

struct CompletionPoint final {
    CompletionDomain domain{CompletionDomain::kCpu};
    uint64_t stage{};
    friend bool operator==(const CompletionPoint&, const CompletionPoint&) = default;
};

/** A result status is scoped to one output, not just to the node as a whole. */
struct OutputCompletionState final {
    ExecutionAttemptId attempt{};
    uint64_t output_port_id{};
    CompletionPoint completion{};
    ExecutionResultStatus status{ExecutionResultStatus::kInvalid};
    std::string semantic_digest{};
    std::string diagnostic{};
};

}  // namespace aeg::pipeline
