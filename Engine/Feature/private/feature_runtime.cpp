#include "feature_runtime.hpp"
#include "feature_graph.hpp"
#include "feature_execution.hpp"
#include "feature_clock_admission.hpp"
#include "render_intent_planner.hpp"
#include "render_intent_evidence.hpp"
#include "feature_outbox.hpp"
#include "feature_input.hpp"
#include "concurrency.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <exception>
#include <functional>
#include <limits>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace aeg::feature {
namespace detail {
struct FeatureOutboxRuntimeIdentity final {};

struct FeatureOutboxOwner final {
    std::shared_ptr<const FeatureOutboxRuntimeIdentity> runtime{};
    FeatureNodeProvenance source{};
    RuntimeGeneration generation{};
    uint64_t definition_digest{0};
    std::vector<SideEffectContract> declarations{};
};

struct FeatureOutboxEntry final {
    std::shared_ptr<const FeatureOutboxOwner> owner{};
    OutboxIntent intent{};
    OutboxReceipt receipt{};
};

struct FeatureOutboxJournal final {
    std::shared_ptr<const FeatureOutboxRuntimeIdentity> identity =
        std::make_shared<const FeatureOutboxRuntimeIdentity>();
    std::unordered_map<FeatureInstanceId, std::shared_ptr<const FeatureOutboxOwner>,
                       StableIdHash<FeatureInstanceIdTag>> owners{};
    std::vector<std::shared_ptr<FeatureOutboxEntry>> pending{};
    uint64_t pending_bytes{0};
};
}  // namespace detail

namespace {
foundation::Result invalid_state(std::string message) {
    return foundation::Result::failure(foundation::ResultCode::kInvalidState, std::move(message));
}

template <typename Instance, typename Process>
auto& invocation_state(Instance& instance, Process* process, StatePolicy policy) {
    return policy == StatePolicy::kInstanceState || process == nullptr ? instance.state : process->state;
}

StateSnapshot state_snapshot(const FeatureInstance& instance, const ProcessInstance* process,
                             StatePolicy policy) {
    const auto& state = invocation_state(instance, process, policy);
    return {state.version, owns_state_payload(policy) ? state.schema_version : Version{},
            owns_state_payload(policy) ? state.payload : std::vector<uint8_t>{},
            process == nullptr ? std::string{} : process->current_state};
}

bool state_matches(const InvocationToken& token, const FeatureInstance& instance,
                    const ProcessInstance* process, StatePolicy policy) {
    const auto& state = invocation_state(instance, process, policy);
    return state.version == token.state_version && state.commit_count == token.input_version &&
           (process == nullptr ? !token.process_revision.is_valid()
                               : process->status == ProcessStatus::kReady &&
                                     process->revision == token.process_revision);
}

struct FlagScope final {
    bool& active;
    bool previous;
    explicit FlagScope(bool& flag) : active(flag), previous(std::exchange(flag, true)) {}
    ~FlagScope() { active = previous; }
};

bool declares_outbox(const std::vector<SideEffectContract>& declarations, SideEffectId effect,
                     uint32_t version) noexcept {
    return effect.is_valid() && version != 0 &&
        std::any_of(declarations.begin(), declarations.end(), [&](const SideEffectContract& declaration) {
            return declaration.id == effect && declaration.version == version &&
                declaration.kind == SideEffectKind::kCommand && declaration.commit == CommitPolicy::kOutbox;
        });
}

void retire_outbox_entry(detail::FeatureOutboxJournal& journal, std::size_t index,
                         detail::OutboxDeliveryState state, foundation::ResultCode reason) noexcept {
    auto& entry = journal.pending[index];
    entry->receipt.state = state;
    entry->receipt.error_code = reason;
    journal.pending_bytes -= entry->intent.payload.size();
    journal.pending.erase(journal.pending.begin() + static_cast<std::ptrdiff_t>(index));
}

void cancel_outbox(detail::FeatureOutboxJournal* journal) noexcept {
    if (!journal) return;
    for (auto& entry : journal->pending) {
        entry->receipt.state = detail::OutboxDeliveryState::kCancelled;
        entry->receipt.error_code = foundation::ResultCode::kCancelled;
    }
    journal->pending.clear();
    journal->pending_bytes = 0;
}

bool same_expansion(const ExpandedFeature& supplied, const ExpandedFeature& live) {
    if (supplied.definition_id != live.definition_id || supplied.version != live.version ||
        supplied.domains != live.domains || supplied.owner_module != live.owner_module ||
        supplied.owner_path.nodes != live.owner_path.nodes || supplied.children.size() != live.children.size())
        return false;
    const FeatureDefinition supplied_definition{ supplied.definition_id, supplied.version,
                                                 supplied.owner_module, supplied.contract };
    const FeatureDefinition live_definition{ live.definition_id, live.version, live.owner_module, live.contract };
    if (feature_contract_digest(supplied_definition) != feature_contract_digest(live_definition)) return false;
    for (size_t index = 0; index < live.children.size(); ++index)
        if (!same_expansion(supplied.children[index], live.children[index])) return false;
    return true;
}

foundation::Result validate_state_payload(const FeatureDefinition& definition,
                                           std::span<const uint8_t> payload, bool& callback_active) {
    if (!owns_state_payload(definition.contract.state))
        return invalid_state("Feature does not own a local state payload");
    if (payload.size() > definition.contract.state_payload.max_size)
        return invalid_state("state payload exceeds the declared byte limit");
    if (definition.validate_state_payload) {
        try {
            const auto validator = definition.validate_state_payload;
            FlagScope scope(callback_active);
            return validator(payload);
        } catch (...) {
            return invalid_state("state payload validator threw an exception");
        }
    }
    return foundation::Result::success();
}
}

namespace detail {
FeatureOutboxBinding::FeatureOutboxBinding(std::shared_ptr<const FeatureOutboxOwner> owner,
                                         SideEffectId effect, uint32_t version) noexcept
    : owner_(std::move(owner)), effect_(effect), version_(version) {}

const FeatureNodeProvenance* FeatureOutboxBinding::source() const noexcept {
    return owner_ ? &owner_->source : nullptr;
}
RuntimeGeneration FeatureOutboxBinding::generation() const noexcept {
    return owner_ ? owner_->generation : RuntimeGeneration{};
}
uint64_t FeatureOutboxBinding::definition_digest() const noexcept {
    return owner_ ? owner_->definition_digest : 0;
}

FeatureOutboxHandle::FeatureOutboxHandle(std::shared_ptr<FeatureOutboxEntry> entry) noexcept
    : entry_(std::move(entry)) {}
const OutboxIntent* FeatureOutboxHandle::intent() const noexcept { return entry_ ? &entry_->intent : nullptr; }
const InvocationToken* FeatureOutboxHandle::invocation() const noexcept {
    return entry_ ? &entry_->receipt.invocation : nullptr;
}
uint32_t FeatureOutboxHandle::entry_index() const noexcept { return entry_ ? entry_->receipt.entry_index : 0; }
OutboxReceipt FeatureOutboxHandle::receipt() const noexcept {
    return entry_ ? entry_->receipt
                  : OutboxReceipt{{}, 0, OutboxDeliveryState::kRejected, foundation::ResultCode::kInvalidState};
}

foundation::Result FeatureOutboxAccess::validate_binding(FeatureRuntime& runtime,
                                                       const FeatureOutboxBinding& binding) {
    if (!runtime.on_owner_thread() || runtime.execution_closed_ || runtime.state_callback_active_)
        return invalid_state("outbox access requires its open, non-reentrant Runtime owner");
    if (!binding.owner_ || !runtime.outbox_ || binding.owner_->runtime != runtime.outbox_->identity ||
        binding.owner_->generation != runtime.generation_)
        return invalid_state("outbox binding belongs to another Runtime or retired generation");
    const auto owner = runtime.outbox_->owners.find(binding.owner_->source.instance_id);
    if (owner == runtime.outbox_->owners.end() || owner->second != binding.owner_ ||
        !declares_outbox(binding.owner_->declarations, binding.effect_, binding.version_))
        return invalid_state("outbox binding has no canonical published owner or protocol");
    return foundation::Result::success();
}

foundation::ResultValue<FeatureOutboxBinding> FeatureOutboxAccess::bind(
    FeatureRuntime& runtime, FeatureInstanceId instance, SideEffectId effect, uint32_t version) {
    if (!runtime.on_owner_thread() || runtime.execution_closed_ || runtime.state_callback_active_)
        return {invalid_state("outbox binding requires its open, non-reentrant Runtime owner"), {}};
    const auto owner = runtime.outbox_->owners.find(instance);
    if (owner == runtime.outbox_->owners.end() || owner->second->generation != runtime.generation_)
        return {invalid_state("outbox owner was not published by this Runtime generation"), {}};
    if (!declares_outbox(owner->second->declarations, effect, version))
        return {foundation::invalid_argument("outbox command ID/version is not declared with outbox commit policy"), {}};
    return {foundation::Result::success(), FeatureOutboxBinding(owner->second, effect, version)};
}

foundation::ResultValue<std::vector<FeatureOutboxHandle>> FeatureOutboxAccess::capture(
    FeatureRuntime& runtime, const FeatureOutboxBinding& binding) {
    if (const auto result = validate_binding(runtime, binding); !result) return {result, {}};
    try {
        std::vector<FeatureOutboxHandle> captured;
        captured.reserve(runtime.outbox_->pending.size());
        for (const auto& entry : runtime.outbox_->pending)
            if (entry->owner == binding.owner_ && entry->intent.effect == binding.effect_ &&
                entry->intent.version == binding.version_)
                captured.push_back(FeatureOutboxHandle(entry));
        return {foundation::Result::success(), std::move(captured)};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("outbox capture allocation failed"), {}};
    }
}

foundation::ResultValue<std::size_t> FeatureOutboxAccess::pending_index(
    FeatureRuntime& runtime, const FeatureOutboxBinding& binding, const FeatureOutboxHandle& handle) {
    if (const auto result = validate_binding(runtime, binding); !result) return {result, {}};
    if (!handle.entry_ || handle.entry_->owner != binding.owner_ ||
        handle.entry_->intent.effect != binding.effect_ || handle.entry_->intent.version != binding.version_ ||
        handle.entry_->receipt.state != OutboxDeliveryState::kPending)
        return {invalid_state("outbox handle is invalid, terminal or belongs to another binding"), {}};
    const auto found = std::find(runtime.outbox_->pending.begin(), runtime.outbox_->pending.end(), handle.entry_);
    if (found == runtime.outbox_->pending.end())
        return {invalid_state("outbox handle is not a canonical pending delivery"), {}};
    return {foundation::Result::success(), static_cast<std::size_t>(found - runtime.outbox_->pending.begin())};
}

foundation::Result FeatureOutboxAccess::consume(
    FeatureRuntime& runtime, const FeatureOutboxBinding& binding, const FeatureOutboxHandle& handle,
    void (*apply)(void*, const OutboxIntent&) noexcept, void* user_data) {
    const auto index = pending_index(runtime, binding, handle);
    if (!index) return index.result;
    if (!apply) return foundation::invalid_argument("outbox application function is null");
    const auto entry = handle.entry_;
    FlagScope scope(runtime.state_callback_active_);
    apply(user_data, entry->intent);
    // Every fallible check and allocation precedes application. The guarded
    // owner cannot mutate the pending vector while its canonical entry applies.
    retire_outbox_entry(*runtime.outbox_, index.value, OutboxDeliveryState::kApplied,
                        foundation::ResultCode::kSuccess);
    return foundation::Result::success();
}

foundation::Result FeatureOutboxAccess::reject(
    FeatureRuntime& runtime, const FeatureOutboxBinding& binding, const FeatureOutboxHandle& handle,
    foundation::ResultCode reason) {
    const auto index = pending_index(runtime, binding, handle);
    if (!index) return index.result;
    if (reason == foundation::ResultCode::kSuccess || reason > foundation::ResultCode::kCancelled)
        return foundation::invalid_argument("outbox rejection requires a valid failure result code");
    retire_outbox_entry(*runtime.outbox_, index.value, OutboxDeliveryState::kRejected, reason);
    return foundation::Result::success();
}
}  // namespace detail

FeatureRuntime::FeatureRuntime(FeatureRegistry& registry, AssetKindResolver asset_kind_resolver,
                               FeatureRuntimeOptions options)
    : registry_(registry), asset_kind_resolver_(std::move(asset_kind_resolver)),
      time_source_(std::make_shared<detail::FeatureTimeSource>(std::move(options.now_ns))),
      generation_(concurrency::allocate_unique_identity()),
      outbox_(std::make_unique<detail::FeatureOutboxJournal>()),
      input_(std::make_unique<detail::FeatureInputState>()),
      execution_(std::make_unique<FeatureExecution>()) {}

FeatureRuntime::~FeatureRuntime() { cancel_outbox(outbox_.get()); }

std::unique_ptr<FeatureRuntime> FeatureRuntime::make_reset_runtime() const {
    return make_reset_runtime(registry_, asset_kind_resolver_);
}

std::unique_ptr<FeatureRuntime> FeatureRuntime::make_reset_runtime(
    FeatureRegistry& registry, AssetKindResolver asset_kind_resolver) const {
    auto replacement = std::make_unique<FeatureRuntime>(registry, std::move(asset_kind_resolver));
    replacement->time_source_ = time_source_;
    return replacement;
}

foundation::Result FeatureRuntime::validate_parameter_assets(
    std::span<const uint8_t> parameters, std::span<const ParameterContract> schema) {
    // Resolution is a host callback. Preserve the same mutation barrier used
    // by other contract callbacks, including while a candidate is prepared.
    FlagScope scope(state_callback_active_);
    return validate_feature_parameter_assets(parameters, schema, asset_kind_resolver_);
}

bool FeatureRuntime::on_owner_thread() const noexcept { return execution_->on_owner_thread(); }

foundation::Result FeatureRuntime::enable_async_execution() {
    if (!on_owner_thread() || state_callback_active_ || execution_closed_)
        return invalid_state("async execution requires an open owner Runtime");
    if (!invocations_.empty()) return invalid_state("cannot change execution policy with live invocations");
    async_execution_ = true;
    return foundation::Result::success();
}

size_t FeatureRuntime::pending_execution_count() const noexcept {
    return on_owner_thread() ? execution_->pending_count() : 0;
}

RuntimeGeneration FeatureRuntime::generation() const noexcept {
    return on_owner_thread() ? generation_ : RuntimeGeneration{};
}

foundation::ResultValue<std::vector<RenderFeatureOutput>> FeatureRuntime::capture_render_outputs(
    const CompiledFeatureGraph& graph) const {
    const auto owners = capture_render_resource_owners(graph);
    if (!owners) return {owners.result, {}};
    std::vector<RenderFeatureOutput> outputs;
    for (const auto& plan : graph.plans) {
        const auto found = render_publications_.find(plan.clock);
        if (found == render_publications_.end()) continue;
        const auto& publication = found->second;
        if (!publication.readiness && publication.readiness.code != foundation::ResultCode::kNotReady)
            return {publication.readiness, {}};
        bool current = render_publication_current(publication);
        for (const auto& captured : publication.owners) {
            const auto live = std::find(owners.value.begin(), owners.value.end(), captured.resource);
            if (live == owners.value.end()) { current = false; break; }
        }
        if (!current) continue; // Retire the entire cohort, never a surviving subset.
        outputs.insert(outputs.end(), publication.outputs.begin(), publication.outputs.end());
    }
    return {foundation::Result::success(), std::move(outputs)};
}

foundation::ResultValue<std::vector<RenderResourceOwner>> FeatureRuntime::capture_render_resource_owners(
    const CompiledFeatureGraph& graph) const {
    if (!on_owner_thread() || execution_closed_) return {invalid_state("render capture requires an open owner Runtime"), {}};
    std::unordered_map<FeatureInstanceId, FeatureNodeId, StableIdHash<FeatureInstanceIdTag>> nodes;
    const auto map_node = [&](const auto& self, FeatureInstanceId id, FeatureNodeId node) -> bool {
        const auto owner = instances_.find(id);
        if (owner == instances_.end()) return false;
        nodes.emplace(id, node);
        for (const auto child : owner->second.children) if (!self(self, child, node)) return false;
        return true;
    };
    for (const auto& provenance : graph.provenance)
        if (!map_node(map_node, provenance.instance_id, provenance.node_id))
            return {invalid_state("render capture references a retired FeatureGraph"), {}};
    std::vector<RenderResourceOwner> outputs;
    std::unordered_set<FeatureInstanceId, StableIdHash<FeatureInstanceIdTag>> seen;
    for (const auto& plan : graph.plans) for (const auto id : plan.instances) {
        if (!nodes.contains(id) || !seen.insert(id).second) continue;
        const auto& owner = instances_.at(id);
        const auto* definition = registry_.find_definition(owner.definition_id);
        if (definition == nullptr || owner.definition_digest != feature_contract_digest(*definition))
            return {invalid_state("render capture definition binding is stale"), {}};
        if (definition->contract.structure != FeatureStructure::kAtomic ||
            definition->contract.domains != to_mask(FeatureDomain::kRender)) continue;
        if (!owner.processes.empty() && processes_.at(owner.processes.front()).status == ProcessStatus::kCancelled) continue;
        outputs.push_back({id, nodes.at(id), owner.definition_digest, owner.render_resource_epoch});
    }
    return {foundation::Result::success(), std::move(outputs)};
}

foundation::Result FeatureRuntime::render_publication_status(ClockId clock) const {
    if (!on_owner_thread() || execution_closed_) return invalid_state("render publication read requires an open owner Runtime");
    const auto found = render_publications_.find(clock);
    if (found != render_publications_.end() && !found->second.readiness &&
        found->second.readiness.code != foundation::ResultCode::kNotReady) return found->second.readiness;
    if (render_dirty_.contains(clock) || plan_busy(clock) || found == render_publications_.end() ||
        !render_publication_current(found->second))
        return foundation::not_ready("Render Clock has no settled current wave");
    return found->second.readiness;
}

bool FeatureRuntime::render_publication_current(const RenderPublication& publication) const {
    for (const auto& captured : publication.owners) {
        const auto owner = instances_.find(captured.resource.instance_id);
        if (owner == instances_.end() || owner->second.definition_digest != captured.resource.definition_digest ||
            owner->second.render_resource_epoch != captured.resource.resource_epoch) return false;
        const auto* definition = registry_.find_definition(owner->second.definition_id);
        if (!definition || feature_contract_digest(*definition) != captured.resource.definition_digest) return false;
        const auto* process = owner->second.processes.empty() ? nullptr : find_process(owner->second.processes.front());
        if ((process && process->status == ProcessStatus::kCancelled) ||
            captured.process_revision != (process ? process->revision : ProcessRevision{})) return false;
    }
    return true;
}

foundation::Result FeatureRuntime::settle_render_publications() {
    auto first_error = foundation::Result::success();
    for (auto it = render_dirty_.begin(); it != render_dirty_.end();) {
        const auto clock = *it;
        if (plan_busy(clock)) { ++it; continue; }
        const auto plan = plans_.find(clock);
        if (plan == plans_.end()) { it = render_dirty_.erase(it); continue; }
        RenderPublication candidate;
        for (const auto id : plan->second.instances) {
            const auto node = render_nodes_.find(id);
            if (node == render_nodes_.end()) continue;
            const auto& owner = instances_.at(id);
            const auto* definition = registry_.find_definition(owner.definition_id);
            if (definition == nullptr || definition->contract.domains != to_mask(FeatureDomain::kRender) ||
                definition->contract.structure != FeatureStructure::kAtomic) continue;
            const auto* process = owner.processes.empty() ? nullptr : find_process(owner.processes.front());
            if (process && process->status == ProcessStatus::kCancelled) continue;
            const RenderResourceOwner resource{id, node->second, owner.definition_digest, owner.render_resource_epoch};
            candidate.owners.push_back({resource, process ? process->revision : ProcessRevision{}});
            if (!owner.render_output || owner.render_output->invocation.generation != generation_) continue;
            const auto& output = *owner.render_output;
            if (output.process_revision != (process ? process->revision : ProcessRevision{})) continue;
            candidate.outputs.push_back({definition->contract, owner.owner_module, node->second, id, owner.owner_path,
                output.invocation, output.intent, resource, output.input_messages});
        }
        if (candidate.owners.empty()) { it = render_dirty_.erase(it); continue; }
        auto& publication = render_publications_[clock];
        if (!render_publication_current(publication)) {
            publication.outputs.clear();
            publication.owners.clear();
        }
        std::vector<std::shared_ptr<const detail::RenderLightListEvidence>> retired;
        for (const auto& output : candidate.outputs) for (const auto& message : output.input_messages) {
            const auto& evidence = message.render_source;
            if (!evidence) continue;
            const auto& source = evidence->source;
            const auto live = instances_.find(source.instance_id);
            const auto* process = live == instances_.end() || live->second.processes.empty() ? nullptr :
                find_process(live->second.processes.front());
            const bool current = live != instances_.end() && source.invocation.generation == generation_ &&
                source.resource_owner.definition_digest == live->second.definition_digest &&
                source.resource_owner.resource_epoch == live->second.render_resource_epoch &&
                (!process || (process->status != ProcessStatus::kCancelled && process->revision == evidence->process_revision));
            if (!current && std::find(retired.begin(), retired.end(), evidence) == retired.end()) retired.push_back(evidence);
        }
        const auto validated = RenderIntentPlanner{}.plan_with_retired_sources(candidate.outputs, publication.outputs, retired);
        if (validated) {
            candidate.readiness = foundation::Result::success();
            publication = std::move(candidate);
        } else {
            publication.readiness = validated.result;
            if (validated.result.code != foundation::ResultCode::kNotReady && first_error) first_error = validated.result;
        }
        it = render_dirty_.erase(it);
    }
    return first_error;
}

foundation::Result FeatureRuntime::shutdown_execution() {
    if (!on_owner_thread() || state_callback_active_)
        return invalid_state("execution shutdown requires the Runtime owner");
    if (execution_closed_) return foundation::Result::success();
    execution_closed_ = true;
    execution_->shutdown();
    cancel_outbox(outbox_.get());
    input_->slots.clear();
    deferred_.clear();
    execution_backlog_.clear();
    release_timings_.clear();
    invocations_.clear();
    render_publications_.clear();
    render_dirty_.clear();
    generation_ = RuntimeGeneration(concurrency::allocate_unique_identity());
    for (auto& [id, instance] : instances_) {
        instance.retry_pending = false;
        instance.retry_state.reset();
        instance.retry_input.clear();
        instance.retry_parameters.clear();
        instance.retry_messages.clear();
        instance.render_output.reset();
        ++instance.render_resource_epoch;
    }
    return foundation::Result::success();
}

foundation::Result FeatureRuntime::pump_completions() {
    return pump_completions_impl(true);
}

foundation::Result FeatureRuntime::pump_completions_impl(bool admit_queued_work) {
    if (!on_owner_thread() || state_callback_active_ || execution_closed_)
        return invalid_state("completion pump requires an open owner Runtime");
    FlagScope boundary(render_boundary_active_);
    auto first_error = foundation::Result::success();
    // Only the set ready at entry is drained. Newly submitted retries are
    // processed by a later pump, so zero-backoff failure cannot monopolize it.
    for (const auto& work : execution_->take_ready()) {
        const auto record = invocations_.find(work->token.invocation_id);
        if (record == invocations_.end() || !validate_invocation(record->second, work->token)) {
            invocations_.erase(work->token.invocation_id);
            discard_dependents(work->instance);
            continue; // Retired results are cleanup, not current-domain errors.
        }
        auto result = finish_invocation(*work);
        if (admit_queued_work && result && instances_.at(work->instance).retry_pending &&
            work->definition.contract.retry.backoff_ticks == 0)
            result = invoke_and_complete(work->instance, {}, true);
        if (!result && first_error) first_error = result;
        if (!result) {
            discard_dependents(work->instance);
            execution_backlog_.erase(work->definition.contract.clock.id);
        }
    }
    const auto deferred = deferred_;
    for (const auto id : admit_queued_work ? deferred : std::vector<FeatureInstanceId>{}) {
        // A preceding owner may have retired this whole dependency chain.
        // The iteration snapshot is not authority to submit removed work.
        if (std::find(deferred_.begin(), deferred_.end(), id) == deferred_.end()) continue;
        if (prerequisites_busy(id)) continue;
        std::erase(deferred_, id);
        const auto instance = instances_.find(id);
        if (instance == instances_.end()) continue;
        if (!instance->second.processes.empty()) {
            const auto* process = find_process(instance->second.processes.front());
            if (process == nullptr || process->status != ProcessStatus::kReady) {
                discard_dependents(id);
                continue;
            }
        }
        const auto result = invoke_and_complete(id, {}, false, true);
        if (!result) {
            discard_dependents(id);
            if (first_error) first_error = result;
        }
    }
    if (const auto result = settle_render_publications(); !result && first_error) first_error = result;
    if (!admit_queued_work) return first_error;
    for (const auto clock : clocks()) {
        const auto queued = execution_backlog_.find(clock);
        if (queued == execution_backlog_.end() || queued->second.empty() || plan_busy(clock)) continue;
        const auto released = queued->second.front();
        queued->second.pop_front();
        if (const auto result = schedule_plan(clock, released); !result) {
            queued->second.clear();
            if (first_error) first_error = result;
        }
    }
    return first_error;
}

bool FeatureRuntime::prerequisites_busy(FeatureInstanceId instance) const {
    const auto* target = registry_.find_definition(instances_.at(instance).definition_id);
    for (const auto& [source_id, routes] : routes_) {
        const auto* source = registry_.find_definition(instances_.at(source_id).definition_id);
        if (source == nullptr || target == nullptr || source->contract.clock.id != target->contract.clock.id) continue;
        for (const auto& route : routes) {
            if (route.target != instance || (route.kind != EdgeKind::kData &&
                route.kind != EdgeKind::kCommand && route.kind != EdgeKind::kReceipt)) continue;
            if (execution_->busy(source_id) || instances_.at(source_id).retry_pending ||
                std::find(deferred_.begin(), deferred_.end(), source_id) != deferred_.end()) return true;
        }
    }
    return false;
}

void FeatureRuntime::discard_dependents(FeatureInstanceId source) {
    std::vector<FeatureInstanceId> rejected{source};
    for (size_t index = 0; index < rejected.size(); ++index) {
        const auto found = routes_.find(rejected[index]);
        if (found == routes_.end()) continue;
        for (const auto& route : found->second) {
            if (route.kind != EdgeKind::kData && route.kind != EdgeKind::kCommand && route.kind != EdgeKind::kReceipt) continue;
            if (std::find(rejected.begin(), rejected.end(), route.target) == rejected.end()) rejected.push_back(route.target);
        }
    }
    for (const auto id : rejected) {
        std::erase(deferred_, id);
        release_timings_.erase(id);
    }
}

foundation::Result FeatureRuntime::collect_clock_workload(
    std::vector<detail::ClockWorkDemand>& demands, const FeatureDefinition* replacement) const {
    for (const auto& [id, owner] : instances_) {
        const auto* current = registry_.find_definition(owner.definition_id);
        if (!current || owner.definition_digest != feature_contract_digest(*current))
            return invalid_state("Clock workload owner binding changed without Runtime migration");
        const auto& contract = replacement && owner.definition_id == replacement->id ? replacement->contract : current->contract;
        demands.push_back({id.value(), contract, owner.owner_path});
    }
    return foundation::Result::success();
}

foundation::ResultValue<FeatureInstanceId> FeatureRuntime::activate(
    const ExpandedFeature& feature, ScopeId scope) {
    if (!on_owner_thread() || execution_closed_) return {invalid_state("Runtime mutation requires its open owner thread"), {}};
    if (state_callback_active_) return {invalid_state("state schema callbacks cannot mutate Runtime"), {}};
    if (!scope.is_valid() || !feature.definition_id.is_valid()) {
        return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                             "feature instance requires definition and scope"), {}};
    }
    const auto live = registry_.expand(feature.definition_id, feature.owner_path);
    if (!live) return {live.result, {}};
    // Validate all descendants before allocating any owner or changing a plan.
    if (!same_expansion(feature, live.value))
        return {invalid_state("Feature expansion belongs to a stale definition contract"), {}};
    std::vector<detail::ClockWorkDemand> workload;
    if (const auto valid = collect_clock_workload(workload); !valid) return {valid, {}};
    uint64_t identity = next_instance_;
    const auto collect = [&](const auto& self, const ExpandedFeature& item) -> void {
        workload.push_back({identity++, item.contract, item.owner_path});
        for (const auto& child : item.children) self(self, child);
    };
    collect(collect, feature);
    if (const auto valid = detail::validate_clock_workload(workload, registry_); !valid) return {valid, {}};
    return activate_expanded(feature, scope);
}

foundation::ResultValue<FeatureInstanceId> FeatureRuntime::activate_expanded(
    const ExpandedFeature& feature, ScopeId scope) {
    const auto id = FeatureInstanceId(next_instance_++);
    FeatureInstance instance{id, feature.definition_id, feature.owner_module, scope, feature.owner_path, {}, {}};
    instance.definition_digest = feature_contract_digest(*registry_.find_definition(feature.definition_id));
    instance.state.schema_version = feature.contract.state_payload.schema_version;
    instances_.emplace(id, instance);
    auto& plan = plans_[feature.contract.clock.id];
    plan.clock = feature.contract.clock.id;
    plan.instances.push_back(id);
    if (feature.contract.execution == ExecutionKind::kProcess ||
        feature.contract.state == StatePolicy::kProcessState) {
        const auto process_id = ProcessId(next_process_++);
        processes_.emplace(process_id, ProcessInstance{process_id, id, feature.owner_module});
        processes_.at(process_id).state.schema_version = feature.contract.state_payload.schema_version;
        instances_.at(id).processes.push_back(process_id);
    }
    for (const auto& child : feature.children) {
        auto child_instance = activate_expanded(child, child.contract.scope.id.is_valid() ? child.contract.scope.id : scope);
        if (!child_instance) {
            instances_.erase(id);
            return child_instance;
        }
        instances_.at(id).children.push_back(child_instance.value);
        if (!feature.contract.allows_child_process || instances_.at(id).processes.empty()) continue;
        auto& parent_process = processes_.at(instances_.at(id).processes.front());
        const auto& child_value = instances_.at(child_instance.value);
        for (const auto child_process_id : child_value.processes) {
            auto child_process = processes_.find(child_process_id);
            if (child_process == processes_.end()) {
                instances_.erase(id);
                return {invalid_state("service child Process instance is missing"), {}};
            }
            child_process->second.parent = parent_process.id;
            parent_process.children.push_back(child_process_id);
        }
    }
    return {foundation::Result::success(), id};
}

foundation::ResultValue<InvocationToken> FeatureRuntime::invoke(FeatureInstanceId instance_id) {
    if (!on_owner_thread() || execution_closed_) return {invalid_state("Runtime mutation requires its open owner thread"), {}};
    if (state_callback_active_) return {invalid_state("state schema callbacks cannot mutate Runtime"), {}};
    if (execution_->busy(instance_id)) return {invalid_state("Feature has an in-flight invocation"), {}};
    auto it = instances_.find(instance_id);
    if (it == instances_.end()) {
        return {foundation::Result::failure(foundation::ResultCode::kNotFound,
                                             "feature instance was not found"), {}};
    }
    if (it->second.retry_pending) return {invalid_state("Feature invocation is waiting for its clock-owned retry"), {}};
    ProcessInstance* process_instance = nullptr;
    if (!it->second.processes.empty()) {
        process_instance = mutable_process(it->second.processes.front());
        if (process_instance == nullptr || process_instance->status != ProcessStatus::kReady) {
            return {invalid_state("feature process is not ready"), {}};
        }
    }
    const auto* definition = registry_.find_definition(it->second.definition_id);
    if (definition == nullptr) return {invalid_state("Feature definition is no longer registered"), {}};
    if (it->second.definition_digest != feature_contract_digest(*definition))
        return {invalid_state("active Feature contract changed without Runtime migration or graph publication"), {}};
    return invoke_internal(it->second, process_instance, definition->contract.state);
}

foundation::Result FeatureRuntime::dispatch_call(FeatureInstanceId instance_id,
                                                 std::span<const uint8_t> input) {
    if (!on_owner_thread() || execution_closed_) return invalid_state("Runtime mutation requires its open owner thread");
    if (state_callback_active_) return invalid_state("state schema callbacks cannot mutate Runtime");
    const auto instance = instances_.find(instance_id);
    if (instance == instances_.end())
        return foundation::Result::failure(foundation::ResultCode::kNotFound,
                                           "feature instance was not found");
    const auto* definition = registry_.find_definition(instance->second.definition_id);
    if (definition == nullptr || definition->contract.execution != ExecutionKind::kCall || !definition->execute)
        return invalid_state("feature instance is not an executable Call Feature");
    return invoke_and_complete(instance_id, input);
}

foundation::Result FeatureRuntime::dispatch_process(FeatureInstanceId instance_id,
                                                    std::span<const uint8_t> input) {
    if (!on_owner_thread() || execution_closed_) return invalid_state("Runtime mutation requires its open owner thread");
    if (state_callback_active_) return invalid_state("state schema callbacks cannot mutate Runtime");
    const auto instance = instances_.find(instance_id);
    if (instance == instances_.end())
        return foundation::Result::failure(foundation::ResultCode::kNotFound,
                                           "feature instance was not found");
    const auto* definition = registry_.find_definition(instance->second.definition_id);
    if (definition == nullptr || definition->contract.execution != ExecutionKind::kProcess ||
        !definition->execute)
        return invalid_state("feature instance is not an executable Process Feature");
    return invoke_and_complete(instance_id, input);
}

foundation::ResultValue<InvocationToken> FeatureRuntime::invoke_internal(
    FeatureInstance& instance, ProcessInstance* process_instance, StatePolicy state_policy) {
    const auto now = time_source_->read();
    if (!now) return {now.result, {}};
    const auto* definition = registry_.find_definition(instance.definition_id);
    const auto expiry = transport_add(now.value, {definition->contract.clock.timing->relative_deadline_ns});
    if (!expiry) return {expiry.result, {}};
    InvocationToken token{};
    token.invocation_id = InvocationId(next_invocation_++);
    token.attempt_id = AttemptId(next_attempt_++);
    token.generation = generation_;
    const auto& state = invocation_state(instance, process_instance, state_policy);
    token.input_version = state.commit_count;
    token.state_version = state.version;
    if (process_instance != nullptr) token.process_revision = process_instance->revision;
    invocations_.emplace(token.invocation_id,
                         InvocationRecord{token, instance.id,
                                          process_instance == nullptr ? ProcessId{} : process_instance->id});
    instance.last_invocation_issued = InvocationAttempt{token, process_instance ? process_instance->id : ProcessId{}};
    invocations_.at(token.invocation_id).timing = {now.value, expiry.value};
    return {foundation::Result::success(), token};
}

foundation::Result FeatureRuntime::tick(ClockId clock) {
    if (!on_owner_thread() || execution_closed_) return invalid_state("Runtime mutation requires its open owner thread");
    if (state_callback_active_) return invalid_state("state schema callbacks cannot mutate Runtime");
    if (!clock.is_valid()) {
        return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                           "clock id is required");
    }
    if (const auto result = pump_completions(); !result) return result;
    const auto released = time_source_->read();
    if (!released) return released.result;
    return release_clock(clock, released.value);
}

foundation::Result FeatureRuntime::release_clock(ClockId clock, TransportTimePoint released) {
    const auto plan = plans_.find(clock);
    if (plan == plans_.end()) {
        return foundation::Result::failure(foundation::ResultCode::kNotFound,
                                           "clock has no active feature plan");
    }
    const auto current_tick = ++clock_ticks_[clock];
    const bool has_deferred = std::any_of(plan->second.instances.begin(), plan->second.instances.end(),
        [&](FeatureInstanceId id) { return std::find(deferred_.begin(), deferred_.end(), id) != deferred_.end(); });
    if ((async_execution_ || has_deferred) && plan_busy(clock)) {
        bool retried = false;
        const auto ids = plan->second.instances;
        for (const auto id : ids) {
            const auto& owner = instances_.at(id);
            if (!owner.retry_pending || owner.retry_due_tick > current_tick || execution_->busy(id)) continue;
            const auto* process = owner.processes.empty() ? nullptr : find_process(owner.processes.front());
            if (process != nullptr && process->status != ProcessStatus::kReady) continue;
            if (const auto result = invoke_and_complete(id, {}, true); !result) return result;
            retried = true;
        }
        if (retried) return foundation::Result::success();
        const auto* definition = registry_.find_definition(instances_.at(ids.front()).definition_id);
        if (definition == nullptr) return invalid_state("clock owner is no longer registered");
        const auto& contract = definition->contract.clock;
        if (contract.late_data_policy == LateDataPolicy::kDrop) return foundation::Result::success();
        if (contract.late_data_policy == LateDataPolicy::kReject)
            return invalid_state("Feature clock still has an active execution plan");
        auto& backlog = execution_backlog_[clock];
        if (backlog.size() >= contract.max_backlog) return invalid_state("Feature async clock backlog exceeded its budget");
        backlog.push_back(released);
        return foundation::Result::success();
    }
    return schedule_plan(clock, released);
}

bool FeatureRuntime::plan_busy(ClockId clock) const {
    const auto plan = plans_.find(clock);
    if (plan == plans_.end()) return false;
    if (std::any_of(invocations_.begin(), invocations_.end(), [&](const auto& entry) {
            const auto owner = instances_.find(entry.second.instance);
            if (owner == instances_.end()) return false;
            const auto* definition = registry_.find_definition(owner->second.definition_id);
            return definition && definition->contract.clock.id == clock;
        })) return true;
    return std::any_of(plan->second.instances.begin(), plan->second.instances.end(), [&](FeatureInstanceId id) {
        return execution_->busy(id) || instances_.at(id).retry_pending ||
               std::find(deferred_.begin(), deferred_.end(), id) != deferred_.end();
    });
}

foundation::Result FeatureRuntime::schedule_plan(ClockId clock, TransportTimePoint released) {
    FlagScope boundary(render_boundary_active_);
    render_dirty_.insert(clock);
    const auto plan = plans_.find(clock);
    if (plan == plans_.end()) return invalid_state("clock plan is no longer active");
    const auto current_tick = clock_ticks_[clock];
    const auto scheduled = plan->second.instances;
    for (const auto instance_id : scheduled) {
        if (execution_->busy(instance_id)) continue;
        if (std::find(deferred_.begin(), deferred_.end(), instance_id) != deferred_.end()) continue;
        const auto instance = instances_.find(instance_id);
        if (instance == instances_.end()) {
            return invalid_state("execution plan references a retired Feature instance");
        }
        const auto* definition = registry_.find_definition(instance->second.definition_id);
        if (definition == nullptr) {
            return invalid_state("Feature implementation is no longer registered");
        }
        if (instance->second.retry_pending) {
            if (!instance->second.processes.empty()) {
                const auto* retry_process = find_process(instance->second.processes.front());
                if (retry_process == nullptr || retry_process->status != ProcessStatus::kReady) continue;
            }
            if (instance->second.retry_due_tick > current_tick) continue;
            if (const auto result = invoke_and_complete(instance_id, {}, true); !result)
                return result;
            continue;
        }
        if (definition->contract.structure == FeatureStructure::kComposite ||
            definition->contract.execution == ExecutionKind::kCall) {
            continue;
        }
        if (!definition->execute) {
            return invalid_state("Atomic Feature has no executable implementation");
        }
        if (!instance->second.processes.empty()) {
            const auto* process_instance = find_process(instance->second.processes.front());
            if (process_instance == nullptr || process_instance->status != ProcessStatus::kReady) {
                continue;
            }
        }
        const auto expiry = transport_add(released, {definition->contract.clock.timing->relative_deadline_ns});
        if (!expiry) return expiry.result;
        release_timings_.try_emplace(instance_id, TransportTiming{released, expiry.value});
        if (prerequisites_busy(instance_id)) {
            deferred_.push_back(instance_id);
            continue;
        }
        if (const auto result = invoke_and_complete(instance_id, {}, false, true); !result)
            return result;
    }
    return settle_render_publications();
}

foundation::Result FeatureRuntime::invoke_and_complete(FeatureInstanceId instance_id,
                                                        std::span<const uint8_t> input,
                                                        bool replay_retry,
                                                        bool clock_scheduled) {
    const auto instance = instances_.find(instance_id);
    if (instance == instances_.end()) {
        return foundation::Result::failure(foundation::ResultCode::kNotFound,
                                           "feature instance was not found");
    }
    const auto* registered_definition = registry_.find_definition(instance->second.definition_id);
    if (registered_definition == nullptr || !registered_definition->execute)
        return invalid_state("Feature instance has no executable implementation");
    // A rejected dispatch must not call the host projector or disturb retry
    // input. invoke() repeats these checks immediately before issuing a token.
    if (execution_->busy(instance_id))
        return invalid_state("Feature has an in-flight invocation");
    if (!instance->second.processes.empty()) {
        const auto* process = find_process(instance->second.processes.front());
        if (process == nullptr || process->status != ProcessStatus::kReady)
            return invalid_state("feature process is not ready");
    }
    if (!replay_retry && input.empty() && prerequisites_busy(instance_id))
        return invalid_state("Feature input is waiting for an upstream owner commit");
    // A callback may trigger replacement/publication. Its executing callable
    // and contract must stay alive until this invocation returns.
    const FeatureDefinition definition = *registered_definition;
    const auto now = time_source_->read();
    if (!now) return now.result;
    auto released = release_timings_.find(instance_id);
    TransportTiming timing;
    if (released != release_timings_.end()) timing = released->second;
    else {
        const auto expiry = transport_add(now.value, {definition.contract.clock.timing->relative_deadline_ns});
        if (!expiry) return expiry.result;
        timing = {now.value, expiry.value};
    }
    release_timings_[instance_id] = timing;
    const auto gate = validate_transport_timing(timing, now.value);
    if (!gate && (gate.code != foundation::ResultCode::kTimeout ||
        definition.contract.clock.timing->deadline_policy == DeadlinePolicy::kRejectCommit)) {
        release_timings_.erase(instance_id);
        auto& retry = instances_.at(instance_id);
        retry.retry_pending = false;
        retry.retry_state.reset();
        retry.retry_origin = {};
        retry.retry_attempts = 0;
        retry.retry_input.clear();
        retry.retry_parameters.clear();
        retry.retry_messages.clear();
        retire_expired_messages(instance_id, now.value);
        return gate;
    }
    if (input.empty()) {
        const auto& messages = replay_retry ? instance->second.retry_messages : instance->second.pending_messages;
        for (const auto& message : messages) {
            const auto valid = message.source.timing ? validate_transport_timing(*message.source.timing, now.value)
                : foundation::invalid_argument("input has no host transport timing");
            if (!valid) {
                retire_expired_messages(instance_id, now.value);
                if (replay_retry) {
                    auto& retry = instances_.at(instance_id);
                    retry.retry_pending = false;
                    retry.retry_state.reset();
                    retry.retry_origin = {};
                    retry.retry_attempts = 0;
                    retry.retry_input.clear();
                    retry.retry_parameters.clear();
                    retry.retry_messages.clear();
                }
                release_timings_.erase(instance_id);
                return valid;
            }
        }
    }
    auto& retry_state = instances_.at(instance_id);
    if (retry_state.retry_pending != replay_retry)
        return invalid_state("Feature invocation is waiting for its clock-owned retry");
    const auto& effective_parameters = replay_retry ? retry_state.retry_parameters : retry_state.parameters;
    if (const auto valid = validate_parameter_assets(effective_parameters, definition.contract.parameters); !valid)
        return valid;
    // Explicit input never consumes the graph queue. Both paths receive the
    // complete parameter envelope independently of their message payload.
    std::vector<uint8_t> routed_input;
    std::vector<uint8_t> parameters;
    std::vector<PortMessage> consumed_messages;
    std::optional<StateSnapshot> snapshot;
    InvocationToken origin{};
    if (replay_retry) {
        if (!retry_state.retry_state) return invalid_state("retry state snapshot is missing");
        snapshot = std::move(retry_state.retry_state);
        origin = retry_state.retry_origin;
        retry_state.retry_state.reset();
        retry_state.retry_origin = {};
        routed_input = std::move(retry_state.retry_input);
        parameters = std::move(retry_state.retry_parameters);
        consumed_messages = std::move(retry_state.retry_messages);
        retry_state.retry_pending = false;
        input = routed_input;
    } else {
        parameters = instance->second.parameters;
        if (input.empty()) {
            routed_input = instance->second.pending_input;
            consumed_messages = instance->second.pending_messages;
        } else {
            routed_input.assign(input.begin(), input.end());
        }
        input = routed_input;
    }
    const auto parameter_records = decode_feature_parameters(parameters, definition.contract.parameters);
    if (!parameter_records) return parameter_records.result;
    const auto canonical_parameters = encode_feature_parameters(parameter_records.value, definition.contract.parameters);
    if (!canonical_parameters) return canonical_parameters.result;
    parameters = canonical_parameters.value;
    if (!replay_retry) {
        const auto bound = input_->slots.find(instance_id);
        if (bound != input_->slots.end()) {
            const auto& slot = bound->second;
            const auto lease = slot.lease.lock();
            if (!lease || slot.generation != generation_ ||
                slot.definition_digest != instance->second.definition_digest ||
                slot.definition_digest != feature_contract_digest(definition))
                return invalid_state("Feature input binding has expired or belongs to a retired owner");
            const auto port = std::find_if(definition.contract.ports.begin(), definition.contract.ports.end(),
                [&](const PortContract& candidate) { return candidate.id == slot.port; });
            if (port == definition.contract.ports.end() || port->direction != PortDirection::kInput ||
                port->schema != slot.schema || port->version != slot.version || !slot.projector)
                return invalid_state("Feature input binding no longer matches its declared input port");
            try {
                std::vector<SideEffectContract> pending_effects;
                const auto canonical = outbox_->owners.find(instance_id);
                if (canonical != outbox_->owners.end() && canonical->second->runtime == outbox_->identity &&
                    canonical->second->source.instance_id == instance_id &&
                    canonical->second->generation == generation_ &&
                    canonical->second->definition_digest == instance->second.definition_digest) {
                    for (const auto& entry : outbox_->pending) {
                        if (entry->owner != canonical->second ||
                            entry->receipt.state != detail::OutboxDeliveryState::kPending ||
                            entry->receipt.invocation.generation != generation_) continue;
                        const auto declaration = std::find_if(canonical->second->declarations.begin(),
                            canonical->second->declarations.end(), [&](const SideEffectContract& effect) {
                                return effect.id == entry->intent.effect && effect.version == entry->intent.version &&
                                    effect.kind == SideEffectKind::kCommand && effect.commit == CommitPolicy::kOutbox;
                            });
                        if (declaration == canonical->second->declarations.end())
                            return invalid_state("pending input effect has no canonical outbox declaration");
                        if (std::none_of(pending_effects.begin(), pending_effects.end(),
                            [&](const SideEffectContract& effect) {
                                return effect.id == declaration->id && effect.version == declaration->version;
                            })) pending_effects.push_back(*declaration);
                    }
                }
                const detail::FeatureInputContext context{instance_id, generation_, definition.contract.clock.id,
                    clock_scheduled && definition.contract.clock.frequency_hz != 0
                        ? 1.0 / definition.contract.clock.frequency_hz : 0.0,
                    routed_input, pending_effects};
                detail::FeatureInputProjection projection;
                {
                    FlagScope scope(state_callback_active_);
                    auto projected = slot.projector(context);
                    if (!projected) return projected.result;
                    projection = std::move(projected.value);
                }
                if (!projection.ready) {
                    if (!clock_scheduled)
                        return foundation::not_ready("Feature host input is not ready for direct dispatch");
                    if (std::find(deferred_.begin(), deferred_.end(), instance_id) == deferred_.end())
                        deferred_.push_back(instance_id);
                    return foundation::Result::success();
                }
                routed_input = std::move(projection.bytes);
                input = routed_input;
            } catch (const std::bad_alloc&) {
                return foundation::out_of_memory("Feature input projection allocation failed");
            } catch (const std::exception& error) {
                return foundation::unknown_exception(error);
            } catch (...) {
                return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                                   "Feature input projector threw an exception");
            }
        }
    }
    const auto attempt_number = retry_state.retry_attempts == 0 ? 1 : retry_state.retry_attempts;
    retry_state.retry_attempts = 0;
    const auto token = invoke(instance_id);
    if (!token) return token.result;
    if (!snapshot) {
        const auto& owner = instances_.at(instance_id);
        const auto* process = owner.processes.empty() ? nullptr : find_process(owner.processes.front());
        snapshot = state_snapshot(owner, process, definition.contract.state);
        origin = token.value;
    }
    const auto& active_owner = instances_.at(instance_id);
    const auto* active_process = active_owner.processes.empty() ? nullptr : find_process(active_owner.processes.front());
    if (token.value.generation != origin.generation || token.value.state_version != origin.state_version ||
        token.value.input_version != origin.input_version ||
        (active_process != nullptr && active_process->current_state != snapshot->current_state)) {
        const auto result = invalid_state("retry state snapshot belongs to a retired state or Process revision");
        record_invocation_failure(instance_id, {token.value, invocations_.at(token.value.invocation_id).process},
                                  InvocationFailureStage::kSubmission, result);
        invocations_.erase(token.value.invocation_id);
        return result;
    }
    invocations_.at(token.value.invocation_id).consumed_messages = consumed_messages;
    invocations_.at(token.value.invocation_id).timing = timing;
    release_timings_.erase(instance_id);
    auto work = std::make_shared<FeatureInvocationWork>();
    work->instance = instance_id;
    work->definition = definition;
    work->token = token.value;
    work->origin = origin;
    work->state = std::move(*snapshot);
    work->input = std::move(routed_input);
    work->parameters = std::move(parameters);
    work->messages = std::move(consumed_messages);
    work->attempt = attempt_number;
    work->time_source = time_source_;
    work->timing = timing;
    if (async_execution_) {
        const auto result = execution_->submit(std::move(work));
        if (!result) {
            record_invocation_failure(instance_id, {token.value, invocations_.at(token.value.invocation_id).process},
                                      InvocationFailureStage::kSubmission, result);
            invocations_.erase(token.value.invocation_id);
        }
        return result;
    }
    // Inline and worker execution share exactly the same callback/finish path.
    for (;;) {
        work->execute();
        const auto result = finish_invocation(*work);
        if (!result || !instances_.at(instance_id).retry_pending || definition.contract.retry.backoff_ticks != 0)
            return result;
        auto& retry = instances_.at(instance_id);
        if (const auto valid = validate_parameter_assets(retry.retry_parameters, definition.contract.parameters); !valid)
            return valid;
        retry.retry_pending = false;
        retry.retry_state.reset();
        retry.retry_origin = {};
        retry.retry_input.clear();
        retry.retry_parameters.clear();
        retry.retry_messages.clear();
        work->attempt = std::exchange(retry.retry_attempts, 0);
        const auto next = invoke(instance_id);
        if (!next) return next.result;
        work->token = next.value;
        work->result.reset();
        invocations_.at(next.value.invocation_id).consumed_messages = work->messages;
        invocations_.at(next.value.invocation_id).timing = timing;
        release_timings_.erase(instance_id);
    }
}

foundation::Result FeatureRuntime::finish_invocation(const FeatureInvocationWork& work) {
    const auto owner_for_timing = instances_.find(work.instance);
    if (owner_for_timing != instances_.end() && work.token.generation == generation_) {
        const auto observed = time_source_->read();
        const auto late = observed && work.timing.expires_at &&
            observed.value.nanoseconds > work.timing.expires_at->nanoseconds;
        owner_for_timing->second.last_execution_timing = InvocationTimingObservation{
            work.timing, work.callback_started, work.callback_finished, late,
            work.callback_started && work.callback_finished &&
                work.callback_finished->nanoseconds - work.callback_started->nanoseconds > work.definition.contract.clock.work_budget_ns};
    }
    if (*work.result) {
        const auto result = complete(work.token, work.result->value);
        if (!result) invocations_.erase(work.token.invocation_id);
        return result;
    }
    const auto record = invocations_.find(work.token.invocation_id);
    if (record != invocations_.end())
        record_invocation_failure(work.instance, {work.token, record->second.process},
                                  InvocationFailureStage::kCallback, work.result->result);
    invocations_.erase(work.token.invocation_id);
    const auto observed = time_source_->read();
    if (!observed) return observed.result;
    retire_expired_messages(work.instance, observed.value);
    const auto& contract = work.definition.contract;
    const auto max_attempts = std::max<uint32_t>(2, contract.retry.max_attempts);
    if (work.result->result.code == foundation::ResultCode::kTimeout ||
        contract.failure != FailurePolicy::kRetry || work.attempt >= max_attempts)
        return work.result->result;
    const auto owner = instances_.find(work.instance);
    const auto* process = owner == instances_.end() || owner->second.processes.empty()
                              ? nullptr : find_process(owner->second.processes.front());
    if (generation_ != work.origin.generation || owner == instances_.end() ||
        !state_matches(work.token, owner->second, process, contract.state))
        return invalid_state("failed invocation state changed before retry");
    auto& retry = owner->second;
    retry.retry_pending = true;
    retry.retry_attempts = work.attempt + 1;
    retry.retry_input = work.input;
    retry.retry_parameters = work.parameters;
    retry.retry_messages = work.messages;
    retry.retry_state = work.state;
    retry.retry_origin = work.origin;
    release_timings_[work.instance] = work.timing;
    if (clock_ticks_[contract.clock.id] > (std::numeric_limits<uint64_t>::max)() - contract.retry.backoff_ticks) {
        retry.retry_pending = false;
        release_timings_.erase(work.instance);
        return foundation::invalid_argument("retry backoff overflows the local Clock counter");
    }
    retry.retry_due_tick = clock_ticks_[contract.clock.id] + contract.retry.backoff_ticks;
    return foundation::Result::success();
}

foundation::Result FeatureRuntime::advance(double delta_seconds) {
    if (!on_owner_thread() || execution_closed_) return invalid_state("Runtime mutation requires its open owner thread");
    if (state_callback_active_) return invalid_state("state schema callbacks cannot mutate Runtime");
    if (!std::isfinite(delta_seconds) || delta_seconds < 0.0)
        return foundation::invalid_argument("Feature clock delta must be finite and nonnegative");
    // Convert at the API boundary, carrying the sub-nanosecond fraction between calls.
    // All release comparisons below are integer/rational and use no epsilon.
    const long double scaled = static_cast<long double>(delta_seconds) * 1'000'000'000.L + delta_remainder_ns_;
    // A carried -0.5 ns belongs to the preceding rounded instant. A zero
    // delta cannot turn that tie into a negative logical step.
    const long double rounded = scaled <= 0 ? 0 : std::round(scaled);
    if (rounded < 0 || rounded >= static_cast<long double>((std::numeric_limits<uint64_t>::max)()))
        return foundation::invalid_argument("Feature logical time delta overflows nanoseconds");
    const auto delta_ns = static_cast<uint64_t>(rounded);
    for (const auto clock : clocks()) {
        const auto found = clock_elapsed_.find(clock);
        const auto elapsed = found == clock_elapsed_.end() ? 0 : found->second.elapsed_ns;
        if (delta_ns > (std::numeric_limits<uint64_t>::max)() - elapsed)
            return foundation::invalid_argument("Feature logical elapsed time overflows nanoseconds");
    }
    if (const auto result = pump_completions(); !result) return result;
    delta_remainder_ns_ = scaled - rounded;
    for (const auto clock : clocks()) {
        const auto& plan = plans_.at(clock);
        if (plan.instances.empty()) continue;
        const auto* definition = registry_.find_definition(instances_.at(plan.instances.front()).definition_id);
        if (definition == nullptr) return invalid_state("Feature clock owner is no longer registered");
        const auto& contract = definition->contract.clock;
        auto& schedule = clock_elapsed_[clock];
        schedule.elapsed_ns += delta_ns;
        const auto next_release = [&]() -> foundation::ResultValue<uint64_t> {
            const auto seconds = schedule.next_period / contract.frequency_hz;
            const auto fraction = schedule.next_period % contract.frequency_hz * 1'000'000'000;
            const auto tail = fraction / contract.frequency_hz +
                (fraction % contract.frequency_hz * 2 >= contract.frequency_hz);
            const auto phase = contract.timing->phase_ns;
            const auto maximum = (std::numeric_limits<uint64_t>::max)();
            if (seconds > (maximum - phase) / 1'000'000'000 ||
                tail > maximum - phase - seconds * 1'000'000'000)
                return {foundation::invalid_argument("Feature phase release overflows nanoseconds"), {}};
            return {foundation::Result::success(), seconds * 1'000'000'000 + tail + phase};
        };
        uint32_t count = 0;
        for (;;) {
            const auto due = next_release();
            if (!due) return due.result;
            if (schedule.elapsed_ns < due.value) break;
            if (count == contract.max_backlog) {
                if (contract.late_data_policy == LateDataPolicy::kReject)
                    return invalid_state("Feature clock backlog exceeded its declared budget");
                if (contract.late_data_policy == LateDataPolicy::kDrop) {
                    const auto unphased = schedule.elapsed_ns - contract.timing->phase_ns;
                    // round(n * 1e9 / Hz) <= T iff n * 1e9 / Hz < T + 0.5.
                    // Split whole seconds before multiplying to avoid overflow.
                    const auto whole = unphased / 1'000'000'000;
                    const auto tail = ((unphased % 1'000'000'000 * 2 + 1) * contract.frequency_hz - 1) / 2'000'000'000;
                    if (whole > ((std::numeric_limits<uint64_t>::max)() - tail - 1) / contract.frequency_hz)
                        return foundation::invalid_argument("Feature dropped period count overflows");
                    schedule.next_period = whole * contract.frequency_hz + tail + 1;
                }
                break;
            }
            const auto released = time_source_->read();
            if (!released) return released.result;
            ++schedule.next_period;
            ++count;
            const auto generation = generation_;
            if (const auto result = release_clock(clock, released.value); !result) return result;
            if (generation_ != generation) return foundation::Result::success();
        }
    }
    return foundation::Result::success();
}

foundation::Result FeatureRuntime::publish_graph(const FeatureGraphDocument& graph,
                                                 CompiledFeatureGraph& compiled) {
    if (!on_owner_thread() || execution_closed_) return invalid_state("Runtime mutation requires its open owner thread");
    if (state_callback_active_) return invalid_state("state schema callbacks cannot mutate Runtime");
    if (!outbox_->pending.empty())
        return foundation::not_ready("pending outbox commands must be applied or rejected before graph publication");
    FeatureRuntime candidate(registry_, asset_kind_resolver_);
    candidate.time_source_ = time_source_;
    candidate.next_instance_ = next_instance_;
    candidate.next_process_ = next_process_;
    candidate.next_invocation_ = next_invocation_;
    candidate.next_attempt_ = next_attempt_;
    std::unordered_map<FeatureInstanceId, FeatureInstanceId,
                       StableIdHash<FeatureInstanceIdTag>> node_to_instance;
    for (const auto& node : graph.nodes) {
        auto owner_path = node.owner_path;
        owner_path.nodes.push_back(node.node_id);
        const auto expanded = registry_.expand(node.definition_id, owner_path);
        if (!expanded) return expanded.result;
        const auto* definition = registry_.find_definition(node.definition_id);
        if (definition == nullptr)
            return foundation::not_found("Feature definition was not found during graph publication");
        if (const auto result = validate_feature_parameter_blob(node.parameter_blob,
                                                                definition->contract.parameters);
            !result) {
            return result;
        }
        const auto activated = candidate.activate(expanded.value, expanded.value.contract.scope.id);
        if (!activated) return activated.result;
        candidate.instances_.at(activated.value).parameters = node.parameter_blob;
        node_to_instance.emplace(FeatureInstanceId(node.node_id.value()), activated.value);
        // Copy actual publication provenance for every domain and descendant.
        // The caller's exported CompiledFeatureGraph is diagnostic data after
        // publication and must never become consumption authority.
        const auto publish_outbox_owner = [&](const auto& self, FeatureInstanceId id) -> foundation::Result {
            const auto& instance = candidate.instances_.at(id);
            const auto* live = registry_.find_definition(instance.definition_id);
            if (!live || instance.definition_digest != feature_contract_digest(*live))
                return invalid_state("outbox publication owner has a stale definition binding");
            if (const auto valid = validate_parameter_assets(instance.parameters, live->contract.parameters); !valid)
                return valid;
            auto owner = std::make_shared<detail::FeatureOutboxOwner>();
            owner->runtime = candidate.outbox_->identity;
            owner->generation = candidate.generation_;
            owner->definition_digest = instance.definition_digest;
            owner->declarations = live->contract.side_effects;
            owner->source.definition_id = instance.definition_id;
            owner->source.node_id = node.node_id;
            owner->source.instance_id = id;
            owner->source.process_id = instance.processes.empty() ? ProcessId{} : instance.processes.front();
            owner->source.owner_path = instance.owner_path;
            const auto root_index = node.owner_path.nodes.size();
            if (root_index >= owner->source.owner_path.nodes.size() ||
                owner->source.owner_path.nodes[root_index] != node.node_id)
                return invalid_state("outbox publication owner path does not match its actual graph node");
            // Root diagnostics use the authored parent path. A descendant
            // retains its full expanded ancestry beneath that authored node.
            if (id == activated.value)
                owner->source.owner_path.nodes.erase(owner->source.owner_path.nodes.begin() +
                                                    static_cast<std::ptrdiff_t>(root_index));
            owner->source.clock = live->contract.clock.id;
            owner->source.domains = live->contract.domains;
            owner->source.owner_module = instance.owner_module;
            candidate.outbox_->owners.emplace(id, std::move(owner));
            for (const auto child : instance.children)
                if (const auto result = self(self, child); !result) return result;
            return foundation::Result::success();
        };
        if (const auto result = publish_outbox_owner(publish_outbox_owner, activated.value); !result) return result;
        for (auto& provenance : compiled.provenance) {
            if (provenance.node_id != node.node_id) continue;
            provenance.instance_id = activated.value;
            const auto process = candidate.process(activated.value);
            provenance.process_id = process ? process.value : ProcessId{};
        }
    }
    // Build the runtime transport table from the authored graph.  The
    // compiler validates the edge schemas and clocks; publication resolves
    // node identities to the freshly activated instances so a generation can
    // never deliver data into an instance from an older graph.
    for (const auto& edge : graph.edges) {
        const auto source = node_to_instance.find(FeatureInstanceId(edge.source_node.value()));
        const auto target = node_to_instance.find(FeatureInstanceId(edge.target_node.value()));
        if (source == node_to_instance.end() || target == node_to_instance.end()) {
            return invalid_state("FeatureGraph transport references an inactive instance");
        }
        const auto* source_definition = registry_.find_definition(
            candidate.instances_.at(source->second).definition_id);
        const auto* target_definition = registry_.find_definition(
            candidate.instances_.at(target->second).definition_id);
        if (source_definition == nullptr || target_definition == nullptr)
            return invalid_state("FeatureGraph transport references an inactive definition");
        const auto source_port = std::find_if(
            source_definition->contract.ports.begin(), source_definition->contract.ports.end(),
            [&edge](const PortContract& port) { return port.id == edge.source_port; });
        const auto target_port = std::find_if(
            target_definition->contract.ports.begin(), target_definition->contract.ports.end(),
            [&edge](const PortContract& port) { return port.id == edge.target_port; });
        if (source_port == source_definition->contract.ports.end() ||
            target_port == target_definition->contract.ports.end())
            return invalid_state("FeatureGraph transport references an undeclared port");
        // A composite input boundary can explicitly map one parent port to
        // multiple child definitions.  Keep every mapped endpoint so a
        // message is delivered to the declared fan-out set.  If there is no
        // mapping for this parent port, retain the direct parent route for
        // composite implementations that consume the boundary themselves.
        struct RoutedTarget final {
            FeatureInstanceId instance{};
            const PortContract* port{};
        };
        std::vector<RoutedTarget> routed_targets;
        if (target_definition->contract.structure == FeatureStructure::kComposite) {
            const auto& target_instance = candidate.instances_.at(target->second);
            bool has_mapping = false;
            for (const auto& mapping : target_definition->contract.child_port_mappings) {
                if (mapping.direction != PortDirection::kInput || mapping.parent_port != edge.target_port)
                    continue;
                has_mapping = true;
                const auto* child_definition = registry_.find_definition(mapping.child_definition);
                if (child_definition == nullptr)
                    return invalid_state("composite input mapping references an inactive child definition");
                const auto child_port = std::find_if(
                    child_definition->contract.ports.begin(), child_definition->contract.ports.end(),
                    [&mapping](const PortContract& port) {
                        return port.id == mapping.child_port && port.direction == mapping.direction;
                    });
                if (child_port == child_definition->contract.ports.end())
                    return invalid_state("composite input mapping references an undeclared child port");
                if (child_port->schema != source_port->schema || child_port->version != source_port->version)
                    return invalid_state("composite input mapping schema does not match parent port");

                const auto child_instance = std::find_if(
                    target_instance.children.begin(), target_instance.children.end(),
                    [&](FeatureInstanceId id) {
                        const auto& instance = candidate.instances_.at(id);
                        return instance.definition_id == mapping.child_definition;
                    });
                if (child_instance == target_instance.children.end())
                    return invalid_state("composite input mapping references an inactive child instance");
                routed_targets.push_back({*child_instance, &*child_port});
            }
            if (!has_mapping) routed_targets.push_back({target->second, &*target_port});
        } else {
            routed_targets.push_back({target->second, &*target_port});
        }
        if (routed_targets.empty())
            return invalid_state("composite input mapping has no routed child endpoint");
        for (const auto& routed_target : routed_targets) {
            candidate.routes_[source->second].push_back(
                {edge.source_port, routed_target.port->id, source_port->schema, source_port->version,
                 routed_target.instance, edge.kind, edge.capacity, edge.deadline_ticks});
            if (auto result = candidate.resource_bus_.register_route(
                    source->second,
                    PortRoute{routed_target.instance, edge.source_port, routed_target.port->id, edge.kind,
                              edge.capacity, edge.deadline_ticks, routed_target.port->version,
                              source_port->schema, routed_target.port->schema, true,
                              source_definition->contract.clock.frequency_hz});
                !result)
                return result;
        }

        // Composite boundaries are explicit: an authored parent edge is
        // projected to every mapped child endpoint. The direct parent route
        // remains available when the target boundary has no mapping, while
        // child projection handles ordinary composites.
        const auto& source_instance = candidate.instances_.at(source->second);
        const auto project_output = [&](FeatureInstanceId child_id,
                                        const FeatureDefinitionId child_definition) -> foundation::Result {
            if (source_port->direction != PortDirection::kOutput) return foundation::Result::success();
            for (const auto& mapping : source_definition->contract.child_port_mappings) {
                if (mapping.direction != PortDirection::kOutput || mapping.parent_port != edge.source_port ||
                    mapping.child_definition != child_definition) continue;
                const auto* child_def = registry_.find_definition(child_definition);
                if (child_def == nullptr) continue;
                const auto child_port = std::find_if(
                    child_def->contract.ports.begin(), child_def->contract.ports.end(),
                    [&mapping](const PortContract& port) {
                        return port.id == mapping.child_port && port.direction == mapping.direction;
                    });
                if (child_port == child_def->contract.ports.end()) continue;
                if (child_port->schema != source_port->schema || child_port->version != source_port->version)
                    return invalid_state("composite output mapping schema does not match parent port");
                for (const auto& routed_target : routed_targets) {
                    candidate.routes_[child_id].push_back(
                        {mapping.child_port, routed_target.port->id, child_port->schema,
                         child_port->version, routed_target.instance, edge.kind, edge.capacity,
                         edge.deadline_ticks});
                    if (auto result = candidate.resource_bus_.register_route(
                            child_id,
                            PortRoute{routed_target.instance, mapping.child_port, routed_target.port->id,
                                      edge.kind, edge.capacity, edge.deadline_ticks,
                                      routed_target.port->version, child_port->schema,
                                      routed_target.port->schema, true,
                                      source_definition->contract.clock.frequency_hz});
                        !result)
                        return result;
                }
            }
            return foundation::Result::success();
        };
        if (source_definition->contract.structure == FeatureStructure::kComposite) {
            for (const auto child_id : source_instance.children) {
                const auto& child_instance = candidate.instances_.at(child_id);
                if (const auto result = project_output(child_id, child_instance.definition_id); !result)
                    return result;
            }
        }
    }
    for (auto& plan : compiled.plans) {
        for (auto& instance : plan.instances) {
            const auto mapped = node_to_instance.find(instance);
            if (mapped != node_to_instance.end()) instance = mapped->second;
        }
        for (const auto instance_id : plan.instances) {
            const auto instance = candidate.instances_.find(instance_id);
            if (instance == candidate.instances_.end() ||
                registry_.find_definition(instance->second.definition_id) == nullptr) {
                return invalid_state("compiled FeatureGraph plan references an inactive instance");
            }
            const auto definition = registry_.find_definition(instance->second.definition_id);
            if (definition->contract.clock.id != plan.clock) {
                return invalid_state("compiled FeatureGraph plan clock does not match its Feature instance");
            }
        }
    }
    // Expand each authored root in compiler order into the clocks owned by
    // its descendants. Composite instances remain state owners, while Atomic
    // descendants retain their own scopes, clocks and execution identities.
    std::unordered_map<ClockId, ExecutionPlan, StableIdHash<ClockIdTag>> ordered_plans;
    std::unordered_set<FeatureInstanceId, StableIdHash<FeatureInstanceIdTag>> emitted_instances;
    std::function<void(FeatureInstanceId)> append_instance = [&](FeatureInstanceId id) {
        if (!emitted_instances.insert(id).second) return;
        const auto& instance = candidate.instances_.at(id);
        const auto* definition = registry_.find_definition(instance.definition_id);
        auto& plan = ordered_plans[definition->contract.clock.id];
        plan.clock = definition->contract.clock.id;
        plan.instances.push_back(id);
        for (const auto child : instance.children) append_instance(child);
    };
    for (const auto& plan : compiled.plans)
        for (const auto instance : plan.instances) append_instance(instance);
    for (const auto& process_graph : graph.process_subgraphs) {
        const auto mapped = node_to_instance.find(FeatureInstanceId(process_graph.owner_node.value()));
        if (mapped == node_to_instance.end()) {
            return invalid_state("process subgraph owner is not an active Feature instance");
        }
        const auto process_id = candidate.process(mapped->second);
        if (!process_id) {
            return invalid_state("process subgraph owner does not expose a Process instance");
        }
        auto process = candidate.processes_.find(process_id.value);
        if (process == candidate.processes_.end()) {
            return invalid_state("process subgraph process instance is missing");
        }
        const auto owner_instance = candidate.instances_.find(process->second.owner);
        const auto* owner_definition = owner_instance == candidate.instances_.end()
                                           ? nullptr
                                           : registry_.find_definition(owner_instance->second.definition_id);
        if (owner_definition == nullptr)
            return invalid_state("process subgraph owner definition is missing");
        const auto machine = canonicalize_process_subgraph(process_graph, owner_definition->contract);
        if (!machine)
            return machine.result;
        process->second.states = process_graph.states;
        process->second.transitions = process_graph.transitions;
        process->second.state_schema_version = process_graph.state_schema_version;
        process->second.initial_state = machine.value.initial_state;
        process->second.terminal_states.clear();
        for (const auto& state : machine.value.states)
            if (state.is_terminal) process->second.terminal_states.push_back(state.name);
        process->second.current_state = process->second.initial_state;
    }
    // Capture the expanded leaf plan before publishing the candidate. The
    // compiler plan still names authored roots; Composite render descendants
    // own their resources and must retain the root's author-node provenance.
    auto render_capture = compiled;
    render_capture.plans.clear();
    for (const auto& [clock, plan] : ordered_plans) render_capture.plans.push_back(plan);
    const auto render_owners = candidate.capture_render_resource_owners(render_capture);
    if (!render_owners) return render_owners.result;
    std::unordered_map<FeatureInstanceId, FeatureNodeId, StableIdHash<FeatureInstanceIdTag>> render_nodes;
    for (const auto& owner : render_owners.value) render_nodes.emplace(owner.instance_id, owner.node_id);
    std::vector<detail::ClockWorkDemand> workload;
    if (const auto valid = candidate.collect_clock_workload(workload); !valid) return valid;
    if (const auto valid = detail::validate_clock_workload(workload, registry_); !valid) return valid;
    execution_->cancel_all();
    input_->slots.clear();
    deferred_.clear();
    execution_backlog_.clear();
    render_publications_.clear();
    render_dirty_.clear();
    render_nodes_ = std::move(render_nodes);
    instances_ = std::move(candidate.instances_);
    processes_ = std::move(candidate.processes_);
    routes_ = std::move(candidate.routes_);
    resource_bus_ = std::move(candidate.resource_bus_);
    outbox_ = std::move(candidate.outbox_);
    // The compiler owns execution order.  `activate()` only creates instances
    // and its incidental registration order must never replace the compiled
    // topological plan used by the runtime.
    plans_ = std::move(ordered_plans);
    clock_elapsed_.clear();
    delta_remainder_ns_ = 0;
    release_timings_.clear();
    clock_ticks_.clear();
    compiled.plans.clear();
    for (const auto clock : clocks()) compiled.plans.push_back(plans_.at(clock));
    invocations_.clear();
    next_instance_ = candidate.next_instance_;
    next_process_ = candidate.next_process_;
    next_invocation_ = candidate.next_invocation_;
    next_attempt_ = candidate.next_attempt_;
    generation_ = candidate.generation_;
    return foundation::Result::success();
}

std::vector<ClockId> FeatureRuntime::clocks() const {
    if (!on_owner_thread()) return {};
    std::vector<ClockId> result;
    result.reserve(plans_.size());
    for (const auto& [clock, plan] : plans_) {
        static_cast<void>(plan);
        result.push_back(clock);
    }
    std::ranges::sort(result);
    return result;
}

ProcessInstance* FeatureRuntime::mutable_process(ProcessId id) noexcept {
    if (!on_owner_thread()) return nullptr;
    const auto it = processes_.find(id);
    return it == processes_.end() ? nullptr : &it->second;
}

bool FeatureRuntime::children_completed(ProcessId id) const noexcept {
    const auto process = processes_.find(id);
    if (process == processes_.end()) return false;
    return std::ranges::all_of(process->second.children,
                               [this](const ProcessId child_id) {
                                   const auto child = processes_.find(child_id);
                                   return child != processes_.end() && child->second.status == ProcessStatus::kCompleted;
                               });
}

void FeatureRuntime::clear_process_retry(ProcessId id) noexcept {
    const auto process = processes_.find(id);
    if (process == processes_.end()) return;
    if (auto owner = instances_.find(process->second.owner); owner != instances_.end()) {
        release_timings_.erase(owner->first);
        owner->second.retry_pending = false;
        owner->second.retry_attempts = 0;
        owner->second.retry_input.clear();
        owner->second.retry_parameters.clear();
        owner->second.retry_messages.clear();
        owner->second.retry_state.reset();
        owner->second.retry_origin = {};
    }
}

void FeatureRuntime::cascade_process_status(ProcessId id, ProcessStatus status) noexcept {
    const auto process = processes_.find(id);
    if (process == processes_.end()) return;
    const auto children = process->second.children;
    for (const auto child_id : children) {
        const auto child = processes_.find(child_id);
        if (child == processes_.end()) continue;
        if (status == ProcessStatus::kSuspended) {
            if (child->second.status == ProcessStatus::kReady) {
                child->second.status = status;
                child->second.revision = ProcessRevision(child->second.revision.value() + 1);
            }
        } else if (status == ProcessStatus::kReady) {
            if (child->second.status == ProcessStatus::kSuspended) {
                child->second.status = status;
                child->second.revision = ProcessRevision(child->second.revision.value() + 1);
            }
        } else if (status == ProcessStatus::kCancelled) {
            if (child->second.status != ProcessStatus::kCompleted &&
                child->second.status != ProcessStatus::kCancelled) {
                child->second.status = status;
                child->second.revision = ProcessRevision(child->second.revision.value() + 1);
                clear_process_retry(child_id);
            }
        }
        cascade_process_status(child_id, status);
    }
}

foundation::Result FeatureRuntime::suspend(ProcessId id) {
    if (!on_owner_thread() || execution_closed_) return invalid_state("Runtime mutation requires its open owner thread");
    if (state_callback_active_) return invalid_state("state schema callbacks cannot mutate Runtime");
    auto* process_instance = mutable_process(id);
    if (process_instance == nullptr || process_instance->status != ProcessStatus::kReady) {
        return invalid_state("process cannot be suspended");
    }
    process_instance->status = ProcessStatus::kSuspended;
    process_instance->revision = ProcessRevision(process_instance->revision.value() + 1);
    cascade_process_status(id, ProcessStatus::kSuspended);
    return foundation::Result::success();
}

foundation::Result FeatureRuntime::resume(ProcessId id) {
    if (!on_owner_thread() || execution_closed_) return invalid_state("Runtime mutation requires its open owner thread");
    if (state_callback_active_) return invalid_state("state schema callbacks cannot mutate Runtime");
    auto* process_instance = mutable_process(id);
    if (process_instance == nullptr || process_instance->status != ProcessStatus::kSuspended) {
        return invalid_state("process cannot be resumed");
    }
    process_instance->status = ProcessStatus::kReady;
    process_instance->revision = ProcessRevision(process_instance->revision.value() + 1);
    cascade_process_status(id, ProcessStatus::kReady);
    return foundation::Result::success();
}

foundation::Result FeatureRuntime::cancel(ProcessId id) {
    if (!on_owner_thread() || execution_closed_) return invalid_state("Runtime mutation requires its open owner thread");
    if (state_callback_active_) return invalid_state("state schema callbacks cannot mutate Runtime");
    auto* process_instance = mutable_process(id);
    if (process_instance == nullptr || process_instance->status == ProcessStatus::kCompleted ||
        process_instance->status == ProcessStatus::kCancelled) {
        return invalid_state("process cannot be cancelled");
    }
    process_instance->status = ProcessStatus::kCancelled;
    process_instance->revision = ProcessRevision(process_instance->revision.value() + 1);
    clear_process_retry(id);
    cascade_process_status(id, ProcessStatus::kCancelled);
    for (const auto& [process_id, value] : processes_) {
        if (value.status != ProcessStatus::kCancelled) continue;
        execution_->cancel(value.owner);
        discard_dependents(value.owner);
        instances_.at(value.owner).render_output.reset();
        ++instances_.at(value.owner).render_resource_epoch;
    }
    return foundation::Result::success();
}

foundation::Result FeatureRuntime::validate_invocation(const InvocationRecord& record,
                                                         const InvocationToken& token) const {
    if (execution_closed_ || token.generation != generation_ || record.token != token)
        return invalid_state("invocation belongs to a retired runtime generation or attempt");
    const auto instance = instances_.find(record.instance);
    if (instance == instances_.end()) return invalid_state("invocation owner instance is gone");
    const auto* definition = registry_.find_definition(instance->second.definition_id);
    if (!definition) return invalid_state("invocation owner definition is gone");
    if (instance->second.definition_digest != feature_contract_digest(*definition))
        return invalid_state("active Feature contract changed without Runtime migration or graph publication");
    const auto* process = record.process.is_valid() ? find_process(record.process) : nullptr;
    if ((record.process.is_valid() && (!process || process->owner != record.instance)) ||
        !state_matches(token, instance->second, process, definition->contract.state))
        return invalid_state("invocation state version or Process revision is stale");
    return foundation::Result::success();
}

void FeatureRuntime::record_invocation_failure(FeatureInstanceId instance_id, const InvocationAttempt& attempt,
                                               InvocationFailureStage stage, const foundation::Result& result) {
    const auto owner = instances_.find(instance_id);
    if (result || owner == instances_.end() || attempt.token.generation != generation_) return;
    if (stage == InvocationFailureStage::kPublication) {
        if (!owner->second.last_invocation_committed ||
            owner->second.last_invocation_committed->attempt.token != attempt.token) return;
    } else {
        const auto record = invocations_.find(attempt.token.invocation_id);
        if (record == invocations_.end() || record->second.instance != instance_id ||
            record->second.process != attempt.process || !validate_invocation(record->second, attempt.token)) return;
    }
    owner->second.last_invocation_failed = InvocationFailure{attempt, stage, result};
}

foundation::ResultValue<InvocationSnapshot> FeatureRuntime::inspect_invocations(FeatureInstanceId instance_id) const {
    if (!on_owner_thread()) return {invalid_state("Runtime read requires its owner thread"), {}};
    const auto instance = instances_.find(instance_id);
    if (instance == instances_.end()) return {foundation::not_found("feature instance was not found"), {}};
    InvocationSnapshot snapshot{};
    snapshot.last_issued = instance->second.last_invocation_issued;
    snapshot.last_failed = instance->second.last_invocation_failed;
    snapshot.last_committed = instance->second.last_invocation_committed;
    snapshot.timing = instance->second.last_execution_timing;
    if (instance->second.retry_pending)
        snapshot.retry = InvocationSnapshot::Retry{instance->second.retry_origin, instance->second.retry_attempts,
                                                    instance->second.retry_due_tick};
    for (const auto& [id, record] : invocations_) {
        if (record.instance != instance_id) continue;
        bool committable = static_cast<bool>(validate_invocation(record, record.token));
        const auto now = time_source_->read();
        if (!now) return {now.result, {}};
        const auto gate = validate_transport_timing(record.timing, now.value);
        const auto* definition = registry_.find_definition(instance->second.definition_id);
        if (!gate && (gate.code != foundation::ResultCode::kTimeout || !definition ||
            definition->contract.clock.timing->deadline_policy == DeadlinePolicy::kRejectCommit)) committable = false;
        for (const auto& message : record.consumed_messages)
            if (!message.source.timing || !validate_transport_timing(*message.source.timing, now.value)) committable = false;
        snapshot.open_attempts.push_back({{record.token, record.process}, committable});
    }
    std::sort(snapshot.open_attempts.begin(), snapshot.open_attempts.end(), [](const auto& a, const auto& b) {
        return a.attempt.token.invocation_id < b.attempt.token.invocation_id;
    });
    return {foundation::Result::success(), std::move(snapshot)};
}

foundation::Result FeatureRuntime::complete(const InvocationToken& token, const StateDelta& delta) {
    if (!on_owner_thread() || execution_closed_) return invalid_state("Runtime mutation requires its open owner thread");
    if (state_callback_active_) return invalid_state("state schema callbacks cannot mutate Runtime");
    const auto record = invocations_.find(token.invocation_id);
    if (record == invocations_.end()) return invalid_state("invocation is unknown or already completed");
    if (const auto result = validate_invocation(record->second, token); !result) return result;
    const auto instance_id = record->second.instance;
    const InvocationAttempt attempt{token, record->second.process};
    const auto result = commit_invocation(token, delta);
    if (!result) {
        const auto& committed = instances_.at(instance_id).last_invocation_committed;
        record_invocation_failure(instance_id, attempt,
            committed && committed->attempt.token == token ? InvocationFailureStage::kPublication : InvocationFailureStage::kCommit,
            result);
    }
    return result;
}

void FeatureRuntime::retire_expired_messages(FeatureInstanceId id, TransportTimePoint now) {
    const auto owner = instances_.find(id);
    if (owner == instances_.end()) return;
    auto& instance = owner->second;
    std::erase_if(instance.pending_messages, [&](const PortMessage& message) {
        if (!message.source.timing) return false;
        const auto valid = validate_transport_timing(*message.source.timing, now);
        return !valid && valid.code == foundation::ResultCode::kTimeout;
    });
    instance.pending_input.clear();
    for (const auto& message : instance.pending_messages)
        instance.pending_input.insert(instance.pending_input.end(), message.payload.begin(), message.payload.end());
    instance.pending_input_messages = static_cast<uint32_t>(instance.pending_messages.size());
}

foundation::Result FeatureRuntime::check_timing(const InvocationRecord& record) {
    const auto now = time_source_->read();
    if (!now) return now.result;
    return check_timing(record, now.value);
}

foundation::Result FeatureRuntime::check_timing(const InvocationRecord& record, TransportTimePoint now) {
    const auto* definition = registry_.find_definition(instances_.at(record.instance).definition_id);
    const auto release = validate_transport_timing(record.timing, now);
    if (!release && release.code == foundation::ResultCode::kTimeout) {
        auto& observed = instances_.at(record.instance).last_execution_timing;
        if (!observed) observed = InvocationTimingObservation{record.timing};
        observed->deadline_exceeded = true;
    }
    if (!release && (release.code != foundation::ResultCode::kTimeout ||
        definition->contract.clock.timing->deadline_policy == DeadlinePolicy::kRejectCommit)) {
        retire_expired_messages(record.instance, now);
        return release;
    }
    for (const auto& message : record.consumed_messages) {
        if (!message.source.timing) return foundation::invalid_argument("captured input has no host transport timing");
        if (const auto valid = validate_transport_timing(*message.source.timing, now); !valid) {
            retire_expired_messages(record.instance, now);
            return valid;
        }
    }
    return foundation::Result::success();
}

foundation::Result FeatureRuntime::commit_invocation(const InvocationToken& token, const StateDelta& delta) {
    const auto invocation = invocations_.find(token.invocation_id);
    if (const auto valid = check_timing(invocation->second); !valid) return valid;
    const auto instance_it = instances_.find(invocation->second.instance);
    const auto* owner_definition = registry_.find_definition(instance_it->second.definition_id);
    const auto* owner_process = invocation->second.process.is_valid() ? find_process(invocation->second.process) : nullptr;
    if (delta.expected_version.is_valid() && delta.expected_version != token.state_version)
        return invalid_state("invocation state version or Process revision is stale");
    if (delta.state_payload) {
        if (const auto result = validate_state_payload(*owner_definition, *delta.state_payload, state_callback_active_); !result) return result;
    }
    if (delta.render_intent)
        if (const auto result = validate_render_intent(*delta.render_intent, owner_definition->contract); !result) return result;
    if (delta.outbox.size() > kMaxOutboxIntentsPerCommit ||
        delta.outbox.size() > detail::kFeatureOutboxMaxPendingEntries - outbox_->pending.size())
        return foundation::invalid_argument("outbox commit exceeds the pending entry budget");
    uint64_t committed_outbox_bytes = outbox_->pending_bytes;
    for (const auto& intent : delta.outbox) {
        if (!declares_outbox(owner_definition->contract.side_effects, intent.effect, intent.version) ||
            intent.payload.size() > kMaxOutboxIntentBytes)
            return foundation::invalid_argument("outbox command protocol or payload byte limit is invalid");
        if (intent.payload.size() > detail::kFeatureOutboxMaxPendingBytes - committed_outbox_bytes)
            return foundation::invalid_argument("outbox commit exceeds the pending payload budget");
        committed_outbox_bytes += intent.payload.size();
    }
    std::vector<std::shared_ptr<detail::FeatureOutboxEntry>> staged_outbox;
    if (!delta.outbox.empty()) {
        const auto owner = outbox_->owners.find(instance_it->second.id);
        if (owner == outbox_->owners.end() || owner->second->generation != token.generation ||
            owner->second->definition_digest != instance_it->second.definition_digest)
            return invalid_state("outbox commands require an actual published Feature owner");
        try {
            staged_outbox = outbox_->pending;
            staged_outbox.reserve(staged_outbox.size() + delta.outbox.size());
            for (uint32_t index = 0; index < delta.outbox.size(); ++index) {
                auto entry = std::make_shared<detail::FeatureOutboxEntry>();
                entry->owner = owner->second;
                entry->intent = delta.outbox[index];
                entry->receipt = {token, index, detail::OutboxDeliveryState::kPending,
                                  foundation::ResultCode::kSuccess};
                staged_outbox.push_back(std::move(entry));
            }
        } catch (const std::bad_alloc&) {
            return foundation::out_of_memory("outbox commit allocation failed");
        }
    }
    std::vector<PortMessage> output_messages = delta.output_messages;
    if (output_messages.empty() && !delta.output_payload.empty()) {
        const auto route_it = routes_.find(invocation->second.instance);
        if (route_it != routes_.end()) {
            std::unordered_set<PortId, StableIdHash<PortIdTag>> output_ports;
            for (const auto& route : route_it->second) output_ports.insert(route.source_port);
            if (output_ports.size() > 1)
                return invalid_state("opaque output payload cannot select between multiple source ports");
            if (!route_it->second.empty()) {
                const auto& route = route_it->second.front();
                PortMessage message{};
                message.source_port = route.source_port;
                message.schema = route.schema;
                message.schema_version = route.schema_version;
                message.payload = delta.output_payload;
                output_messages.push_back(std::move(message));
            }
        }
    }
    // Completion is a transaction across Feature state and ResourceBus state.
    // Work on private copies until every version/transition/output check has
    // succeeded; a rejected transition must not publish its messages.
    auto staged_instances = instances_;
    auto staged_processes = processes_;
    auto staged_resource_bus = resource_bus_;
    std::unordered_map<FeatureInstanceId, std::vector<PortMessage>,
                       StableIdHash<FeatureInstanceIdTag>> staged_queues;
    for (const auto& [id, instance] : staged_instances) {
        staged_queues[id] = instance.pending_messages;
        if (id == invocation->second.instance && !invocation->second.consumed_messages.empty()) {
            std::unordered_set<MessageDeliveryId, StableIdHash<MessageDeliveryIdTag>> consumed;
            for (const auto& message : invocation->second.consumed_messages) consumed.insert(message.delivery_id);
            auto& queued = staged_queues[id];
            queued.erase(std::remove_if(queued.begin(), queued.end(), [&](const PortMessage& message) {
                return consumed.contains(message.delivery_id);
            }), queued.end());
        }
    }
    auto staged_instance = staged_instances.find(invocation->second.instance);
    if (staged_instance == staged_instances.end())
        return invalid_state("invocation owner instance is gone");
    // Multiple targeted messages can export the same operation in one commit.
    // Share that declaration, while the router still assigns distinct deliveries.
    std::unordered_map<const RenderLightCullingScene*, std::shared_ptr<const detail::RenderLightListEvidence>> render_declarations;
    const auto transport_now = time_source_->read();
    if (!transport_now) return transport_now.result;
    for (auto& message : output_messages) {
        if (!message.source_port.is_valid()) {
            return invalid_state("output message must select a source port");
        }
        message.source = {instance_it->second.id, token, instance_it->second.definition_digest,
                          instance_it->second.render_resource_epoch, owner_definition->contract.clock.id};
        message.source.timing = TransportTiming{invocation->second.timing.origin, {}};
        if (message.timestamp_ticks == 0)
            message.timestamp_ticks = clock_ticks_[owner_definition->contract.clock.id];
        if (message.schema_version == 0) {
            const auto route_it = routes_.find(invocation->second.instance);
            if (route_it != routes_.end()) {
                const auto route = std::find_if(route_it->second.begin(), route_it->second.end(),
                                                [&message](const EdgeRoute& candidate) {
                                                    return candidate.source_port == message.source_port;
                                                });
                if (route != route_it->second.end()) {
                    if (!message.schema.is_valid()) message.schema = route->schema;
                    message.schema_version = route->schema_version;
                }
            }
        }
        message.render_source.reset(); // Callback values cannot supply host evidence.
        const auto declaration = detail::light_list_export_declaration(
            delta.render_intent ? &*delta.render_intent : nullptr, message, token);
        if (!declaration) return declaration.result;
        if (declaration.value) {
            auto& evidence = render_declarations[declaration.value];
            if (!evidence) {
                const auto node = render_nodes_.find(instance_it->second.id);
                if (node == render_nodes_.end()) return invalid_state("light-list export requires graph provenance");
                auto stamped = std::make_shared<detail::RenderLightListEvidence>();
                auto& source = stamped->source;
                source.contract = owner_definition->contract; source.owner_module = instance_it->second.owner_module;
                source.node_id = node->second; source.instance_id = instance_it->second.id;
                source.owner_path = instance_it->second.owner_path; source.invocation = token;
                source.intent.operations = {*declaration.value};
                source.resource_owner = {source.instance_id, source.node_id, instance_it->second.definition_digest, instance_it->second.render_resource_epoch};
                stamped->process_revision = owner_process ? ProcessRevision(owner_process->revision.value() + (!delta.next_state.empty() ? 1 : 0)) : ProcessRevision{};
                evidence = std::move(stamped);
            }
            message.render_source = evidence;
        }
        foundation::Result result = foundation::Result::success();
        if (message.kind == EdgeKind::kReceipt) {
            result = staged_resource_bus.send_receipt(
                invocation->second.instance, std::move(message),
                transport_now.value, staged_queues);
        } else {
            result = staged_resource_bus.send_message(
                invocation->second.instance, std::move(message),
                transport_now.value, staged_queues);
        }
        if (!result)
            return result;
    }
    if (invocation->second.process.is_valid()) {
        auto process_it = staged_processes.find(invocation->second.process);
        auto* process_instance = process_it == staged_processes.end() ? nullptr : &process_it->second;
        if (process_instance == nullptr ||
            !state_matches(token, staged_instance->second, process_instance, owner_definition->contract.state)) {
            return invalid_state("invocation state version is stale");
        }
        if (delta.process_completed && !children_completed(process_instance->id)) {
            return invalid_state("Process cannot complete while a managed child Process is unfinished");
        }
        if (!delta.next_state.empty()) {
            const auto edge = process_instance->current_state + "->" + delta.next_state;
            if (std::find(process_instance->transitions.begin(), process_instance->transitions.end(), edge) ==
                process_instance->transitions.end()) {
                return invalid_state("process transition is not declared");
            }
            process_instance->current_state = delta.next_state;
            process_instance->revision = ProcessRevision(process_instance->revision.value() + 1);
        }
        if (delta.process_completed)
            process_instance->status = ProcessStatus::kCompleted;
        else if (!delta.next_state.empty() &&
                 std::find(process_instance->terminal_states.begin(), process_instance->terminal_states.end(),
                           process_instance->current_state) != process_instance->terminal_states.end() &&
                 children_completed(process_instance->id))
            process_instance->status = ProcessStatus::kCompleted;
    } else {
        if (!delta.next_state.empty() || delta.process_completed)
            return invalid_state("non-Process Feature cannot submit a Process transition");
        if (!state_matches(token, staged_instance->second, static_cast<const ProcessInstance*>(nullptr),
                           owner_definition->contract.state))
            return invalid_state("invocation instance state version is stale");
    }
    auto* staged_process = invocation->second.process.is_valid()
                               ? &staged_processes.at(invocation->second.process) : nullptr;
    auto& state = invocation_state(staged_instance->second, staged_process, owner_definition->contract.state);
    if (delta.state_payload) state.payload = *delta.state_payload;
    state.version = StateVersion(token.state_version.value() + 1);
    state.payload_revision = delta.payload_revision;
    ++state.commit_count;
    if (delta.render_intent)
        staged_instance->second.render_output = FeatureInstance::CommittedRenderIntent{
            token, *delta.render_intent, invocation->second.consumed_messages,
            staged_process ? staged_process->revision : ProcessRevision{}};

    // Queue policy (especially Snapshot latest-value replacement and queue
    // overflow) has already been applied to staged_queues.  Commit the full
    // projected queue rather than appending a suffix, then derive the opaque
    // input view from the canonical messages.  This also preserves a freshly
    // self-routed message after consuming the previous one.
    for (auto& [target_id, queued] : staged_queues) {
        auto target = staged_instances.find(target_id);
        if (target == staged_instances.end())
            return invalid_state("invocation output target instance is gone");
        target->second.pending_messages = std::move(queued);
        target->second.pending_input.clear();
        for (const auto& message : target->second.pending_messages) {
            target->second.pending_input.insert(target->second.pending_input.end(),
                                                message.payload.begin(), message.payload.end());
        }
        target->second.pending_input_messages =
            static_cast<uint32_t>(target->second.pending_messages.size());
    }

    staged_instance->second.last_invocation_committed = InvocationCommit{
        {token, invocation->second.process}, state.version, staged_process ? staged_process->revision : ProcessRevision{}};
    if (const auto valid = check_timing(invocation->second); !valid) return valid;
    const auto final_now = time_source_->read();
    if (!final_now) return final_now.result;
    if (const auto valid = check_timing(invocation->second, final_now.value); !valid) return valid;
    for (const auto& [id, owner] : staged_instances) for (const auto& message : owner.pending_messages) {
        if (message.source.invocation != token || !message.source.timing) continue;
        if (const auto valid = validate_transport_timing(*message.source.timing, final_now.value); !valid) return valid;
    }
    // check_timing may record Observe diagnostics after staging copied the owner.
    staged_instance->second.last_execution_timing = instance_it->second.last_execution_timing;
    instances_ = std::move(staged_instances);
    processes_ = std::move(staged_processes);
    resource_bus_ = std::move(staged_resource_bus);
    if (!delta.outbox.empty()) {
        outbox_->pending.swap(staged_outbox);
        outbox_->pending_bytes = committed_outbox_bytes;
    }
    invocations_.erase(invocation);
    render_dirty_.insert(owner_definition->contract.clock.id);
    return render_boundary_active_ ? foundation::Result::success() : settle_render_publications();
}

foundation::Result FeatureRuntime::transition(ProcessId id, std::string_view next_state) {
    if (!on_owner_thread() || execution_closed_) return invalid_state("Runtime mutation requires its open owner thread");
    if (state_callback_active_) return invalid_state("state schema callbacks cannot mutate Runtime");
    auto* process_instance = mutable_process(id);
    if (process_instance == nullptr || process_instance->status != ProcessStatus::kReady ||
        process_instance->states.empty() || next_state.empty()) {
        return invalid_state("process transition is not available");
    }
    if (std::find(process_instance->states.begin(), process_instance->states.end(), next_state) ==
        process_instance->states.end()) {
        return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                           "process transition target is not declared");
    }
    if (process_instance->current_state == next_state)
        return foundation::Result::success();
    const auto edge = process_instance->current_state + "->" + std::string(next_state);
    if (std::find(process_instance->transitions.begin(), process_instance->transitions.end(), edge) ==
        process_instance->transitions.end())
        return invalid_state("process transition is not declared");
    auto owner = instances_.find(process_instance->owner);
    const auto* definition = owner == instances_.end() ? nullptr : registry_.find_definition(owner->second.definition_id);
    if (definition == nullptr) return invalid_state("Process state owner is no longer registered");
    process_instance->current_state = std::string(next_state);
    process_instance->revision = ProcessRevision(process_instance->revision.value() + 1);
    auto& state = invocation_state(owner->second, process_instance, definition->contract.state);
    state.version = StateVersion(state.version.value() + 1);
    ++state.commit_count;
    if (std::find(process_instance->terminal_states.begin(), process_instance->terminal_states.end(),
                  process_instance->current_state) != process_instance->terminal_states.end() &&
        children_completed(process_instance->id))
        process_instance->status = ProcessStatus::kCompleted;

    // Explicit owner transitions invalidate the captured state of both
    // queued work and retry backoff, including transitions into terminal states.
    clear_process_retry(id);
    execution_->cancel(process_instance->owner);
    discard_dependents(process_instance->owner);
    owner->second.render_output.reset();
    ++owner->second.render_resource_epoch;
    return foundation::Result::success();
}

foundation::ResultValue<InvocationToken> FeatureRuntime::retry(ProcessId id) {
    if (!on_owner_thread() || execution_closed_) return {invalid_state("Runtime mutation requires its open owner thread"), {}};
    if (state_callback_active_) return {invalid_state("state schema callbacks cannot mutate Runtime"), {}};
    auto* process_instance = mutable_process(id);
    if (process_instance == nullptr || process_instance->status == ProcessStatus::kCancelled ||
        process_instance->status == ProcessStatus::kCompleted) {
        return {invalid_state("process cannot be retried"), {}};
    }
    if (execution_->busy(process_instance->owner))
        return {invalid_state("Process has an in-flight invocation"), {}};
    if (const auto owner = instances_.find(process_instance->owner);
        owner != instances_.end() && owner->second.retry_pending)
        return {invalid_state("Process invocation is waiting for its clock-owned retry"), {}};
    process_instance->status = ProcessStatus::kReady;
    process_instance->revision = ProcessRevision(process_instance->revision.value() + 1);
    clear_process_retry(id);
    return invoke(process_instance->owner);
}

foundation::Result FeatureRuntime::replace_definition(const FeatureDefinition& definition,
                                                       const StateMigration& migration) {
    if (!on_owner_thread() || execution_closed_) return invalid_state("Runtime mutation requires its open owner thread");
    if (state_callback_active_) return invalid_state("state schema callbacks cannot mutate Runtime");
    if (!outbox_->pending.empty())
        return foundation::not_ready("pending outbox commands must be applied or rejected before definition replacement");
    const auto* current = registry_.find_definition(definition.id);
    if (current == nullptr) {
        return foundation::Result::failure(foundation::ResultCode::kNotFound,
                                           "feature definition is not registered");
    }
    if (migration.from_definition != current->version) {
        return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                           "state migration origin does not match the active definition");
    }
    if (migration.to_definition != definition.version) {
        return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                           "state migration target does not match definition");
    }
    if (migration.from_schema.major == 0 || migration.to_schema.major == 0) {
        return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                           "state migration schema versions are required");
    }
    if (const auto result = validate_feature_contract(definition.contract); !result) return result;
    std::vector<detail::ClockWorkDemand> workload;
    if (const auto valid = collect_clock_workload(workload, &definition); !valid) return valid;
    if (const auto valid = detail::validate_clock_workload(workload, registry_); !valid) return valid;
    const bool local_state = owns_state_payload(current->contract.state);
    const auto current_digest = feature_contract_digest(*current);
    const auto replacement_digest = feature_contract_digest(definition);
    if (local_state && (migration.from_schema != current->contract.state_payload.schema_version ||
                        migration.to_schema != definition.contract.state_payload.schema_version))
        return invalid_state("state migration schema does not match the source/target payload contract");
    auto staged_instances = instances_;
    auto staged_processes = processes_;
    for (auto& [instance_id, instance] : staged_instances) {
        static_cast<void>(instance_id);
        if (instance.definition_id != definition.id) continue;
        if (instance.definition_digest != current_digest)
            return invalid_state("active Feature binding does not match the migration origin contract");
        if (current->contract.state != definition.contract.state ||
            current->contract.execution != definition.contract.execution ||
            current->contract.structure != definition.contract.structure)
            return invalid_state("active Feature cannot change its state owner or lifecycle kind");
        if (instance.retry_pending)
            return invalid_state("Feature cannot be replaced while an invocation awaits retry");
        if (const auto result = validate_parameter_assets(instance.parameters, definition.contract.parameters); !result)
            return result;
        instance.definition_digest = replacement_digest;
        ++instance.render_resource_epoch;
        if (!local_state) continue;
        auto* process = instance.processes.empty() ? nullptr : &staged_processes.at(instance.processes.front());
        auto& state = invocation_state(instance, process, current->contract.state);
        if (state.schema_version != migration.from_schema)
            return invalid_state("stored state schema does not match the migration origin");
        if (migration.from_schema != migration.to_schema && !state.payload.empty() && !migration.migrate_payload)
            return invalid_state("schema-changing state requires an explicit payload migration");
        if (migration.migrate_payload) {
            try {
                const auto migrate = migration.migrate_payload;
                FlagScope scope(state_callback_active_);
                const auto migrated = migrate(state_snapshot(instance, process, current->contract.state));
                if (!migrated) return migrated.result;
                state.payload = migrated.value;
            } catch (...) {
                return invalid_state("state payload migration threw an exception");
            }
        }
        if (const auto result = validate_state_payload(definition, state.payload, state_callback_active_); !result) return result;
        if (migration.migrate_payload || migration.from_schema != migration.to_schema) {
            state.version = StateVersion(state.version.value() + 1);
            ++state.commit_count;
        }
        state.schema_version = migration.to_schema;
    }
    const auto old_clock = current->contract.clock.id;
    const auto new_clock = definition.contract.clock.id;
    const bool schedule_changed = !same_clock_schedule(current->contract.clock, definition.contract.clock);
    const RuntimeGeneration replacement_generation(concurrency::allocate_unique_identity());
    decltype(outbox_->owners) staged_outbox_owners;
    try {
        staged_outbox_owners.reserve(outbox_->owners.size());
        for (const auto& [id, published] : outbox_->owners) {
            const auto& instance = staged_instances.at(id);
            const auto* live = instance.definition_id == definition.id ? &definition
                                                                       : registry_.find_definition(instance.definition_id);
            if (!live) return invalid_state("outbox publication definition is no longer registered");
            auto owner = std::make_shared<detail::FeatureOutboxOwner>(*published);
            owner->generation = replacement_generation;
            owner->definition_digest = instance.definition_digest;
            owner->source.clock = live->contract.clock.id;
            owner->source.domains = live->contract.domains;
            owner->declarations = live->contract.side_effects;
            staged_outbox_owners.emplace(id, std::move(owner));
        }
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("outbox replacement owner allocation failed");
    }
    if (auto result = registry_.replace_definition(definition); !result) {
        return result;
    }
    if (old_clock == new_clock && schedule_changed) clock_elapsed_.erase(new_clock);
    if (old_clock != new_clock) {
        std::vector<FeatureInstanceId> moved;
        for (const auto& [instance_id, instance] : instances_) {
            if (instance.definition_id == definition.id) moved.push_back(instance_id);
        }
        auto old_plan = plans_.find(old_clock);
        if (old_plan != plans_.end()) {
            auto& ids = old_plan->second.instances;
            ids.erase(std::remove_if(ids.begin(), ids.end(), [&moved](FeatureInstanceId id) {
                          return std::find(moved.begin(), moved.end(), id) != moved.end();
                      }),
                      ids.end());
            if (ids.empty()) plans_.erase(old_plan);
        }
        if (!moved.empty()) {
            auto& new_plan = plans_[new_clock];
            new_plan.clock = new_clock;
            new_plan.instances.insert(new_plan.instances.end(), moved.begin(), moved.end());
        }
        clock_elapsed_.erase(old_clock);
        clock_ticks_.erase(old_clock);
    }
    execution_->cancel_all();
    deferred_.clear();
    execution_backlog_.clear();
    release_timings_.clear();
    instances_ = std::move(staged_instances);
    processes_ = std::move(staged_processes);
    render_publications_.clear();
    render_dirty_.clear();
    outbox_->owners = std::move(staged_outbox_owners);
    generation_ = replacement_generation;
    return foundation::Result::success();
}

foundation::ResultValue<ProcessId> FeatureRuntime::process(FeatureInstanceId instance) const {
    if (!on_owner_thread()) return {invalid_state("Runtime read requires its owner thread"), {}};
    const auto it = instances_.find(instance);
    if (it == instances_.end() || it->second.processes.empty()) {
        return {foundation::Result::failure(foundation::ResultCode::kNotFound,
                                             "feature instance has no process"), {}};
    }
    return {foundation::Result::success(), it->second.processes.front()};
}

const FeatureInstance* FeatureRuntime::find_instance(FeatureInstanceId id) const noexcept {
    if (!on_owner_thread()) return nullptr;
    const auto it = instances_.find(id);
    return it == instances_.end() ? nullptr : &it->second;
}

const ProcessInstance* FeatureRuntime::find_process(ProcessId id) const noexcept {
    if (!on_owner_thread()) return nullptr;
    const auto it = processes_.find(id);
    return it == processes_.end() ? nullptr : &it->second;
}
}  // namespace aeg::feature
