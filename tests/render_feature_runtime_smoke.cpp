/**
 * @file render_feature_runtime_smoke.cpp
 * @brief Exercises RenderFeature plan snapshots and RenderGraph runtime adaptation.
 */
#include <memory>
#include <array>

#include <pipeline.hpp>
#include <render_feature_runtime.hpp>
#include <render_graph.hpp>
#include <rhi.hpp>

namespace {
    struct RuntimeFeatureUserData {
        bool fail_after_graph_build{false};
    };

    using aeg::foundation::Result;
    using aeg::foundation::ResultCode;
    using aeg::pipeline::FeatureDomain;
    using aeg::pipeline::FeaturePointFlags;
    using aeg::pipeline::PipelineScope;
    using aeg::rhi::Device;
    using aeg::rhi::DeviceCreateDesc;
    using aeg::rhi::Format;
    using aeg::rhi::MemoryClass;
    using aeg::rhi::PipelineStage;
    using aeg::rhi::QueueClass;
    using aeg::rhi::ResourceUsage;
    using aeg::rhi::ShaderStage;
    using aeg::rhi::TextureCapability;
    using aeg::rhi::TextureDesc;

    class RuntimeRenderPipeline final : public aeg::pipeline::Pipeline {
    public:
        RuntimeRenderPipeline() {
            descriptor_.id = aeg::pipeline::PipelineId("RuntimeRenderPipeline");
            descriptor_.name = "RuntimeRenderPipeline";
            descriptor_.category = aeg::pipeline::PipelineCategory::kRendering;
            descriptor_.domains = aeg::pipeline::to_mask(FeatureDomain::kRender);
            descriptor_.supported_scopes = aeg::pipeline::to_mask(PipelineScope::kFrame);
        }

        [[nodiscard]] const aeg::pipeline::PipelineDescriptor& descriptor() const noexcept override {
            return descriptor_;
        }

        [[nodiscard]] Result build(aeg::pipeline::PipelineBuilder& builder) const override {
            const auto anchor = builder.add_stage({
                aeg::pipeline::StageId("RuntimeRenderAnchor"),
                aeg::pipeline::EnginePhase::kRenderBuild,
                aeg::pipeline::ExecutionDomain::kMainThread,
                aeg::pipeline::TimePolicy::kFrame,
                false,
            });
            if (!anchor) {
                return builder.status();
            }

            const auto point = builder.add_feature_point({
                aeg::pipeline::FeaturePointId("RuntimeRenderPoint"),
                "RuntimeRenderPoint",
                {},
                aeg::pipeline::StageId("RuntimeRenderAnchor"),
                aeg::pipeline::FeaturePointPosition::kBefore,
                aeg::pipeline::to_mask(FeatureDomain::kRender),
                aeg::pipeline::to_mask(PipelineScope::kFrame),
                FeaturePointFlags::kMayCreateArtifacts |
                    FeaturePointFlags::kMayCreateComputePass |
                    FeaturePointFlags::kMayWriteFinalOutput,
            });
            return point ? Result::success() : builder.status();
        }

    private:
        aeg::pipeline::PipelineDescriptor descriptor_{};
    };

    class RuntimeRenderFeature final : public aeg::pipeline::RenderFeature {
    public:
        [[nodiscard]] const aeg::pipeline::FeatureDescriptor& descriptor() const noexcept override {
            static const aeg::pipeline::FeatureDescriptor descriptor{
                aeg::pipeline::FeatureId("RuntimeRenderFeature"),
                "RuntimeRenderFeature",
                FeatureDomain::kRender,
                aeg::pipeline::to_mask(FeatureDomain::kRender),
                aeg::pipeline::to_mask(PipelineScope::kFrame),
                {1, 0, 0},
                {},
            };
            return descriptor;
        }

        [[nodiscard]] Result build_render_graph(aeg::pipeline::RenderGraphBuilder& builder) const override {
            if (!builder.is_runtime() || builder.scope() != PipelineScope::kFrame) {
                return Result::failure(ResultCode::kInvalidState,
                                       "runtime render feature received an invalid builder");
            }

            const auto input = builder.find_texture(
                aeg::pipeline::RenderResourceId("RuntimeHostInput"));
            if (!input) {
                return input.result;
            }

            TextureDesc output_desc{};
            output_desc.extent = {4, 4, 1};
            output_desc.format = Format::kRgba8Unorm;
            output_desc.capabilities = TextureCapability::kStorage;
            output_desc.memory = MemoryClass::kDeviceLocal;

            const auto output = builder.create_texture(
                aeg::pipeline::RenderResourceId("RuntimeRenderOutput"),
                output_desc,
                aeg::pipeline::RenderGraphResourceLifetime::kTransient);
            if (!output) {
                return output.result;
            }

            const auto pass = builder.add_compute_pass(
                "RuntimeRenderPass",
                [input, output](aeg::pipeline::RenderPassBuilder& pass_builder) {
                    if (const Result result = pass_builder.read(
                            input.value,
                            ResourceUsage::kStorageRead,
                            PipelineStage::kCompute,
                            ShaderStage::kCompute);
                        !result) {
                        return result;
                    }
                    return pass_builder.write(
                            output.value,
                            ResourceUsage::kStorageWrite,
                            PipelineStage::kCompute,
                            ShaderStage::kCompute);
                },
                [](aeg::pipeline::RenderPassExecutionContext&) {
                    return Result::success();
                },
                QueueClass::kGraphics);
            if (!pass) {
                return pass.result;
            }

            const Result export_result = builder.export_texture(output.value);
            if (!export_result) {
                return export_result;
            }
            if (const auto* user_data = static_cast<const RuntimeFeatureUserData*>(builder.user_data());
                user_data != nullptr && user_data->fail_after_graph_build) {
                return Result::failure(ResultCode::kInvalidState,
                                       "runtime render feature requested a build failure");
            }
            return Result::success();
        }

    protected:
        [[nodiscard]] Result setup(aeg::pipeline::RenderGraphBuilder& builder) const override {
            const auto stage = builder.add_stage({
                aeg::pipeline::StageId("RuntimeRenderFeatureStage"),
                aeg::pipeline::EnginePhase::kRenderBuild,
                aeg::pipeline::ExecutionDomain::kMainThread,
                aeg::pipeline::TimePolicy::kFrame,
                false,
            });
            return stage ? Result::success() : Result::failure(
                ResultCode::kInvalidState,
                "runtime render feature declaration failed");
        }
    };
}  // namespace

int main() {
    DeviceCreateDesc device_desc{};
    device_desc.application_name = "AEG RenderFeature Runtime Smoke";
    auto device_result = aeg::rhi::create_device(device_desc);
    if (!device_result) {
        return 1;
    }
    std::unique_ptr<Device> device = std::move(device_result.value);

    aeg::pipeline::PipelineRegistry registry;
    if (const Result result = registry.register_pipeline(
            [] { return std::make_unique<RuntimeRenderPipeline>(); });
        !result) {
        return 2;
    }
    if (const Result result = registry.register_feature(
            [] { return std::make_unique<RuntimeRenderFeature>(); });
        !result) {
        return 3;
    }
    if (const Result result = registry.bind_feature({
            aeg::pipeline::FeatureId("RuntimeRenderFeature"),
            aeg::pipeline::PipelineId("RuntimeRenderPipeline"),
            PipelineScope::kFrame,
            aeg::pipeline::FeaturePointId("RuntimeRenderPoint"),
            0,
            true,
            {},
        });
        !result) {
        return 4;
    }

    const auto compiled = aeg::pipeline::PipelineCompiler{}.compile({
        .pipeline = aeg::pipeline::PipelineId("RuntimeRenderPipeline"),
        .scope = PipelineScope::kFrame,
        .available_capabilities = 0,
        .registry = &registry,
        .instance = aeg::pipeline::PipelineInstanceId("RuntimeRenderInstance"),
    });
    if (!compiled || compiled.value.feature_contributions.size() != 1 ||
        compiled.value.feature_implementations.size() != 1 ||
        compiled.value.feature_implementations.front().implementation == nullptr) {
        return 5;
    }

    aeg::pipeline::PipelinePlan plan = std::move(compiled.value);
    const aeg::pipeline::FeatureInstanceId feature_instance = aeg::pipeline::make_feature_instance_id(
        aeg::pipeline::FeatureId("RuntimeRenderFeature"),
        aeg::pipeline::PipelineId("RuntimeRenderPipeline"),
        PipelineScope::kFrame,
        aeg::pipeline::FeaturePointId("RuntimeRenderPoint"));
    if (const Result result = registry.unbind_feature(feature_instance); !result) {
        return 6;
    }
    if (const Result result = registry.unregister_feature(
            aeg::pipeline::FeatureId("RuntimeRenderFeature"));
        !result) {
        return 7;
    }

    aeg::render_graph::RenderGraph graph(*device);
    TextureDesc input_desc{};
    input_desc.extent = {4, 4, 1};
    input_desc.format = Format::kRgba8Unorm;
    input_desc.capabilities = TextureCapability::kStorage;
    input_desc.memory = MemoryClass::kDeviceLocal;
    const auto input_result = graph.create_texture(
        input_desc, aeg::render_graph::ResourceLifetime::kPersistent);
    if (!input_result) {
        return 8;
    }

    aeg::render_graph::RenderGraph foreign_graph(*device);
    const auto foreign_input_result = foreign_graph.create_texture(
        input_desc, aeg::render_graph::ResourceLifetime::kPersistent);
    if (!foreign_input_result) {
        return 9;
    }
    const std::array<aeg::render_graph::RenderGraphFeatureTextureBinding, 1> foreign_bindings{{
        {aeg::pipeline::RenderResourceId("RuntimeHostInput"), foreign_input_result.value},
    }};
    const aeg::render_graph::RenderGraphFeatureRuntimeDesc foreign_runtime_desc{
        {},
        foreign_bindings,
        nullptr,
    };
    aeg::render_graph::RenderGraphFeatureRuntime foreign_runtime(
        graph, plan, registry, foreign_runtime_desc);
    if (const Result result = foreign_runtime.build();
        result || result.code != ResultCode::kNotFound) {
        return 10;
    }

    const std::array<aeg::render_graph::RenderGraphFeatureTextureBinding, 1> input_bindings{{
        {aeg::pipeline::RenderResourceId("RuntimeHostInput"), input_result.value},
    }};
    const aeg::render_graph::RenderGraphFeatureRuntimeDesc runtime_desc{
        {},
        input_bindings,
        nullptr,
    };
    aeg::render_graph::RenderGraphFeatureRuntime runtime(graph, plan, registry, runtime_desc);
    if (const Result result = runtime.build(); !result) {
        return 11;
    }
    if (const Result result = runtime.build(); result || result.code != ResultCode::kInvalidState) {
        return 12;
    }
    if (const Result result = graph.compile(); !result) {
        return 13;
    }
    if (graph.compile_info().pass_count != 1) {
        return 14;
    }
    if (graph.compile_info().culled_pass_count != 0) {
        return 15;
    }
    if (graph.compile_info().barrier_count == 0) {
        return 16;
    }

    aeg::render_graph::RenderGraph rollback_graph(*device);
    const auto rollback_input_result = rollback_graph.create_texture(
        input_desc, aeg::render_graph::ResourceLifetime::kPersistent);
    if (!rollback_input_result) {
        return 17;
    }
    const std::array<aeg::render_graph::RenderGraphFeatureTextureBinding, 1> rollback_bindings{{
        {aeg::pipeline::RenderResourceId("RuntimeHostInput"), rollback_input_result.value},
    }};
    RuntimeFeatureUserData failure_data{true};
    const aeg::render_graph::RenderGraphFeatureRuntimeDesc rollback_runtime_desc{
        {},
        rollback_bindings,
        &failure_data,
    };
    aeg::render_graph::RenderGraphFeatureRuntime rollback_runtime(
        rollback_graph, plan, registry, rollback_runtime_desc);
    if (const Result result = rollback_runtime.build();
        result || result.code != ResultCode::kInvalidState) {
        return 18;
    }
    if (!rollback_graph.contains_texture(rollback_input_result.value)) {
        return 19;
    }
    if (const Result result = rollback_graph.compile(); !result ||
        rollback_graph.compile_info().resource_count != 1 ||
        rollback_graph.compile_info().pass_count != 0) {
        return 20;
    }

    failure_data.fail_after_graph_build = false;
    if (const Result result = rollback_runtime.build(); !result) {
        return 21;
    }
    if (const Result result = rollback_graph.compile(); !result ||
        rollback_graph.compile_info().resource_count != 2 ||
        rollback_graph.compile_info().pass_count != 1) {
        return 22;
    }

    aeg::render_graph::RenderGraph no_op_graph(*device);
    const auto no_op_texture = no_op_graph.create_texture(
        input_desc, aeg::render_graph::ResourceLifetime::kPersistent);
    if (!no_op_texture || !no_op_graph.export_texture(no_op_texture.value)) {
        return 23;
    }
    if (const Result result = no_op_graph.compile(); !result) {
        return 24;
    }
    const uint64_t no_op_plan_revision = no_op_graph.plan().revision;
    aeg::pipeline::PipelinePlan no_op_plan{};
    no_op_plan.revision = 1;
    no_op_plan.pipeline = aeg::pipeline::PipelineId("NoOpRenderPipeline");
    no_op_plan.scope = PipelineScope::kFrame;
    aeg::pipeline::PipelineRegistry no_op_registry;
    aeg::render_graph::RenderGraphFeatureRuntime no_op_runtime(
        no_op_graph, no_op_plan, no_op_registry);
    if (const Result result = no_op_runtime.build(); !result ||
        no_op_graph.plan().revision != no_op_plan_revision ||
        no_op_graph.compile_info().resource_count != 1) {
        return 25;
    }

    auto retained_plan = std::make_shared<const aeg::pipeline::PipelinePlan>(plan);
    std::weak_ptr<const aeg::pipeline::PipelinePlan> weak_retained_plan = retained_plan;
    aeg::render_graph::RenderGraph retained_graph(*device);
    const auto retained_input_result = retained_graph.create_texture(
        input_desc, aeg::render_graph::ResourceLifetime::kPersistent);
    if (!retained_input_result) {
        return 26;
    }
    const std::array<aeg::render_graph::RenderGraphFeatureTextureBinding, 1> retained_bindings{{
        {aeg::pipeline::RenderResourceId("RuntimeHostInput"), retained_input_result.value},
    }};
    const aeg::render_graph::RenderGraphFeatureRuntimeDesc retained_runtime_desc{
        {}, retained_bindings, nullptr,
    };
    aeg::render_graph::RenderGraphFeatureRuntime retained_runtime(
        retained_graph, retained_plan, registry, retained_runtime_desc);
    if (const Result result = retained_runtime.build(); !result ||
        !retained_graph.compile()) {
        return 27;
    }

    aeg::render_graph::FrameContext frame(*device, {.frames_in_flight = 2});
    if (const Result result = frame.begin_frame(); !result) {
        return 28;
    }
    retained_plan.reset();
    const auto execution = retained_graph.execute(frame);
    if (!execution || !frame.end_frame() || weak_retained_plan.expired()) {
        return 29;
    }
    if (!frame.begin_frame() || weak_retained_plan.expired() || !frame.end_frame()) {
        return 30;
    }
    if (!frame.begin_frame() || !weak_retained_plan.expired() || !frame.end_frame()) {
        return 31;
    }

    aeg::render_graph::RenderGraphExecutionLease empty_lease;
    if (!frame.begin_frame()) {
        return 32;
    }
    aeg::render_graph::RenderGraphExecuteDesc invalid_execute_desc{};
    invalid_execute_desc.execution_leases = {&empty_lease, 1};
    const auto invalid_execution = retained_graph.execute(frame, invalid_execute_desc);
    if (invalid_execution || invalid_execution.result.code != ResultCode::kInvalidArgument) {
        return 33;
    }

    auto leased_plan = std::make_shared<const aeg::pipeline::PipelinePlan>(plan);
    std::weak_ptr<const aeg::pipeline::PipelinePlan> weak_leased_plan = leased_plan;
    aeg::render_graph::RenderGraphExecutionLease plan_lease(leased_plan);
    leased_plan.reset();
    if (!frame.begin_frame()) {
        return 34;
    }
    aeg::render_graph::RenderGraphExecuteDesc execute_desc{};
    execute_desc.execution_leases = {&plan_lease, 1};
    const auto leased_execution = retained_graph.execute(frame, execute_desc);
    if (!leased_execution || !frame.end_frame() || weak_leased_plan.expired()) {
        return 35;
    }
    plan_lease = {};
    if (!frame.begin_frame() || weak_leased_plan.expired() || !frame.end_frame()) {
        return 36;
    }
    if (!frame.begin_frame() || !weak_leased_plan.expired() || !frame.end_frame()) {
        return 37;
    }
    return 0;
}
