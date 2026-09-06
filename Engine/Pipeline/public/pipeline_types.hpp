/**
 * @file pipeline_types.hpp
 * @brief Public data types for the domain-oriented pipeline graph.
 */
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>
#include <type_traits>
#include <vector>

#include "result.hpp"

namespace aeg::pipeline {
    class PipelineFeature;

    inline constexpr uint32_t kInvalidPipelineHandleIndex = std::numeric_limits<uint32_t>::max();

    /** @brief Hash-backed stable identity used by pipeline contracts. */
    template <typename Tag>
    class StableId final {
    public:
        constexpr StableId() noexcept = default;
        explicit constexpr StableId(uint64_t value) noexcept : value_(value) {
        }
        explicit constexpr StableId(std::string_view name) noexcept : value_(hash_name(name)) {
        }

        [[nodiscard]] constexpr bool is_valid() const noexcept { return value_ != 0; }
        [[nodiscard]] constexpr uint64_t value() const noexcept { return value_; }

        friend constexpr bool operator==(StableId, StableId) = default;
        friend constexpr auto operator<=>(StableId, StableId) = default;

    private:
        static constexpr uint64_t hash_name(std::string_view name) noexcept {
            uint64_t hash = 14695981039346656037ull;
            for (const char character : name) {
                hash ^= static_cast<uint8_t>(character);
                hash *= 1099511628211ull;
            }
            return hash == 0 ? 1 : hash;
        }

        uint64_t value_{0};
    };

    /** @brief Hash function for StableId values used in unordered containers. */
    template <typename Tag>
    struct StableIdHash final {
        [[nodiscard]] std::size_t operator()(StableId<Tag> id) const noexcept {
            return static_cast<std::size_t>(id.value());
        }
    };

    struct ModuleIdTag;
    struct PipelineIdTag;
    struct PipelineInstanceIdTag;
    struct PhaseIdTag;
    struct StageIdTag;
    struct TaskIdTag;
    struct FeatureIdTag;
    struct FeatureInstanceIdTag;
    struct FeaturePartIdTag;
    struct FeaturePointIdTag;
    struct ArtifactIdTag;
    struct ArtifactTypeIdTag;

    using ModuleId = StableId<ModuleIdTag>;
    using PipelineId = StableId<PipelineIdTag>;
    using PipelineInstanceId = StableId<PipelineInstanceIdTag>;
    using PhaseId = StableId<PhaseIdTag>;
    using StageId = StableId<StageIdTag>;
    using TaskId = StableId<TaskIdTag>;
    using FeatureId = StableId<FeatureIdTag>;
    using FeatureInstanceId = StableId<FeatureInstanceIdTag>;
    using FeaturePartId = StableId<FeaturePartIdTag>;
    using FeaturePointId = StableId<FeaturePointIdTag>;
    using ArtifactId = StableId<ArtifactIdTag>;
    using ArtifactTypeId = StableId<ArtifactTypeIdTag>;

    using PlanRevision = uint64_t;
    using ArtifactRevision = uint64_t;

    /** @brief Scope that owns a pipeline instance or artifact. */
    enum class PipelineScope : uint8_t {
        kEngine = 0,
        kWorld = 1,
        kFrame = 2,
        kEditor = 3,
    };

    /** @brief Domain that defines the semantic meaning of a pipeline contribution. */
    enum class FeatureDomain : uint8_t {
        kLogic = 0,
        kPhysics = 1,
        kRender = 2,
        kAudio = 3,
        kStream = 4,
        kComposite = 5,
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
    using FeatureDomainMask = uint32_t;
    using FeaturePriority = int32_t;

    /** @brief Converts one pipeline scope into its mask representation. */
    [[nodiscard]] constexpr PipelineScopeMask to_mask(PipelineScope scope) noexcept {
        return 1u << static_cast<uint8_t>(scope);
    }

    /** @brief Converts one feature domain into its mask representation. */
    [[nodiscard]] constexpr FeatureDomainMask to_mask(FeatureDomain domain) noexcept {
        return domain == FeatureDomain::kComposite ? 0u : 1u << static_cast<uint8_t>(domain);
    }

    inline constexpr PipelineScopeMask kAllPipelineScopes = to_mask(PipelineScope::kEngine) |
                                                            to_mask(PipelineScope::kWorld) |
                                                            to_mask(PipelineScope::kFrame) |
                                                            to_mask(PipelineScope::kEditor);
    inline constexpr FeatureDomainMask kAllFeatureDomains = to_mask(FeatureDomain::kLogic) |
                                                            to_mask(FeatureDomain::kPhysics) |
                                                            to_mask(FeatureDomain::kRender) |
                                                            to_mask(FeatureDomain::kAudio) |
                                                            to_mask(FeatureDomain::kStream);

    /** @brief Position of a feature extension relative to its anchor stage. */
    enum class FeaturePointPosition : uint8_t {
        kBefore = 0,
        kInside = 1,
        kAfter = 2,
    };

    /** @brief Permissions granted by a pipeline feature point. */
    enum class FeaturePointFlags : uint32_t {
        kNone = 0,
        kMayCreateArtifacts = 1u << 0,
        kMayCreateGraphicsPass = 1u << 1,
        kMayCreateComputePass = 1u << 2,
        kMayCreateCopyPass = 1u << 3,
        kMayUseDedicatedQueue = 1u << 4,
        kMayWriteFinalOutput = 1u << 5,
        kMayPublishSnapshot = 1u << 6,
    };

    constexpr FeaturePointFlags operator|(FeaturePointFlags left, FeaturePointFlags right) noexcept {
        return static_cast<FeaturePointFlags>(static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
    }

    constexpr FeaturePointFlags operator&(FeaturePointFlags left, FeaturePointFlags right) noexcept {
        return static_cast<FeaturePointFlags>(static_cast<uint32_t>(left) & static_cast<uint32_t>(right));
    }

    [[nodiscard]] constexpr bool has_flag(FeaturePointFlags value, FeaturePointFlags flag) noexcept {
        return (value & flag) != FeaturePointFlags::kNone;
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

    /** @brief Scheduling and retention properties of a task. */
    enum class TaskFlags : uint32_t {
        kNone = 0,
        kParallelSafe = 1u << 0,
        kOptional = 1u << 1,
        kSideEffect = 1u << 2,
        kMustRun = 1u << 3,
        kMayBlock = 1u << 4,
        kExportsCompletion = 1u << 5,
    };

    constexpr TaskFlags operator|(TaskFlags left, TaskFlags right) noexcept {
        return static_cast<TaskFlags>(static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
    }

    constexpr TaskFlags operator&(TaskFlags left, TaskFlags right) noexcept {
        return static_cast<TaskFlags>(static_cast<uint32_t>(left) & static_cast<uint32_t>(right));
    }

    [[nodiscard]] constexpr bool has_flag(TaskFlags value, TaskFlags flag) noexcept {
        return (value & flag) != TaskFlags::kNone;
    }

    /** @brief Version attached to a public pipeline contract. */
    struct Version {
        uint32_t major{0};
        uint32_t minor{0};
        uint32_t patch{0};

        [[nodiscard]] constexpr bool is_valid() const noexcept { return major != 0 || minor != 0 || patch != 0; }
        friend constexpr bool operator==(Version, Version) = default;
        friend constexpr auto operator<=>(Version, Version) = default;
    };

    /** @brief Static metadata for one pipeline definition. */
    struct PipelineDescriptor {
        PipelineId id{};
        std::string_view name{};
        Version version{1, 0, 0};
        PipelineCategory category{PipelineCategory::kCustom};
        PipelineScopeMask supported_scopes{kAllPipelineScopes};
        FeatureDomainMask domains{0};
        ExecutionDomain default_execution_domain{ExecutionDomain::kMainThread};
        TimePolicy default_time_policy{TimePolicy::kFrame};
        ModuleId owner_module{};
        CapabilityMask required_capabilities{0};
    };

    /** @brief Static metadata for one feature type. */
    struct FeatureDescriptor {
        FeatureId id{};
        std::string_view name{};
        FeatureDomain domain{FeatureDomain::kLogic};
        FeatureDomainMask domain_mask{0};
        PipelineScopeMask supported_scopes{kAllPipelineScopes};
        Version version{1, 0, 0};
        ModuleId owner_module{};
    };

    /** @brief Public extension anchor exposed by a pipeline definition. */
    struct FeaturePointDescriptor {
        FeaturePointId id{};
        std::string_view name{};
        PipelineId pipeline{};
        StageId anchor_stage{};
        FeaturePointPosition position{FeaturePointPosition::kBefore};
        FeatureDomainMask allowed_domains{0};
        PipelineScopeMask allowed_scopes{kAllPipelineScopes};
        FeaturePointFlags flags{FeaturePointFlags::kNone};
    };

    /** @brief Binding of one feature type to one pipeline extension point. */
    struct FeatureBindingDescriptor {
        FeatureId feature{};
        PipelineId pipeline{};
        PipelineScope scope{PipelineScope::kEngine};
        FeaturePointId point{};
        FeaturePriority priority{0};
        bool enabled{true};
        FeatureInstanceId instance{};
    };

    /** @brief Derives a deterministic identity for a feature binding. */
    [[nodiscard]] inline FeatureInstanceId make_feature_instance_id(FeatureId feature,
                                                                    PipelineId pipeline,
                                                                    PipelineScope scope,
                                                                    FeaturePointId point) noexcept {
        uint64_t hash = 14695981039346656037ull;
        const auto mix = [&hash](uint64_t value) constexpr {
            hash ^= value;
            hash *= 1099511628211ull;
        };
        mix(feature.value());
        mix(pipeline.value());
        mix(static_cast<uint8_t>(scope));
        mix(point.value());
        return FeatureInstanceId(hash == 0 ? 1 : hash);
    }

    /** @brief Derives a deterministic identity for a Composite feature part. */
    [[nodiscard]] inline FeaturePartId make_feature_part_id(FeatureInstanceId instance,
                                                             uint32_t part_index) noexcept {
        uint64_t hash = instance.value() ^ 0x9e3779b97f4a7c15ull;
        hash ^= static_cast<uint64_t>(part_index) + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
        return FeaturePartId(hash == 0 ? 1 : hash);
    }

    /** @brief Static metadata for one stage in a pipeline definition. */
    struct StageDescriptor {
        StageId id{};
        EnginePhase phase{EnginePhase::kFrameBegin};
        ExecutionDomain execution_domain{ExecutionDomain::kMainThread};
        TimePolicy time_policy{TimePolicy::kFrame};
        bool parallelizable{false};
        PhaseId phase_id{};
    };

    class PipelineTaskContext;
    using TaskExecutionFunction = foundation::Result (*)(PipelineTaskContext& context);

    /** @brief Static metadata for one executable task. */
    struct TaskDescriptor {
        TaskId id{};
        TaskFlags flags{TaskFlags::kNone};
        TaskExecutionFunction execute{nullptr};
        void* user_data{nullptr};
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

    /** @brief Opaque handle shared between Pipeline and RenderGraph runtime APIs. */
    template <typename Tag>
    struct RenderGraphHandle final {
        uint32_t index{kInvalidPipelineHandleIndex};
        uint32_t generation{0};
        uint64_t graph_id{0};

        [[nodiscard]] constexpr bool is_valid() const noexcept {
            return index != kInvalidPipelineHandleIndex && generation != 0 && graph_id != 0;
        }

        constexpr explicit operator bool() const noexcept { return is_valid(); }
        friend constexpr bool operator==(RenderGraphHandle, RenderGraphHandle) = default;
    };

    struct PipelineStageHandleTag;
    struct PipelineTaskHandleTag;
    struct PipelineArtifactHandleTag;
    struct PipelineFeaturePointHandleTag;
    struct RenderGraphResourceIdTag;
    struct RenderGraphBufferHandleTag;
    struct RenderGraphTextureHandleTag;
    struct RenderGraphPassHandleTag;

    using PipelineStageHandle = DefinitionHandle<PipelineStageHandleTag>;
    using PipelineTaskHandle = DefinitionHandle<PipelineTaskHandleTag>;
    using PipelineArtifactHandle = DefinitionHandle<PipelineArtifactHandleTag>;
    using PipelineFeaturePointHandle = DefinitionHandle<PipelineFeaturePointHandleTag>;
    using RenderResourceId = StableId<RenderGraphResourceIdTag>;
    using RenderGraphBuffer = RenderGraphHandle<RenderGraphBufferHandleTag>;
    using RenderGraphTexture = RenderGraphHandle<RenderGraphTextureHandleTag>;
    using RenderGraphPass = RenderGraphHandle<RenderGraphPassHandleTag>;

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

    /** @brief Internal definition record for a task and its owning stage. */
    struct PipelineTaskRecord {
        TaskDescriptor descriptor{};
        uint32_t stage_index{kInvalidPipelineHandleIndex};
    };

    /** @brief Internal definition record for an artifact. */
    struct PipelineArtifactRecord {
        ArtifactDescriptor descriptor{};
    };

    /** @brief Internal record for one public feature extension point. */
    struct PipelineFeaturePointRecord {
        FeaturePointDescriptor descriptor{};
        uint32_t anchor_stage_index{kInvalidPipelineHandleIndex};
    };

    /** @brief Internal record for a feature binding declared by a pipeline definition. */
    struct PipelineFeatureBindingRecord {
        FeatureBindingDescriptor descriptor{};
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

    /** @brief Provenance record for one feature contribution in a compiled plan. */
    struct PipelineFeatureContributionRecord {
        FeatureInstanceId feature_instance{};
        FeatureId feature{};
        FeaturePartId part{};
        FeaturePointId point{};
        FeatureDomain domain{FeatureDomain::kLogic};
        uint32_t first_stage_index{kInvalidPipelineHandleIndex};
        uint32_t stage_count{0};
        FeaturePointPosition point_position{FeaturePointPosition::kBefore};
        FeaturePointFlags point_flags{FeaturePointFlags::kNone};
    };

    /** @brief Keeps one compiled feature contribution implementation alive. */
    struct PipelineFeatureImplementationRecord {
        FeatureInstanceId feature_instance{};
        FeatureId feature{};
        FeaturePartId part{};
        std::shared_ptr<const PipelineFeature> implementation{};
    };

    /** @brief Immutable result of PipelineBuilder definition collection. */
    struct PipelineDefinition {
        PipelineDescriptor descriptor{};
        std::vector<PipelineStageRecord> stages{};
        std::vector<PipelineTaskRecord> tasks{};
        std::vector<PipelineArtifactRecord> artifacts{};
        std::vector<PipelineFeaturePointRecord> feature_points{};
        std::vector<PipelineFeatureBindingRecord> feature_bindings{};
        std::vector<PipelineBindingRecord> bindings{};
        std::vector<PipelineOrderingRecord> orderings{};
        std::vector<PipelineExposureRecord> exposures{};
    };

    /** @brief Reason for a dependency edge in a compiled plan. */
    enum class PipelineDependencyKind : uint8_t {
        kArtifact = 0,
        kOrdering = 1,
    };

    /** @brief One executable node in a compiled pipeline plan. */
    struct PipelineNode {
        TaskId task{};
        StageId stage{};
        EnginePhase phase{EnginePhase::kFrameBegin};
        ExecutionDomain execution_domain{ExecutionDomain::kMainThread};
        TimePolicy time_policy{TimePolicy::kFrame};
        TaskFlags flags{TaskFlags::kNone};
        TaskExecutionFunction execute{nullptr};
        void* user_data{nullptr};
        bool synthetic{false};
        bool stage_parallelizable{false};
        std::vector<ArtifactBinding> artifact_bindings{};
        PhaseId phase_id{};
    };

    /** @brief One directed dependency edge between compiled nodes. */
    struct PipelineDependency {
        uint32_t source_node{kInvalidPipelineHandleIndex};
        uint32_t destination_node{kInvalidPipelineHandleIndex};
        PipelineDependencyKind kind{PipelineDependencyKind::kArtifact};
        ArtifactId artifact{};
    };

    /** @brief A set of nodes that can be scheduled after earlier batches complete. */
    struct PipelineExecutionBatch {
        std::vector<uint32_t> node_indices{};
        bool parallelizable{false};
    };

    /** @brief Immutable executable plan emitted by PipelineCompiler. */
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
        std::vector<PipelineFeatureContributionRecord> feature_contributions{};
        std::vector<PipelineFeatureImplementationRecord> feature_implementations{};
        std::vector<ArtifactDescriptor> artifacts{};
        std::vector<PipelineExposureRecord> exposures{};

        // Keeps definition callbacks and their owner object alive after registry removal.
        std::shared_ptr<const void> definition_owner{};
        std::vector<std::shared_ptr<const void>> feature_owners{};
    };

    using PipelinePlanHandle = std::shared_ptr<const PipelinePlan>;
}  // namespace aeg::pipeline
