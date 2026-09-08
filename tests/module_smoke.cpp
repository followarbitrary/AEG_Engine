/**
 * @file module_smoke.cpp
 * @brief Exercises module registration transactions and dependency checks.
 */
#include <iostream>
#include <memory>

#include "module.hpp"

namespace {
using aeg::foundation::Result;
using aeg::pipeline::EnginePhase;
using aeg::pipeline::ExecutionDomain;
using aeg::pipeline::FeatureDescriptor;
using aeg::pipeline::FeatureId;
using aeg::pipeline::LogicFeature;
using aeg::pipeline::Pipeline;
using aeg::pipeline::PipelineBuilder;
using aeg::pipeline::PipelineDescriptor;
using aeg::pipeline::PipelineId;
using aeg::pipeline::PipelineScopeMask;
using aeg::pipeline::StageDescriptor;
using aeg::pipeline::StageId;
using aeg::pipeline::TaskDescriptor;
using aeg::pipeline::TaskId;

constexpr aeg::pipeline::ModuleId kBaseModule("Game.Base");
constexpr aeg::pipeline::ModuleId kDependentModule("Game.Dependent");
constexpr PipelineId kBasePipeline("Game.BasePipeline");
constexpr FeatureId kBaseFeature("Game.BaseFeature");
constexpr aeg::pipeline::PhaseId kBasePhase("Game.BasePhase");
constexpr aeg::reflection::TypeId kBaseType("Game.BaseType");

template <typename Value>
bool require(const Value& condition, const char* message) {
    if (!static_cast<bool>(condition)) {
        std::cerr << message << '\n';
    }
    return static_cast<bool>(condition);
}

Result no_op_task(aeg::pipeline::PipelineTaskContext&) {
    return Result::success();
}

class BasePipeline final : public Pipeline {
public:
    [[nodiscard]] const PipelineDescriptor& descriptor() const noexcept override { return descriptor_; }

    [[nodiscard]] Result build(PipelineBuilder& builder) const override {
        const auto stage = builder.add_stage(
            StageDescriptor{StageId("Game.BaseStage"), EnginePhase::kStartup, ExecutionDomain::kMainThread});
        const auto task = builder.add_task(TaskDescriptor{TaskId("Game.BaseTask"), {}, &no_op_task, nullptr});
        if (!stage || !task) {
            return aeg::foundation::invalid_state("module smoke pipeline declaration failed");
        }
        return builder.bind_task(task, stage);
    }

private:
    PipelineDescriptor descriptor_{PipelineDescriptor{kBasePipeline,
                                                      "Game.BasePipeline",
                                                      {1, 0, 0},
                                                      aeg::pipeline::PipelineCategory::kGameplay,
                                                      aeg::pipeline::kAllPipelineScopes,
                                                      0,
                                                      ExecutionDomain::kMainThread,
                                                      aeg::pipeline::TimePolicy::kStartup,
                                                      kBaseModule,
                                                      0}};
};

class BaseFeature final : public LogicFeature {
public:
    [[nodiscard]] const FeatureDescriptor& descriptor() const noexcept override { return descriptor_; }

protected:
    [[nodiscard]] Result setup(BuilderType&) const override { return Result::success(); }

private:
    FeatureDescriptor descriptor_{FeatureDescriptor{kBaseFeature,
                                                    "Game.BaseFeature",
                                                    aeg::pipeline::FeatureDomain::kLogic,
                                                    aeg::pipeline::to_mask(aeg::pipeline::FeatureDomain::kLogic),
                                                    aeg::pipeline::kAllPipelineScopes,
                                                    {1, 0, 0},
                                                    kBaseModule}};
};

class BaseModule final : public aeg::module::Module {
public:
    [[nodiscard]] const aeg::module::ModuleDescriptor& descriptor() const noexcept override { return descriptor_; }

    [[nodiscard]] Result register_module(aeg::module::ModuleRegistrationContext& context) override {
        if (const Result result =
                context.register_type(aeg::reflection::TypeDescriptor{kBaseType,
                                                                      "Game.BaseType",
                                                                      1,
                                                                      aeg::reflection::TypeKind::kComponent,
                                                                      sizeof(uint32_t),
                                                                      alignof(uint32_t),
                                                                      {}});
            !result) {
            return result;
        }
        if (const Result result = context.register_phase(
                aeg::pipeline::PhaseDescriptor{kBasePhase, "Game.BasePhase", aeg::pipeline::PhaseId("Startup"),
                                               aeg::pipeline::PhaseOrderRelation::kAfter});
            !result) {
            return result;
        }
        if (const Result result = context.register_feature([] { return std::make_unique<BaseFeature>(); }); !result) {
            return result;
        }
        return context.register_pipeline([] { return std::make_unique<BasePipeline>(); });
    }

    [[nodiscard]] Result unregister_module() override { return Result::success(); }

private:
    aeg::module::ModuleDescriptor descriptor_{kBaseModule, "Game.Base", {1, 0, 0}, {}};
};

class DependentModule final : public aeg::module::Module {
public:
    [[nodiscard]] const aeg::module::ModuleDescriptor& descriptor() const noexcept override { return descriptor_; }
    [[nodiscard]] Result register_module(aeg::module::ModuleRegistrationContext&) override { return Result::success(); }
    [[nodiscard]] Result unregister_module() override { return Result::success(); }

private:
    aeg::module::ModuleDescriptor descriptor_{
        kDependentModule, "Game.Dependent", {1, 0, 0}, {{kBaseModule, {1, 0, 0}}}};
};

class FailingModule final : public aeg::module::Module {
public:
    [[nodiscard]] const aeg::module::ModuleDescriptor& descriptor() const noexcept override { return descriptor_; }
    [[nodiscard]] Result register_module(aeg::module::ModuleRegistrationContext& context) override {
        if (const Result result = context.register_pipeline([] { return std::make_unique<BasePipeline>(); }); !result) {
            return result;
        }
        return aeg::foundation::Result::failure(aeg::foundation::ResultCode::kUnknown, "intentional module failure");
    }
    [[nodiscard]] Result unregister_module() override { return Result::success(); }

private:
    aeg::module::ModuleDescriptor descriptor_{aeg::pipeline::ModuleId("Game.Failing"), "Game.Failing", {1, 0, 0}, {}};
};
}  // namespace

int main() {
    aeg::pipeline::PipelineRegistry pipeline_registry;
    aeg::reflection::TypeRegistry type_registry;
    aeg::module::ModuleRegistry modules(pipeline_registry, &type_registry);

    if (!require(!modules.register_module([] { return std::make_unique<DependentModule>(); }),
                 "module dependency failure was not reported") ||
        !require(modules.register_module([] { return std::make_unique<BaseModule>(); }),
                 "base module registration failed") ||
        !require(modules.contains(kBaseModule), "registered module cannot be found") ||
        !require(pipeline_registry.find_definition(kBasePipeline), "module pipeline was not registered") ||
        !require(pipeline_registry.find_feature_descriptor(kBaseFeature), "module feature was not registered") ||
        !require(pipeline_registry.phase_registry().contains(kBasePhase), "module phase was not registered") ||
        !require(type_registry.contains(kBaseType), "module type was not registered")) {
        return 1;
    }

    if (!require(modules.register_module([] { return std::make_unique<DependentModule>(); }),
                 "dependent module registration failed") ||
        !require(!modules.unregister_module(kBaseModule), "dependent module did not block base unload") ||
        !require(modules.unregister_module(kDependentModule), "dependent module unload failed") ||
        !require(modules.unregister_module(kBaseModule), "base module unload failed") ||
        !require(!pipeline_registry.find_definition(kBasePipeline), "module pipeline survived unload") ||
        !require(!pipeline_registry.find_feature_descriptor(kBaseFeature), "module feature survived unload") ||
        !require(!pipeline_registry.phase_registry().contains(kBasePhase), "module phase survived unload") ||
        !require(!type_registry.contains(kBaseType), "module type survived unload")) {
        return 1;
    }

    if (!require(!modules.register_module([] { return std::make_unique<FailingModule>(); }),
                 "failing module was accepted") ||
        !require(!pipeline_registry.find_definition(kBasePipeline), "failed module registration was not rolled back")) {
        return 1;
    }

    return 0;
}
