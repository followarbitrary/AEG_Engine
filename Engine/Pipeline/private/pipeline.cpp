/**
 * @file pipeline.cpp
 * @brief Implements pipeline definition collection, compilation, and scheduling.
 */
#include "../public/pipeline.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <numeric>
#include <queue>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aeg::pipeline {
    namespace {
        inline constexpr uint32_t kKnownTaskFlags = static_cast<uint32_t>(TaskFlags::kParallelSafe) |
                                                    static_cast<uint32_t>(TaskFlags::kOptional) |
                                                    static_cast<uint32_t>(TaskFlags::kSideEffect) |
                                                    static_cast<uint32_t>(TaskFlags::kMustRun) |
                                                    static_cast<uint32_t>(TaskFlags::kMayBlock) |
                                                    static_cast<uint32_t>(TaskFlags::kExportsCompletion);

        inline constexpr uint32_t kKnownAccessFlags = static_cast<uint32_t>(AccessFlags::kOptional) |
                                                      static_cast<uint32_t>(AccessFlags::kPreserveContents) |
                                                      static_cast<uint32_t>(AccessFlags::kDiscardContents) |
                                                      static_cast<uint32_t>(AccessFlags::kExclusive) |
                                                      static_cast<uint32_t>(AccessFlags::kDeterministicAppend);

        inline constexpr uint32_t kKnownFeaturePointFlags =
            static_cast<uint32_t>(FeaturePointFlags::kMayCreateArtifacts) |
            static_cast<uint32_t>(FeaturePointFlags::kMayCreateGraphicsPass) |
            static_cast<uint32_t>(FeaturePointFlags::kMayCreateComputePass) |
            static_cast<uint32_t>(FeaturePointFlags::kMayCreateCopyPass) |
            static_cast<uint32_t>(FeaturePointFlags::kMayUseDedicatedQueue) |
            static_cast<uint32_t>(FeaturePointFlags::kMayWriteFinalOutput);

        std::string append_message(std::string prefix, const foundation::Result& result) {
            if (!result.message.empty()) {
                prefix += ": ";
                prefix += result.message;
            }
            return prefix;
        }

        [[nodiscard]] bool is_valid_scope(PipelineScope scope) noexcept {
            return static_cast<uint8_t>(scope) <= static_cast<uint8_t>(PipelineScope::kEditor);
        }

        [[nodiscard]] bool is_valid_execution_domain(ExecutionDomain domain) noexcept {
            return static_cast<uint8_t>(domain) <= static_cast<uint8_t>(ExecutionDomain::kGpu);
        }

        [[nodiscard]] bool is_valid_time_policy(TimePolicy policy) noexcept {
            return static_cast<uint8_t>(policy) <= static_cast<uint8_t>(TimePolicy::kShutdown);
        }

        [[nodiscard]] bool is_valid_feature_domain(FeatureDomain domain) noexcept {
            return static_cast<uint8_t>(domain) <= static_cast<uint8_t>(FeatureDomain::kComposite);
        }

        [[nodiscard]] bool is_valid_feature_point_position(FeaturePointPosition position) noexcept {
            return static_cast<uint8_t>(position) <= static_cast<uint8_t>(FeaturePointPosition::kAfter);
        }

        [[nodiscard]] FeatureDomainMask get_feature_domain_mask(const FeatureDescriptor& descriptor) noexcept {
            if (descriptor.domain == FeatureDomain::kComposite) {
                return descriptor.domain_mask;
            }
            return descriptor.domain_mask == 0 ? to_mask(descriptor.domain) : descriptor.domain_mask;
        }

        [[nodiscard]] bool is_valid_engine_phase(EnginePhase phase) noexcept {
            return static_cast<uint8_t>(phase) <= static_cast<uint8_t>(EnginePhase::kShutdown);
        }

        [[nodiscard]] bool is_writer(ArtifactAccess access) noexcept {
            return access == ArtifactAccess::kWrite || access == ArtifactAccess::kReadWrite ||
                   access == ArtifactAccess::kAppend || access == ArtifactAccess::kPublishSnapshot;
        }

        [[nodiscard]] bool is_reader(ArtifactAccess access) noexcept {
            return access == ArtifactAccess::kRead || access == ArtifactAccess::kReadWrite ||
                   access == ArtifactAccess::kConsume;
        }

        [[nodiscard]] bool is_append_writer(ArtifactAccess access) noexcept {
            return access == ArtifactAccess::kAppend;
        }

        [[nodiscard]] bool is_parallel_safe(const PipelineNode& node) noexcept {
            return !node.synthetic && node.stage_parallelizable &&
                   node.execution_domain == ExecutionDomain::kCpuWorker &&
                   has_flag(node.flags, TaskFlags::kParallelSafe) &&
                   !has_flag(node.flags, TaskFlags::kSideEffect);
        }

        [[nodiscard]] foundation::Result invalid_argument(std::string message) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument, std::move(message));
        }

        [[nodiscard]] foundation::Result invalid_state(std::string message) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidState, std::move(message));
        }

        [[nodiscard]] foundation::Result not_found(std::string message) {
            return foundation::Result::failure(foundation::ResultCode::kNotFound, std::move(message));
        }

        [[nodiscard]] foundation::Result unsupported(std::string message) {
            return foundation::Result::failure(foundation::ResultCode::kUnsupported, std::move(message));
        }

        [[nodiscard]] foundation::Result execute_node(const PipelineNode& node,
                                                        PipelineExecutionContext& context) {
            if (node.synthetic || node.execute == nullptr) {
                return foundation::Result::success();
            }

            PipelineTaskContext task_context(context, node);
            try {
                return node.execute(task_context);
            } catch (const std::exception& exception) {
                return foundation::Result::failure(foundation::ResultCode::kUnknown, exception.what());
            } catch (...) {
                return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                                   "pipeline task threw an unknown exception");
            }
        }

        [[nodiscard]] foundation::Result with_task_context(const PipelineNode& node,
                                                           foundation::Result result) {
            if (result) {
                return result;
            }

            std::string message = "pipeline task failed";
            if (node.task.is_valid()) {
                message += " (id=" + std::to_string(node.task.value()) + ")";
            }
            return foundation::Result::failure(result.code, append_message(std::move(message), result));
        }

        class WorkerPool final {
        public:
            explicit WorkerPool(uint32_t worker_count) {
                workers_.reserve(worker_count);
                for (uint32_t worker_index = 0; worker_index < worker_count; ++worker_index) {
                    workers_.emplace_back([this, worker_index] { worker_loop(worker_index); });
                }
            }

            ~WorkerPool() {
                {
                    std::lock_guard lock(mutex_);
                    stopping_ = true;
                }
                work_available_.notify_all();
                for (std::thread& worker : workers_) {
                    if (worker.joinable()) {
                        worker.join();
                    }
                }
            }

            WorkerPool(const WorkerPool&) = delete;
            WorkerPool& operator=(const WorkerPool&) = delete;

            [[nodiscard]] uint32_t worker_count() const noexcept {
                return static_cast<uint32_t>(workers_.size());
            }

            [[nodiscard]] foundation::Result run(const std::vector<PipelineNode>& nodes,
                                                  std::span<const uint32_t> node_indices,
                                                  PipelineExecutionContext& context,
                                                  bool parallel) {
                if (node_indices.empty()) {
                    return foundation::Result::success();
                }

                if (workers_.empty()) {
                    for (const uint32_t node_index : node_indices) {
                        foundation::Result result = execute_node(nodes[node_index], context);
                        if (!result) {
                            return with_task_context(nodes[node_index], std::move(result));
                        }
                    }
                    return foundation::Result::success();
                }

                Batch batch{nodes, node_indices, context, parallel ? worker_count() : 1};
                {
                    std::lock_guard lock(mutex_);
                    batch_ = &batch;
                }
                work_available_.notify_all();

                std::unique_lock lock(mutex_);
                batch_finished_.wait(lock, [&] { return batch_ == nullptr; });
                return batch.result;
            }

        private:
            struct Batch {
                const std::vector<PipelineNode>& nodes;
                std::span<const uint32_t> node_indices;
                PipelineExecutionContext& context;
                uint32_t active_workers{1};
                std::atomic<size_t> next_index{0};
                uint32_t remaining_workers{0};
                std::atomic<bool> failed{false};
                foundation::Result result{};
                std::mutex result_mutex;

                Batch(const std::vector<PipelineNode>& batch_nodes,
                      std::span<const uint32_t> batch_indices,
                      PipelineExecutionContext& batch_context,
                      uint32_t requested_workers)
                    : nodes(batch_nodes),
                      node_indices(batch_indices),
                      context(batch_context),
                      active_workers(requested_workers),
                      remaining_workers(requested_workers) {
                }
            };

            void worker_loop(uint32_t worker_index) {
                while (true) {
                    Batch* batch = nullptr;
                    {
                        std::unique_lock lock(mutex_);
                        work_available_.wait(lock, [&] { return stopping_ || batch_ != nullptr; });
                        if (stopping_) {
                            return;
                        }

                        batch = batch_;
                        if (worker_index >= batch->active_workers) {
                            work_available_.wait(lock, [&] { return stopping_ || batch_ != batch; });
                            if (stopping_) {
                                return;
                            }
                            continue;
                        }
                    }

                    while (!batch->failed) {
                        const size_t next_index = batch->next_index.fetch_add(1, std::memory_order_relaxed);
                        if (next_index >= batch->node_indices.size()) {
                            break;
                        }

                        const uint32_t node_index = batch->node_indices[next_index];
                        foundation::Result result = execute_node(batch->nodes[node_index], batch->context);
                        if (!result) {
                            if (!batch->failed.exchange(true, std::memory_order_acq_rel)) {
                                std::lock_guard result_lock(batch->result_mutex);
                                batch->result = with_task_context(batch->nodes[node_index], std::move(result));
                            }
                            break;
                        }
                    }

                    {
                        std::lock_guard lock(mutex_);
                        if (--batch->remaining_workers == 0) {
                            batch_ = nullptr;
                            batch_finished_.notify_one();
                            work_available_.notify_all();
                        }
                    }
                }
            }

            mutable std::mutex mutex_;
            std::condition_variable work_available_;
            std::condition_variable batch_finished_;
            std::vector<std::thread> workers_;
            Batch* batch_{nullptr};
            bool stopping_{false};
        };
    }  // namespace

    PipelineBuilder::PipelineBuilder(const PipelineDescriptor& descriptor) : descriptor_(descriptor) {
    }

    PipelineBuilder::PipelineBuilder(const PipelineDefinition& definition)
        : descriptor_(definition.descriptor),
          stages_(definition.stages),
          tasks_(definition.tasks),
          artifacts_(definition.artifacts),
          feature_points_(definition.feature_points),
          feature_bindings_(definition.feature_bindings),
          bindings_(definition.bindings),
          orderings_(definition.orderings),
          exposures_(definition.exposures) {
    }

    void PipelineBuilder::set_failure(foundation::ResultCode code, std::string message) {
        if (status_) {
            status_ = foundation::Result::failure(code, std::move(message));
        }
    }

    bool PipelineBuilder::validate_stage(PipelineStageHandle stage) const noexcept {
        return stage.is_valid() && stage.generation == kBuilderGeneration && stage.index < stages_.size();
    }

    bool PipelineBuilder::validate_task(PipelineTaskHandle task) const noexcept {
        return task.is_valid() && task.generation == kBuilderGeneration && task.index < tasks_.size();
    }

    bool PipelineBuilder::validate_artifact(PipelineArtifactHandle artifact) const noexcept {
        return artifact.is_valid() && artifact.generation == kBuilderGeneration && artifact.index < artifacts_.size();
    }

    bool PipelineBuilder::validate_feature_point(PipelineFeaturePointHandle point) const noexcept {
        return point.is_valid() && point.generation == kBuilderGeneration && point.index < feature_points_.size();
    }

    uint32_t PipelineBuilder::find_stage(StageId id) const noexcept {
        for (uint32_t index = 0; index < stages_.size(); ++index) {
            if (stages_[index].descriptor.id == id) {
                return index;
            }
        }
        return kInvalidPipelineHandleIndex;
    }

    uint32_t PipelineBuilder::find_artifact(ArtifactId id) const noexcept {
        for (uint32_t index = 0; index < artifacts_.size(); ++index) {
            if (artifacts_[index].descriptor.id == id) {
                return index;
            }
        }
        return kInvalidPipelineHandleIndex;
    }

    uint32_t PipelineBuilder::find_feature_point(FeaturePointId id) const noexcept {
        for (uint32_t index = 0; index < feature_points_.size(); ++index) {
            if (feature_points_[index].descriptor.id == id) {
                return index;
            }
        }
        return kInvalidPipelineHandleIndex;
    }

    PipelineStageHandle PipelineBuilder::add_stage(const StageDescriptor& descriptor) {
        if (!status_) {
            return {};
        }
        if (!descriptor.id.is_valid()) {
            set_failure(foundation::ResultCode::kInvalidArgument, "stage id is invalid");
            return {};
        }
        if (!is_valid_engine_phase(descriptor.phase)) {
            set_failure(foundation::ResultCode::kInvalidArgument, "stage engine phase is invalid");
            return {};
        }
        if (!is_valid_execution_domain(descriptor.execution_domain)) {
            set_failure(foundation::ResultCode::kInvalidArgument, "stage execution domain is invalid");
            return {};
        }
        if (!is_valid_time_policy(descriptor.time_policy)) {
            set_failure(foundation::ResultCode::kInvalidArgument, "stage time policy is invalid");
            return {};
        }
        if (find_stage(descriptor.id) != kInvalidPipelineHandleIndex) {
            set_failure(foundation::ResultCode::kInvalidArgument,
                        "duplicate stage id=" + std::to_string(descriptor.id.value()));
            return {};
        }

        const uint32_t index = static_cast<uint32_t>(stages_.size());
        stages_.push_back(PipelineStageRecord{descriptor});
        return PipelineStageHandle{index, kBuilderGeneration};
    }

    PipelineTaskHandle PipelineBuilder::add_task(const TaskDescriptor& descriptor) {
        if (!status_) {
            return {};
        }
        if (!descriptor.id.is_valid()) {
            set_failure(foundation::ResultCode::kInvalidArgument, "task id is invalid");
            return {};
        }
        if (descriptor.execute == nullptr) {
            set_failure(foundation::ResultCode::kInvalidArgument, "task execution function is null");
            return {};
        }
        if ((static_cast<uint32_t>(descriptor.flags) & ~kKnownTaskFlags) != 0) {
            set_failure(foundation::ResultCode::kInvalidArgument, "task flags contain unknown bits");
            return {};
        }
        for (const PipelineTaskRecord& task : tasks_) {
            if (task.descriptor.id == descriptor.id) {
                set_failure(foundation::ResultCode::kInvalidArgument,
                            "duplicate task id=" + std::to_string(descriptor.id.value()));
                return {};
            }
        }

        const uint32_t index = static_cast<uint32_t>(tasks_.size());
        tasks_.push_back(PipelineTaskRecord{descriptor, kInvalidPipelineHandleIndex});
        return PipelineTaskHandle{index, kBuilderGeneration};
    }

    PipelineArtifactHandle PipelineBuilder::declare_artifact(const ArtifactDescriptor& descriptor) {
        if (!status_) {
            return {};
        }
        if (!descriptor.id.is_valid() || !descriptor.type.is_valid()) {
            set_failure(foundation::ResultCode::kInvalidArgument, "artifact id or type is invalid");
            return {};
        }
        if (descriptor.version == 0) {
            set_failure(foundation::ResultCode::kInvalidArgument, "artifact version must be non-zero");
            return {};
        }
        if (!is_valid_scope(descriptor.owner_scope)) {
            set_failure(foundation::ResultCode::kInvalidArgument, "artifact owner scope is invalid");
            return {};
        }
        if ((descriptor.visible_to & ~kAllPipelineScopes) != 0) {
            set_failure(foundation::ResultCode::kInvalidArgument, "artifact visibility contains unknown scopes");
            return {};
        }
        if (find_artifact(descriptor.id) != kInvalidPipelineHandleIndex) {
            set_failure(foundation::ResultCode::kInvalidArgument,
                        "duplicate artifact id=" + std::to_string(descriptor.id.value()));
            return {};
        }

        const uint32_t index = static_cast<uint32_t>(artifacts_.size());
        artifacts_.push_back(PipelineArtifactRecord{descriptor});
        return PipelineArtifactHandle{index, kBuilderGeneration};
    }

    PipelineFeaturePointHandle PipelineBuilder::add_feature_point(const FeaturePointDescriptor& descriptor) {
        if (!status_) {
            return {};
        }
        if (!descriptor.id.is_valid() || descriptor.name.empty()) {
            set_failure(foundation::ResultCode::kInvalidArgument,
                        "feature point id or name is invalid");
            return {};
        }
        if (descriptor.pipeline.is_valid() && descriptor.pipeline != descriptor_.id) {
            set_failure(foundation::ResultCode::kInvalidArgument,
                        "feature point belongs to another pipeline");
            return {};
        }
        if (!descriptor.anchor_stage.is_valid()) {
            set_failure(foundation::ResultCode::kInvalidArgument,
                        "feature point anchor stage is invalid");
            return {};
        }
        const uint32_t anchor_stage_index = find_stage(descriptor.anchor_stage);
        if (anchor_stage_index == kInvalidPipelineHandleIndex) {
            set_failure(foundation::ResultCode::kNotFound,
                        "feature point anchor stage is not declared");
            return {};
        }
        if (!is_valid_feature_point_position(descriptor.position)) {
            set_failure(foundation::ResultCode::kInvalidArgument,
                        "feature point position is invalid");
            return {};
        }
        if (descriptor.allowed_domains == 0 ||
            (descriptor.allowed_domains & ~kAllFeatureDomains) != 0) {
            set_failure(foundation::ResultCode::kInvalidArgument,
                        "feature point domain mask is invalid");
            return {};
        }
        if (descriptor.allowed_scopes == 0 ||
            (descriptor.allowed_scopes & ~kAllPipelineScopes) != 0) {
            set_failure(foundation::ResultCode::kInvalidArgument,
                        "feature point scope mask is invalid");
            return {};
        }
        if ((static_cast<uint32_t>(descriptor.flags) & ~kKnownFeaturePointFlags) != 0) {
            set_failure(foundation::ResultCode::kInvalidArgument,
                        "feature point flags contain unknown bits");
            return {};
        }
        if (find_feature_point(descriptor.id) != kInvalidPipelineHandleIndex) {
            set_failure(foundation::ResultCode::kInvalidArgument,
                        "duplicate feature point id=" + std::to_string(descriptor.id.value()));
            return {};
        }

        FeaturePointDescriptor normalized = descriptor;
        normalized.pipeline = descriptor_.id;
        const uint32_t index = static_cast<uint32_t>(feature_points_.size());
        feature_points_.push_back(PipelineFeaturePointRecord{normalized, anchor_stage_index});
        return PipelineFeaturePointHandle{index, kBuilderGeneration};
    }

    foundation::Result PipelineBuilder::bind_feature(const FeatureBindingDescriptor& binding) {
        if (!status_) {
            return status_;
        }
        if (!binding.feature.is_valid() || !binding.point.is_valid()) {
            set_failure(foundation::ResultCode::kInvalidArgument,
                        "feature binding identity is invalid");
            return status_;
        }
        if (binding.pipeline.is_valid() && binding.pipeline != descriptor_.id) {
            set_failure(foundation::ResultCode::kInvalidArgument,
                        "feature binding belongs to another pipeline");
            return status_;
        }
        if (!is_valid_scope(binding.scope)) {
            set_failure(foundation::ResultCode::kInvalidArgument,
                        "feature binding scope is invalid");
            return status_;
        }
        const uint32_t point_index = find_feature_point(binding.point);
        if (point_index == kInvalidPipelineHandleIndex) {
            set_failure(foundation::ResultCode::kNotFound,
                        "feature binding references an undeclared feature point");
            return status_;
        }
        const FeaturePointDescriptor& point = feature_points_[point_index].descriptor;
        if ((point.allowed_scopes & to_mask(binding.scope)) == 0 ||
            (descriptor_.supported_scopes & to_mask(binding.scope)) == 0) {
            set_failure(foundation::ResultCode::kUnsupported,
                        "feature binding scope is not supported by the pipeline or feature point");
            return status_;
        }

        FeatureBindingDescriptor normalized = binding;
        normalized.pipeline = descriptor_.id;
        if (!normalized.instance.is_valid()) {
            normalized.instance = make_feature_instance_id(normalized.feature,
                                                            normalized.pipeline,
                                                            normalized.scope,
                                                            normalized.point);
        }
        for (const PipelineFeatureBindingRecord& existing : feature_bindings_) {
            const FeatureBindingDescriptor& candidate = existing.descriptor;
            if (candidate.feature == normalized.feature && candidate.pipeline == normalized.pipeline &&
                candidate.scope == normalized.scope && candidate.point == normalized.point) {
                set_failure(foundation::ResultCode::kInvalidArgument,
                            "feature is already bound to this feature point");
                return status_;
            }
            if (candidate.instance == normalized.instance) {
                set_failure(foundation::ResultCode::kInvalidArgument,
                            "feature binding instance id collides with an existing binding");
                return status_;
            }
        }
        feature_bindings_.push_back(PipelineFeatureBindingRecord{normalized});
        return foundation::Result::success();
    }

    foundation::Result PipelineBuilder::bind_task(PipelineTaskHandle task, PipelineStageHandle stage) {
        if (!status_) {
            return status_;
        }
        if (!validate_task(task) || !validate_stage(stage)) {
            set_failure(foundation::ResultCode::kInvalidArgument, "task or stage handle is invalid");
            return status_;
        }
        PipelineTaskRecord& task_record = tasks_[task.index];
        if (task_record.stage_index != kInvalidPipelineHandleIndex) {
            set_failure(foundation::ResultCode::kInvalidState, "task is already bound to a stage");
            return status_;
        }
        task_record.stage_index = stage.index;
        return foundation::Result::success();
    }

    foundation::Result PipelineBuilder::add_binding(PipelineStageHandle stage,
                                                     const ArtifactBinding& binding,
                                                     ArtifactAccess expected_access) {
        if (!status_) {
            return status_;
        }
        if (!validate_stage(stage)) {
            set_failure(foundation::ResultCode::kInvalidArgument, "stage handle is invalid");
            return status_;
        }
        if (binding.access != expected_access) {
            set_failure(foundation::ResultCode::kInvalidArgument, "artifact access does not match builder method");
            return status_;
        }
        if (!binding.artifact.is_valid()) {
            set_failure(foundation::ResultCode::kInvalidArgument, "artifact binding id is invalid");
            return status_;
        }
        const uint32_t artifact_index = find_artifact(binding.artifact);
        if (artifact_index == kInvalidPipelineHandleIndex) {
            set_failure(foundation::ResultCode::kNotFound,
                        "artifact binding references an undeclared artifact");
            return status_;
        }
        if (binding.required_version == 0 ||
            binding.required_version != artifacts_[artifact_index].descriptor.version) {
            set_failure(foundation::ResultCode::kInvalidArgument,
                        "artifact binding version does not match its declaration");
            return status_;
        }
        if ((static_cast<uint32_t>(binding.flags) & ~kKnownAccessFlags) != 0) {
            set_failure(foundation::ResultCode::kInvalidArgument, "artifact access flags contain unknown bits");
            return status_;
        }
        if (has_flag(binding.flags, AccessFlags::kPreserveContents) &&
            has_flag(binding.flags, AccessFlags::kDiscardContents)) {
            set_failure(foundation::ResultCode::kInvalidArgument,
                        "preserve-contents and discard-contents are mutually exclusive");
            return status_;
        }
        if (has_flag(binding.flags, AccessFlags::kOptional) &&
            expected_access != ArtifactAccess::kRead && expected_access != ArtifactAccess::kConsume) {
            set_failure(foundation::ResultCode::kInvalidArgument,
                        "optional access is only valid for read or consume");
            return status_;
        }
        if (has_flag(binding.flags, AccessFlags::kDeterministicAppend) &&
            expected_access != ArtifactAccess::kAppend) {
            set_failure(foundation::ResultCode::kInvalidArgument,
                        "deterministic append is only valid for append access");
            return status_;
        }
        if (has_flag(binding.flags, AccessFlags::kDiscardContents) &&
            expected_access != ArtifactAccess::kWrite && expected_access != ArtifactAccess::kReadWrite) {
            set_failure(foundation::ResultCode::kInvalidArgument,
                        "discard-contents is only valid for write or read/write access");
            return status_;
        }

        for (PipelineBindingRecord& existing : bindings_) {
            if (existing.stage_index == stage.index && existing.binding.artifact == binding.artifact) {
                if (existing.binding.access != binding.access ||
                    existing.binding.required_version != binding.required_version) {
                    set_failure(foundation::ResultCode::kInvalidArgument,
                                "stage declares conflicting access for one artifact");
                    return status_;
                }
                const AccessFlags merged_flags = existing.binding.flags | binding.flags;
                if (has_flag(merged_flags, AccessFlags::kPreserveContents) &&
                    has_flag(merged_flags, AccessFlags::kDiscardContents)) {
                    set_failure(foundation::ResultCode::kInvalidArgument,
                                "merged artifact access flags are mutually exclusive");
                    return status_;
                }
                existing.binding.flags = merged_flags;
                return foundation::Result::success();
            }
        }

        bindings_.push_back(PipelineBindingRecord{stage.index, binding});
        return foundation::Result::success();
    }

    foundation::Result PipelineBuilder::reads(PipelineStageHandle stage, const ArtifactBinding& binding) {
        return add_binding(stage, binding, ArtifactAccess::kRead);
    }

    foundation::Result PipelineBuilder::writes(PipelineStageHandle stage, const ArtifactBinding& binding) {
        return add_binding(stage, binding, ArtifactAccess::kWrite);
    }

    foundation::Result PipelineBuilder::reads_writes(PipelineStageHandle stage, const ArtifactBinding& binding) {
        return add_binding(stage, binding, ArtifactAccess::kReadWrite);
    }

    foundation::Result PipelineBuilder::runs_before(PipelineStageHandle stage, StageId other) {
        if (!status_) {
            return status_;
        }
        if (!validate_stage(stage) || !other.is_valid()) {
            set_failure(foundation::ResultCode::kInvalidArgument, "ordering relation contains an invalid stage");
            return status_;
        }
        if (stages_[stage.index].descriptor.id == other) {
            set_failure(foundation::ResultCode::kInvalidArgument, "stage cannot run before itself");
            return status_;
        }
        orderings_.push_back(PipelineOrderingRecord{stage.index, other, true});
        return foundation::Result::success();
    }

    foundation::Result PipelineBuilder::runs_after(PipelineStageHandle stage, StageId other) {
        if (!status_) {
            return status_;
        }
        if (!validate_stage(stage) || !other.is_valid()) {
            set_failure(foundation::ResultCode::kInvalidArgument, "ordering relation contains an invalid stage");
            return status_;
        }
        if (stages_[stage.index].descriptor.id == other) {
            set_failure(foundation::ResultCode::kInvalidArgument, "stage cannot run after itself");
            return status_;
        }
        orderings_.push_back(PipelineOrderingRecord{stage.index, other, false});
        return foundation::Result::success();
    }

    foundation::Result PipelineBuilder::expose_artifact(ArtifactId artifact, PipelineScope target_scope) {
        if (!status_) {
            return status_;
        }
        if (!artifact.is_valid() || find_artifact(artifact) == kInvalidPipelineHandleIndex) {
            set_failure(foundation::ResultCode::kNotFound, "exposed artifact is not declared");
            return status_;
        }
        if (!is_valid_scope(target_scope)) {
            set_failure(foundation::ResultCode::kInvalidArgument, "exposure target scope is invalid");
            return status_;
        }
        for (const PipelineExposureRecord& existing : exposures_) {
            if (existing.artifact == artifact && existing.target_scope == target_scope) {
                return foundation::Result::success();
            }
        }
        exposures_.push_back(PipelineExposureRecord{artifact, target_scope});
        return foundation::Result::success();
    }

    foundation::ResultValue<PipelineDefinition> PipelineBuilder::finish() {
        if (finished_) {
            return {invalid_state("pipeline builder has already been finalized"), {}};
        }
        finished_ = true;

        if (!status_) {
            return {status_, {}};
        }
        if (!descriptor_.id.is_valid() || descriptor_.name.empty()) {
            return {invalid_argument("pipeline id or name is invalid"), {}};
        }
        if (!descriptor_.version.is_valid()) {
            return {invalid_argument("pipeline version is invalid"), {}};
        }
        if (descriptor_.supported_scopes == 0 ||
            (descriptor_.supported_scopes & ~kAllPipelineScopes) != 0) {
            return {invalid_argument("pipeline supported scopes are invalid"), {}};
        }
        if (descriptor_.domains != 0 && (descriptor_.domains & ~kAllFeatureDomains) != 0) {
            return {invalid_argument("pipeline domain mask contains unknown bits"), {}};
        }
        if (stages_.empty()) {
            return {invalid_argument("pipeline must declare at least one stage"), {}};
        }
        for (const PipelineTaskRecord& task : tasks_) {
            if (task.stage_index == kInvalidPipelineHandleIndex) {
                return {invalid_argument("every task must be bound to a stage"), {}};
            }
        }
        for (const PipelineOrderingRecord& ordering : orderings_) {
            if (ordering.source_stage_index >= stages_.size() || find_stage(ordering.target_stage) ==
                                                                    kInvalidPipelineHandleIndex) {
                return {not_found("ordering relation references an undeclared stage"), {}};
            }
        }
        for (const PipelineFeaturePointRecord& point : feature_points_) {
            if (point.anchor_stage_index >= stages_.size() ||
                point.descriptor.pipeline != descriptor_.id ||
                find_stage(point.descriptor.anchor_stage) != point.anchor_stage_index) {
                return {invalid_state("feature point contains an invalid anchor"), {}};
            }
        }
        for (const PipelineFeatureBindingRecord& binding : feature_bindings_) {
            if (binding.descriptor.pipeline != descriptor_.id ||
                find_feature_point(binding.descriptor.point) == kInvalidPipelineHandleIndex ||
                !binding.descriptor.instance.is_valid()) {
                return {invalid_state("feature binding contains an invalid reference"), {}};
            }
        }

        PipelineDefinition definition;
        definition.descriptor = descriptor_;
        definition.stages = std::move(stages_);
        definition.tasks = std::move(tasks_);
        definition.artifacts = std::move(artifacts_);
        definition.feature_points = std::move(feature_points_);
        definition.feature_bindings = std::move(feature_bindings_);
        definition.bindings = std::move(bindings_);
        definition.orderings = std::move(orderings_);
        definition.exposures = std::move(exposures_);
        return {foundation::Result::success(), std::move(definition)};
    }

    namespace {
        const FeatureDescriptor& default_logic_feature_descriptor() {
            static const FeatureDescriptor descriptor{
                {}, "LogicFeature", FeatureDomain::kLogic, to_mask(FeatureDomain::kLogic), kAllPipelineScopes,
                {1, 0, 0}, {}};
            return descriptor;
        }

        const FeatureDescriptor& default_physics_feature_descriptor() {
            static const FeatureDescriptor descriptor{
                {}, "PhysicsFeature", FeatureDomain::kPhysics, to_mask(FeatureDomain::kPhysics), kAllPipelineScopes,
                {1, 0, 0}, {}};
            return descriptor;
        }

        const FeatureDescriptor& default_render_feature_descriptor() {
            static const FeatureDescriptor descriptor{
                {}, "RenderFeature", FeatureDomain::kRender, to_mask(FeatureDomain::kRender), kAllPipelineScopes,
                {1, 0, 0}, {}};
            return descriptor;
        }

        const FeatureDescriptor& default_audio_feature_descriptor() {
            static const FeatureDescriptor descriptor{
                {}, "AudioFeature", FeatureDomain::kAudio, to_mask(FeatureDomain::kAudio), kAllPipelineScopes,
                {1, 0, 0}, {}};
            return descriptor;
        }

        const FeatureDescriptor& default_stream_feature_descriptor() {
            static const FeatureDescriptor descriptor{
                {}, "StreamFeature", FeatureDomain::kStream, to_mask(FeatureDomain::kStream), kAllPipelineScopes,
                {1, 0, 0}, {}};
            return descriptor;
        }
    }  // namespace

    foundation::Result FeatureBuildContext::reject(foundation::ResultCode code, std::string message) {
        if (builder_ == nullptr) {
            return foundation::Result::failure(code, std::move(message));
        }
        builder_->set_failure(code, std::move(message));
        return builder_->status();
    }

    uint32_t FeatureBuildContext::stage_count() const noexcept {
        return builder_ == nullptr ? 0 : static_cast<uint32_t>(builder_->stages_.size());
    }

    void FeatureBuildContext::on_stage_added(PipelineStageHandle stage) {
        if (builder_ == nullptr || !stage) {
            return;
        }
        switch (feature_point_.position) {
            case FeaturePointPosition::kBefore:
                static_cast<void>(builder_->runs_before(stage, feature_point_.anchor_stage));
                break;
            case FeaturePointPosition::kInside:
            case FeaturePointPosition::kAfter:
                static_cast<void>(builder_->runs_after(stage, feature_point_.anchor_stage));
                break;
        }
    }

    void FeatureBuildContext::record_contribution(FeatureDomain domain, uint32_t first_stage_index) {
        if (contribution_records_ == nullptr || builder_ == nullptr) {
            return;
        }
        contribution_records_->push_back(PipelineFeatureContributionRecord{
            feature_instance_id_,
            feature_id_,
            part_id_,
            feature_point_.id,
            domain,
            first_stage_index,
            static_cast<uint32_t>(builder_->stages_.size()) - first_stage_index,
        });
    }

    LogicGraphBuilder FeatureBuildContext::logic_builder() noexcept {
        return LogicGraphBuilder(*this);
    }

    PhysicsGraphBuilder FeatureBuildContext::physics_builder() noexcept {
        return PhysicsGraphBuilder(*this);
    }

    RenderGraphBuilder FeatureBuildContext::render_builder() noexcept {
        return RenderGraphBuilder(*this);
    }

    AudioGraphBuilder FeatureBuildContext::audio_builder() noexcept {
        return AudioGraphBuilder(*this);
    }

    StreamGraphBuilder FeatureBuildContext::stream_builder() noexcept {
        return StreamGraphBuilder(*this);
    }

    FeatureDomain PipelineFeature::feature_domain() const noexcept {
        return descriptor().domain;
    }

    FeatureDomainMask PipelineFeature::feature_domain_mask() const noexcept {
        return get_feature_domain_mask(descriptor());
    }

    const FeatureDescriptor& LogicFeature::descriptor() const noexcept {
        return default_logic_feature_descriptor();
    }

    foundation::Result LogicFeature::collect_contributions(FeatureBuildContext& context) const {
        FeatureBuildContext domain_context = context.for_domain(kDomain);
        if (!domain_context.domain_is_active(kDomain)) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                "logic feature is not allowed at this feature point");
        }
        LogicGraphBuilder builder = domain_context.logic_builder();
        const uint32_t first_stage_index = domain_context.stage_count();
        const foundation::Result result = setup(builder);
        if (result) {
            domain_context.record_contribution(kDomain, first_stage_index);
        }
        return result;
    }

    const FeatureDescriptor& PhysicsFeature::descriptor() const noexcept {
        return default_physics_feature_descriptor();
    }

    foundation::Result PhysicsFeature::collect_contributions(FeatureBuildContext& context) const {
        FeatureBuildContext domain_context = context.for_domain(kDomain);
        if (!domain_context.domain_is_active(kDomain)) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                "physics feature is not allowed at this feature point");
        }
        PhysicsGraphBuilder builder = domain_context.physics_builder();
        const uint32_t first_stage_index = domain_context.stage_count();
        const foundation::Result result = setup(builder);
        if (result) {
            domain_context.record_contribution(kDomain, first_stage_index);
        }
        return result;
    }

    const FeatureDescriptor& RenderFeature::descriptor() const noexcept {
        return default_render_feature_descriptor();
    }

    foundation::Result RenderFeature::collect_contributions(FeatureBuildContext& context) const {
        FeatureBuildContext domain_context = context.for_domain(kDomain);
        if (!domain_context.domain_is_active(kDomain)) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                "render feature is not allowed at this feature point");
        }
        RenderGraphBuilder builder = domain_context.render_builder();
        const uint32_t first_stage_index = domain_context.stage_count();
        const foundation::Result result = setup(builder);
        if (result) {
            domain_context.record_contribution(kDomain, first_stage_index);
        }
        return result;
    }

    const FeatureDescriptor& AudioFeature::descriptor() const noexcept {
        return default_audio_feature_descriptor();
    }

    foundation::Result AudioFeature::collect_contributions(FeatureBuildContext& context) const {
        FeatureBuildContext domain_context = context.for_domain(kDomain);
        if (!domain_context.domain_is_active(kDomain)) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                "audio feature is not allowed at this feature point");
        }
        AudioGraphBuilder builder = domain_context.audio_builder();
        const uint32_t first_stage_index = domain_context.stage_count();
        const foundation::Result result = setup(builder);
        if (result) {
            domain_context.record_contribution(kDomain, first_stage_index);
        }
        return result;
    }

    const FeatureDescriptor& StreamFeature::descriptor() const noexcept {
        return default_stream_feature_descriptor();
    }

    foundation::Result StreamFeature::collect_contributions(FeatureBuildContext& context) const {
        FeatureBuildContext domain_context = context.for_domain(kDomain);
        if (!domain_context.domain_is_active(kDomain)) {
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                "stream feature is not allowed at this feature point");
        }
        StreamGraphBuilder builder = domain_context.stream_builder();
        const uint32_t first_stage_index = domain_context.stage_count();
        const foundation::Result result = setup(builder);
        if (result) {
            domain_context.record_contribution(kDomain, first_stage_index);
        }
        return result;
    }

    foundation::Result PipelineRegistry::register_pipeline(PipelineFactory factory) {
        if (!factory) {
            return invalid_argument("pipeline factory is empty");
        }

        std::unique_ptr<Pipeline> pipeline = factory();
        if (!pipeline) {
            return invalid_argument("pipeline factory returned null");
        }

        const PipelineDescriptor& descriptor = pipeline->descriptor();
        const PipelineId pipeline_id = descriptor.id;
        PipelineBuilder builder(descriptor);
        const foundation::Result build_result = pipeline->build(builder);
        if (!build_result) {
            return foundation::Result::failure(
                build_result.code,
                append_message("pipeline definition build failed", build_result));
        }

        foundation::ResultValue<PipelineDefinition> definition_result = builder.finish();
        if (!definition_result) {
            return foundation::Result::failure(
                definition_result.result.code,
                append_message("pipeline definition validation failed", definition_result.result));
        }

        if (!pipeline_id.is_valid()) {
            return invalid_argument("pipeline id is invalid");
        }

        std::shared_ptr<Pipeline> implementation(std::move(pipeline));
        std::shared_ptr<const PipelineDefinition> definition =
            std::make_shared<const PipelineDefinition>(std::move(definition_result.value));

        std::unique_lock lock(mutex_);
        if (registrations_.find(pipeline_id) != registrations_.end()) {
            return invalid_state("pipeline is already registered");
        }
        registrations_.emplace(pipeline_id, Registration{std::move(implementation), std::move(definition)});
        return foundation::Result::success();
    }

    foundation::Result PipelineRegistry::unregister_pipeline(PipelineId id) {
        if (!id.is_valid()) {
            return invalid_argument("pipeline id is invalid");
        }
        std::unique_lock lock(mutex_);
        const auto iterator = registrations_.find(id);
        if (iterator == registrations_.end()) {
            return not_found("pipeline is not registered");
        }
        for (const auto& [instance, binding] : feature_bindings_) {
            static_cast<void>(instance);
            if (binding.pipeline == id) {
                return invalid_state("pipeline has active feature bindings");
            }
        }
        registrations_.erase(iterator);
        return foundation::Result::success();
    }

    foundation::ResultValue<std::shared_ptr<const PipelineDefinition>> PipelineRegistry::find_definition(
        PipelineId id) const {
        const foundation::ResultValue<Registration> registration = acquire_registration(id);
        if (!registration) {
            return {registration.result, {}};
        }
        return {foundation::Result::success(), registration.value.definition};
    }

    foundation::Result PipelineRegistry::register_feature(FeatureFactory factory) {
        if (!factory) {
            return invalid_argument("feature factory is empty");
        }

        std::unique_ptr<PipelineFeature> feature;
        try {
            feature = factory();
        } catch (const std::exception& exception) {
            return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                                std::string("feature factory threw: ") + exception.what());
        } catch (...) {
            return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                                "feature factory threw an unknown exception");
        }
        if (!feature) {
            return invalid_argument("feature factory returned null");
        }

        FeatureDescriptor descriptor = feature->descriptor();
        if (!descriptor.id.is_valid() || descriptor.name.empty()) {
            return invalid_argument("feature id or name is invalid");
        }
        if (!descriptor.version.is_valid()) {
            return invalid_argument("feature version is invalid");
        }
        if (!is_valid_feature_domain(descriptor.domain)) {
            return invalid_argument("feature domain is invalid");
        }
        if (descriptor.supported_scopes == 0 ||
            (descriptor.supported_scopes & ~kAllPipelineScopes) != 0) {
            return invalid_argument("feature supported scopes are invalid");
        }
        const FeatureDomainMask implementation_mask = feature->feature_domain_mask();
        const FeatureDomainMask expected_mask = feature->feature_domain() == FeatureDomain::kComposite
                                                     ? implementation_mask
                                                     : to_mask(feature->feature_domain());
        if (feature->feature_domain() != descriptor.domain || expected_mask == 0 ||
            (expected_mask & ~kAllFeatureDomains) != 0) {
            return invalid_argument("feature domain metadata does not match its implementation");
        }
        if (descriptor.domain_mask == 0) {
            descriptor.domain_mask = expected_mask;
        }
        if (descriptor.domain_mask != expected_mask) {
            return invalid_argument("feature domain mask does not match its implementation");
        }

        std::shared_ptr<PipelineFeature> implementation(std::move(feature));
        std::unique_lock lock(mutex_);
        if (features_.find(descriptor.id) != features_.end()) {
            return invalid_state("feature is already registered");
        }
        features_.emplace(descriptor.id, FeatureRegistration{std::move(implementation), descriptor});
        return foundation::Result::success();
    }

    foundation::Result PipelineRegistry::unregister_feature(FeatureId id) {
        if (!id.is_valid()) {
            return invalid_argument("feature id is invalid");
        }
        std::unique_lock lock(mutex_);
        const auto iterator = features_.find(id);
        if (iterator == features_.end()) {
            return not_found("feature is not registered");
        }
        for (const auto& [instance, binding] : feature_bindings_) {
            static_cast<void>(instance);
            if (binding.feature == id) {
                return invalid_state("feature has active bindings");
            }
        }
        for (const auto& [pipeline_id, registration] : registrations_) {
            static_cast<void>(pipeline_id);
            for (const PipelineFeatureBindingRecord& binding : registration.definition->feature_bindings) {
                if (binding.descriptor.feature == id) {
                    return invalid_state("feature is referenced by a pipeline definition");
                }
            }
        }
        features_.erase(iterator);
        return foundation::Result::success();
    }

    foundation::ResultValue<FeatureDescriptor> PipelineRegistry::find_feature_descriptor(FeatureId id) const {
        const foundation::ResultValue<FeatureRegistration> registration = acquire_feature_registration(id);
        if (!registration) {
            return {registration.result, {}};
        }
        return {foundation::Result::success(), registration.value.descriptor};
    }

    foundation::Result PipelineRegistry::bind_feature(const FeatureBindingDescriptor& binding) {
        if (!binding.feature.is_valid() || !binding.pipeline.is_valid() || !binding.point.is_valid()) {
            return invalid_argument("feature binding identity is invalid");
        }
        if (!is_valid_scope(binding.scope)) {
            return invalid_argument("feature binding scope is invalid");
        }

        std::unique_lock lock(mutex_);
        const auto feature_iterator = features_.find(binding.feature);
        if (feature_iterator == features_.end()) {
            return not_found("feature is not registered");
        }
        const auto pipeline_iterator = registrations_.find(binding.pipeline);
        if (pipeline_iterator == registrations_.end()) {
            return not_found("feature binding pipeline is not registered");
        }
        const PipelineDefinition& definition = *pipeline_iterator->second.definition;
        if ((definition.descriptor.supported_scopes & to_mask(binding.scope)) == 0 ||
            (feature_iterator->second.descriptor.supported_scopes & to_mask(binding.scope)) == 0) {
            return unsupported("feature binding scope is not supported");
        }

        const PipelineFeaturePointRecord* point = nullptr;
        for (const PipelineFeaturePointRecord& candidate : definition.feature_points) {
            if (candidate.descriptor.id == binding.point) {
                point = &candidate;
                break;
            }
        }
        if (point == nullptr) {
            return not_found("feature binding point is not declared by the pipeline");
        }
        if ((point->descriptor.allowed_scopes & to_mask(binding.scope)) == 0) {
            return unsupported("feature binding scope is not allowed by the feature point");
        }

        const FeatureDomainMask binding_domains = get_feature_domain_mask(feature_iterator->second.descriptor);
        if (binding_domains == 0 || (binding_domains & ~point->descriptor.allowed_domains) != 0) {
            return unsupported("feature domain is not allowed by the feature point");
        }
        if (definition.descriptor.domains != 0 &&
            (binding_domains & ~definition.descriptor.domains) != 0) {
            return unsupported("feature domain is not enabled by the pipeline");
        }

        FeatureBindingDescriptor normalized = binding;
        if (!normalized.instance.is_valid()) {
            normalized.instance = make_feature_instance_id(normalized.feature,
                                                            normalized.pipeline,
                                                            normalized.scope,
                                                            normalized.point);
        }
        for (const auto& [instance, existing] : feature_bindings_) {
            static_cast<void>(instance);
            if (existing.feature == normalized.feature && existing.pipeline == normalized.pipeline &&
                existing.scope == normalized.scope && existing.point == normalized.point) {
                return invalid_state("feature is already bound to this feature point");
            }
            if (existing.instance == normalized.instance) {
                return invalid_argument("feature binding instance id collides with an existing binding");
            }
        }
        for (const PipelineFeatureBindingRecord& existing : definition.feature_bindings) {
            const FeatureBindingDescriptor& candidate = existing.descriptor;
            if (candidate.feature == normalized.feature && candidate.scope == normalized.scope &&
                candidate.point == normalized.point) {
                return invalid_state("feature is already bound by the pipeline definition");
            }
            if (candidate.instance == normalized.instance) {
                return invalid_argument("feature binding instance id collides with a definition binding");
            }
        }
        feature_bindings_.emplace(normalized.instance, normalized);
        return foundation::Result::success();
    }

    foundation::Result PipelineRegistry::unbind_feature(FeatureInstanceId instance) {
        if (!instance.is_valid()) {
            return invalid_argument("feature binding instance id is invalid");
        }
        std::unique_lock lock(mutex_);
        const auto iterator = feature_bindings_.find(instance);
        if (iterator == feature_bindings_.end()) {
            return not_found("feature binding is not registered");
        }
        feature_bindings_.erase(iterator);
        return foundation::Result::success();
    }

    foundation::ResultValue<PipelineRegistry::Registration> PipelineRegistry::acquire_registration(PipelineId id) const {
        if (!id.is_valid()) {
            return {invalid_argument("pipeline id is invalid"), {}};
        }
        std::shared_lock lock(mutex_);
        const auto iterator = registrations_.find(id);
        if (iterator == registrations_.end()) {
            return {not_found("pipeline is not registered"), {}};
        }
        return {foundation::Result::success(), iterator->second};
    }

    foundation::ResultValue<PipelineRegistry::FeatureRegistration> PipelineRegistry::acquire_feature_registration(
        FeatureId id) const {
        if (!id.is_valid()) {
            return {invalid_argument("feature id is invalid"), {}};
        }
        std::shared_lock lock(mutex_);
        const auto iterator = features_.find(id);
        if (iterator == features_.end()) {
            return {not_found("feature is not registered"), {}};
        }
        return {foundation::Result::success(), iterator->second};
    }

    foundation::ResultValue<std::vector<FeatureBindingDescriptor>> PipelineRegistry::acquire_feature_bindings(
        PipelineId pipeline,
        PipelineScope scope) const {
        if (!pipeline.is_valid()) {
            return {invalid_argument("feature binding pipeline id is invalid"), {}};
        }
        if (!is_valid_scope(scope)) {
            return {invalid_argument("feature binding scope is invalid"), {}};
        }
        std::vector<FeatureBindingDescriptor> result;
        std::shared_lock lock(mutex_);
        for (const auto& [instance, binding] : feature_bindings_) {
            static_cast<void>(instance);
            if (binding.pipeline == pipeline && binding.scope == scope) {
                result.push_back(binding);
            }
        }
        return {foundation::Result::success(), std::move(result)};
    }

    foundation::ResultValue<PipelinePlan> PipelineCompiler::compile(const PipelineCompileInput& input) const {
        if (input.registry == nullptr) {
            return {invalid_argument("pipeline compile registry is null"), {}};
        }
        if (!input.pipeline.is_valid()) {
            return {invalid_argument("pipeline id is invalid"), {}};
        }
        if (!is_valid_scope(input.scope)) {
            return {invalid_argument("pipeline compile scope is invalid"), {}};
        }

        const foundation::ResultValue<PipelineRegistry::Registration> registration =
            input.registry->acquire_registration(input.pipeline);
        if (!registration) {
            return {registration.result, {}};
        }

        const PipelineDefinition& base_definition = *registration.value.definition;
        if ((base_definition.descriptor.supported_scopes & to_mask(input.scope)) == 0) {
            return {unsupported("pipeline does not support the requested scope"), {}};
        }
        if ((base_definition.descriptor.required_capabilities & ~input.available_capabilities) != 0) {
            return {unsupported("pipeline required capabilities are unavailable"), {}};
        }

        PipelineBuilder contribution_builder(base_definition);
        std::vector<PipelineFeatureContributionRecord> feature_contributions;
        std::vector<std::shared_ptr<const void>> feature_owners;
        std::vector<FeatureBindingDescriptor> feature_bindings;
        feature_bindings.reserve(base_definition.feature_bindings.size());
        for (const PipelineFeatureBindingRecord& record : base_definition.feature_bindings) {
            if (record.descriptor.scope == input.scope) {
                feature_bindings.push_back(record.descriptor);
            }
        }
        const foundation::ResultValue<std::vector<FeatureBindingDescriptor>> registered_bindings =
            input.registry->acquire_feature_bindings(input.pipeline, input.scope);
        if (!registered_bindings) {
            return {registered_bindings.result, {}};
        }
        feature_bindings.insert(feature_bindings.end(),
                                registered_bindings.value.begin(),
                                registered_bindings.value.end());
        std::sort(feature_bindings.begin(), feature_bindings.end(), [](const FeatureBindingDescriptor& left,
                                                                       const FeatureBindingDescriptor& right) {
            if (left.point != right.point) {
                return left.point < right.point;
            }
            if (left.priority != right.priority) {
                return left.priority < right.priority;
            }
            if (left.feature != right.feature) {
                return left.feature < right.feature;
            }
            return left.instance < right.instance;
        });

        struct FeatureExpansion {
            FeatureBindingDescriptor binding{};
            FeaturePointPosition position{FeaturePointPosition::kBefore};
            std::vector<uint32_t> stage_indices{};
        };
        std::vector<FeatureExpansion> feature_expansions;
        for (size_t binding_index = 0; binding_index < feature_bindings.size(); ++binding_index) {
            const FeatureBindingDescriptor& binding = feature_bindings[binding_index];
            if (!binding.enabled) {
                continue;
            }
            for (size_t previous_index = 0; previous_index < binding_index; ++previous_index) {
                const FeatureBindingDescriptor& previous = feature_bindings[previous_index];
                if (previous.enabled && previous.feature == binding.feature &&
                    previous.pipeline == binding.pipeline && previous.scope == binding.scope &&
                    previous.point == binding.point) {
                    return {invalid_argument("feature is bound to the same feature point more than once"), {}};
                }
            }

            const foundation::ResultValue<PipelineRegistry::FeatureRegistration> feature_registration =
                input.registry->acquire_feature_registration(binding.feature);
            if (!feature_registration) {
                return {feature_registration.result, {}};
            }
            const FeatureDescriptor& feature_descriptor = feature_registration.value.descriptor;
            const PipelineFeaturePointRecord* point = nullptr;
            for (const PipelineFeaturePointRecord& candidate : base_definition.feature_points) {
                if (candidate.descriptor.id == binding.point) {
                    point = &candidate;
                    break;
                }
            }
            if (point == nullptr) {
                return {not_found("feature binding point is not declared by the pipeline"), {}};
            }
            if ((feature_descriptor.supported_scopes & to_mask(input.scope)) == 0 ||
                (point->descriptor.allowed_scopes & to_mask(input.scope)) == 0) {
                return {unsupported("feature binding scope is not supported"), {}};
            }
            const FeatureDomainMask binding_domains = get_feature_domain_mask(feature_descriptor);
            if (binding_domains == 0 || (binding_domains & ~point->descriptor.allowed_domains) != 0) {
                return {unsupported("feature domain is not allowed by the feature point"), {}};
            }
            if (base_definition.descriptor.domains != 0 &&
                (binding_domains & ~base_definition.descriptor.domains) != 0) {
                return {unsupported("feature domain is not enabled by the pipeline"), {}};
            }
            if (!binding.instance.is_valid()) {
                return {invalid_state("feature binding instance id is invalid"), {}};
            }

            const uint32_t first_stage_index = static_cast<uint32_t>(contribution_builder.stages_.size());
            FeatureBuildContext context(contribution_builder,
                                        input.scope,
                                        input.pipeline,
                                        binding.feature,
                                        binding.instance,
                                        {},
                                        point->descriptor,
                                        feature_registration.value.implementation->feature_domain(),
                                        &feature_contributions);
            foundation::Result contribution_result;
            try {
                contribution_result = feature_registration.value.implementation->collect_contributions(context);
            } catch (const std::exception& exception) {
                contribution_result = foundation::Result::failure(foundation::ResultCode::kUnknown,
                                                                   exception.what());
            } catch (...) {
                contribution_result = foundation::Result::failure(
                    foundation::ResultCode::kUnknown,
                    "feature contribution threw an unknown exception");
            }
            if (!contribution_result) {
                return {foundation::Result::failure(
                            contribution_result.code,
                            append_message("feature contribution failed", contribution_result)),
                        {}};
            }
            if (!contribution_builder.status()) {
                return {contribution_builder.status(), {}};
            }

            FeatureExpansion expansion;
            expansion.binding = binding;
            expansion.position = point->descriptor.position;
            for (uint32_t stage_index = first_stage_index;
                 stage_index < contribution_builder.stages_.size();
                 ++stage_index) {
                expansion.stage_indices.push_back(stage_index);
            }
            feature_expansions.push_back(std::move(expansion));
            feature_owners.push_back(std::shared_ptr<const void>(feature_registration.value.implementation));
        }

        for (size_t left_index = 0; left_index < feature_expansions.size(); ++left_index) {
            const FeatureExpansion& left = feature_expansions[left_index];
            for (size_t right_index = left_index + 1; right_index < feature_expansions.size(); ++right_index) {
                const FeatureExpansion& right = feature_expansions[right_index];
                if (left.binding.point != right.binding.point || left.position != right.position) {
                    continue;
                }
                for (const uint32_t source_stage : left.stage_indices) {
                    for (const uint32_t destination_stage : right.stage_indices) {
                        contribution_builder.orderings_.push_back(PipelineOrderingRecord{
                            source_stage,
                            contribution_builder.stages_[destination_stage].descriptor.id,
                            true,
                        });
                    }
                }
            }
        }

        foundation::ResultValue<PipelineDefinition> expanded_definition = contribution_builder.finish();
        if (!expanded_definition) {
            return {expanded_definition.result, {}};
        }
        const PipelineDefinition& definition = expanded_definition.value;

        PipelinePlan plan;
        plan.pipeline = definition.descriptor.id;
        plan.instance = input.instance;
        plan.definition_version = definition.descriptor.version;
        plan.scope = input.scope;
        plan.definition_owner = std::shared_ptr<const void>(registration.value.implementation);
        plan.feature_contributions = std::move(feature_contributions);
        plan.feature_owners = std::move(feature_owners);

        std::vector<uint32_t> stage_order(definition.stages.size());
        std::iota(stage_order.begin(), stage_order.end(), 0);
        std::sort(stage_order.begin(), stage_order.end(), [&](uint32_t left, uint32_t right) {
            return definition.stages[left].descriptor.id < definition.stages[right].descriptor.id;
        });

        std::vector<std::vector<uint32_t>> stage_nodes(definition.stages.size());
        for (const uint32_t original_stage_index : stage_order) {
            const StageDescriptor& stage = definition.stages[original_stage_index].descriptor;
            std::vector<uint32_t> task_order;
            for (uint32_t task_index = 0; task_index < definition.tasks.size(); ++task_index) {
                if (definition.tasks[task_index].stage_index == original_stage_index) {
                    task_order.push_back(task_index);
                }
            }
            std::sort(task_order.begin(), task_order.end(), [&](uint32_t left, uint32_t right) {
                return definition.tasks[left].descriptor.id < definition.tasks[right].descriptor.id;
            });

            if (task_order.empty()) {
                stage_nodes[original_stage_index].push_back(static_cast<uint32_t>(plan.nodes.size()));
                plan.nodes.push_back(PipelineNode{
                    {},
                    stage.id,
                    stage.phase,
                    stage.execution_domain,
                    stage.time_policy,
                    TaskFlags::kNone,
                    nullptr,
                    nullptr,
                    true,
                    stage.parallelizable,
                });
                continue;
            }

            for (const uint32_t task_index : task_order) {
                const TaskDescriptor& task = definition.tasks[task_index].descriptor;
                stage_nodes[original_stage_index].push_back(static_cast<uint32_t>(plan.nodes.size()));
                plan.nodes.push_back(PipelineNode{
                    task.id,
                    stage.id,
                    stage.phase,
                    stage.execution_domain,
                    stage.time_policy,
                    task.flags,
                    task.execute,
                    task.user_data,
                    false,
                    stage.parallelizable,
                });
            }
        }

        std::unordered_map<uint64_t, uint32_t> dependency_indices;
        const auto add_dependency = [&](uint32_t source_node,
                                        uint32_t destination_node,
                                        PipelineDependencyKind kind,
                                        ArtifactId artifact) {
            if (source_node == destination_node) {
                return;
            }
            const uint64_t key = (static_cast<uint64_t>(source_node) << 32u) | destination_node;
            if (dependency_indices.find(key) != dependency_indices.end()) {
                return;
            }
            dependency_indices.emplace(key, static_cast<uint32_t>(plan.dependencies.size()));
            plan.dependencies.push_back(
                PipelineDependency{source_node, destination_node, kind, artifact});
        };

        const auto add_stage_dependency = [&](uint32_t source_stage,
                                               uint32_t destination_stage,
                                               PipelineDependencyKind kind,
                                               ArtifactId artifact) {
            if (source_stage == destination_stage) {
                return;
            }
            for (const uint32_t source_node : stage_nodes[source_stage]) {
                for (const uint32_t destination_node : stage_nodes[destination_stage]) {
                    add_dependency(source_node, destination_node, kind, artifact);
                }
            }
        };

        const size_t engine_phase_count = static_cast<size_t>(EnginePhase::kShutdown) + 1;
        std::vector<std::vector<uint32_t>> stages_by_phase(engine_phase_count);
        for (uint32_t stage_index = 0; stage_index < definition.stages.size(); ++stage_index) {
            const size_t phase_index = static_cast<size_t>(definition.stages[stage_index].descriptor.phase);
            stages_by_phase[phase_index].push_back(stage_index);
        }

        uint32_t previous_phase = kInvalidPipelineHandleIndex;
        for (uint32_t phase_index = 0; phase_index < stages_by_phase.size(); ++phase_index) {
            if (stages_by_phase[phase_index].empty()) {
                continue;
            }
            if (previous_phase != kInvalidPipelineHandleIndex) {
                for (const uint32_t source_stage : stages_by_phase[previous_phase]) {
                    for (const uint32_t destination_stage : stages_by_phase[phase_index]) {
                        add_stage_dependency(source_stage,
                                             destination_stage,
                                             PipelineDependencyKind::kOrdering,
                                             {});
                    }
                }
            }
            previous_phase = phase_index;
        }

        struct BindingReference {
            uint32_t stage_index{kInvalidPipelineHandleIndex};
            const ArtifactBinding* binding{nullptr};
        };

        std::vector<std::vector<BindingReference>> artifact_bindings(definition.artifacts.size());
        for (const PipelineBindingRecord& binding_record : definition.bindings) {
            const uint32_t artifact_index = [&] {
                for (uint32_t index = 0; index < definition.artifacts.size(); ++index) {
                    if (definition.artifacts[index].descriptor.id == binding_record.binding.artifact) {
                        return index;
                    }
                }
                return kInvalidPipelineHandleIndex;
            }();
            if (artifact_index == kInvalidPipelineHandleIndex) {
                return {not_found("binding references an undeclared artifact"), {}};
            }
            artifact_bindings[artifact_index].push_back(
                BindingReference{binding_record.stage_index, &binding_record.binding});
        }

        for (uint32_t artifact_index = 0; artifact_index < definition.artifacts.size(); ++artifact_index) {
            const ArtifactDescriptor& artifact = definition.artifacts[artifact_index].descriptor;
            std::vector<BindingReference> writers;
            std::vector<BindingReference> readers;
            for (const BindingReference& reference : artifact_bindings[artifact_index]) {
                if (is_writer(reference.binding->access)) {
                    writers.push_back(reference);
                }
                if (is_reader(reference.binding->access)) {
                    readers.push_back(reference);
                }
            }

            if (writers.empty()) {
                const bool all_optional = !readers.empty() && std::all_of(
                    readers.begin(), readers.end(), [](const BindingReference& reference) {
                        return has_flag(reference.binding->flags, AccessFlags::kOptional);
                    });
                if (!readers.empty() && !artifact.external_input && !all_optional) {
                    return {invalid_argument("artifact has readers but no producer"), {}};
                }
                continue;
            }

            if (writers.size() > 1) {
                const bool all_append = std::all_of(
                    writers.begin(), writers.end(), [](const BindingReference& reference) {
                        return is_append_writer(reference.binding->access);
                    });
                const bool deterministic_append = all_append && std::all_of(
                    writers.begin(), writers.end(), [](const BindingReference& reference) {
                        return has_flag(reference.binding->flags, AccessFlags::kDeterministicAppend);
                    });
                if (!deterministic_append) {
                    return {invalid_argument("artifact has multiple unordered producers"), {}};
                }
                std::sort(writers.begin(), writers.end(), [&](const BindingReference& left,
                                                               const BindingReference& right) {
                    return definition.stages[left.stage_index].descriptor.id <
                           definition.stages[right.stage_index].descriptor.id;
                });
                for (size_t index = 1; index < writers.size(); ++index) {
                    add_stage_dependency(writers[index - 1].stage_index,
                                         writers[index].stage_index,
                                         PipelineDependencyKind::kArtifact,
                                         artifact.id);
                }
                for (const BindingReference& writer : writers) {
                    for (const BindingReference& reader : readers) {
                        add_stage_dependency(writer.stage_index,
                                             reader.stage_index,
                                             PipelineDependencyKind::kArtifact,
                                             artifact.id);
                    }
                }
                continue;
            }

            const BindingReference& writer = writers.front();
            for (const BindingReference& reader : readers) {
                add_stage_dependency(writer.stage_index,
                                     reader.stage_index,
                                     PipelineDependencyKind::kArtifact,
                                     artifact.id);
            }
        }

        for (const PipelineOrderingRecord& ordering : definition.orderings) {
            uint32_t target_stage = kInvalidPipelineHandleIndex;
            for (uint32_t index = 0; index < definition.stages.size(); ++index) {
                if (definition.stages[index].descriptor.id == ordering.target_stage) {
                    target_stage = index;
                    break;
                }
            }
            if (target_stage == kInvalidPipelineHandleIndex) {
                return {not_found("explicit ordering references an undeclared stage"), {}};
            }
            if (ordering.source_runs_before) {
                add_stage_dependency(ordering.source_stage_index,
                                     target_stage,
                                     PipelineDependencyKind::kOrdering,
                                     {});
            } else {
                add_stage_dependency(target_stage,
                                     ordering.source_stage_index,
                                     PipelineDependencyKind::kOrdering,
                                     {});
            }
        }

        std::vector<std::vector<uint32_t>> outgoing(plan.nodes.size());
        std::vector<uint32_t> indegree(plan.nodes.size(), 0);
        for (const PipelineDependency& dependency : plan.dependencies) {
            if (dependency.source_node >= plan.nodes.size() || dependency.destination_node >= plan.nodes.size()) {
                return {invalid_state("compiled dependency references an invalid node"), {}};
            }
            outgoing[dependency.source_node].push_back(dependency.destination_node);
            ++indegree[dependency.destination_node];
        }

        std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<>> ready;
        for (uint32_t node_index = 0; node_index < indegree.size(); ++node_index) {
            if (indegree[node_index] == 0) {
                ready.push(node_index);
            }
        }

        std::vector<uint32_t> remaining_indegree = indegree;
        while (!ready.empty()) {
            std::vector<uint32_t> current_batch;
            while (!ready.empty()) {
                current_batch.push_back(ready.top());
                ready.pop();
            }

            PipelineExecutionBatch batch;
            batch.node_indices = current_batch;
            batch.parallelizable = std::all_of(
                current_batch.begin(), current_batch.end(), [&](uint32_t node_index) {
                    return is_parallel_safe(plan.nodes[node_index]);
                });
            plan.topological_order.insert(plan.topological_order.end(), current_batch.begin(), current_batch.end());
            plan.batches.push_back(std::move(batch));

            for (const uint32_t source_node : current_batch) {
                for (const uint32_t destination_node : outgoing[source_node]) {
                    if (--remaining_indegree[destination_node] == 0) {
                        ready.push(destination_node);
                    }
                }
            }
        }

        if (plan.topological_order.size() != plan.nodes.size()) {
            return {invalid_argument("pipeline dependency graph contains a cycle"), {}};
        }

        static std::atomic<PlanRevision> next_revision{1};
        plan.revision = next_revision.fetch_add(1, std::memory_order_relaxed);
        return {foundation::Result::success(), std::move(plan)};
    }

    struct PipelineScheduler::Impl final {
        explicit Impl(uint32_t requested_worker_count)
            : worker_pool([&] {
                  const uint32_t hardware_count = std::max(1u, std::thread::hardware_concurrency());
                  return std::max(1u, requested_worker_count == 0 ? hardware_count : requested_worker_count);
              }()) {
        }

        WorkerPool worker_pool;
        mutable std::mutex execute_mutex;
    };

    PipelineScheduler::PipelineScheduler(uint32_t worker_count) : impl_(std::make_unique<Impl>(worker_count)) {
    }

    PipelineScheduler::~PipelineScheduler() = default;

    PipelineScheduler::PipelineScheduler(PipelineScheduler&&) noexcept = default;

    PipelineScheduler& PipelineScheduler::operator=(PipelineScheduler&&) noexcept = default;

    uint32_t PipelineScheduler::worker_count() const noexcept {
        return impl_ == nullptr ? 0 : impl_->worker_pool.worker_count();
    }

    foundation::Result PipelineScheduler::execute(const PipelinePlan& plan,
                                                   PipelineExecutionContext& context) const {
        if (impl_ == nullptr) {
            return invalid_state("pipeline scheduler is not initialized");
        }
        if (plan.revision == 0 || !plan.pipeline.is_valid()) {
            return invalid_state("pipeline plan is invalid");
        }
        if (plan.scope != context.scope()) {
            return invalid_state("pipeline plan scope does not match execution context scope");
        }

        std::lock_guard execute_lock(impl_->execute_mutex);
        for (const PipelineExecutionBatch& batch : plan.batches) {
            std::vector<uint32_t> cpu_worker_nodes;
            std::vector<uint32_t> main_thread_nodes;
            cpu_worker_nodes.reserve(batch.node_indices.size());
            main_thread_nodes.reserve(batch.node_indices.size());

            for (const uint32_t node_index : batch.node_indices) {
                if (node_index >= plan.nodes.size()) {
                    return invalid_state("pipeline batch references an invalid node");
                }
                const PipelineNode& node = plan.nodes[node_index];
                switch (node.execution_domain) {
                    case ExecutionDomain::kMainThread:
                        main_thread_nodes.push_back(node_index);
                        break;
                    case ExecutionDomain::kCpuWorker:
                        cpu_worker_nodes.push_back(node_index);
                        break;
                    case ExecutionDomain::kIoWorker:
                    case ExecutionDomain::kRenderThread:
                    case ExecutionDomain::kGpu:
                        return unsupported("pipeline scheduler only executes MainThread and CpuWorker nodes");
                }
            }

            if (!cpu_worker_nodes.empty()) {
                const foundation::Result result = impl_->worker_pool.run(
                    plan.nodes, cpu_worker_nodes, context, batch.parallelizable);
                if (!result) {
                    return result;
                }
            }

            for (const uint32_t node_index : main_thread_nodes) {
                foundation::Result result = execute_node(plan.nodes[node_index], context);
                if (!result) {
                    return with_task_context(plan.nodes[node_index], std::move(result));
                }
            }
        }
        return foundation::Result::success();
    }

    foundation::Result PipelineScopeHost::bind_pipeline(PipelineId pipeline) {
        if (!is_valid_scope(scope_)) {
            return invalid_state("pipeline scope host was created with an invalid scope");
        }
        if (active_) {
            return invalid_state("cannot bind a pipeline while the scope host is active");
        }
        if (!pipeline.is_valid()) {
            return invalid_argument("pipeline id is invalid");
        }
        pipeline_ = pipeline;
        active_plan_.reset();
        return foundation::Result::success();
    }

    foundation::Result PipelineScopeHost::rebuild(const PipelineRegistry& registry,
                                                   CapabilityMask available_capabilities) {
        if (active_) {
            return invalid_state("cannot rebuild an active scope host");
        }
        if (!pipeline_.is_valid()) {
            return invalid_state("scope host has no bound pipeline");
        }
        foundation::ResultValue<PipelinePlan> compiled = PipelineCompiler{}.compile({
            .pipeline = pipeline_,
            .scope = scope_,
            .available_capabilities = available_capabilities,
            .registry = &registry,
            .instance = instance_,
        });
        if (!compiled) {
            return compiled.result;
        }
        active_plan_.emplace(std::move(compiled.value));
        return foundation::Result::success();
    }

    foundation::Result PipelineScopeHost::activate(const PipelineActivationContext&) {
        if (active_) {
            return invalid_state("pipeline scope host is already active");
        }
        if (!pipeline_.is_valid() || !active_plan_.has_value()) {
            return invalid_state("scope host must have a bound and compiled pipeline");
        }
        active_ = true;
        return foundation::Result::success();
    }

    foundation::Result PipelineScopeHost::deactivate(const PipelineDeactivationContext&) {
        if (!active_) {
            return invalid_state("pipeline scope host is not active");
        }
        active_ = false;
        return foundation::Result::success();
    }

    foundation::Result PipelineScopeHost::execute(PipelineScheduler& scheduler,
                                                   PipelineExecutionContext& context) const {
        if (!active_ || !active_plan_.has_value()) {
            return invalid_state("pipeline scope host is not active or has no compiled plan");
        }
        if (context.scope() != scope_) {
            return invalid_state("pipeline execution context scope does not match the host scope");
        }
        return scheduler.execute(*active_plan_, context);
    }
}  // namespace aeg::pipeline
