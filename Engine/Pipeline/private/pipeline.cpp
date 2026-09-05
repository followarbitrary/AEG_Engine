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

        PipelineDefinition definition;
        definition.descriptor = descriptor_;
        definition.stages = std::move(stages_);
        definition.tasks = std::move(tasks_);
        definition.artifacts = std::move(artifacts_);
        definition.bindings = std::move(bindings_);
        definition.orderings = std::move(orderings_);
        definition.exposures = std::move(exposures_);
        return {foundation::Result::success(), std::move(definition)};
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

        const PipelineDefinition& definition = *registration.value.definition;
        if ((definition.descriptor.supported_scopes & to_mask(input.scope)) == 0) {
            return {unsupported("pipeline does not support the requested scope"), {}};
        }
        if ((definition.descriptor.required_capabilities & ~input.available_capabilities) != 0) {
            return {unsupported("pipeline required capabilities are unavailable"), {}};
        }

        PipelinePlan plan;
        plan.pipeline = definition.descriptor.id;
        plan.definition_version = definition.descriptor.version;
        plan.scope = input.scope;
        plan.definition_owner = std::shared_ptr<const void>(registration.value.implementation);

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
}  // namespace aeg::pipeline
