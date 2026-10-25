/**
 * @file pipeline_types.hpp
 * @brief Engine-private data types for technical projection compilation and execution.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "result.hpp"
#include "stable_id.hpp"
#include "module_identity.hpp"
#include "feature_types.hpp"
#include "render_graph_types.hpp"
#include "rhi_struct.hpp"

namespace aeg::pipeline {
inline constexpr uint32_t kInvalidPipelineHandleIndex = std::numeric_limits<uint32_t>::max();

/** @brief Hash-backed stable identity used by pipeline contracts. */
template <typename Tag>
using StableId = foundation::StableId<Tag>;

/** @brief Hash function for StableId values used in unordered containers. */
template <typename Tag>
using StableIdHash = foundation::StableIdHash<Tag>;

using ModuleIdTag = module::ModuleIdTag;
struct PipelineIdTag;
struct PipelineInstanceIdTag;
struct PhaseIdTag;
struct StageIdTag;
struct ArtifactIdTag;
struct ArtifactTypeIdTag;
struct RenderPassTypeIdTag;
struct RenderPassInstanceIdTag;

using ModuleId = module::ModuleId;
using PipelineId = StableId<PipelineIdTag>;
using PipelineInstanceId = StableId<PipelineInstanceIdTag>;
using PhaseId = StableId<PhaseIdTag>;
using StageId = StableId<StageIdTag>;
using ArtifactId = StableId<ArtifactIdTag>;
using ArtifactTypeId = StableId<ArtifactTypeIdTag>;
using RenderPassTypeId = StableId<RenderPassTypeIdTag>;
using RenderPassInstanceId = StableId<RenderPassInstanceIdTag>;

using PlanRevision = uint64_t;
using ArtifactRevision = uint64_t;

/** @brief Scope that owns a pipeline instance or artifact. */
enum class PipelineScope : uint8_t {
    kEngine = 0,
    kWorld = 1,
    kFrame = 2,
    kEditor = 3,
};

/** @brief Execution resource used by a pipeline node. */
enum class ExecutionDomain : uint8_t {
    kMainThread = 0,
    kCpuWorker = 1,
    kIoWorker = 2,
    kRenderThread = 3,
    kGpu = 4,
};

/** @brief Time mechanism that activates a pipeline node. */
enum class TimePolicy : uint8_t {
    kStartup = 0,
    kFrame = 1,
    kFixedTick = 2,
    kEvent = 3,
    kShutdown = 4,
};

/** @brief Global lifecycle anchor shared by all pipeline definitions. */
enum class EnginePhase : uint8_t {
    kStartup = 0,
    kFrameBegin = 1,
    kInput = 2,
    kPreUpdate = 3,
    kFixedUpdate = 4,
    kVariableUpdate = 5,
    kWorldCommit = 6,
    kRenderExtract = 7,
    kRenderBuild = 8,
    kRenderExecute = 9,
    kPresent = 10,
    kFrameEnd = 11,
    kShutdown = 12,
};

/** @brief Ordering relation used when registering a custom global Phase. */
enum class PhaseOrderRelation : uint8_t {
    kBefore = 0,
    kAfter = 1,
};

/** @brief Stable identity of one built-in EnginePhase. */
[[nodiscard]] constexpr PhaseId to_phase_id(EnginePhase phase) noexcept {
    switch (phase) {
        case EnginePhase::kStartup:
            return PhaseId("Startup");
        case EnginePhase::kFrameBegin:
            return PhaseId("FrameBegin");
        case EnginePhase::kInput:
            return PhaseId("Input");
        case EnginePhase::kPreUpdate:
            return PhaseId("PreUpdate");
        case EnginePhase::kFixedUpdate:
            return PhaseId("FixedUpdate");
        case EnginePhase::kVariableUpdate:
            return PhaseId("VariableUpdate");
        case EnginePhase::kWorldCommit:
            return PhaseId("WorldCommit");
        case EnginePhase::kRenderExtract:
            return PhaseId("RenderExtract");
        case EnginePhase::kRenderBuild:
            return PhaseId("RenderBuild");
        case EnginePhase::kRenderExecute:
            return PhaseId("RenderExecute");
        case EnginePhase::kPresent:
            return PhaseId("Present");
        case EnginePhase::kFrameEnd:
            return PhaseId("FrameEnd");
        case EnginePhase::kShutdown:
            return PhaseId("Shutdown");
    }
    return {};
}

/** @brief Declaration of one global Phase and its relative ordering anchor. */
struct PhaseDescriptor {
    PhaseId id{};
    std::string_view name{};
    PhaseId anchor_phase{};
    PhaseOrderRelation relation{PhaseOrderRelation::kAfter};
};

/** @brief Consistent global Phase order and revision captured from one registry read. */
struct PhaseRegistrySnapshot {
    uint64_t revision{0};
    std::vector<PhaseId> ordered_phases{};
};

using PipelineScopeMask = uint32_t;

/** @brief Converts one pipeline scope into its mask representation. */
[[nodiscard]] constexpr PipelineScopeMask to_mask(PipelineScope scope) noexcept {
    return 1u << static_cast<uint8_t>(scope);
}

inline constexpr PipelineScopeMask kAllPipelineScopes =
    to_mask(PipelineScope::kEngine) | to_mask(PipelineScope::kWorld) | to_mask(PipelineScope::kFrame) |
    to_mask(PipelineScope::kEditor);

/** @brief Permissions granted to one engine-owned native graph build. */
enum class RenderGraphBuildFlags : uint32_t {
    kNone = 0,
    kMayCreateResources = 1u << 0,
    kMayCreateGraphicsPass = 1u << 1,
    kMayCreateComputePass = 1u << 2,
    kMayCreateCopyPass = 1u << 3,
    kMayUseDedicatedQueue = 1u << 4,
    kMayWriteFinalOutput = 1u << 5,
};

constexpr RenderGraphBuildFlags operator|(RenderGraphBuildFlags left, RenderGraphBuildFlags right) noexcept {
    return static_cast<RenderGraphBuildFlags>(static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
}

constexpr RenderGraphBuildFlags operator&(RenderGraphBuildFlags left, RenderGraphBuildFlags right) noexcept {
    return static_cast<RenderGraphBuildFlags>(static_cast<uint32_t>(left) & static_cast<uint32_t>(right));
}

[[nodiscard]] constexpr bool has_flag(RenderGraphBuildFlags value, RenderGraphBuildFlags flag) noexcept {
    return (value & flag) != RenderGraphBuildFlags::kNone;
}

/** @brief Classification used for registration and tooling. */
enum class PipelineCategory : uint8_t {
    kCore = 0,
    kGameplay = 1,
    kSimulation = 2,
    kRendering = 3,
    kAudio = 4,
    kStreaming = 5,
    kTooling = 6,
    kCustom = 7,
};

/** @brief Backend-neutral capability required by a pipeline definition. */
enum class Capability : uint8_t {
    kParallelCpuExecution = 0,
    kGpuExecution = 1,
    kGraphicsQueue = 2,
    kComputeQueue = 3,
    kCopyQueue = 4,
    kRayTracing = 5,
    kBindlessResources = 6,
    kShaderReflection = 7,
    kAsyncPipelineCompilation = 8,
};

using CapabilityMask = uint64_t;

/** @brief Semantic storage class of an artifact. */
enum class ArtifactStorage : uint8_t {
    kValue = 0,
    kMutableState = 1,
    kImmutableSnapshot = 2,
    kCommandBuffer = 3,
    kCpuResource = 4,
    kGpuLogicalResource = 5,
    kCompletionToken = 6,
};

/** @brief Minimum execution lifetime of an artifact. */
enum class ArtifactLifetime : uint8_t {
    kTask = 0,
    kStage = 1,
    kFrame = 2,
    kFixedTick = 3,
    kWorld = 4,
    kEngine = 5,
};

/** @brief Object responsible for retaining and destroying an artifact. */
enum class OwnershipPolicy : uint8_t {
    kScopeOwned = 0,
    kGraphOwned = 1,
    kProducerOwned = 2,
    kSharedImmutable = 3,
    kExternalBorrowed = 4,
};

/** @brief Operation a stage performs on an artifact. */
enum class ArtifactAccess : uint8_t {
    kRead = 0,
    kWrite = 1,
    kReadWrite = 2,
    kAppend = 3,
    kConsume = 4,
    kPublishSnapshot = 5,
};

/** @brief Additional constraints attached to an artifact access. */
enum class AccessFlags : uint32_t {
    kNone = 0,
    kOptional = 1u << 0,
    kPreserveContents = 1u << 1,
    kDiscardContents = 1u << 2,
    kExclusive = 1u << 3,
    kDeterministicAppend = 1u << 4,
};

constexpr AccessFlags operator|(AccessFlags left, AccessFlags right) noexcept {
    return static_cast<AccessFlags>(static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
}

constexpr AccessFlags operator&(AccessFlags left, AccessFlags right) noexcept {
    return static_cast<AccessFlags>(static_cast<uint32_t>(left) & static_cast<uint32_t>(right));
}

[[nodiscard]] constexpr bool has_flag(AccessFlags value, AccessFlags flag) noexcept {
    return (value & flag) != AccessFlags::kNone;
}

/** @brief Internal projections reuse the module contract version type. */
using Version = module::Version;

/** @brief Static metadata for one pipeline definition. */
struct PipelineDescriptor {
    PipelineId id{};
    std::string name{};
    Version version{1, 0, 0};
    PipelineCategory category{PipelineCategory::kCustom};
    PipelineScopeMask supported_scopes{kAllPipelineScopes};
    ExecutionDomain default_execution_domain{ExecutionDomain::kMainThread};
    TimePolicy default_time_policy{TimePolicy::kFrame};
    ModuleId owner_module{};
    CapabilityMask required_capabilities{0};
};

/** @brief Static metadata for one stage in a pipeline definition. */
struct StageDescriptor {
    StageId id{};
    EnginePhase phase{EnginePhase::kFrameBegin};
    ExecutionDomain execution_domain{ExecutionDomain::kMainThread};
    TimePolicy time_policy{TimePolicy::kFrame};
    bool parallelizable{false};
    PhaseId phase_id{};
};

/** @brief Static metadata for one artifact contract. */
struct ArtifactDescriptor {
    ArtifactId id{};
    ArtifactTypeId type{};
    PipelineScope owner_scope{PipelineScope::kEngine};
    ArtifactStorage storage{ArtifactStorage::kValue};
    ArtifactLifetime lifetime{ArtifactLifetime::kTask};
    OwnershipPolicy ownership{OwnershipPolicy::kProducerOwned};
    uint32_t version{1};
    PipelineScopeMask visible_to{0};
    bool external_input{false};
};

/** @brief One stage-to-artifact access declaration. */
struct ArtifactBinding {
    ArtifactId artifact{};
    ArtifactAccess access{ArtifactAccess::kRead};
    uint32_t required_version{0};
    AccessFlags flags{AccessFlags::kNone};
};

template <typename Tag>
struct DefinitionHandle final {
    uint32_t index{kInvalidPipelineHandleIndex};
    uint32_t generation{0};

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return index != kInvalidPipelineHandleIndex && generation != 0;
    }

    constexpr explicit operator bool() const noexcept { return is_valid(); }
    friend constexpr bool operator==(DefinitionHandle, DefinitionHandle) = default;
};

struct PipelineStageHandleTag;
struct PipelineArtifactHandleTag;
using RenderGraphResourceIdTag = feature::RenderResourceIdTag;

using PipelineStageHandle = DefinitionHandle<PipelineStageHandleTag>;
using PipelineArtifactHandle = DefinitionHandle<PipelineArtifactHandleTag>;
using RenderResourceId = feature::RenderResourceId;
using RenderGraphBuffer = render_graph::RenderGraphBuffer;
using RenderGraphTexture = render_graph::RenderGraphTexture;
using RenderGraphPass = render_graph::RenderGraphPass;

/** @brief Lifetime policy for a logical render-graph resource. */
enum class RenderGraphResourceLifetime : uint8_t {
    kTransient = 0,
    kPersistent = 1,
    kImported = 2,
};

/** @brief Execution category of a render-graph pass. */
enum class RenderGraphPassType : uint8_t {
    kGraphics = 0,
    kCompute = 1,
    kCopy = 2,
};

/** @brief Byte range of a logical render-graph buffer. */
struct RenderGraphBufferRange {
    uint64_t offset{0};
    uint64_t size{std::numeric_limits<uint64_t>::max()};
};

/** @brief Internal definition record for a stage. */
struct PipelineStageRecord {
    StageDescriptor descriptor{};
};

/** @brief Internal definition record for an artifact. */
struct PipelineArtifactRecord {
    ArtifactDescriptor descriptor{};
};

/** @brief Internal definition record for one artifact access. */
struct PipelineBindingRecord {
    uint32_t stage_index{kInvalidPipelineHandleIndex};
    ArtifactBinding binding{};
};

/** @brief Explicit ordering relation between two stages. */
struct PipelineOrderingRecord {
    uint32_t source_stage_index{kInvalidPipelineHandleIndex};
    StageId target_stage{};
    bool source_runs_before{true};
};

/** @brief Cross-scope exposure declaration for an artifact. */
struct PipelineExposureRecord {
    ArtifactId artifact{};
    PipelineScope target_scope{PipelineScope::kEngine};
};

/** @brief Immutable result of PipelineBuilder definition collection. */
struct PipelineDefinition {
    PipelineDescriptor descriptor{};
    std::vector<PipelineStageRecord> stages{};
    std::vector<PipelineArtifactRecord> artifacts{};
    std::vector<PipelineBindingRecord> bindings{};
    std::vector<PipelineOrderingRecord> orderings{};
    std::vector<PipelineExposureRecord> exposures{};
};

/** @brief Reason for a dependency edge in a compiled plan. */
enum class PipelineDependencyKind : uint8_t {
    kArtifact = 0,
    kOrdering = 1,
};

/** @brief One technical Stage node in a compiled plan. */
struct PipelineNode {
    StageId stage{};
    EnginePhase phase{EnginePhase::kFrameBegin};
    ExecutionDomain execution_domain{ExecutionDomain::kMainThread};
    TimePolicy time_policy{TimePolicy::kFrame};
    bool stage_parallelizable{false};
    std::vector<ArtifactBinding> artifact_bindings{};
    PhaseId phase_id{};
    uint32_t definition_stage_index{kInvalidPipelineHandleIndex};
};

/** @brief One directed dependency edge between compiled nodes. */
struct PipelineDependency {
    uint32_t source_node{kInvalidPipelineHandleIndex};
    uint32_t destination_node{kInvalidPipelineHandleIndex};
    PipelineDependencyKind kind{PipelineDependencyKind::kArtifact};
    ArtifactId artifact{};
    /** @brief True when the Editor added this ordering at runtime rather than from the pipeline definition. */
    bool runtime_override{false};
};

/** @brief A topological Stage batch; observation never dispatches worker tasks. */
struct PipelineExecutionBatch {
    std::vector<uint32_t> node_indices{};
};

/** @brief Immutable Stage plan emitted by the Pipeline compiler bridge. */
struct PipelinePlan {
    PlanRevision revision{0};
    PipelineId pipeline{};
    PipelineInstanceId instance{};
    Version definition_version{};
    PipelineScope scope{PipelineScope::kEngine};
    uint64_t phase_revision{0};
    std::vector<PipelineNode> nodes{};
    std::vector<PipelineDependency> dependencies{};
    std::vector<uint32_t> topological_order{};
    std::vector<PipelineExecutionBatch> batches{};
    std::vector<ArtifactDescriptor> artifacts{};
    std::vector<PipelineExposureRecord> exposures{};

    // Retains actual immutable declaration data after registry removal.
    std::shared_ptr<const PipelineDefinition> definition_owner{};
};

using PipelinePlanHandle = std::shared_ptr<const PipelinePlan>;
}  // namespace aeg::pipeline
