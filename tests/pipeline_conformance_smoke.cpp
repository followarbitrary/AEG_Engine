/**
 * @file pipeline_conformance_smoke.cpp
 * @brief Verifies the public conformance contract for custom pipelines and features.
 */
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <pipeline.hpp>
#include <thread>

namespace {
using aeg::foundation::Result;
using aeg::foundation::ResultCode;
using aeg::pipeline::AccessFlags;
using aeg::pipeline::ArtifactAccess;
using aeg::pipeline::ArtifactDescriptor;
using aeg::pipeline::ArtifactId;
using aeg::pipeline::ArtifactLifetime;
using aeg::pipeline::ArtifactStorage;
using aeg::pipeline::ArtifactTypeId;
using aeg::pipeline::Capability;
using aeg::pipeline::CapabilityMask;
using aeg::pipeline::CompositeFeature;
using aeg::pipeline::EnginePhase;
using aeg::pipeline::ExecutionDomain;
using aeg::pipeline::FeatureBindingDescriptor;
using aeg::pipeline::FeatureDescriptor;
using aeg::pipeline::FeatureDomain;
using aeg::pipeline::FeatureId;
using aeg::pipeline::FeaturePointFlags;
using aeg::pipeline::FeaturePointId;
using aeg::pipeline::FeaturePointPosition;
using aeg::pipeline::LogicFeature;
using aeg::pipeline::OwnershipPolicy;
using aeg::pipeline::PhaseId;
using aeg::pipeline::PhaseOrderRelation;
using aeg::pipeline::Pipeline;
using aeg::pipeline::PipelineCategory;
using aeg::pipeline::PipelineId;
using aeg::pipeline::PipelineScope;
using aeg::pipeline::PipelineScopeHost;
using aeg::pipeline::PipelineTaskContext;
using aeg::pipeline::RenderFeature;
using aeg::pipeline::StageId;
using aeg::pipeline::TaskFlags;
using aeg::pipeline::TimePolicy;

inline constexpr ArtifactId kFeatureValue{"ConformanceFeatureValue"};
inline constexpr ArtifactId kLogicFeatureArtifact{"ConformanceLogicArtifact"};
inline constexpr ArtifactId kNoProducerArtifact{"ConformanceNoProducerArtifact"};
inline constexpr ArtifactId kMultipleWriterArtifact{"ConformanceMultipleWriterArtifact"};
inline constexpr ArtifactId kBadVersionArtifact{"ConformanceBadVersionArtifact"};
inline constexpr ArtifactTypeId kIntegerType{"ConformanceInteger"};
inline constexpr FeaturePointId kLogicPoint{"ConformanceLogicPoint"};
inline constexpr FeaturePointId kLateLogicPoint{"ConformanceLateLogicPoint"};
inline constexpr FeaturePointId kCompositePoint{"ConformanceCompositePoint"};
inline constexpr FeaturePointId kRestrictedPoint{"ConformanceRestrictedPoint"};
inline constexpr FeaturePointId kRenderPoint{"ConformanceRenderPoint"};
inline constexpr PhaseId kCustomPhase{"ConformanceCustomPhase"};

constexpr CapabilityMask capability(Capability capability_value) noexcept {
    return CapabilityMask{1} << static_cast<uint8_t>(capability_value);
}

inline constexpr CapabilityMask kRequiredCapabilities =
    capability(Capability::kParallelCpuExecution) | capability(Capability::kGpuExecution);

struct ConformanceState final {
    std::atomic<uint32_t> logic_runs{0};
    std::atomic<uint32_t> composite_logic_runs{0};
    std::atomic<uint32_t> composite_render_runs{0};
    std::atomic<uint32_t> render_runs{0};
    std::atomic<uint32_t> late_logic_runs{0};
    std::atomic<uint32_t> custom_phase_runs{0};
    std::atomic<uint32_t> consumer_runs{0};
    std::atomic<int> observed_value{-1};
    std::mutex thread_mutex;
    std::thread::id logic_thread{};
    std::thread::id composite_logic_thread{};
    std::thread::id composite_render_thread{};
    std::thread::id render_thread{};
};

enum class Counter : uint8_t {
    kCompositeLogic,
    kCompositeRender,
    kRender,
    kLateLogic,
    kCustomPhase,
};

void remember_thread(ConformanceState& state, std::thread::id& slot) {
    std::lock_guard lock(state.thread_mutex);
    slot = std::this_thread::get_id();
}

[[nodiscard]] Result record_counter(PipelineTaskContext& context, Counter counter) {
    auto* state = static_cast<ConformanceState*>(context.user_data());
    if (state == nullptr) {
        return Result::failure(ResultCode::kInvalidArgument, "conformance task state is null");
    }

    std::atomic<uint32_t>* count = nullptr;
    std::thread::id* thread = nullptr;
    switch (counter) {
        case Counter::kCompositeLogic:
            count = &state->composite_logic_runs;
            thread = &state->composite_logic_thread;
            break;
        case Counter::kCompositeRender:
            count = &state->composite_render_runs;
            thread = &state->composite_render_thread;
            break;
        case Counter::kRender:
            count = &state->render_runs;
            thread = &state->render_thread;
            break;
        case Counter::kLateLogic:
            count = &state->late_logic_runs;
            break;
        case Counter::kCustomPhase:
            count = &state->custom_phase_runs;
            break;
    }
    count->fetch_add(1, std::memory_order_relaxed);
    if (thread != nullptr) {
        remember_thread(*state, *thread);
    }
    return Result::success();
}

[[nodiscard]] Result write_feature_value(PipelineTaskContext& context) {
    auto* state = static_cast<ConformanceState*>(context.user_data());
    if (state == nullptr) {
        return Result::failure(ResultCode::kInvalidArgument, "logic feature state is null");
    }
    auto writer = context.write_artifact(kFeatureValue, kIntegerType, 1);
    if (!writer) {
        return writer.result;
    }
    int* value = writer.value.as<int>(kIntegerType);
    if (value == nullptr) {
        return Result::failure(ResultCode::kInvalidState, "logic feature writer has an invalid payload");
    }
    *value = 42;
    const Result result = writer.value.commit();
    if (result) {
        state->logic_runs.fetch_add(1, std::memory_order_relaxed);
        remember_thread(*state, state->logic_thread);
    }
    return result;
}

[[nodiscard]] Result consume_feature_value(PipelineTaskContext& context) {
    auto* state = static_cast<ConformanceState*>(context.user_data());
    if (state == nullptr) {
        return Result::failure(ResultCode::kInvalidArgument, "consumer task state is null");
    }
    const auto view = context.read_artifact(kFeatureValue, kIntegerType, 1);
    if (!view) {
        return view.result;
    }
    const int* value = view.value.as<int>(kIntegerType);
    if (value == nullptr || *value != 42) {
        return Result::failure(ResultCode::kInvalidState, "feature artifact dependency was not respected");
    }
    state->observed_value.store(*value, std::memory_order_relaxed);
    state->consumer_runs.fetch_add(1, std::memory_order_relaxed);
    return Result::success();
}

[[nodiscard]] Result composite_logic_task(PipelineTaskContext& context) {
    return record_counter(context, Counter::kCompositeLogic);
}

[[nodiscard]] Result composite_render_task(PipelineTaskContext& context) {
    return record_counter(context, Counter::kCompositeRender);
}

[[nodiscard]] Result render_task(PipelineTaskContext& context) {
    return record_counter(context, Counter::kRender);
}

[[nodiscard]] Result late_logic_task(PipelineTaskContext& context) {
    return record_counter(context, Counter::kLateLogic);
}

[[nodiscard]] Result custom_phase_task(PipelineTaskContext& context) {
    return record_counter(context, Counter::kCustomPhase);
}

[[nodiscard]] ArtifactDescriptor make_artifact(ArtifactId id, ArtifactStorage storage, ArtifactLifetime lifetime,
                                               OwnershipPolicy ownership, uint32_t version = 1,
                                               bool external_input = false) {
    ArtifactDescriptor descriptor{};
    descriptor.id = id;
    descriptor.type = kIntegerType;
    descriptor.owner_scope = PipelineScope::kEngine;
    descriptor.storage = storage;
    descriptor.lifetime = lifetime;
    descriptor.ownership = ownership;
    descriptor.version = version;
    descriptor.visible_to = aeg::pipeline::to_mask(PipelineScope::kEngine);
    descriptor.external_input = external_input;
    return descriptor;
}

[[nodiscard]] FeatureBindingDescriptor make_binding(FeatureId feature, PipelineId pipeline, FeaturePointId point,
                                                    PipelineScope scope = PipelineScope::kEngine) {
    return FeatureBindingDescriptor{feature, pipeline, scope, point, 0, true, {}};
}

class ConformancePipeline final : public Pipeline {
public:
    explicit ConformancePipeline(ConformanceState& state) : state_(&state) {
        descriptor_.id = PipelineId("PipelineConformance");
        descriptor_.name = "PipelineConformance";
        descriptor_.version = {1, 0, 0};
        descriptor_.category = PipelineCategory::kCustom;
        descriptor_.supported_scopes = aeg::pipeline::to_mask(PipelineScope::kEngine);
        descriptor_.domains =
            aeg::pipeline::to_mask(FeatureDomain::kLogic) | aeg::pipeline::to_mask(FeatureDomain::kRender);
        descriptor_.required_capabilities = kRequiredCapabilities;
    }

    [[nodiscard]] const aeg::pipeline::PipelineDescriptor& descriptor() const noexcept override { return descriptor_; }

    [[nodiscard]] Result build(aeg::pipeline::PipelineBuilder& builder) const override {
        const auto logic_anchor = builder.add_stage({
            StageId("ConformanceLogicAnchor"),
            EnginePhase::kPreUpdate,
            ExecutionDomain::kMainThread,
            TimePolicy::kFrame,
            false,
        });
        const auto composite_anchor = builder.add_stage({
            StageId("ConformanceCompositeAnchor"),
            EnginePhase::kVariableUpdate,
            ExecutionDomain::kMainThread,
            TimePolicy::kFrame,
            false,
        });
        const auto consumer_stage = builder.add_stage({
            StageId("ConformanceConsumer"),
            EnginePhase::kVariableUpdate,
            ExecutionDomain::kMainThread,
            TimePolicy::kFrame,
            false,
        });
        const auto render_anchor = builder.add_stage({
            StageId("ConformanceRenderAnchor"),
            EnginePhase::kRenderBuild,
            ExecutionDomain::kMainThread,
            TimePolicy::kFrame,
            false,
        });
        const auto custom_phase_stage = builder.add_stage({
            StageId("ConformanceCustomPhaseStage"),
            EnginePhase::kVariableUpdate,
            ExecutionDomain::kMainThread,
            TimePolicy::kFrame,
            false,
            kCustomPhase,
        });
        if (!logic_anchor || !composite_anchor || !consumer_stage || !render_anchor || !custom_phase_stage) {
            return builder.status();
        }

        const auto feature_value = builder.declare_artifact(make_artifact(
            kFeatureValue, ArtifactStorage::kMutableState, ArtifactLifetime::kEngine, OwnershipPolicy::kProducerOwned));
        if (!feature_value) {
            return builder.status();
        }

        const auto consumer_task = builder.add_task({
            aeg::pipeline::TaskId("ConformanceConsumerTask"),
            TaskFlags::kMustRun,
            &consume_feature_value,
            state_,
        });
        const auto custom_task = builder.add_task({
            aeg::pipeline::TaskId("ConformanceCustomPhaseTask"),
            TaskFlags::kMustRun | TaskFlags::kSideEffect,
            &custom_phase_task,
            state_,
        });
        if (!consumer_task || !custom_task) {
            return builder.status();
        }
        if (const Result result = builder.bind_task(consumer_task, consumer_stage); !result) {
            return result;
        }
        if (const Result result = builder.bind_task(custom_task, custom_phase_stage); !result) {
            return result;
        }
        if (const Result result =
                builder.reads(consumer_stage, {kFeatureValue, ArtifactAccess::kRead, 1, AccessFlags::kNone});
            !result) {
            return result;
        }

        const auto add_point = [&](FeaturePointId id, const char* name, StageId anchor,
                                   aeg::pipeline::FeatureDomainMask domains, FeaturePointPosition position,
                                   FeaturePointFlags flags) {
            return builder.add_feature_point({
                id,
                name,
                {},
                anchor,
                position,
                domains,
                aeg::pipeline::to_mask(PipelineScope::kEngine),
                flags,
            });
        };
        if (!add_point(kLogicPoint, "ConformanceLogicPoint", StageId("ConformanceLogicAnchor"),
                       aeg::pipeline::to_mask(FeatureDomain::kLogic), FeaturePointPosition::kBefore,
                       FeaturePointFlags::kMayCreateArtifacts) ||
            !add_point(kLateLogicPoint, "ConformanceLateLogicPoint", StageId("ConformanceLogicAnchor"),
                       aeg::pipeline::to_mask(FeatureDomain::kLogic), FeaturePointPosition::kAfter,
                       FeaturePointFlags::kNone) ||
            !add_point(kCompositePoint, "ConformanceCompositePoint", StageId("ConformanceCompositeAnchor"),
                       aeg::pipeline::to_mask(FeatureDomain::kLogic) | aeg::pipeline::to_mask(FeatureDomain::kRender),
                       FeaturePointPosition::kAfter, FeaturePointFlags::kMayCreateArtifacts) ||
            !add_point(kRestrictedPoint, "ConformanceRestrictedPoint", StageId("ConformanceLogicAnchor"),
                       aeg::pipeline::to_mask(FeatureDomain::kLogic), FeaturePointPosition::kAfter,
                       FeaturePointFlags::kNone) ||
            !add_point(kRenderPoint, "ConformanceRenderPoint", StageId("ConformanceRenderAnchor"),
                       aeg::pipeline::to_mask(FeatureDomain::kRender), FeaturePointPosition::kAfter,
                       FeaturePointFlags::kMayCreateComputePass)) {
            return builder.status();
        }
        return Result::success();
    }

private:
    aeg::pipeline::PipelineDescriptor descriptor_{};
    ConformanceState* state_{nullptr};
};

class ConformanceLogicFeature final : public LogicFeature {
public:
    explicit ConformanceLogicFeature(ConformanceState& state) : state_(&state) {
        descriptor_.id = FeatureId("ConformanceLogicFeature");
        descriptor_.name = "ConformanceLogicFeature";
        descriptor_.domain = FeatureDomain::kLogic;
        descriptor_.domain_mask = aeg::pipeline::to_mask(FeatureDomain::kLogic);
        descriptor_.supported_scopes = aeg::pipeline::to_mask(PipelineScope::kEngine);
        descriptor_.version = {1, 0, 0};
    }

    [[nodiscard]] const FeatureDescriptor& descriptor() const noexcept override { return descriptor_; }

protected:
    [[nodiscard]] Result setup(BuilderType& builder) const override {
        const auto artifact =
            builder.declare_artifact(make_artifact(kLogicFeatureArtifact, ArtifactStorage::kMutableState,
                                                   ArtifactLifetime::kEngine, OwnershipPolicy::kProducerOwned));
        if (!artifact) {
            return Result::failure(ResultCode::kUnsupported, "logic feature artifact creation was denied");
        }

        const auto stage = builder.add_stage({
            StageId("ConformanceLogicFeatureStage"),
            EnginePhase::kPreUpdate,
            ExecutionDomain::kCpuWorker,
            TimePolicy::kFrame,
            true,
        });
        const auto task = builder.add_task({
            aeg::pipeline::TaskId("ConformanceLogicFeatureTask"),
            TaskFlags::kParallelSafe,
            &write_feature_value,
            state_,
        });
        if (!stage || !task) {
            return Result::failure(ResultCode::kInvalidState, "logic feature declaration failed");
        }
        if (const Result result = builder.bind_task(task, stage); !result) {
            return result;
        }
        return builder.writes(stage, {kFeatureValue, ArtifactAccess::kWrite, 1, AccessFlags::kDiscardContents});
    }

private:
    FeatureDescriptor descriptor_{};
    ConformanceState* state_{nullptr};
};

class CompositeLogicPart final : public LogicFeature {
public:
    explicit CompositeLogicPart(ConformanceState& state) : state_(&state) {}

protected:
    [[nodiscard]] Result setup(BuilderType& builder) const override {
        const auto stage = builder.add_stage({
            StageId("ConformanceCompositeLogicStage"),
            EnginePhase::kVariableUpdate,
            ExecutionDomain::kCpuWorker,
            TimePolicy::kFrame,
            true,
        });
        const auto task = builder.add_task({
            aeg::pipeline::TaskId("ConformanceCompositeLogicTask"),
            TaskFlags::kParallelSafe,
            &composite_logic_task,
            state_,
        });
        if (!stage || !task) {
            return Result::failure(ResultCode::kInvalidState, "composite logic part declaration failed");
        }
        return builder.bind_task(task, stage);
    }

private:
    ConformanceState* state_{nullptr};
};

class CompositeRenderPart final : public RenderFeature {
public:
    explicit CompositeRenderPart(ConformanceState& state) : state_(&state) {}

protected:
    [[nodiscard]] Result setup(BuilderType& builder) const override {
        const auto stage = builder.add_stage({
            StageId("ConformanceCompositeRenderStage"),
            EnginePhase::kVariableUpdate,
            ExecutionDomain::kGpu,
            TimePolicy::kFrame,
            true,
        });
        const auto task = builder.add_task({
            aeg::pipeline::TaskId("ConformanceCompositeRenderTask"),
            TaskFlags::kParallelSafe,
            &composite_render_task,
            state_,
        });
        if (!stage || !task) {
            return Result::failure(ResultCode::kInvalidState, "composite render part declaration failed");
        }
        return builder.bind_task(task, stage);
    }

private:
    ConformanceState* state_{nullptr};
};

class ConformanceCompositeFeature final : public CompositeFeature<CompositeLogicPart, CompositeRenderPart> {
public:
    explicit ConformanceCompositeFeature(ConformanceState& state)
        : CompositeFeature<CompositeLogicPart, CompositeRenderPart>(CompositeLogicPart(state),
                                                                    CompositeRenderPart(state)) {
        descriptor_.id = FeatureId("ConformanceCompositeFeature");
        descriptor_.name = "ConformanceCompositeFeature";
        descriptor_.domain = FeatureDomain::kComposite;
        descriptor_.domain_mask = kDomainMask;
        descriptor_.supported_scopes = aeg::pipeline::to_mask(PipelineScope::kEngine);
        descriptor_.version = {1, 0, 0};
    }

    [[nodiscard]] const FeatureDescriptor& descriptor() const noexcept override { return descriptor_; }

private:
    FeatureDescriptor descriptor_{};
};

class ConformanceRenderFeature final : public RenderFeature {
public:
    explicit ConformanceRenderFeature(ConformanceState& state) : state_(&state) {
        descriptor_.id = FeatureId("ConformanceRenderFeature");
        descriptor_.name = "ConformanceRenderFeature";
        descriptor_.domain = FeatureDomain::kRender;
        descriptor_.domain_mask = aeg::pipeline::to_mask(FeatureDomain::kRender);
        descriptor_.supported_scopes = aeg::pipeline::to_mask(PipelineScope::kEngine);
        descriptor_.version = {1, 0, 0};
    }

    [[nodiscard]] const FeatureDescriptor& descriptor() const noexcept override { return descriptor_; }

protected:
    [[nodiscard]] Result setup(BuilderType& builder) const override {
        const auto stage = builder.add_stage({
            StageId("ConformanceRenderFeatureStage"),
            EnginePhase::kRenderBuild,
            ExecutionDomain::kRenderThread,
            TimePolicy::kFrame,
            true,
        });
        const auto task = builder.add_task({
            aeg::pipeline::TaskId("ConformanceRenderFeatureTask"),
            TaskFlags::kParallelSafe,
            &render_task,
            state_,
        });
        if (!stage || !task) {
            return Result::failure(ResultCode::kInvalidState, "render feature declaration failed");
        }
        return builder.bind_task(task, stage);
    }

private:
    FeatureDescriptor descriptor_{};
    ConformanceState* state_{nullptr};
};

class LateLogicFeature final : public LogicFeature {
public:
    explicit LateLogicFeature(ConformanceState& state) : state_(&state) {
        descriptor_.id = FeatureId("ConformanceLateLogicFeature");
        descriptor_.name = "ConformanceLateLogicFeature";
        descriptor_.domain = FeatureDomain::kLogic;
        descriptor_.domain_mask = aeg::pipeline::to_mask(FeatureDomain::kLogic);
        descriptor_.supported_scopes = aeg::pipeline::to_mask(PipelineScope::kEngine);
        descriptor_.version = {1, 0, 0};
    }

    [[nodiscard]] const FeatureDescriptor& descriptor() const noexcept override { return descriptor_; }

protected:
    [[nodiscard]] Result setup(BuilderType& builder) const override {
        const auto stage = builder.add_stage({
            StageId("ConformanceLateLogicStage"),
            EnginePhase::kVariableUpdate,
            ExecutionDomain::kCpuWorker,
            TimePolicy::kFrame,
            true,
        });
        const auto task = builder.add_task({
            aeg::pipeline::TaskId("ConformanceLateLogicTask"),
            TaskFlags::kParallelSafe,
            &late_logic_task,
            state_,
        });
        if (!stage || !task) {
            return Result::failure(ResultCode::kInvalidState, "late logic feature declaration failed");
        }
        return builder.bind_task(task, stage);
    }

private:
    FeatureDescriptor descriptor_{};
    ConformanceState* state_{nullptr};
};

class FrameOnlyFeature final : public LogicFeature {
public:
    FrameOnlyFeature() {
        descriptor_.id = FeatureId("ConformanceFrameOnlyFeature");
        descriptor_.name = "ConformanceFrameOnlyFeature";
        descriptor_.domain = FeatureDomain::kLogic;
        descriptor_.domain_mask = aeg::pipeline::to_mask(FeatureDomain::kLogic);
        descriptor_.supported_scopes = aeg::pipeline::to_mask(PipelineScope::kFrame);
        descriptor_.version = {1, 0, 0};
    }

    [[nodiscard]] const FeatureDescriptor& descriptor() const noexcept override { return descriptor_; }

protected:
    [[nodiscard]] Result setup(BuilderType&) const override { return Result::success(); }

private:
    FeatureDescriptor descriptor_{};
};

class UnauthorizedArtifactFeature final : public LogicFeature {
public:
    UnauthorizedArtifactFeature() {
        descriptor_.id = FeatureId("ConformanceUnauthorizedArtifactFeature");
        descriptor_.name = "ConformanceUnauthorizedArtifactFeature";
        descriptor_.domain = FeatureDomain::kLogic;
        descriptor_.domain_mask = aeg::pipeline::to_mask(FeatureDomain::kLogic);
        descriptor_.supported_scopes = aeg::pipeline::to_mask(PipelineScope::kEngine);
        descriptor_.version = {1, 0, 0};
    }

    [[nodiscard]] const FeatureDescriptor& descriptor() const noexcept override { return descriptor_; }

protected:
    [[nodiscard]] Result setup(BuilderType& builder) const override {
        const auto artifact =
            builder.declare_artifact(make_artifact(ArtifactId("DeniedFeatureArtifact"), ArtifactStorage::kValue,
                                                   ArtifactLifetime::kTask, OwnershipPolicy::kProducerOwned));
        if (artifact) {
            return Result::failure(ResultCode::kInvalidState, "restricted feature point granted artifact creation");
        }
        return Result::failure(ResultCode::kUnsupported, "restricted feature point rejected artifact creation");
    }

private:
    FeatureDescriptor descriptor_{};
};

class BadDomainFeature final : public RenderFeature {
public:
    BadDomainFeature() {
        descriptor_.id = FeatureId("ConformanceBadDomainFeature");
        descriptor_.name = "ConformanceBadDomainFeature";
        descriptor_.domain = FeatureDomain::kLogic;
        descriptor_.domain_mask = aeg::pipeline::to_mask(FeatureDomain::kLogic);
        descriptor_.supported_scopes = aeg::pipeline::to_mask(PipelineScope::kEngine);
        descriptor_.version = {1, 0, 0};
    }

    [[nodiscard]] const FeatureDescriptor& descriptor() const noexcept override { return descriptor_; }

protected:
    [[nodiscard]] Result setup(BuilderType&) const override { return Result::success(); }

private:
    FeatureDescriptor descriptor_{};
};

class LogicOnlyPipeline final : public Pipeline {
public:
    LogicOnlyPipeline() {
        descriptor_.id = PipelineId("ConformanceLogicOnlyPipeline");
        descriptor_.name = "ConformanceLogicOnlyPipeline";
        descriptor_.category = PipelineCategory::kCustom;
        descriptor_.supported_scopes = aeg::pipeline::to_mask(PipelineScope::kEngine);
        descriptor_.domains = aeg::pipeline::to_mask(FeatureDomain::kLogic);
    }

    [[nodiscard]] const aeg::pipeline::PipelineDescriptor& descriptor() const noexcept override { return descriptor_; }

    [[nodiscard]] Result build(aeg::pipeline::PipelineBuilder& builder) const override {
        const auto stage = builder.add_stage({
            StageId("ConformanceLogicOnlyAnchor"),
            EnginePhase::kVariableUpdate,
            ExecutionDomain::kMainThread,
            TimePolicy::kFrame,
            false,
        });
        if (!stage) {
            return builder.status();
        }
        const auto point = builder.add_feature_point({
            FeaturePointId("ConformanceLogicOnlyRenderPoint"),
            "ConformanceLogicOnlyRenderPoint",
            {},
            StageId("ConformanceLogicOnlyAnchor"),
            FeaturePointPosition::kAfter,
            aeg::pipeline::to_mask(FeatureDomain::kRender),
            aeg::pipeline::to_mask(PipelineScope::kEngine),
            FeaturePointFlags::kNone,
        });
        return point ? Result::success() : builder.status();
    }

private:
    aeg::pipeline::PipelineDescriptor descriptor_{};
};

class NoProducerPipeline final : public Pipeline {
public:
    NoProducerPipeline() {
        descriptor_.id = PipelineId("ConformanceNoProducerPipeline");
        descriptor_.name = "ConformanceNoProducerPipeline";
        descriptor_.category = PipelineCategory::kCustom;
    }

    [[nodiscard]] const aeg::pipeline::PipelineDescriptor& descriptor() const noexcept override { return descriptor_; }

    [[nodiscard]] Result build(aeg::pipeline::PipelineBuilder& builder) const override {
        const auto stage = builder.add_stage({
            StageId("ConformanceNoProducerStage"),
            EnginePhase::kVariableUpdate,
            ExecutionDomain::kMainThread,
            TimePolicy::kFrame,
            false,
        });
        const auto artifact = builder.declare_artifact(make_artifact(
            kNoProducerArtifact, ArtifactStorage::kValue, ArtifactLifetime::kFrame, OwnershipPolicy::kProducerOwned));
        if (!stage || !artifact) {
            return builder.status();
        }
        return builder.reads(stage, {kNoProducerArtifact, ArtifactAccess::kRead, 1, AccessFlags::kNone});
    }

private:
    aeg::pipeline::PipelineDescriptor descriptor_{};
};

class MultipleWriterPipeline final : public Pipeline {
public:
    MultipleWriterPipeline() {
        descriptor_.id = PipelineId("ConformanceMultipleWriterPipeline");
        descriptor_.name = "ConformanceMultipleWriterPipeline";
        descriptor_.category = PipelineCategory::kCustom;
    }

    [[nodiscard]] const aeg::pipeline::PipelineDescriptor& descriptor() const noexcept override { return descriptor_; }

    [[nodiscard]] Result build(aeg::pipeline::PipelineBuilder& builder) const override {
        const auto first = builder.add_stage({
            StageId("ConformanceWriterA"),
            EnginePhase::kVariableUpdate,
            ExecutionDomain::kMainThread,
            TimePolicy::kFrame,
            false,
        });
        const auto second = builder.add_stage({
            StageId("ConformanceWriterB"),
            EnginePhase::kVariableUpdate,
            ExecutionDomain::kMainThread,
            TimePolicy::kFrame,
            false,
        });
        const auto artifact =
            builder.declare_artifact(make_artifact(kMultipleWriterArtifact, ArtifactStorage::kValue,
                                                   ArtifactLifetime::kFrame, OwnershipPolicy::kProducerOwned));
        if (!first || !second || !artifact) {
            return builder.status();
        }
        if (const Result result = builder.writes(
                first, {kMultipleWriterArtifact, ArtifactAccess::kWrite, 1, AccessFlags::kDiscardContents});
            !result) {
            return result;
        }
        return builder.writes(second,
                              {kMultipleWriterArtifact, ArtifactAccess::kWrite, 1, AccessFlags::kDiscardContents});
    }

private:
    aeg::pipeline::PipelineDescriptor descriptor_{};
};

class CyclePipeline final : public Pipeline {
public:
    CyclePipeline() {
        descriptor_.id = PipelineId("ConformanceCyclePipeline");
        descriptor_.name = "ConformanceCyclePipeline";
        descriptor_.category = PipelineCategory::kCustom;
    }

    [[nodiscard]] const aeg::pipeline::PipelineDescriptor& descriptor() const noexcept override { return descriptor_; }

    [[nodiscard]] Result build(aeg::pipeline::PipelineBuilder& builder) const override {
        const auto first = builder.add_stage({
            StageId("ConformanceCycleFirst"),
            EnginePhase::kFrameBegin,
            ExecutionDomain::kMainThread,
            TimePolicy::kFrame,
            false,
        });
        const auto second = builder.add_stage({
            StageId("ConformanceCycleSecond"),
            EnginePhase::kFrameBegin,
            ExecutionDomain::kMainThread,
            TimePolicy::kFrame,
            false,
        });
        if (!first || !second) {
            return builder.status();
        }
        if (const Result result = builder.runs_before(first, StageId("ConformanceCycleSecond")); !result) {
            return result;
        }
        return builder.runs_before(second, StageId("ConformanceCycleFirst"));
    }

private:
    aeg::pipeline::PipelineDescriptor descriptor_{};
};

class BadOrderingPipeline final : public Pipeline {
public:
    BadOrderingPipeline() {
        descriptor_.id = PipelineId("ConformanceBadOrderingPipeline");
        descriptor_.name = "ConformanceBadOrderingPipeline";
        descriptor_.category = PipelineCategory::kCustom;
    }

    [[nodiscard]] const aeg::pipeline::PipelineDescriptor& descriptor() const noexcept override { return descriptor_; }

    [[nodiscard]] Result build(aeg::pipeline::PipelineBuilder& builder) const override {
        const auto stage = builder.add_stage({
            StageId("ConformanceBadOrderingStage"),
            EnginePhase::kFrameBegin,
            ExecutionDomain::kMainThread,
            TimePolicy::kFrame,
            false,
        });
        if (!stage) {
            return builder.status();
        }
        return builder.runs_before(stage, StageId("ConformanceMissingStage"));
    }

private:
    aeg::pipeline::PipelineDescriptor descriptor_{};
};

class BadVersionPipeline final : public Pipeline {
public:
    BadVersionPipeline() {
        descriptor_.id = PipelineId("ConformanceBadVersionPipeline");
        descriptor_.name = "ConformanceBadVersionPipeline";
        descriptor_.category = PipelineCategory::kCustom;
    }

    [[nodiscard]] const aeg::pipeline::PipelineDescriptor& descriptor() const noexcept override { return descriptor_; }

    [[nodiscard]] Result build(aeg::pipeline::PipelineBuilder& builder) const override {
        const auto stage = builder.add_stage({
            StageId("ConformanceBadVersionStage"),
            EnginePhase::kFrameBegin,
            ExecutionDomain::kMainThread,
            TimePolicy::kFrame,
            false,
        });
        const auto artifact = builder.declare_artifact(make_artifact(
            kBadVersionArtifact, ArtifactStorage::kValue, ArtifactLifetime::kFrame, OwnershipPolicy::kProducerOwned));
        if (!stage || !artifact) {
            return builder.status();
        }
        return builder.reads(stage, {kBadVersionArtifact, ArtifactAccess::kRead, 2, AccessFlags::kNone});
    }

private:
    aeg::pipeline::PipelineDescriptor descriptor_{};
};

class UnregisteredPhasePipeline final : public Pipeline {
public:
    UnregisteredPhasePipeline() {
        descriptor_.id = PipelineId("ConformanceUnregisteredPhasePipeline");
        descriptor_.name = "ConformanceUnregisteredPhasePipeline";
        descriptor_.category = PipelineCategory::kCustom;
    }

    [[nodiscard]] const aeg::pipeline::PipelineDescriptor& descriptor() const noexcept override { return descriptor_; }

    [[nodiscard]] Result build(aeg::pipeline::PipelineBuilder& builder) const override {
        const auto stage = builder.add_stage({
            StageId("ConformanceUnregisteredPhaseStage"),
            EnginePhase::kVariableUpdate,
            ExecutionDomain::kMainThread,
            TimePolicy::kFrame,
            false,
            PhaseId("ConformanceUnregisteredPhase"),
        });
        return stage ? Result::success() : builder.status();
    }

private:
    aeg::pipeline::PipelineDescriptor descriptor_{};
};
}  // namespace

int main() {
    aeg::pipeline::PipelineRegistry registry;
    if (const Result result = registry.phase_registry().register_phase({
            kCustomPhase,
            "ConformanceCustomPhase",
            aeg::pipeline::to_phase_id(EnginePhase::kVariableUpdate),
            PhaseOrderRelation::kAfter,
        });
        !result) {
        return 1;
    }
    if (const Result result = registry.phase_registry().register_phase({
            kCustomPhase,
            "ConformanceCustomPhase",
            aeg::pipeline::to_phase_id(EnginePhase::kVariableUpdate),
            PhaseOrderRelation::kAfter,
        });
        result || result.code != ResultCode::kInvalidState) {
        return 2;
    }

    const auto phase_snapshot = registry.phase_registry().snapshot();
    if (!phase_snapshot) {
        return 3;
    }
    const auto custom_phase_iterator =
        std::find(phase_snapshot.value.ordered_phases.begin(), phase_snapshot.value.ordered_phases.end(), kCustomPhase);
    const auto variable_phase_iterator =
        std::find(phase_snapshot.value.ordered_phases.begin(), phase_snapshot.value.ordered_phases.end(),
                  aeg::pipeline::to_phase_id(EnginePhase::kVariableUpdate));
    const auto world_commit_iterator =
        std::find(phase_snapshot.value.ordered_phases.begin(), phase_snapshot.value.ordered_phases.end(),
                  aeg::pipeline::to_phase_id(EnginePhase::kWorldCommit));
    if (custom_phase_iterator == phase_snapshot.value.ordered_phases.end() ||
        variable_phase_iterator == phase_snapshot.value.ordered_phases.end() ||
        world_commit_iterator == phase_snapshot.value.ordered_phases.end() ||
        !(variable_phase_iterator < custom_phase_iterator && custom_phase_iterator < world_commit_iterator)) {
        return 4;
    }

    ConformanceState state;
    if (const Result result =
            registry.register_pipeline([&state] { return std::make_unique<ConformancePipeline>(state); });
        !result) {
        return 5;
    }
    if (const Result result =
            registry.register_pipeline([&state] { return std::make_unique<ConformancePipeline>(state); });
        result || result.code != ResultCode::kInvalidState) {
        return 6;
    }
    if (const Result result =
            registry.register_feature([&state] { return std::make_unique<ConformanceLogicFeature>(state); });
        !result) {
        return 7;
    }
    if (const Result result =
            registry.register_feature([&state] { return std::make_unique<ConformanceLogicFeature>(state); });
        result || result.code != ResultCode::kInvalidState) {
        return 8;
    }
    if (const Result result =
            registry.register_feature([&state] { return std::make_unique<ConformanceCompositeFeature>(state); });
        !result) {
        return 9;
    }
    if (const Result result =
            registry.register_feature([&state] { return std::make_unique<ConformanceRenderFeature>(state); });
        !result) {
        return 10;
    }
    if (const Result result = registry.register_feature([] { return std::make_unique<FrameOnlyFeature>(); }); !result) {
        return 11;
    }
    if (const Result result = registry.register_feature([] { return std::make_unique<UnauthorizedArtifactFeature>(); });
        !result) {
        return 12;
    }
    if (const Result result = registry.register_feature([] { return std::make_unique<BadDomainFeature>(); });
        result || result.code != ResultCode::kInvalidArgument) {
        return 13;
    }

    const PipelineId conformance_pipeline{"PipelineConformance"};
    const FeatureBindingDescriptor logic_binding =
        make_binding(FeatureId("ConformanceLogicFeature"), conformance_pipeline, kLogicPoint);
    const FeatureBindingDescriptor composite_binding =
        make_binding(FeatureId("ConformanceCompositeFeature"), conformance_pipeline, kCompositePoint);
    const FeatureBindingDescriptor render_binding =
        make_binding(FeatureId("ConformanceRenderFeature"), conformance_pipeline, kRenderPoint);
    if (const Result result = registry.bind_feature(logic_binding); !result) {
        return 14;
    }
    if (const Result result = registry.bind_feature(logic_binding);
        result || result.code != ResultCode::kInvalidState) {
        return 15;
    }
    if (const Result result = registry.bind_feature(composite_binding); !result) {
        return 16;
    }
    if (const Result result = registry.bind_feature(render_binding); !result) {
        return 17;
    }
    if (const Result result =
            registry.bind_feature(make_binding(FeatureId("MissingFeature"), conformance_pipeline, kLogicPoint));
        result || result.code != ResultCode::kNotFound) {
        return 18;
    }
    if (const Result result = registry.bind_feature(
            make_binding(FeatureId("ConformanceLogicFeature"), PipelineId("MissingPipeline"), kLogicPoint));
        result || result.code != ResultCode::kNotFound) {
        return 19;
    }
    if (const Result result = registry.bind_feature(
            make_binding(FeatureId("ConformanceLogicFeature"), conformance_pipeline, FeaturePointId("MissingPoint")));
        result || result.code != ResultCode::kNotFound) {
        return 20;
    }
    if (const Result result = registry.bind_feature(
            make_binding(FeatureId("ConformanceFrameOnlyFeature"), conformance_pipeline, kLogicPoint));
        result || result.code != ResultCode::kUnsupported) {
        return 21;
    }
    if (const Result result = registry.bind_feature(
            make_binding(FeatureId("ConformanceRenderFeature"), conformance_pipeline, kLogicPoint));
        result || result.code != ResultCode::kUnsupported) {
        return 22;
    }

    aeg::pipeline::PipelineCompiler compiler;
    const auto missing_capability_plan = compiler.compile({
        .pipeline = conformance_pipeline,
        .scope = PipelineScope::kEngine,
        .available_capabilities = 0,
        .registry = &registry,
    });
    if (missing_capability_plan || missing_capability_plan.result.code != ResultCode::kUnsupported) {
        return 23;
    }

    const auto compiled = compiler.compile({
        .pipeline = conformance_pipeline,
        .scope = PipelineScope::kEngine,
        .available_capabilities = kRequiredCapabilities,
        .registry = &registry,
    });
    if (!compiled || compiled.value.phase_revision != phase_snapshot.value.revision ||
        compiled.value.nodes.size() != 9 || compiled.value.feature_contributions.size() != 4 ||
        compiled.value.feature_implementations.size() != 4 || compiled.value.feature_owners.size() != 3 ||
        compiled.value.artifacts.size() != 2 ||
        compiled.value.topological_order.size() != compiled.value.nodes.size()) {
        return 24;
    }

    const auto find_node_index = [&](StageId stage_id) -> uint32_t {
        for (uint32_t index = 0; index < compiled.value.nodes.size(); ++index) {
            if (compiled.value.nodes[index].stage == stage_id) {
                return index;
            }
        }
        return aeg::pipeline::kInvalidPipelineHandleIndex;
    };
    const uint32_t custom_phase_node = find_node_index(StageId("ConformanceCustomPhaseStage"));
    const uint32_t logic_feature_node = find_node_index(StageId("ConformanceLogicFeatureStage"));
    const uint32_t render_feature_node = find_node_index(StageId("ConformanceRenderFeatureStage"));
    const uint32_t consumer_node = find_node_index(StageId("ConformanceConsumer"));
    const auto has_dependency = [&](uint32_t source, uint32_t destination) {
        return std::any_of(compiled.value.dependencies.begin(), compiled.value.dependencies.end(),
                           [source, destination](const auto& dependency) {
                               return dependency.source_node == source && dependency.destination_node == destination;
                           });
    };
    if (custom_phase_node == aeg::pipeline::kInvalidPipelineHandleIndex ||
        compiled.value.nodes[custom_phase_node].phase_id != kCustomPhase ||
        logic_feature_node == aeg::pipeline::kInvalidPipelineHandleIndex ||
        render_feature_node == aeg::pipeline::kInvalidPipelineHandleIndex ||
        consumer_node == aeg::pipeline::kInvalidPipelineHandleIndex ||
        compiled.value.nodes[logic_feature_node].execute == nullptr ||
        compiled.value.nodes[render_feature_node].execute == nullptr ||
        !has_dependency(logic_feature_node, consumer_node)) {
        return 25;
    }

    uint32_t composite_contribution_count = 0;
    uint32_t composite_part_count = 0;
    for (const auto& contribution : compiled.value.feature_contributions) {
        if (contribution.feature != FeatureId("ConformanceCompositeFeature")) {
            continue;
        }
        ++composite_contribution_count;
        if (contribution.part.is_valid() &&
            (contribution.domain == FeatureDomain::kLogic || contribution.domain == FeatureDomain::kRender)) {
            ++composite_part_count;
        }
    }
    if (composite_contribution_count != 2 || composite_part_count != 2) {
        return 26;
    }

    const FeatureBindingDescriptor denied_binding =
        make_binding(FeatureId("ConformanceUnauthorizedArtifactFeature"), conformance_pipeline, kRestrictedPoint);
    if (const Result result = registry.bind_feature(denied_binding); !result) {
        return 27;
    }
    const auto denied_plan = compiler.compile({
        .pipeline = conformance_pipeline,
        .scope = PipelineScope::kEngine,
        .available_capabilities = kRequiredCapabilities,
        .registry = &registry,
    });
    if (denied_plan || denied_plan.result.code != ResultCode::kUnsupported) {
        return 28;
    }
    if (const Result result = registry.unbind_feature(aeg::pipeline::make_feature_instance_id(
            denied_binding.feature, denied_binding.pipeline, denied_binding.scope, denied_binding.point));
        !result) {
        return 29;
    }

    PipelineScopeHost host(PipelineScope::kEngine, aeg::pipeline::PipelineInstanceId("ConformanceHost"));
    if (const Result result = host.bind_pipeline(conformance_pipeline); !result) {
        return 30;
    }
    if (const Result result = host.rebuild(registry, kRequiredCapabilities); !result) {
        return 31;
    }
    aeg::pipeline::PipelinePlanHandle initial_plan = host.active_plan_snapshot();
    if (initial_plan == nullptr || host.has_pending_plan()) {
        return 32;
    }
    const std::size_t initial_node_count = initial_plan->nodes.size();
    if (const Result result = host.artifacts().emplace_artifact<int>(kFeatureValue, kIntegerType, 0); !result) {
        return 33;
    }
    if (const Result result = host.activate({}); !result) {
        return 34;
    }

    aeg::pipeline::PipelineScheduler scheduler({1, 1, 1, 1});
    aeg::pipeline::PipelineExecutionContext context(nullptr, PipelineScope::kEngine);
    if (const Result result = host.execute(scheduler, context);
        !result || state.logic_runs.load(std::memory_order_relaxed) != 1 ||
        state.composite_logic_runs.load(std::memory_order_relaxed) != 1 ||
        state.composite_render_runs.load(std::memory_order_relaxed) != 1 ||
        state.render_runs.load(std::memory_order_relaxed) != 1 ||
        state.custom_phase_runs.load(std::memory_order_relaxed) != 1 ||
        state.consumer_runs.load(std::memory_order_relaxed) != 1 ||
        state.observed_value.load(std::memory_order_relaxed) != 42) {
        return 35;
    }
    const std::thread::id main_thread = std::this_thread::get_id();
    {
        std::lock_guard lock(state.thread_mutex);
        if (state.logic_thread == main_thread || state.composite_logic_thread == main_thread ||
            state.composite_render_thread == main_thread || state.render_thread == main_thread) {
            return 36;
        }
    }

    if (const Result result = registry.register_feature([&state] { return std::make_unique<LateLogicFeature>(state); });
        !result) {
        return 37;
    }
    const FeatureBindingDescriptor late_binding =
        make_binding(FeatureId("ConformanceLateLogicFeature"), conformance_pipeline, kLateLogicPoint);
    if (const Result result = registry.bind_feature(late_binding); !result) {
        return 38;
    }
    if (const Result result = host.rebuild(registry, kRequiredCapabilities); !result || !host.has_pending_plan()) {
        return 39;
    }
    aeg::pipeline::PipelinePlanHandle active_before_apply = host.active_plan_snapshot();
    if (active_before_apply == nullptr || active_before_apply->nodes.size() != initial_node_count) {
        return 40;
    }
    if (const Result result = host.apply_pending_plan();
        !result || host.has_pending_plan() || host.retired_plan_count() != 1) {
        return 41;
    }
    aeg::pipeline::PipelinePlanHandle active_after_apply = host.active_plan_snapshot();
    if (active_after_apply == nullptr || active_after_apply->nodes.size() <= initial_node_count) {
        return 42;
    }
    if (const Result result = host.collect_retired_plans(); !result || host.retired_plan_count() != 1) {
        return 43;
    }
    active_before_apply.reset();
    initial_plan.reset();
    if (const Result result = host.collect_retired_plans(); !result || host.retired_plan_count() != 0) {
        return 44;
    }

    aeg::pipeline::PipelineExecutionContext second_context(nullptr, PipelineScope::kEngine);
    if (const Result result = host.execute(scheduler, second_context);
        !result || state.logic_runs.load(std::memory_order_relaxed) != 2 ||
        state.consumer_runs.load(std::memory_order_relaxed) != 2 ||
        state.late_logic_runs.load(std::memory_order_relaxed) != 1) {
        return 45;
    }
    active_after_apply.reset();
    if (const Result result = host.deactivate({}); !result) {
        return 46;
    }

    const auto logic_instance = aeg::pipeline::make_feature_instance_id(logic_binding.feature, logic_binding.pipeline,
                                                                        logic_binding.scope, logic_binding.point);
    const auto composite_instance = aeg::pipeline::make_feature_instance_id(
        composite_binding.feature, composite_binding.pipeline, composite_binding.scope, composite_binding.point);
    const auto render_instance = aeg::pipeline::make_feature_instance_id(
        render_binding.feature, render_binding.pipeline, render_binding.scope, render_binding.point);
    const auto late_instance = aeg::pipeline::make_feature_instance_id(late_binding.feature, late_binding.pipeline,
                                                                       late_binding.scope, late_binding.point);
    if (const Result result = registry.unregister_feature(logic_binding.feature);
        result || result.code != ResultCode::kInvalidState) {
        return 47;
    }
    if (const Result result = registry.unregister_pipeline(conformance_pipeline);
        result || result.code != ResultCode::kInvalidState) {
        return 48;
    }
    if (const Result result = registry.unbind_feature(logic_instance); !result) {
        return 49;
    }
    if (const Result result = registry.unbind_feature(composite_instance); !result) {
        return 50;
    }
    if (const Result result = registry.unbind_feature(render_instance); !result) {
        return 51;
    }
    if (const Result result = registry.unbind_feature(late_instance); !result) {
        return 52;
    }
    if (const Result result = registry.unregister_feature(FeatureId("ConformanceLateLogicFeature")); !result) {
        return 53;
    }
    if (const Result result = registry.register_pipeline([] { return std::make_unique<LogicOnlyPipeline>(); });
        !result) {
        return 54;
    }
    if (const Result result = registry.bind_feature(make_binding(FeatureId("ConformanceRenderFeature"),
                                                                 PipelineId("ConformanceLogicOnlyPipeline"),
                                                                 FeaturePointId("ConformanceLogicOnlyRenderPoint")));
        result || result.code != ResultCode::kUnsupported) {
        return 55;
    }
    if (const Result result = registry.unregister_pipeline(PipelineId("ConformanceLogicOnlyPipeline")); !result) {
        return 56;
    }

    if (const Result result = registry.unregister_feature(FeatureId("ConformanceCompositeFeature")); !result) {
        return 57;
    }
    if (const Result result = registry.unregister_feature(FeatureId("ConformanceRenderFeature")); !result) {
        return 58;
    }
    if (const Result result = registry.unregister_feature(FeatureId("ConformanceFrameOnlyFeature")); !result) {
        return 59;
    }
    if (const Result result = registry.unregister_feature(FeatureId("ConformanceUnauthorizedArtifactFeature"));
        !result) {
        return 60;
    }
    if (const Result result = registry.unregister_feature(FeatureId("ConformanceLogicFeature")); !result) {
        return 61;
    }
    if (const Result result = registry.unregister_pipeline(conformance_pipeline); !result) {
        return 62;
    }

    if (const Result result = registry.register_pipeline([] { return std::make_unique<NoProducerPipeline>(); });
        !result) {
        return 63;
    }
    const auto no_producer = compiler.compile({
        .pipeline = PipelineId("ConformanceNoProducerPipeline"),
        .scope = PipelineScope::kEngine,
        .available_capabilities = 0,
        .registry = &registry,
    });
    if (no_producer || no_producer.result.code != ResultCode::kInvalidArgument) {
        return 64;
    }
    if (const Result result = registry.unregister_pipeline(PipelineId("ConformanceNoProducerPipeline")); !result) {
        return 65;
    }

    if (const Result result = registry.register_pipeline([] { return std::make_unique<MultipleWriterPipeline>(); });
        !result) {
        return 66;
    }
    const auto multiple_writer = compiler.compile({
        .pipeline = PipelineId("ConformanceMultipleWriterPipeline"),
        .scope = PipelineScope::kEngine,
        .available_capabilities = 0,
        .registry = &registry,
    });
    if (multiple_writer || multiple_writer.result.code != ResultCode::kInvalidArgument) {
        return 67;
    }
    if (const Result result = registry.unregister_pipeline(PipelineId("ConformanceMultipleWriterPipeline")); !result) {
        return 68;
    }

    if (const Result result = registry.register_pipeline([] { return std::make_unique<CyclePipeline>(); }); !result) {
        return 69;
    }
    const auto cycle = compiler.compile({
        .pipeline = PipelineId("ConformanceCyclePipeline"),
        .scope = PipelineScope::kEngine,
        .available_capabilities = 0,
        .registry = &registry,
    });
    if (cycle || cycle.result.code != ResultCode::kInvalidArgument) {
        return 70;
    }
    if (const Result result = registry.unregister_pipeline(PipelineId("ConformanceCyclePipeline")); !result) {
        return 71;
    }

    if (const Result result = registry.register_pipeline([] { return std::make_unique<BadOrderingPipeline>(); });
        result || result.code != ResultCode::kNotFound) {
        return 72;
    }
    if (const Result result = registry.register_pipeline([] { return std::make_unique<BadVersionPipeline>(); });
        result || result.code != ResultCode::kInvalidArgument) {
        return 73;
    }

    if (const Result result = registry.register_pipeline([] { return std::make_unique<UnregisteredPhasePipeline>(); });
        !result) {
        return 74;
    }
    const auto unregistered_phase = compiler.compile({
        .pipeline = PipelineId("ConformanceUnregisteredPhasePipeline"),
        .scope = PipelineScope::kEngine,
        .available_capabilities = 0,
        .registry = &registry,
    });
    if (unregistered_phase || unregistered_phase.result.code != ResultCode::kNotFound) {
        return 75;
    }
    if (const Result result = registry.unregister_pipeline(PipelineId("ConformanceUnregisteredPhasePipeline"));
        !result) {
        return 76;
    }
    if (const Result result = registry.phase_registry().unregister_phase(kCustomPhase); !result) {
        return 77;
    }
    return 0;
}
