/**
 * @file pipeline.hpp
 * @brief Public interfaces for defining, compiling, and executing pipelines.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pipeline_types.hpp"

namespace aeg::pipeline {
    class FeatureBuildContext;
    class ArtifactRegistry;
    class ArtifactScopeBridge;

    template <FeatureDomain Domain>
    class DomainGraphBuilder;

    using LogicGraphBuilder = DomainGraphBuilder<FeatureDomain::kLogic>;
    using PhysicsGraphBuilder = DomainGraphBuilder<FeatureDomain::kPhysics>;
    using RenderGraphBuilder = DomainGraphBuilder<FeatureDomain::kRender>;
    using AudioGraphBuilder = DomainGraphBuilder<FeatureDomain::kAudio>;
    using StreamGraphBuilder = DomainGraphBuilder<FeatureDomain::kStream>;

    /** @brief Type-erased payload installed into an ArtifactRegistry. */
    struct ArtifactPayload {
        ArtifactTypeId type{};
        std::shared_ptr<const void> owner{};
        const void* data{nullptr};
        std::size_t size{0};
        std::size_t alignment{alignof(std::max_align_t)};
        bool writable{false};

        /** @brief Creates an owned payload backed by a shared object. */
        template <typename Type>
        [[nodiscard]] static ArtifactPayload owned(ArtifactTypeId type,
                                                   std::shared_ptr<Type> value,
                                                   bool allow_writes = true) {
            ArtifactPayload payload;
            payload.type = type;
            payload.owner = std::move(value);
            payload.data = payload.owner.get();
            payload.size = sizeof(Type);
            payload.alignment = alignof(Type);
            payload.writable = allow_writes && !std::is_const_v<Type>;
            return payload;
        }

        /** @brief Creates a non-owning immutable view over external memory. */
        [[nodiscard]] static ArtifactPayload borrowed(ArtifactTypeId type,
                                                       const void* data,
                                                       std::size_t size,
                                                       std::size_t alignment = alignof(std::max_align_t)) {
            ArtifactPayload payload;
            payload.type = type;
            payload.owner = std::shared_ptr<const void>(data, [](const void*) {});
            payload.data = data;
            payload.size = size;
            payload.alignment = alignment;
            payload.writable = false;
            return payload;
        }
    };

    /** @brief Read-only runtime lease for one published artifact value. */
    class ArtifactView final {
    public:
        ArtifactView() noexcept = default;

        [[nodiscard]] bool is_valid() const noexcept { return valid_; }
        [[nodiscard]] ArtifactId id() const noexcept { return id_; }
        [[nodiscard]] ArtifactTypeId type() const noexcept { return type_; }
        [[nodiscard]] PipelineScope owner_scope() const noexcept { return owner_scope_; }
        [[nodiscard]] ArtifactStorage storage() const noexcept { return storage_; }
        [[nodiscard]] uint32_t version() const noexcept { return version_; }
        [[nodiscard]] ArtifactRevision revision() const noexcept { return revision_; }
        [[nodiscard]] const void* data() const noexcept { return data_; }
        [[nodiscard]] std::size_t size() const noexcept { return size_; }
        [[nodiscard]] std::size_t alignment() const noexcept { return alignment_; }

        template <typename Type>
        [[nodiscard]] const Type* as(ArtifactTypeId expected_type) const noexcept {
            if (!valid_ || type_ != expected_type || data_ == nullptr || size_ < sizeof(Type)) {
                return nullptr;
            }
            return static_cast<const Type*>(data_);
        }

    private:
        ArtifactView(ArtifactId id,
                     ArtifactTypeId type,
                     PipelineScope owner_scope,
                     ArtifactStorage storage,
                     uint32_t version,
                     ArtifactRevision revision,
                     const void* data,
                     std::size_t size,
                     std::size_t alignment,
                     std::shared_ptr<const void> owner) noexcept
            : id_(id),
              type_(type),
              owner_scope_(owner_scope),
              storage_(storage),
              version_(version),
              revision_(revision),
              data_(data),
              size_(size),
              alignment_(alignment),
              owner_(std::move(owner)),
              valid_(true) {
        }

        ArtifactId id_{};
        ArtifactTypeId type_{};
        PipelineScope owner_scope_{PipelineScope::kEngine};
        ArtifactStorage storage_{ArtifactStorage::kValue};
        uint32_t version_{0};
        ArtifactRevision revision_{0};
        const void* data_{nullptr};
        std::size_t size_{0};
        std::size_t alignment_{0};
        std::shared_ptr<const void> owner_{};
        bool valid_{false};

        friend class ArtifactRegistry;
        friend class ArtifactScopeBridge;
    };

    /** @brief Exclusive runtime lease for writing one artifact value. */
    class ArtifactWriter final {
    public:
        ArtifactWriter() noexcept = default;
        ~ArtifactWriter();

        ArtifactWriter(const ArtifactWriter&) = delete;
        ArtifactWriter& operator=(const ArtifactWriter&) = delete;
        ArtifactWriter(ArtifactWriter&&) noexcept;
        ArtifactWriter& operator=(ArtifactWriter&&) noexcept;

        [[nodiscard]] bool is_valid() const noexcept;
        [[nodiscard]] ArtifactId id() const noexcept;
        [[nodiscard]] ArtifactTypeId type() const noexcept;
        [[nodiscard]] PipelineScope owner_scope() const noexcept;
        [[nodiscard]] ArtifactStorage storage() const noexcept;
        [[nodiscard]] uint32_t version() const noexcept;
        [[nodiscard]] ArtifactRevision revision() const noexcept;
        [[nodiscard]] void* data() const noexcept;
        [[nodiscard]] std::size_t size() const noexcept;
        [[nodiscard]] std::size_t alignment() const noexcept;

        template <typename Type>
        [[nodiscard]] Type* as(ArtifactTypeId expected_type) const noexcept {
            if (!is_valid() || type() != expected_type || data() == nullptr || size() < sizeof(Type)) {
                return nullptr;
            }
            return static_cast<Type*>(data());
        }

        /** @brief Publishes the mutation and releases the exclusive lease. */
        [[nodiscard]] foundation::Result commit();

    private:
        struct Impl;

        explicit ArtifactWriter(std::unique_ptr<Impl> impl) noexcept;

        std::unique_ptr<Impl> impl_{};

        friend class ArtifactRegistry;
    };

    /** @brief Runtime storage and contract table for one pipeline Scope. */
    class ArtifactRegistry final {
    public:
        explicit ArtifactRegistry(PipelineScope scope = PipelineScope::kEngine);
        ~ArtifactRegistry();

        ArtifactRegistry(const ArtifactRegistry&) = delete;
        ArtifactRegistry& operator=(const ArtifactRegistry&) = delete;
        ArtifactRegistry(ArtifactRegistry&&) = delete;
        ArtifactRegistry& operator=(ArtifactRegistry&&) = delete;

        [[nodiscard]] PipelineScope scope() const noexcept;
        [[nodiscard]] bool contains(ArtifactId id) const noexcept;

        /** @brief Installs one immutable artifact contract without publishing a value. */
        [[nodiscard]] foundation::Result install_contract(const ArtifactDescriptor& descriptor);
        /** @brief Installs all contracts carried by a compiled plan. */
        [[nodiscard]] foundation::Result install_plan(const PipelinePlan& plan);
        /** @brief Removes a contract and any value owned by this registry. */
        [[nodiscard]] foundation::Result unregister_artifact(ArtifactId id);
        /** @brief Returns the installed immutable contract for an artifact. */
        [[nodiscard]] foundation::ResultValue<ArtifactDescriptor> find_descriptor(ArtifactId id) const;
        /** @brief Reads the most recently published artifact value. */
        [[nodiscard]] foundation::ResultValue<ArtifactView> read(
            ArtifactId id,
            ArtifactTypeId expected_type = {},
            uint32_t required_version = 0) const;
        /** @brief Acquires an exclusive lease for an artifact owned by this Scope. */
        [[nodiscard]] foundation::ResultValue<ArtifactWriter> write(
            ArtifactId id,
            ArtifactTypeId expected_type = {},
            uint32_t required_version = 0);
        /** @brief Publishes or replaces an artifact value owned by this Scope. */
        [[nodiscard]] foundation::Result publish(ArtifactId id, ArtifactPayload payload);
        /** @brief Releases payloads whose lifetime has ended. */
        [[nodiscard]] foundation::Result release_lifetime(ArtifactLifetime lifetime);

        template <typename Type, typename... Arguments>
        [[nodiscard]] foundation::Result emplace_artifact(ArtifactId id,
                                                           ArtifactTypeId type,
                                                           Arguments&&... arguments) {
            try {
                std::shared_ptr<Type> value = std::make_shared<Type>(
                    std::forward<Arguments>(arguments)...);
                const foundation::ResultValue<ArtifactDescriptor> descriptor = find_descriptor(id);
                if (!descriptor) {
                    return descriptor.result;
                }
                if (!type.is_valid() || descriptor.value.type != type) {
                    return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                        "artifact type does not match its contract");
                }
                const bool allow_writes = descriptor.value.storage != ArtifactStorage::kImmutableSnapshot &&
                                          descriptor.value.ownership != OwnershipPolicy::kSharedImmutable;
                return publish(id, ArtifactPayload::owned(descriptor.value.type,
                                                           std::move(value),
                                                           allow_writes));
            } catch (const std::bad_alloc&) {
                return foundation::Result::failure(foundation::ResultCode::kOutOfMemory,
                                                    "artifact allocation failed");
            } catch (const std::exception& exception) {
                return foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what());
            } catch (...) {
                return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                                    "artifact construction threw an unknown exception");
            }
        }

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_{};

        friend class ArtifactScopeBridge;
        friend class PipelineExecutionContext;
    };

    /** @brief Read-only snapshot bridge from a lower Scope to an upper Scope. */
    class ArtifactScopeBridge final {
    public:
        ArtifactScopeBridge(PipelineScope source_scope,
                            PipelineScope target_scope,
                            const ArtifactRegistry& source_registry);
        ~ArtifactScopeBridge();

        ArtifactScopeBridge(const ArtifactScopeBridge&) = delete;
        ArtifactScopeBridge& operator=(const ArtifactScopeBridge&) = delete;
        ArtifactScopeBridge(ArtifactScopeBridge&&) = delete;
        ArtifactScopeBridge& operator=(ArtifactScopeBridge&&) = delete;

        [[nodiscard]] PipelineScope source_scope() const noexcept;
        [[nodiscard]] PipelineScope target_scope() const noexcept;

        /** @brief Allows one immutable snapshot contract to cross the Scope boundary. */
        [[nodiscard]] foundation::Result expose(ArtifactId id);
        /** @brief Captures all exposed source values and replaces the previous snapshot set. */
        [[nodiscard]] foundation::Result capture();
        /** @brief Reads a value from the most recently captured snapshot set. */
        [[nodiscard]] foundation::ResultValue<ArtifactView> read(
            ArtifactId id,
            ArtifactTypeId expected_type = {},
            uint32_t required_version = 0) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_{};
    };

    /** @brief Runtime context shared by tasks during one pipeline execution. */
    class PipelineExecutionContext final {
    public:
        explicit PipelineExecutionContext(void* user_data = nullptr,
                                           PipelineScope scope = PipelineScope::kEngine,
                                           ArtifactRegistry* artifact_registry = nullptr,
                                           const ArtifactScopeBridge* scope_bridge = nullptr) noexcept
            : user_data_(user_data),
              scope_(scope),
              artifact_registry_(artifact_registry),
              scope_bridge_(scope_bridge) {
        }

        [[nodiscard]] void* user_data() const noexcept { return user_data_; }
        [[nodiscard]] PipelineScope scope() const noexcept { return scope_; }
        [[nodiscard]] ArtifactRegistry* artifact_registry() const noexcept { return artifact_registry_; }
        [[nodiscard]] const ArtifactScopeBridge* scope_bridge() const noexcept { return scope_bridge_; }
        void set_artifact_runtime(ArtifactRegistry* artifact_registry,
                                   const ArtifactScopeBridge* scope_bridge = nullptr) noexcept {
            artifact_registry_ = artifact_registry;
            scope_bridge_ = scope_bridge;
        }

    private:
        void* user_data_{nullptr};
        PipelineScope scope_{PipelineScope::kEngine};
        ArtifactRegistry* artifact_registry_{nullptr};
        const ArtifactScopeBridge* scope_bridge_{nullptr};
        const PipelinePlan* active_plan_{nullptr};

        [[nodiscard]] foundation::Result begin_plan(const PipelinePlan& plan) noexcept;
        void end_plan() noexcept;
        [[nodiscard]] foundation::ResultValue<ArtifactView> read_artifact(
            const PipelineNode& node,
            ArtifactId id,
            ArtifactTypeId expected_type,
            uint32_t required_version) const;
        [[nodiscard]] foundation::ResultValue<ArtifactWriter> write_artifact(
            const PipelineNode& node,
            ArtifactId id,
            ArtifactTypeId expected_type,
            uint32_t required_version);
        [[nodiscard]] foundation::Result publish_artifact(
            const PipelineNode& node,
            ArtifactId id,
            ArtifactPayload payload);

        friend class PipelineScheduler;
        friend class PipelineScopeHost;
        friend class PipelineTaskContext;
    };

    /** @brief Per-task view passed to a TaskExecutionFunction. */
    class PipelineTaskContext final {
    public:
        PipelineTaskContext(PipelineExecutionContext& execution_context, const PipelineNode& node) noexcept
            : execution_context_(execution_context), node_(node) {
        }

        [[nodiscard]] PipelineExecutionContext& execution_context() const noexcept { return execution_context_; }
        [[nodiscard]] const PipelineNode& node() const noexcept { return node_; }
        [[nodiscard]] void* user_data() const noexcept { return node_.user_data; }
        [[nodiscard]] foundation::ResultValue<ArtifactView> read_artifact(
            ArtifactId id,
            ArtifactTypeId expected_type = {},
            uint32_t required_version = 0) const;
        [[nodiscard]] foundation::ResultValue<ArtifactWriter> write_artifact(
            ArtifactId id,
            ArtifactTypeId expected_type = {},
            uint32_t required_version = 0) const;
        [[nodiscard]] foundation::Result publish_artifact(ArtifactId id,
                                                           ArtifactPayload payload) const;

    private:
        PipelineExecutionContext& execution_context_;
        const PipelineNode& node_;
    };

    /** @brief Declarative builder used by Pipeline::build(). */
    class PipelineBuilder final {
    public:
        explicit PipelineBuilder(const PipelineDescriptor& descriptor);
        ~PipelineBuilder() = default;

        PipelineBuilder(const PipelineBuilder&) = delete;
        PipelineBuilder& operator=(const PipelineBuilder&) = delete;
        PipelineBuilder(PipelineBuilder&&) = delete;
        PipelineBuilder& operator=(PipelineBuilder&&) = delete;

        /** @brief Adds a stage declaration to the current definition. */
        [[nodiscard]] PipelineStageHandle add_stage(const StageDescriptor& descriptor);
        /** @brief Adds an executable task declaration to the current definition. */
        [[nodiscard]] PipelineTaskHandle add_task(const TaskDescriptor& descriptor);
        /** @brief Adds an artifact contract to the current definition. */
        [[nodiscard]] PipelineArtifactHandle declare_artifact(const ArtifactDescriptor& descriptor);
        /** @brief Publishes a feature extension point on an anchor stage. */
        [[nodiscard]] PipelineFeaturePointHandle add_feature_point(const FeaturePointDescriptor& descriptor);
        /** @brief Declares a feature binding owned by this pipeline definition. */
        [[nodiscard]] foundation::Result bind_feature(const FeatureBindingDescriptor& binding);

        /** @brief Binds a task to a stage. */
        [[nodiscard]] foundation::Result bind_task(PipelineTaskHandle task, PipelineStageHandle stage);
        /** @brief Declares a read access for a stage. */
        [[nodiscard]] foundation::Result reads(PipelineStageHandle stage, const ArtifactBinding& binding);
        /** @brief Declares a write access for a stage. */
        [[nodiscard]] foundation::Result writes(PipelineStageHandle stage, const ArtifactBinding& binding);
        /** @brief Declares a read/write access for a stage. */
        [[nodiscard]] foundation::Result reads_writes(PipelineStageHandle stage, const ArtifactBinding& binding);
        /** @brief Declares immutable snapshot publication for a stage. */
        [[nodiscard]] foundation::Result publish_snapshot(PipelineStageHandle stage,
                                                           const ArtifactBinding& binding);
        /** @brief Requires one stage to execute before another stage. */
        [[nodiscard]] foundation::Result runs_before(PipelineStageHandle stage, StageId other);
        /** @brief Requires one stage to execute after another stage. */
        [[nodiscard]] foundation::Result runs_after(PipelineStageHandle stage, StageId other);
        /** @brief Exposes an artifact to another pipeline scope. */
        [[nodiscard]] foundation::Result expose_artifact(ArtifactId artifact, PipelineScope target_scope);

        /** @brief Returns the first definition error recorded by the builder. */
        [[nodiscard]] const foundation::Result& status() const noexcept { return status_; }
        /** @brief Finalizes the definition and transfers its declaration records. */
        [[nodiscard]] foundation::ResultValue<PipelineDefinition> finish();

    private:
        [[nodiscard]] bool validate_stage(PipelineStageHandle stage) const noexcept;
        [[nodiscard]] bool validate_task(PipelineTaskHandle task) const noexcept;
        [[nodiscard]] bool validate_artifact(PipelineArtifactHandle artifact) const noexcept;
        [[nodiscard]] bool validate_feature_point(PipelineFeaturePointHandle point) const noexcept;
        [[nodiscard]] uint32_t find_stage(StageId id) const noexcept;
        [[nodiscard]] uint32_t find_artifact(ArtifactId id) const noexcept;
        [[nodiscard]] uint32_t find_feature_point(FeaturePointId id) const noexcept;
        [[nodiscard]] foundation::Result add_binding(PipelineStageHandle stage,
                                                       const ArtifactBinding& binding,
                                                       ArtifactAccess expected_access);
        void set_failure(foundation::ResultCode code, std::string message);

        explicit PipelineBuilder(const PipelineDefinition& definition);

        static constexpr uint32_t kBuilderGeneration = 1;

        PipelineDescriptor descriptor_{};
        foundation::Result status_{};
        std::vector<PipelineStageRecord> stages_{};
        std::vector<PipelineTaskRecord> tasks_{};
        std::vector<PipelineArtifactRecord> artifacts_{};
        std::vector<PipelineFeaturePointRecord> feature_points_{};
        std::vector<PipelineFeatureBindingRecord> feature_bindings_{};
        std::vector<PipelineBindingRecord> bindings_{};
        std::vector<PipelineOrderingRecord> orderings_{};
        std::vector<PipelineExposureRecord> exposures_{};
        bool finished_{false};

        friend class FeatureBuildContext;
        friend class PipelineCompiler;
    };

    /** @brief Static pipeline definition supplied to a Registry. */
    class Pipeline {
    public:
        virtual ~Pipeline() = default;

        [[nodiscard]] virtual const PipelineDescriptor& descriptor() const noexcept = 0;
        [[nodiscard]] virtual foundation::Result build(PipelineBuilder& builder) const = 0;
    };

    /** @brief Type-erased interface for a registered pipeline feature. */
    class PipelineFeature {
    public:
        virtual ~PipelineFeature() = default;

        [[nodiscard]] virtual const FeatureDescriptor& descriptor() const noexcept = 0;
        [[nodiscard]] virtual FeatureDomain feature_domain() const noexcept;
        [[nodiscard]] virtual FeatureDomainMask feature_domain_mask() const noexcept;
        [[nodiscard]] virtual foundation::Result collect_contributions(FeatureBuildContext& context) const = 0;
    };

    /** @brief Construction-time context that restricts a feature to one pipeline extension point. */
    class FeatureBuildContext final {
    public:
        [[nodiscard]] PipelineScope scope() const noexcept { return scope_; }
        [[nodiscard]] PipelineId pipeline_id() const noexcept { return pipeline_id_; }
        [[nodiscard]] FeatureId feature_id() const noexcept { return feature_id_; }
        [[nodiscard]] FeatureInstanceId feature_instance_id() const noexcept { return feature_instance_id_; }
        [[nodiscard]] FeaturePartId part_id() const noexcept { return part_id_; }
        [[nodiscard]] FeaturePointId feature_point() const noexcept { return feature_point_.id; }
        [[nodiscard]] FeatureDomain domain() const noexcept { return active_domain_; }
        [[nodiscard]] const FeaturePointDescriptor& feature_point_descriptor() const noexcept {
            return feature_point_;
        }
        [[nodiscard]] bool domain_is_active(FeatureDomain domain) const noexcept {
            return active_domain_ == domain && domain != FeatureDomain::kComposite &&
                   (feature_point_.allowed_domains & to_mask(domain)) != 0;
        }
        [[nodiscard]] bool allows(FeaturePointFlags flag) const noexcept {
            return has_flag(feature_point_.flags, flag);
        }
        [[nodiscard]] uint32_t stage_count() const noexcept;

        [[nodiscard]] FeatureBuildContext for_domain(FeatureDomain domain) const noexcept {
            FeatureBuildContext result(*this);
            result.active_domain_ = domain;
            return result;
        }

        [[nodiscard]] FeatureBuildContext for_part(FeaturePartId part) const noexcept {
            FeatureBuildContext result(*this);
            result.part_id_ = part;
            return result;
        }

        [[nodiscard]] LogicGraphBuilder logic_builder() noexcept;
        [[nodiscard]] PhysicsGraphBuilder physics_builder() noexcept;
        [[nodiscard]] RenderGraphBuilder render_builder() noexcept;
        [[nodiscard]] AudioGraphBuilder audio_builder() noexcept;
        [[nodiscard]] StreamGraphBuilder stream_builder() noexcept;

        FeatureBuildContext(const FeatureBuildContext&) noexcept = default;
        FeatureBuildContext& operator=(const FeatureBuildContext&) noexcept = default;

    private:
        FeatureBuildContext(PipelineBuilder& builder,
                            PipelineScope scope,
                            PipelineId pipeline_id,
                            FeatureId feature_id,
                            FeatureInstanceId feature_instance_id,
                            FeaturePartId part_id,
                            const FeaturePointDescriptor& feature_point,
                            FeatureDomain active_domain,
                            std::vector<PipelineFeatureContributionRecord>* contribution_records) noexcept
            : builder_(&builder),
              scope_(scope),
              pipeline_id_(pipeline_id),
              feature_id_(feature_id),
              feature_instance_id_(feature_instance_id),
              part_id_(part_id),
              feature_point_(feature_point),
              active_domain_(active_domain),
              contribution_records_(contribution_records) {
        }

        [[nodiscard]] foundation::Result reject(foundation::ResultCode code, std::string message);
        void on_stage_added(PipelineStageHandle stage);
        void record_contribution(FeatureDomain domain, uint32_t first_stage_index);

        PipelineBuilder* builder_{nullptr};
        PipelineScope scope_{PipelineScope::kEngine};
        PipelineId pipeline_id_{};
        FeatureId feature_id_{};
        FeatureInstanceId feature_instance_id_{};
        FeaturePartId part_id_{};
        FeaturePointDescriptor feature_point_{};
        FeatureDomain active_domain_{FeatureDomain::kLogic};
        std::vector<PipelineFeatureContributionRecord>* contribution_records_{nullptr};

        friend class PipelineCompiler;
        friend class LogicFeature;
        friend class PhysicsFeature;
        friend class RenderFeature;
        friend class AudioFeature;
        friend class StreamFeature;
        template <FeatureDomain Domain>
        friend class DomainGraphBuilder;
    };

    /** @brief Domain-restricted view over the pipeline declaration builder. */
    template <FeatureDomain Domain>
    class DomainGraphBuilder final {
    public:
        static constexpr FeatureDomain kDomain = Domain;

        [[nodiscard]] PipelineStageHandle add_stage(const StageDescriptor& descriptor);
        [[nodiscard]] PipelineTaskHandle add_task(const TaskDescriptor& descriptor);
        [[nodiscard]] PipelineArtifactHandle declare_artifact(const ArtifactDescriptor& descriptor);

        [[nodiscard]] foundation::Result bind_task(PipelineTaskHandle task, PipelineStageHandle stage);
        [[nodiscard]] foundation::Result reads(PipelineStageHandle stage, const ArtifactBinding& binding);
        [[nodiscard]] foundation::Result writes(PipelineStageHandle stage, const ArtifactBinding& binding);
        [[nodiscard]] foundation::Result reads_writes(PipelineStageHandle stage, const ArtifactBinding& binding);
        [[nodiscard]] foundation::Result publish_snapshot(PipelineStageHandle stage,
                                                           const ArtifactBinding& binding);
        [[nodiscard]] foundation::Result runs_before(PipelineStageHandle stage, StageId other);
        [[nodiscard]] foundation::Result runs_after(PipelineStageHandle stage, StageId other);
        [[nodiscard]] foundation::Result expose_artifact(ArtifactId artifact, PipelineScope target_scope);

    private:
        explicit DomainGraphBuilder(FeatureBuildContext& context) noexcept : context_(&context) {
        }

        [[nodiscard]] bool is_usable() const noexcept {
            return context_ != nullptr && context_->domain_is_active(Domain);
        }

        FeatureBuildContext* context_{nullptr};

        friend class FeatureBuildContext;
    };

    /** @brief Feature restricted to CPU and non-visual computation contributions. */
    class LogicFeature : public PipelineFeature {
    public:
        static constexpr FeatureDomain kDomain = FeatureDomain::kLogic;
        using BuilderType = LogicGraphBuilder;

        [[nodiscard]] const FeatureDescriptor& descriptor() const noexcept override;
        [[nodiscard]] FeatureDomain feature_domain() const noexcept final { return kDomain; }
        [[nodiscard]] FeatureDomainMask feature_domain_mask() const noexcept final { return to_mask(kDomain); }
        [[nodiscard]] foundation::Result collect_contributions(FeatureBuildContext& context) const final;

    protected:
        [[nodiscard]] virtual foundation::Result setup(BuilderType& builder) const = 0;
    };

    /** @brief Feature restricted to simulation and physics graph contributions. */
    class PhysicsFeature : public PipelineFeature {
    public:
        static constexpr FeatureDomain kDomain = FeatureDomain::kPhysics;
        using BuilderType = PhysicsGraphBuilder;

        [[nodiscard]] const FeatureDescriptor& descriptor() const noexcept override;
        [[nodiscard]] FeatureDomain feature_domain() const noexcept final { return kDomain; }
        [[nodiscard]] FeatureDomainMask feature_domain_mask() const noexcept final { return to_mask(kDomain); }
        [[nodiscard]] foundation::Result collect_contributions(FeatureBuildContext& context) const final;

    protected:
        [[nodiscard]] virtual foundation::Result setup(BuilderType& builder) const = 0;
    };

    /** @brief Feature restricted to render graph and presentation contributions. */
    class RenderFeature : public PipelineFeature {
    public:
        static constexpr FeatureDomain kDomain = FeatureDomain::kRender;
        using BuilderType = RenderGraphBuilder;

        [[nodiscard]] const FeatureDescriptor& descriptor() const noexcept override;
        [[nodiscard]] FeatureDomain feature_domain() const noexcept final { return kDomain; }
        [[nodiscard]] FeatureDomainMask feature_domain_mask() const noexcept final { return to_mask(kDomain); }
        [[nodiscard]] foundation::Result collect_contributions(FeatureBuildContext& context) const final;

    protected:
        [[nodiscard]] virtual foundation::Result setup(BuilderType& builder) const = 0;
    };

    /** @brief Feature restricted to audio graph and spatial sound contributions. */
    class AudioFeature : public PipelineFeature {
    public:
        static constexpr FeatureDomain kDomain = FeatureDomain::kAudio;
        using BuilderType = AudioGraphBuilder;

        [[nodiscard]] const FeatureDescriptor& descriptor() const noexcept override;
        [[nodiscard]] FeatureDomain feature_domain() const noexcept final { return kDomain; }
        [[nodiscard]] FeatureDomainMask feature_domain_mask() const noexcept final { return to_mask(kDomain); }
        [[nodiscard]] foundation::Result collect_contributions(FeatureBuildContext& context) const final;

    protected:
        [[nodiscard]] virtual foundation::Result setup(BuilderType& builder) const = 0;
    };

    /** @brief Feature restricted to IO, decoding, and resource streaming contributions. */
    class StreamFeature : public PipelineFeature {
    public:
        static constexpr FeatureDomain kDomain = FeatureDomain::kStream;
        using BuilderType = StreamGraphBuilder;

        [[nodiscard]] const FeatureDescriptor& descriptor() const noexcept override;
        [[nodiscard]] FeatureDomain feature_domain() const noexcept final { return kDomain; }
        [[nodiscard]] FeatureDomainMask feature_domain_mask() const noexcept final { return to_mask(kDomain); }
        [[nodiscard]] foundation::Result collect_contributions(FeatureBuildContext& context) const final;

    protected:
        [[nodiscard]] virtual foundation::Result setup(BuilderType& builder) const = 0;
    };

    /** @brief Member-composition adapter for a feature spanning multiple domains. */
    template <typename... Parts>
    class CompositeFeature : public PipelineFeature {
    public:
        static_assert(sizeof...(Parts) > 0, "CompositeFeature requires at least one part");
        static_assert((std::is_base_of_v<PipelineFeature, Parts> && ...),
                      "CompositeFeature parts must derive from PipelineFeature");
        static_assert((requires { Parts::kDomain; } && ...),
                      "CompositeFeature parts must expose a domain tag");
        static_assert((requires { typename Parts::BuilderType; } && ...),
                      "CompositeFeature parts must expose a domain builder type");
        static_assert(((Parts::kDomain != FeatureDomain::kComposite) && ...),
                      "CompositeFeature cannot contain another CompositeFeature");

        static constexpr FeatureDomain kDomain = FeatureDomain::kComposite;
        static constexpr FeatureDomainMask kDomainMask = (to_mask(Parts::kDomain) | ...);

        CompositeFeature()
            requires((std::is_default_constructible_v<Parts> && ...)) = default;

        template <typename... Arguments>
            requires(sizeof...(Arguments) == sizeof...(Parts))
        explicit CompositeFeature(Arguments&&... arguments)
            : parts_(std::forward<Arguments>(arguments)...) {
        }

        [[nodiscard]] FeatureDomain feature_domain() const noexcept final { return kDomain; }
        [[nodiscard]] FeatureDomainMask feature_domain_mask() const noexcept final { return kDomainMask; }

        [[nodiscard]] foundation::Result collect_contributions(FeatureBuildContext& context) const final {
            foundation::Result result = foundation::Result::success();
            uint32_t part_index = 0;
            const auto visit_part = [&](const auto& part) {
                if (!result) {
                    return;
                }
                using PartType = std::remove_cvref_t<decltype(part)>;
                FeatureBuildContext part_context = context.for_part(
                    make_feature_part_id(context.feature_instance_id(), part_index++));
                part_context = part_context.for_domain(PartType::kDomain);
                result = part.collect_contributions(part_context);
            };
            std::apply([&](const auto&... part) { (visit_part(part), ...); }, parts_);
            return result;
        }

        template <typename Part>
        [[nodiscard]] Part& part() noexcept {
            return std::get<Part>(parts_);
        }

        template <typename Part>
        [[nodiscard]] const Part& part() const noexcept {
            return std::get<Part>(parts_);
        }

    protected:
        std::tuple<Parts...> parts_{};
    };

    /** @brief Runtime owner-independent interface for a pipeline instance. */
    struct PipelineActivationContext {
        void* user_data{nullptr};
    };

    /** @brief Runtime teardown context for a pipeline instance. */
    struct PipelineDeactivationContext {
        void* user_data{nullptr};
    };

    /** @brief Pipeline definition bound to one concrete Scope owner. */
    class PipelineInstance {
    public:
        virtual ~PipelineInstance() = default;

        [[nodiscard]] virtual PipelineInstanceId id() const noexcept = 0;
        [[nodiscard]] virtual PipelineId pipeline_id() const noexcept = 0;
        [[nodiscard]] virtual PipelineScope scope() const noexcept = 0;
        [[nodiscard]] virtual void* owner() const noexcept = 0;
        [[nodiscard]] virtual foundation::Result activate(const PipelineActivationContext& context) = 0;
        [[nodiscard]] virtual foundation::Result deactivate(const PipelineDeactivationContext& context) = 0;
    };

    using PipelineFactory = std::function<std::unique_ptr<Pipeline>()>;
    using FeatureFactory = std::function<std::unique_ptr<PipelineFeature>()>;

    /** @brief Thread-safe registry of immutable pipeline definitions. */
    class PipelineRegistry final {
    public:
        PipelineRegistry() = default;
        ~PipelineRegistry() = default;

        PipelineRegistry(const PipelineRegistry&) = delete;
        PipelineRegistry& operator=(const PipelineRegistry&) = delete;
        PipelineRegistry(PipelineRegistry&&) = delete;
        PipelineRegistry& operator=(PipelineRegistry&&) = delete;

        /** @brief Creates, builds, validates, and registers one pipeline type. */
        [[nodiscard]] foundation::Result register_pipeline(PipelineFactory factory);
        /** @brief Removes a pipeline type from future compiler lookups. */
        [[nodiscard]] foundation::Result unregister_pipeline(PipelineId id);
        /** @brief Returns a shared immutable view of one registered definition. */
        [[nodiscard]] foundation::ResultValue<std::shared_ptr<const PipelineDefinition>> find_definition(
            PipelineId id) const;
        /** @brief Creates, validates, and registers one feature type. */
        [[nodiscard]] foundation::Result register_feature(FeatureFactory factory);
        /** @brief Removes a feature type that has no active bindings. */
        [[nodiscard]] foundation::Result unregister_feature(FeatureId id);
        /** @brief Returns immutable metadata for one registered feature. */
        [[nodiscard]] foundation::ResultValue<FeatureDescriptor> find_feature_descriptor(FeatureId id) const;
        /** @brief Binds a registered feature to a pipeline feature point. */
        [[nodiscard]] foundation::Result bind_feature(const FeatureBindingDescriptor& binding);
        /** @brief Removes one feature binding by its stable instance identity. */
        [[nodiscard]] foundation::Result unbind_feature(FeatureInstanceId instance);

    private:
        struct Registration {
            std::shared_ptr<Pipeline> implementation{};
            std::shared_ptr<const PipelineDefinition> definition{};
        };

        struct FeatureRegistration {
            std::shared_ptr<PipelineFeature> implementation{};
            FeatureDescriptor descriptor{};
        };

        [[nodiscard]] foundation::ResultValue<Registration> acquire_registration(PipelineId id) const;
        [[nodiscard]] foundation::ResultValue<FeatureRegistration> acquire_feature_registration(FeatureId id) const;
        [[nodiscard]] foundation::ResultValue<std::vector<FeatureBindingDescriptor>> acquire_feature_bindings(
            PipelineId pipeline,
            PipelineScope scope) const;

        mutable std::shared_mutex mutex_;
        std::unordered_map<PipelineId, Registration, StableIdHash<PipelineIdTag>> registrations_{};
        std::unordered_map<FeatureId, FeatureRegistration, StableIdHash<FeatureIdTag>> features_{};
        std::unordered_map<FeatureInstanceId,
                           FeatureBindingDescriptor,
                           StableIdHash<FeatureInstanceIdTag>>
            feature_bindings_{};

        friend class PipelineCompiler;
    };

    /** @brief Input required to compile one registered pipeline definition. */
    struct PipelineCompileInput {
        PipelineId pipeline{};
        PipelineScope scope{PipelineScope::kEngine};
        CapabilityMask available_capabilities{0};
        const PipelineRegistry* registry{nullptr};
        PipelineInstanceId instance{};
    };

    /** @brief Converts one registered definition into an immutable execution plan. */
    class PipelineCompiler final {
    public:
        [[nodiscard]] foundation::ResultValue<PipelinePlan> compile(const PipelineCompileInput& input) const;
    };

    /** @brief Executes compiled CPU-capable pipeline batches. */
    class PipelineScheduler final {
    public:
        explicit PipelineScheduler(uint32_t worker_count = 0);
        ~PipelineScheduler();

        PipelineScheduler(const PipelineScheduler&) = delete;
        PipelineScheduler& operator=(const PipelineScheduler&) = delete;
        PipelineScheduler(PipelineScheduler&&) noexcept;
        PipelineScheduler& operator=(PipelineScheduler&&) noexcept;

        /** @brief Executes batches in dependency order and parallelizes safe batches. */
        [[nodiscard]] foundation::Result execute(const PipelinePlan& plan,
                                                  PipelineExecutionContext& context) const;
        /** @brief Returns the number of worker threads owned by this scheduler. */
        [[nodiscard]] uint32_t worker_count() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_{};
    };

    /** @brief Generic Scope owner for one independently compiled pipeline instance. */
    class PipelineScopeHost : public PipelineInstance {
    public:
        explicit PipelineScopeHost(PipelineScope scope,
                                   PipelineInstanceId instance = {},
                                   void* owner = nullptr)
            : scope_(scope), instance_(instance), owner_(owner), artifacts_(scope) {
        }

        PipelineScopeHost(const PipelineScopeHost&) = delete;
        PipelineScopeHost& operator=(const PipelineScopeHost&) = delete;
        PipelineScopeHost(PipelineScopeHost&&) = delete;
        PipelineScopeHost& operator=(PipelineScopeHost&&) = delete;

        [[nodiscard]] PipelineInstanceId id() const noexcept override { return instance_; }
        [[nodiscard]] PipelineScope scope() const noexcept override { return scope_; }
        [[nodiscard]] PipelineInstanceId instance_id() const noexcept { return instance_; }
        [[nodiscard]] PipelineId pipeline_id() const noexcept override { return pipeline_; }
        [[nodiscard]] void* owner() const noexcept override { return owner_; }
        [[nodiscard]] bool is_active() const noexcept { return active_; }
        [[nodiscard]] ArtifactRegistry& artifacts() noexcept { return artifacts_; }
        [[nodiscard]] const ArtifactRegistry& artifacts() const noexcept { return artifacts_; }
        [[nodiscard]] const PipelinePlan* active_plan() const noexcept {
            return active_plan_.has_value() ? &*active_plan_ : nullptr;
        }

        [[nodiscard]] foundation::Result bind_pipeline(PipelineId pipeline);
        [[nodiscard]] foundation::Result rebuild(const PipelineRegistry& registry,
                                                  CapabilityMask available_capabilities = 0);
        void set_scope_bridge(const ArtifactScopeBridge* scope_bridge) noexcept {
            scope_bridge_ = scope_bridge;
        }
        [[nodiscard]] foundation::Result activate(const PipelineActivationContext& context) override;
        [[nodiscard]] foundation::Result deactivate(const PipelineDeactivationContext& context) override;
        [[nodiscard]] foundation::Result execute(PipelineScheduler& scheduler,
                                                  PipelineExecutionContext& context) const;

    private:
        PipelineScope scope_{PipelineScope::kEngine};
        PipelineInstanceId instance_{};
        void* owner_{nullptr};
        PipelineId pipeline_{};
        mutable ArtifactRegistry artifacts_;
        const ArtifactScopeBridge* scope_bridge_{nullptr};
        std::optional<PipelinePlan> active_plan_{};
        bool active_{false};
    };

    template <FeatureDomain Domain>
    PipelineStageHandle DomainGraphBuilder<Domain>::add_stage(const StageDescriptor& descriptor) {
        if (!is_usable()) {
            return {};
        }
        const PipelineStageHandle stage = context_->builder_->add_stage(descriptor);
        if (stage) {
            context_->on_stage_added(stage);
        }
        return stage;
    }

    template <FeatureDomain Domain>
    PipelineTaskHandle DomainGraphBuilder<Domain>::add_task(const TaskDescriptor& descriptor) {
        return is_usable() ? context_->builder_->add_task(descriptor) : PipelineTaskHandle{};
    }

    template <FeatureDomain Domain>
    PipelineArtifactHandle DomainGraphBuilder<Domain>::declare_artifact(const ArtifactDescriptor& descriptor) {
        if (!is_usable()) {
            return {};
        }
        if (!context_->allows(FeaturePointFlags::kMayCreateArtifacts)) {
            context_->reject(foundation::ResultCode::kUnsupported,
                             "feature point does not allow artifact creation");
            return {};
        }
        return context_->builder_->declare_artifact(descriptor);
    }

    template <FeatureDomain Domain>
    foundation::Result DomainGraphBuilder<Domain>::bind_task(PipelineTaskHandle task, PipelineStageHandle stage) {
        return is_usable() ? context_->builder_->bind_task(task, stage)
                           : context_->reject(foundation::ResultCode::kInvalidState,
                                              "feature builder domain is not active");
    }

    template <FeatureDomain Domain>
    foundation::Result DomainGraphBuilder<Domain>::reads(PipelineStageHandle stage,
                                                          const ArtifactBinding& binding) {
        return is_usable() ? context_->builder_->reads(stage, binding)
                           : context_->reject(foundation::ResultCode::kInvalidState,
                                              "feature builder domain is not active");
    }

    template <FeatureDomain Domain>
    foundation::Result DomainGraphBuilder<Domain>::writes(PipelineStageHandle stage,
                                                           const ArtifactBinding& binding) {
        return is_usable() ? context_->builder_->writes(stage, binding)
                           : context_->reject(foundation::ResultCode::kInvalidState,
                                              "feature builder domain is not active");
    }

    template <FeatureDomain Domain>
    foundation::Result DomainGraphBuilder<Domain>::reads_writes(PipelineStageHandle stage,
                                                                 const ArtifactBinding& binding) {
        return is_usable() ? context_->builder_->reads_writes(stage, binding)
                           : context_->reject(foundation::ResultCode::kInvalidState,
                                              "feature builder domain is not active");
    }

    template <FeatureDomain Domain>
    foundation::Result DomainGraphBuilder<Domain>::publish_snapshot(
        PipelineStageHandle stage,
        const ArtifactBinding& binding) {
        if (!is_usable()) {
            return context_->reject(foundation::ResultCode::kInvalidState,
                                    "feature builder domain is not active");
        }
        if (!context_->allows(FeaturePointFlags::kMayPublishSnapshot)) {
            return context_->reject(foundation::ResultCode::kUnsupported,
                                    "feature point does not allow snapshot publication");
        }
        return context_->builder_->publish_snapshot(stage, binding);
    }

    template <FeatureDomain Domain>
    foundation::Result DomainGraphBuilder<Domain>::runs_before(PipelineStageHandle stage, StageId other) {
        return is_usable() ? context_->builder_->runs_before(stage, other)
                           : context_->reject(foundation::ResultCode::kInvalidState,
                                              "feature builder domain is not active");
    }

    template <FeatureDomain Domain>
    foundation::Result DomainGraphBuilder<Domain>::runs_after(PipelineStageHandle stage, StageId other) {
        return is_usable() ? context_->builder_->runs_after(stage, other)
                           : context_->reject(foundation::ResultCode::kInvalidState,
                                              "feature builder domain is not active");
    }

    template <FeatureDomain Domain>
    foundation::Result DomainGraphBuilder<Domain>::expose_artifact(ArtifactId artifact,
                                                                    PipelineScope target_scope) {
        return is_usable() ? context_->builder_->expose_artifact(artifact, target_scope)
                           : context_->reject(foundation::ResultCode::kInvalidState,
                                              "feature builder domain is not active");
    }
}  // namespace aeg::pipeline
