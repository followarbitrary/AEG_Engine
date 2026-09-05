/**
 * @file pipeline_extension_smoke.cpp
 * @brief Exercises pipeline feature registration, binding, and composition.
 */
#include <memory>

#include <pipeline.hpp>

namespace {
    [[nodiscard]] aeg::foundation::Result feature_task(aeg::pipeline::PipelineTaskContext&) {
        return aeg::foundation::Result::success();
    }

    class ExtensionPipeline final : public aeg::pipeline::Pipeline {
    public:
        ExtensionPipeline() {
            descriptor_.id = aeg::pipeline::PipelineId("ExtensionPipeline");
            descriptor_.name = "ExtensionPipeline";
            descriptor_.category = aeg::pipeline::PipelineCategory::kCore;
            descriptor_.domains = aeg::pipeline::to_mask(aeg::pipeline::FeatureDomain::kLogic) |
                                  aeg::pipeline::to_mask(aeg::pipeline::FeatureDomain::kPhysics);
        }

        [[nodiscard]] const aeg::pipeline::PipelineDescriptor& descriptor() const noexcept override {
            return descriptor_;
        }

        [[nodiscard]] aeg::foundation::Result build(aeg::pipeline::PipelineBuilder& builder) const override {
            const auto anchor = builder.add_stage({
                aeg::pipeline::StageId("ExtensionAnchor"),
                aeg::pipeline::EnginePhase::kVariableUpdate,
                aeg::pipeline::ExecutionDomain::kMainThread,
                aeg::pipeline::TimePolicy::kFrame,
                false,
            });
            if (!anchor) {
                return builder.status();
            }

            const auto logic_point = builder.add_feature_point({
                aeg::pipeline::FeaturePointId("LogicExtensionPoint"),
                "LogicExtensionPoint",
                {},
                aeg::pipeline::StageId("ExtensionAnchor"),
                aeg::pipeline::FeaturePointPosition::kBefore,
                aeg::pipeline::to_mask(aeg::pipeline::FeatureDomain::kLogic),
                aeg::pipeline::to_mask(aeg::pipeline::PipelineScope::kEngine),
                aeg::pipeline::FeaturePointFlags::kMayCreateArtifacts,
            });
            if (!logic_point) {
                return builder.status();
            }

            const auto composite_point = builder.add_feature_point({
                aeg::pipeline::FeaturePointId("CompositeExtensionPoint"),
                "CompositeExtensionPoint",
                {},
                aeg::pipeline::StageId("ExtensionAnchor"),
                aeg::pipeline::FeaturePointPosition::kAfter,
                aeg::pipeline::to_mask(aeg::pipeline::FeatureDomain::kLogic) |
                    aeg::pipeline::to_mask(aeg::pipeline::FeatureDomain::kPhysics),
                aeg::pipeline::to_mask(aeg::pipeline::PipelineScope::kEngine),
                aeg::pipeline::FeaturePointFlags::kMayCreateArtifacts,
            });
            if (!composite_point) {
                return builder.status();
            }
            return aeg::foundation::Result::success();
        }

    private:
        aeg::pipeline::PipelineDescriptor descriptor_{};
    };

    class LogicExtensionFeature final : public aeg::pipeline::LogicFeature {
    public:
        [[nodiscard]] const aeg::pipeline::FeatureDescriptor& descriptor() const noexcept override {
            static const aeg::pipeline::FeatureDescriptor descriptor{
                aeg::pipeline::FeatureId("LogicExtensionFeature"),
                "LogicExtensionFeature",
                aeg::pipeline::FeatureDomain::kLogic,
                aeg::pipeline::to_mask(aeg::pipeline::FeatureDomain::kLogic),
                aeg::pipeline::to_mask(aeg::pipeline::PipelineScope::kEngine),
                {1, 0, 0},
                {},
            };
            return descriptor;
        }

    protected:
        [[nodiscard]] aeg::foundation::Result setup(aeg::pipeline::LogicGraphBuilder& builder) const override {
            const auto stage = builder.add_stage({
                aeg::pipeline::StageId("LogicExtensionStage"),
                aeg::pipeline::EnginePhase::kPreUpdate,
                aeg::pipeline::ExecutionDomain::kCpuWorker,
                aeg::pipeline::TimePolicy::kFrame,
                true,
            });
            const auto task = builder.add_task({
                aeg::pipeline::TaskId("LogicExtensionTask"),
                aeg::pipeline::TaskFlags::kParallelSafe,
                &feature_task,
                nullptr,
            });
            if (!stage || !task) {
                return aeg::foundation::Result::failure(aeg::foundation::ResultCode::kInvalidState,
                                                         "logic feature declaration failed");
            }
            return builder.bind_task(task, stage);
        }
    };

    class LogicPart final : public aeg::pipeline::LogicFeature {
    protected:
        [[nodiscard]] aeg::foundation::Result setup(aeg::pipeline::LogicGraphBuilder& builder) const override {
            const auto stage = builder.add_stage({
                aeg::pipeline::StageId("CompositeLogicStage"),
                aeg::pipeline::EnginePhase::kVariableUpdate,
                aeg::pipeline::ExecutionDomain::kCpuWorker,
                aeg::pipeline::TimePolicy::kFrame,
                true,
            });
            const auto task = builder.add_task({
                aeg::pipeline::TaskId("CompositeLogicTask"),
                aeg::pipeline::TaskFlags::kParallelSafe,
                &feature_task,
                nullptr,
            });
            if (!stage || !task) {
                return aeg::foundation::Result::failure(aeg::foundation::ResultCode::kInvalidState,
                                                         "composite logic declaration failed");
            }
            return builder.bind_task(task, stage);
        }
    };

    class PhysicsPart final : public aeg::pipeline::PhysicsFeature {
    protected:
        [[nodiscard]] aeg::foundation::Result setup(aeg::pipeline::PhysicsGraphBuilder& builder) const override {
            const auto stage = builder.add_stage({
                aeg::pipeline::StageId("CompositePhysicsStage"),
                aeg::pipeline::EnginePhase::kVariableUpdate,
                aeg::pipeline::ExecutionDomain::kCpuWorker,
                aeg::pipeline::TimePolicy::kFrame,
                true,
            });
            const auto task = builder.add_task({
                aeg::pipeline::TaskId("CompositePhysicsTask"),
                aeg::pipeline::TaskFlags::kParallelSafe,
                &feature_task,
                nullptr,
            });
            if (!stage || !task) {
                return aeg::foundation::Result::failure(aeg::foundation::ResultCode::kInvalidState,
                                                         "composite physics declaration failed");
            }
            return builder.bind_task(task, stage);
        }
    };

    class CompositeExtensionFeature final
        : public aeg::pipeline::CompositeFeature<LogicPart, PhysicsPart> {
    public:
        [[nodiscard]] const aeg::pipeline::FeatureDescriptor& descriptor() const noexcept override {
            static const aeg::pipeline::FeatureDescriptor descriptor{
                aeg::pipeline::FeatureId("CompositeExtensionFeature"),
                "CompositeExtensionFeature",
                aeg::pipeline::FeatureDomain::kComposite,
                kDomainMask,
                aeg::pipeline::to_mask(aeg::pipeline::PipelineScope::kEngine),
                {1, 0, 0},
                {},
            };
            return descriptor;
        }
    };

    class RenderOnlyFeature final : public aeg::pipeline::RenderFeature {
    public:
        [[nodiscard]] const aeg::pipeline::FeatureDescriptor& descriptor() const noexcept override {
            static const aeg::pipeline::FeatureDescriptor descriptor{
                aeg::pipeline::FeatureId("RenderOnlyFeature"),
                "RenderOnlyFeature",
                aeg::pipeline::FeatureDomain::kRender,
                aeg::pipeline::to_mask(aeg::pipeline::FeatureDomain::kRender),
                aeg::pipeline::to_mask(aeg::pipeline::PipelineScope::kEngine),
                {1, 0, 0},
                {},
            };
            return descriptor;
        }

    protected:
        [[nodiscard]] aeg::foundation::Result setup(aeg::pipeline::RenderGraphBuilder&) const override {
            return aeg::foundation::Result::success();
        }
    };
}  // namespace

int main() {
    aeg::pipeline::PipelineRegistry registry;
    if (const auto result = registry.register_pipeline([] { return std::make_unique<ExtensionPipeline>(); });
        !result) {
        return 1;
    }
    if (const auto result = registry.register_feature([] { return std::make_unique<LogicExtensionFeature>(); });
        !result) {
        return 2;
    }
    if (const auto result = registry.register_feature([] { return std::make_unique<CompositeExtensionFeature>(); });
        !result) {
        return 3;
    }
    if (const auto result = registry.register_feature([] { return std::make_unique<RenderOnlyFeature>(); }); !result) {
        return 4;
    }

    if (const auto result = registry.bind_feature({
            aeg::pipeline::FeatureId("LogicExtensionFeature"),
            aeg::pipeline::PipelineId("ExtensionPipeline"),
            aeg::pipeline::PipelineScope::kEngine,
            aeg::pipeline::FeaturePointId("LogicExtensionPoint"),
            0,
            true,
            {},
        });
        !result) {
        return 5;
    }
    if (const auto result = registry.bind_feature({
            aeg::pipeline::FeatureId("CompositeExtensionFeature"),
            aeg::pipeline::PipelineId("ExtensionPipeline"),
            aeg::pipeline::PipelineScope::kEngine,
            aeg::pipeline::FeaturePointId("CompositeExtensionPoint"),
            0,
            true,
            {},
        });
        !result) {
        return 6;
    }
    if (const auto result = registry.bind_feature({
            aeg::pipeline::FeatureId("RenderOnlyFeature"),
            aeg::pipeline::PipelineId("ExtensionPipeline"),
            aeg::pipeline::PipelineScope::kEngine,
            aeg::pipeline::FeaturePointId("LogicExtensionPoint"),
            0,
            true,
            {},
        });
        result || result.code != aeg::foundation::ResultCode::kUnsupported) {
        return 7;
    }

    const auto compiled = aeg::pipeline::PipelineCompiler{}.compile({
        .pipeline = aeg::pipeline::PipelineId("ExtensionPipeline"),
        .scope = aeg::pipeline::PipelineScope::kEngine,
        .available_capabilities = 0,
        .registry = &registry,
        .instance = aeg::pipeline::PipelineInstanceId("ExtensionInstance"),
    });
    if (!compiled) {
        return 8;
    }
    if (compiled.value.nodes.size() != 4 || compiled.value.feature_contributions.size() != 3 ||
        compiled.value.feature_implementations.size() != 3 || compiled.value.feature_owners.size() != 2 ||
        compiled.value.instance !=
                                                       aeg::pipeline::PipelineInstanceId("ExtensionInstance")) {
        return 9;
    }

    const auto find_node = [&](aeg::pipeline::StageId stage) {
        for (uint32_t index = 0; index < compiled.value.nodes.size(); ++index) {
            if (compiled.value.nodes[index].stage == stage) {
                return index;
            }
        }
        return aeg::pipeline::kInvalidPipelineHandleIndex;
    };
    const auto has_dependency = [&](aeg::pipeline::StageId source, aeg::pipeline::StageId destination) {
        const uint32_t source_node = find_node(source);
        const uint32_t destination_node = find_node(destination);
        for (const aeg::pipeline::PipelineDependency& dependency : compiled.value.dependencies) {
            if (dependency.source_node == source_node && dependency.destination_node == destination_node) {
                return true;
            }
        }
        return false;
    };
    if (!has_dependency(aeg::pipeline::StageId("LogicExtensionStage"),
                        aeg::pipeline::StageId("ExtensionAnchor")) ||
        !has_dependency(aeg::pipeline::StageId("ExtensionAnchor"),
                        aeg::pipeline::StageId("CompositeLogicStage")) ||
        !has_dependency(aeg::pipeline::StageId("ExtensionAnchor"),
                        aeg::pipeline::StageId("CompositePhysicsStage"))) {
        return 10;
    }

    uint32_t composite_part_count = 0;
    for (const aeg::pipeline::PipelineFeatureContributionRecord& contribution :
         compiled.value.feature_contributions) {
        if (contribution.feature == aeg::pipeline::FeatureId("CompositeExtensionFeature")) {
            if (!contribution.part.is_valid()) {
                return 11;
            }
            ++composite_part_count;
        }
    }
    if (composite_part_count != 2) {
        return 12;
    }

    aeg::pipeline::PipelineScopeHost host(
        aeg::pipeline::PipelineScope::kEngine,
        aeg::pipeline::PipelineInstanceId("ExtensionHost"));
    if (const auto result = host.bind_pipeline(aeg::pipeline::PipelineId("ExtensionPipeline")); !result) {
        return 13;
    }
    if (const auto result = host.rebuild(registry); !result) {
        return 14;
    }
    if (const auto result = host.activate({}); !result) {
        return 15;
    }
    aeg::pipeline::PipelineExecutionContext execution_context(nullptr, aeg::pipeline::PipelineScope::kEngine);
    aeg::pipeline::PipelineScheduler scheduler(2);
    if (const auto result = host.execute(scheduler, execution_context); !result) {
        return 16;
    }
    if (const auto result = host.deactivate({}); !result) {
        return 17;
    }

    const aeg::pipeline::FeatureInstanceId logic_instance = aeg::pipeline::make_feature_instance_id(
        aeg::pipeline::FeatureId("LogicExtensionFeature"),
        aeg::pipeline::PipelineId("ExtensionPipeline"),
        aeg::pipeline::PipelineScope::kEngine,
        aeg::pipeline::FeaturePointId("LogicExtensionPoint"));
    if (const auto result = registry.unregister_feature(aeg::pipeline::FeatureId("LogicExtensionFeature"));
        result || result.code != aeg::foundation::ResultCode::kInvalidState) {
        return 18;
    }
    if (const auto result = registry.unbind_feature(logic_instance); !result) {
        return 19;
    }
    if (const auto result = registry.unregister_feature(aeg::pipeline::FeatureId("LogicExtensionFeature")); !result) {
        return 20;
    }

    return 0;
}
