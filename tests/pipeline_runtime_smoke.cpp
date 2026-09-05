/**
 * @file pipeline_runtime_smoke.cpp
 * @brief Exercises runtime artifact access and cross-scope snapshot bridging.
 */
#include <memory>

#include <pipeline.hpp>

namespace {
    constexpr aeg::pipeline::ArtifactTypeId kIntegerType{"RuntimeInteger"};
    constexpr aeg::pipeline::ArtifactTypeId kSnapshotType{"WorldSnapshot"};

    struct ExecutionState {
        int observed_value{0};
        int observed_snapshot{0};
    };

    [[nodiscard]] aeg::foundation::Result write_runtime_value(aeg::pipeline::PipelineTaskContext& context) {
        auto writer = context.write_artifact(aeg::pipeline::ArtifactId("RuntimeValue"), kIntegerType);
        if (!writer) {
            return writer.result;
        }
        int* value = writer.value.as<int>(kIntegerType);
        if (value == nullptr) {
            return aeg::foundation::Result::failure(aeg::foundation::ResultCode::kInvalidState,
                                                    "runtime artifact writer type is invalid");
        }
        *value = 42;
        return writer.value.commit();
    }

    [[nodiscard]] aeg::foundation::Result read_runtime_value(aeg::pipeline::PipelineTaskContext& context) {
        auto view = context.read_artifact(aeg::pipeline::ArtifactId("RuntimeValue"), kIntegerType);
        if (!view) {
            return view.result;
        }
        const int* value = view.value.as<int>(kIntegerType);
        auto* state = static_cast<ExecutionState*>(context.execution_context().user_data());
        if (value == nullptr || state == nullptr) {
            return aeg::foundation::Result::failure(aeg::foundation::ResultCode::kInvalidState,
                                                    "runtime artifact view is invalid");
        }
        state->observed_value = *value;
        return aeg::foundation::Result::success();
    }

    [[nodiscard]] aeg::foundation::Result read_world_snapshot(aeg::pipeline::PipelineTaskContext& context) {
        auto view = context.read_artifact(aeg::pipeline::ArtifactId("WorldSnapshot"), kSnapshotType);
        if (!view) {
            return view.result;
        }
        const int* value = view.value.as<int>(kSnapshotType);
        auto* state = static_cast<ExecutionState*>(context.execution_context().user_data());
        if (value == nullptr || state == nullptr || view.value.owner_scope() != aeg::pipeline::PipelineScope::kWorld) {
            return aeg::foundation::Result::failure(aeg::foundation::ResultCode::kInvalidState,
                                                    "world snapshot view is invalid");
        }
        state->observed_snapshot = *value;
        return aeg::foundation::Result::success();
    }

    [[nodiscard]] aeg::foundation::Result publish_world_snapshot(aeg::pipeline::PipelineTaskContext& context) {
        std::shared_ptr<const int> value = std::make_shared<const int>(7);
        return context.publish_artifact(
            aeg::pipeline::ArtifactId("WorldSnapshot"),
            aeg::pipeline::ArtifactPayload::owned(kSnapshotType, std::move(value), false));
    }

    class RuntimePipeline final : public aeg::pipeline::Pipeline {
    public:
        RuntimePipeline() {
            descriptor_.id = aeg::pipeline::PipelineId("RuntimePipeline");
            descriptor_.name = "RuntimePipeline";
            descriptor_.category = aeg::pipeline::PipelineCategory::kCore;
        }

        [[nodiscard]] const aeg::pipeline::PipelineDescriptor& descriptor() const noexcept override {
            return descriptor_;
        }

        [[nodiscard]] aeg::foundation::Result build(aeg::pipeline::PipelineBuilder& builder) const override {
            const auto write_stage = builder.add_stage({
                aeg::pipeline::StageId("WriteRuntime"),
                aeg::pipeline::EnginePhase::kPreUpdate,
                aeg::pipeline::ExecutionDomain::kMainThread,
                aeg::pipeline::TimePolicy::kFrame,
                false,
            });
            const auto read_stage = builder.add_stage({
                aeg::pipeline::StageId("ReadRuntime"),
                aeg::pipeline::EnginePhase::kVariableUpdate,
                aeg::pipeline::ExecutionDomain::kMainThread,
                aeg::pipeline::TimePolicy::kFrame,
                false,
            });
            const auto artifact = builder.declare_artifact({
                aeg::pipeline::ArtifactId("RuntimeValue"),
                kIntegerType,
                aeg::pipeline::PipelineScope::kEngine,
                aeg::pipeline::ArtifactStorage::kValue,
                aeg::pipeline::ArtifactLifetime::kFrame,
                aeg::pipeline::OwnershipPolicy::kProducerOwned,
                1,
                aeg::pipeline::to_mask(aeg::pipeline::PipelineScope::kEngine),
                false,
            });
            if (!write_stage || !read_stage || !artifact) {
                return builder.status();
            }

            const auto write_task = builder.add_task({
                aeg::pipeline::TaskId("WriteRuntimeTask"),
                aeg::pipeline::TaskFlags::kMustRun,
                &write_runtime_value,
                nullptr,
            });
            const auto read_task = builder.add_task({
                aeg::pipeline::TaskId("ReadRuntimeTask"),
                aeg::pipeline::TaskFlags::kMustRun,
                &read_runtime_value,
                nullptr,
            });
            if (!write_task || !read_task) {
                return builder.status();
            }
            if (const auto result = builder.bind_task(write_task, write_stage); !result) {
                return result;
            }
            if (const auto result = builder.bind_task(read_task, read_stage); !result) {
                return result;
            }
            if (const auto result = builder.writes(
                    write_stage,
                    {aeg::pipeline::ArtifactId("RuntimeValue"), aeg::pipeline::ArtifactAccess::kWrite, 1,
                     aeg::pipeline::AccessFlags::kDiscardContents});
                !result) {
                return result;
            }
            return builder.reads(
                read_stage,
                {aeg::pipeline::ArtifactId("RuntimeValue"), aeg::pipeline::ArtifactAccess::kRead, 1,
                 aeg::pipeline::AccessFlags::kNone});
        }

    private:
        aeg::pipeline::PipelineDescriptor descriptor_{};
    };

    class FrameSnapshotPipeline final : public aeg::pipeline::Pipeline {
    public:
        FrameSnapshotPipeline() {
            descriptor_.id = aeg::pipeline::PipelineId("FrameSnapshotPipeline");
            descriptor_.name = "FrameSnapshotPipeline";
            descriptor_.category = aeg::pipeline::PipelineCategory::kRendering;
            descriptor_.supported_scopes = aeg::pipeline::to_mask(aeg::pipeline::PipelineScope::kFrame);
        }

        [[nodiscard]] const aeg::pipeline::PipelineDescriptor& descriptor() const noexcept override {
            return descriptor_;
        }

        [[nodiscard]] aeg::foundation::Result build(aeg::pipeline::PipelineBuilder& builder) const override {
            const auto stage = builder.add_stage({
                aeg::pipeline::StageId("ReadWorldSnapshot"),
                aeg::pipeline::EnginePhase::kRenderExtract,
                aeg::pipeline::ExecutionDomain::kMainThread,
                aeg::pipeline::TimePolicy::kFrame,
                false,
            });
            const auto artifact = builder.declare_artifact({
                aeg::pipeline::ArtifactId("WorldSnapshot"),
                kSnapshotType,
                aeg::pipeline::PipelineScope::kWorld,
                aeg::pipeline::ArtifactStorage::kImmutableSnapshot,
                aeg::pipeline::ArtifactLifetime::kWorld,
                aeg::pipeline::OwnershipPolicy::kSharedImmutable,
                1,
                0,
                true,
            });
            if (!stage || !artifact) {
                return builder.status();
            }
            const auto task = builder.add_task({
                aeg::pipeline::TaskId("ReadWorldSnapshotTask"),
                aeg::pipeline::TaskFlags::kMustRun,
                &read_world_snapshot,
                nullptr,
            });
            if (!task) {
                return builder.status();
            }
            if (const auto result = builder.bind_task(task, stage); !result) {
                return result;
            }
            if (const auto result = builder.expose_artifact(
                    aeg::pipeline::ArtifactId("WorldSnapshot"), aeg::pipeline::PipelineScope::kFrame);
                !result) {
                return result;
            }
            return builder.reads(
                stage,
                {aeg::pipeline::ArtifactId("WorldSnapshot"), aeg::pipeline::ArtifactAccess::kRead, 1,
                 aeg::pipeline::AccessFlags::kNone});
        }

    private:
        aeg::pipeline::PipelineDescriptor descriptor_{};
    };

    class WorldSnapshotPipeline final : public aeg::pipeline::Pipeline {
    public:
        WorldSnapshotPipeline() {
            descriptor_.id = aeg::pipeline::PipelineId("WorldSnapshotPipeline");
            descriptor_.name = "WorldSnapshotPipeline";
            descriptor_.category = aeg::pipeline::PipelineCategory::kSimulation;
            descriptor_.supported_scopes = aeg::pipeline::to_mask(aeg::pipeline::PipelineScope::kWorld);
        }

        [[nodiscard]] const aeg::pipeline::PipelineDescriptor& descriptor() const noexcept override {
            return descriptor_;
        }

        [[nodiscard]] aeg::foundation::Result build(aeg::pipeline::PipelineBuilder& builder) const override {
            const auto stage = builder.add_stage({
                aeg::pipeline::StageId("PublishWorldSnapshot"),
                aeg::pipeline::EnginePhase::kWorldCommit,
                aeg::pipeline::ExecutionDomain::kMainThread,
                aeg::pipeline::TimePolicy::kFrame,
                false,
            });
            const auto artifact = builder.declare_artifact({
                aeg::pipeline::ArtifactId("WorldSnapshot"),
                kSnapshotType,
                aeg::pipeline::PipelineScope::kWorld,
                aeg::pipeline::ArtifactStorage::kImmutableSnapshot,
                aeg::pipeline::ArtifactLifetime::kWorld,
                aeg::pipeline::OwnershipPolicy::kSharedImmutable,
                1,
                0,
                false,
            });
            if (!stage || !artifact) {
                return builder.status();
            }
            const auto task = builder.add_task({
                aeg::pipeline::TaskId("PublishWorldSnapshotTask"),
                aeg::pipeline::TaskFlags::kMustRun,
                &publish_world_snapshot,
                nullptr,
            });
            if (!task) {
                return builder.status();
            }
            if (const auto result = builder.bind_task(task, stage); !result) {
                return result;
            }
            return builder.publish_snapshot(
                stage,
                {aeg::pipeline::ArtifactId("WorldSnapshot"),
                 aeg::pipeline::ArtifactAccess::kPublishSnapshot,
                 1,
                 aeg::pipeline::AccessFlags::kNone});
        }

    private:
        aeg::pipeline::PipelineDescriptor descriptor_{};
    };
}  // namespace

int main() {
    aeg::pipeline::PipelineRegistry registry;
    if (const auto result = registry.register_pipeline([] { return std::make_unique<RuntimePipeline>(); }); !result) {
        return 1;
    }
    if (const auto result = registry.register_pipeline([] { return std::make_unique<FrameSnapshotPipeline>(); });
        !result) {
        return 2;
    }
    if (const auto result = registry.register_pipeline([] { return std::make_unique<WorldSnapshotPipeline>(); });
        !result) {
        return 3;
    }

    aeg::pipeline::PipelineScopeHost runtime_host(
        aeg::pipeline::PipelineScope::kEngine,
        aeg::pipeline::PipelineInstanceId("RuntimeHost"));
    if (const auto result = runtime_host.bind_pipeline(aeg::pipeline::PipelineId("RuntimePipeline")); !result) {
        return 4;
    }
    if (const auto result = runtime_host.rebuild(registry); !result) {
        return 5;
    }
    if (const auto result = runtime_host.artifacts().emplace_artifact<int>(
            aeg::pipeline::ArtifactId("RuntimeValue"), kIntegerType, 0);
        !result) {
        return 6;
    }

    if (const auto initial = runtime_host.artifacts().read(
            aeg::pipeline::ArtifactId("RuntimeValue"), kIntegerType);
        !initial || initial.value.revision() != 1) {
        return 7;
    }

    if (const auto result = runtime_host.activate({}); !result) {
        return 8;
    }
    ExecutionState state;
    aeg::pipeline::PipelineExecutionContext runtime_context(
        &state,
        aeg::pipeline::PipelineScope::kEngine,
        &runtime_host.artifacts());
    aeg::pipeline::PipelineScheduler scheduler(1);
    if (const auto result = runtime_host.execute(scheduler, runtime_context); !result ||
        state.observed_value != 42) {
        return 9;
    }
    const auto updated = runtime_host.artifacts().read(
        aeg::pipeline::ArtifactId("RuntimeValue"), kIntegerType);
    if (!updated || updated.value.revision() != 2) {
        return 10;
    }
    if (const auto result = runtime_host.deactivate({}); !result) {
        return 11;
    }

    aeg::pipeline::PipelineScopeHost world_host(
        aeg::pipeline::PipelineScope::kWorld,
        aeg::pipeline::PipelineInstanceId("WorldHost"));
    if (const auto result = world_host.bind_pipeline(aeg::pipeline::PipelineId("WorldSnapshotPipeline"));
        !result) {
        return 12;
    }
    if (const auto result = world_host.rebuild(registry); !result) {
        return 13;
    }
    if (const auto result = world_host.activate({}); !result) {
        return 14;
    }
    aeg::pipeline::PipelineExecutionContext world_context(nullptr, aeg::pipeline::PipelineScope::kWorld);
    if (const auto result = world_host.execute(scheduler, world_context); !result) {
        return 15;
    }

    aeg::pipeline::ArtifactScopeBridge bridge(
        aeg::pipeline::PipelineScope::kWorld,
        aeg::pipeline::PipelineScope::kFrame,
        world_host.artifacts());
    if (const auto result = bridge.expose(aeg::pipeline::ArtifactId("WorldSnapshot")); !result ||
        !bridge.capture()) {
        return 16;
    }
    const auto captured = bridge.read(aeg::pipeline::ArtifactId("WorldSnapshot"), kSnapshotType);
    const int* captured_value = captured ? captured.value.as<int>(kSnapshotType) : nullptr;
    if (!captured || captured_value == nullptr || *captured_value != 7) {
        return 17;
    }

    aeg::pipeline::PipelineScopeHost frame_host(
        aeg::pipeline::PipelineScope::kFrame,
        aeg::pipeline::PipelineInstanceId("FrameHost"));
    if (const auto result = frame_host.bind_pipeline(aeg::pipeline::PipelineId("FrameSnapshotPipeline")); !result) {
        return 18;
    }
    if (const auto result = frame_host.rebuild(registry); !result) {
        return 19;
    }
    frame_host.set_scope_bridge(&bridge);
    if (const auto result = frame_host.activate({}); !result) {
        return 20;
    }
    state = {};
    aeg::pipeline::PipelineExecutionContext frame_context(&state, aeg::pipeline::PipelineScope::kFrame);
    if (const auto result = frame_host.execute(scheduler, frame_context); !result ||
        state.observed_snapshot != 7) {
        return 21;
    }
    if (const auto result = frame_host.deactivate({}); !result) {
        return 22;
    }
    if (const auto result = world_host.deactivate({}); !result) {
        return 23;
    }

    return 0;
}
