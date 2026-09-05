/**
 * @file pipeline.hpp
 * @brief Public interfaces for defining, compiling, and executing pipelines.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pipeline_types.hpp"

namespace aeg::pipeline {
    /** @brief Runtime context shared by tasks during one pipeline execution. */
    class PipelineExecutionContext final {
    public:
        explicit PipelineExecutionContext(void* user_data = nullptr,
                                           PipelineScope scope = PipelineScope::kEngine) noexcept
            : user_data_(user_data), scope_(scope) {
        }

        [[nodiscard]] void* user_data() const noexcept { return user_data_; }
        [[nodiscard]] PipelineScope scope() const noexcept { return scope_; }

    private:
        void* user_data_{nullptr};
        PipelineScope scope_{PipelineScope::kEngine};
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

        /** @brief Binds a task to a stage. */
        [[nodiscard]] foundation::Result bind_task(PipelineTaskHandle task, PipelineStageHandle stage);
        /** @brief Declares a read access for a stage. */
        [[nodiscard]] foundation::Result reads(PipelineStageHandle stage, const ArtifactBinding& binding);
        /** @brief Declares a write access for a stage. */
        [[nodiscard]] foundation::Result writes(PipelineStageHandle stage, const ArtifactBinding& binding);
        /** @brief Declares a read/write access for a stage. */
        [[nodiscard]] foundation::Result reads_writes(PipelineStageHandle stage, const ArtifactBinding& binding);
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
        [[nodiscard]] uint32_t find_stage(StageId id) const noexcept;
        [[nodiscard]] uint32_t find_artifact(ArtifactId id) const noexcept;
        [[nodiscard]] foundation::Result add_binding(PipelineStageHandle stage,
                                                       const ArtifactBinding& binding,
                                                       ArtifactAccess expected_access);
        void set_failure(foundation::ResultCode code, std::string message);

        static constexpr uint32_t kBuilderGeneration = 1;

        PipelineDescriptor descriptor_{};
        foundation::Result status_{};
        std::vector<PipelineStageRecord> stages_{};
        std::vector<PipelineTaskRecord> tasks_{};
        std::vector<PipelineArtifactRecord> artifacts_{};
        std::vector<PipelineBindingRecord> bindings_{};
        std::vector<PipelineOrderingRecord> orderings_{};
        std::vector<PipelineExposureRecord> exposures_{};
        bool finished_{false};
    };

    /** @brief Static pipeline definition supplied to a Registry. */
    class Pipeline {
    public:
        virtual ~Pipeline() = default;

        [[nodiscard]] virtual const PipelineDescriptor& descriptor() const noexcept = 0;
        [[nodiscard]] virtual foundation::Result build(PipelineBuilder& builder) const = 0;
    };

    using PipelineFactory = std::function<std::unique_ptr<Pipeline>()>;

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

    private:
        struct Registration {
            std::shared_ptr<Pipeline> implementation{};
            std::shared_ptr<const PipelineDefinition> definition{};
        };

        [[nodiscard]] foundation::ResultValue<Registration> acquire_registration(PipelineId id) const;

        mutable std::shared_mutex mutex_;
        std::unordered_map<PipelineId, Registration, StableIdHash<PipelineIdTag>> registrations_{};

        friend class PipelineCompiler;
    };

    /** @brief Input required to compile one registered pipeline definition. */
    struct PipelineCompileInput {
        PipelineId pipeline{};
        PipelineScope scope{PipelineScope::kEngine};
        CapabilityMask available_capabilities{0};
        const PipelineRegistry* registry{nullptr};
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
}  // namespace aeg::pipeline
