/**
 * @file feature_graph_runtime_host.hpp
 * @brief Staged publication host for compiled FeatureGraph runtime generations.
 */
#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "feature_graph.hpp"

namespace aeg::application::runtime {

/** Owning values for engine tools; query/control are called on the Runtime owner thread. */
struct FeatureRuntimeOwnerSnapshot final {
    feature::RenderResourceOwner owner{};
    feature::FeatureDefinitionId definition_id{};
    feature::ModuleIdentity owner_module{};
    feature::ScopeId scope{};
    feature::OwnerPath owner_path{};
    feature::FeatureContract contract{};
    feature::StateStore state{};
    std::vector<uint8_t> parameters{};
    std::vector<feature::FeatureInstanceId> children{};
    std::vector<feature::ProcessInstance> processes{};
    feature::InvocationSnapshot invocations{};
    /** Presentation intent is independent of the general invocation history. */
    std::optional<feature::FeatureInstance::CommittedRenderIntent> render_output{};
};

struct FeatureRuntimeNodeSnapshot final {
    feature::RuntimeGeneration generation{};
    std::string asset_uuid{};
    feature::FeatureNodeId node_id{};
    /** Editor-owned source trace; empty for Runtime hosts without project files. */
    std::filesystem::path source_path{};
    /** Root first, followed by its actual live descendants. */
    std::vector<FeatureRuntimeOwnerSnapshot> owners{};
};

/** Source attribution is separate from the namespaced runtime identity. */
struct FeatureRuntimeExecutionProvenance final {
    feature::FeatureNodeId authored_node_id{};
    feature::OwnerPath authored_owner_path{};
    feature::FeatureNodeProvenance runtime{};
    /** Stable authored parent path, then the root node and descendant definition suffix for children. */
    feature::OwnerPath authored_instance_path{};
};

/** Owning view of the published generation, independent of pending editor drafts. */
struct FeatureRuntimeExecutionSnapshot final {
    feature::RuntimeGeneration generation{};
    std::string asset_uuid{};
    std::filesystem::path source_path{};
    uint64_t graph_version{0};
    uint64_t draft_revision{0};
    uint64_t contract_digest{0};
    std::vector<feature::ExecutionPlan> plans{};
    std::vector<FeatureRuntimeExecutionProvenance> provenance{};
};

enum class RuntimeProcessAction : uint8_t { kSuspend, kResume, kCancel };

struct RuntimeProcessCommand final {
    feature::RuntimeGeneration generation{};
    std::string asset_uuid{};
    feature::FeatureNodeId node_id{};
    feature::RenderResourceOwner owner{};
    feature::ProcessId process{};
    feature::ProcessRevision expected_revision{};
    RuntimeProcessAction action{};
};

class FeatureGraphRuntimeHost final {
public:
    FeatureGraphRuntimeHost(feature::FeatureRuntime& runtime, const feature::FeatureRegistry& registry) noexcept
        : runtime_(runtime), registry_(registry), generation_(runtime.generation()) {}

    [[nodiscard]] foundation::Result stage(const feature::FeatureGraphDocument& graph);
    [[nodiscard]] foundation::Result stage_asset(std::span<const std::byte> bytes);
    [[nodiscard]] foundation::Result apply_pending();
    [[nodiscard]] bool has_pending() const noexcept { return !pending_documents_.empty(); }
    [[nodiscard]] foundation::ResultValue<FeatureRuntimeNodeSnapshot> inspect_node(
        std::string_view asset_uuid, feature::FeatureNodeId node_id) const;
    [[nodiscard]] foundation::ResultValue<FeatureRuntimeExecutionSnapshot> inspect_execution(
        std::string_view asset_uuid) const;
    [[nodiscard]] foundation::Result control_process(const RuntimeProcessCommand& command);
    [[nodiscard]] foundation::ResultValue<std::vector<feature::RenderFeatureOutput>> capture_render_outputs() const;
    [[nodiscard]] foundation::Result render_publication_status(feature::ClockId clock) const;
    [[nodiscard]] feature::RuntimeGeneration generation() const noexcept { return generation_; }
    [[nodiscard]] const feature::CompiledFeatureGraph* active() const noexcept {
        return active_ ? &*active_ : nullptr;
    }
    /** @brief Returns the authored graph identities that form the active batch. */
    [[nodiscard]] const std::vector<feature::FeatureGraphDocument>& active_documents() const noexcept {
        return active_documents_;
    }

private:
    [[nodiscard]] foundation::Result validate_active() const;
    feature::FeatureRuntime& runtime_;
    const feature::FeatureRegistry& registry_;
    std::vector<feature::FeatureGraphDocument> pending_documents_{};
    std::optional<feature::CompiledFeatureGraph> active_{};
    std::vector<feature::FeatureGraphDocument> active_documents_{};
    feature::RuntimeGeneration generation_{};
};

}  // namespace aeg::application::runtime
