/**
 * @file render_feature_runtime_smoke.cpp
 * @brief Exercises RenderFeature plan snapshots and RenderGraph runtime adaptation.
 */
#include <array>
#include <memory>
#include <pipeline.hpp>
#include <pipeline_configuration.hpp>
#include <render_feature_flow.hpp>
#include <render_feature_runtime.hpp>
#include <render_graph.hpp>
#include <render_pass.hpp>
#include <render_state_flow.hpp>
#include <rhi.hpp>
#include <vector>

namespace {
struct RuntimeFeatureUserData {
    bool fail_after_graph_build{false};
    aeg::pipeline::RenderGraphTexture observed_state_output{};
    uint32_t observed_state_layer_count{0};
    std::vector<aeg::pipeline::RenderGraphTexture> observed_state_inputs{};
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

[[nodiscard]] bool verify_present_flow_filtering() {
    aeg::pipeline::PipelinePlan plan{};
    plan.nodes.resize(4);
    plan.nodes[0].definition_stage_index = 0;
    plan.nodes[1].definition_stage_index = 1;
    plan.nodes[2].definition_stage_index = 2;
    plan.nodes[3].definition_stage_index = 3;
    plan.nodes[3].phase = aeg::pipeline::EnginePhase::kPresent;
    plan.dependencies = {
        {0, 1},
        {1, 2},
        {2, 3},
    };

    aeg::pipeline::PipelineFeatureContributionRecord connected{};
    connected.domain = FeatureDomain::kRender;
    connected.first_stage_index = 0;
    connected.stage_count = 1;
    if (!aeg::render_graph::render_feature_contribution_reaches_present(plan, connected)) {
        return false;
    }

    plan.dependencies.pop_back();
    if (aeg::render_graph::render_feature_contribution_reaches_present(plan, connected)) {
        return false;
    }

    aeg::pipeline::PipelineFeatureContributionRecord unmapped{};
    unmapped.domain = FeatureDomain::kRender;
    unmapped.first_stage_index = 99;
    unmapped.stage_count = 1;
    if (aeg::render_graph::render_feature_contribution_reaches_present(plan, unmapped)) {
        return false;
    }

    plan.nodes.pop_back();
    if (!aeg::render_graph::render_feature_contribution_reaches_present(plan, connected)) {
        return false;
    }
    return true;
}

[[nodiscard]] bool verify_render_state_layer_flow() {
    aeg::pipeline::PipelinePlan plan{};
    plan.nodes.resize(5);
    for (uint32_t index = 0; index < plan.nodes.size(); ++index) {
        plan.nodes[index].definition_stage_index = index;
    }
    plan.nodes[3].phase = aeg::pipeline::EnginePhase::kPresent;
    plan.dependencies = {
        {0, 2},
        {1, 2},
        {2, 3},
    };
    for (const uint32_t index : {0u, 1u, 2u, 4u}) {
        aeg::pipeline::PipelineFeatureContributionRecord contribution{};
        contribution.domain = aeg::pipeline::FeatureDomain::kRender;
        contribution.first_stage_index = index;
        contribution.stage_count = 1;
        plan.feature_contributions.push_back(contribution);
    }

    const auto flow = aeg::render_graph::build_render_state_flow(plan);
    const std::array<aeg::render_graph::RenderStateLinkOverride, 1> detached_present{{{2, 3, false}}};
    const auto detached_flow = aeg::render_graph::build_render_state_flow(plan, detached_present);
    const std::array<aeg::render_graph::RenderStateLinkOverride, 1> cyclic_link{{{2, 0, true}}};
    const auto cyclic_flow = aeg::render_graph::build_render_state_flow(plan, cyclic_link);
    return flow.present_node_count == 1 && !flow.has_cycle && flow.contributions.size() == 4 &&
           flow.contributions[0].reaches_present && flow.contributions[0].input_count == 0 &&
           flow.contributions[0].layer_count == 1 && flow.contributions[1].reaches_present &&
           flow.contributions[1].input_count == 0 && flow.contributions[1].layer_count == 1 &&
           flow.contributions[2].reaches_present && flow.contributions[2].input_count == 2 &&
           flow.contributions[2].layer_count == 2 &&
           flow.contributions[2].input_contributions == std::vector<uint32_t>{0, 1} &&
           flow.contributions[0].output_contributions == std::vector<uint32_t>{2} &&
           flow.contributions[1].output_contributions == std::vector<uint32_t>{2} &&
           !flow.contributions[3].reaches_present && !detached_flow.contributions[0].reaches_present &&
           !detached_flow.contributions[1].reaches_present && !detached_flow.contributions[2].reaches_present &&
           cyclic_flow.has_cycle;
}

class RuntimeRenderPipeline final : public aeg::pipeline::Pipeline {
public:
    RuntimeRenderPipeline() {
        descriptor_.id = aeg::pipeline::PipelineId("RuntimeRenderPipeline");
        descriptor_.name = "RuntimeRenderPipeline";
        descriptor_.category = aeg::pipeline::PipelineCategory::kRendering;
        descriptor_.domains = aeg::pipeline::to_mask(FeatureDomain::kRender);
        descriptor_.supported_scopes = aeg::pipeline::to_mask(PipelineScope::kFrame);
    }

    [[nodiscard]] const aeg::pipeline::PipelineDescriptor& descriptor() const noexcept override { return descriptor_; }

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
            FeaturePointFlags::kMayCreateArtifacts | FeaturePointFlags::kMayCreateComputePass |
                FeaturePointFlags::kMayWriteFinalOutput,
        });
        return point ? Result::success() : builder.status();
    }

private:
    aeg::pipeline::PipelineDescriptor descriptor_{};
};

class RuntimeRenderFeature final : public aeg::pipeline::RenderFeature {
public:
    [[nodiscard]] bool uses_render_state_output() const noexcept override { return true; }

    [[nodiscard]] const aeg::pipeline::FeatureDescriptor& descriptor() const noexcept override {
        static const aeg::pipeline::FeatureDescriptor kDescriptor{
            aeg::pipeline::FeatureId("RuntimeRenderFeature"),
            "RuntimeRenderFeature",
            FeatureDomain::kRender,
            aeg::pipeline::to_mask(FeatureDomain::kRender),
            aeg::pipeline::to_mask(PipelineScope::kFrame),
            {1, 0, 0},
            {},
        };
        return kDescriptor;
    }

    [[nodiscard]] Result build_render_graph(aeg::pipeline::RenderGraphBuilder& builder) const override {
        if (auto* user_data = static_cast<RuntimeFeatureUserData*>(builder.user_data()); user_data != nullptr) {
            user_data->observed_state_output = builder.state_output();
            user_data->observed_state_layer_count = builder.state_layer_count();
            user_data->observed_state_inputs.assign(builder.state_inputs().begin(), builder.state_inputs().end());
        }
        if (!builder.is_runtime() || builder.scope() != PipelineScope::kFrame) {
            return Result::failure(ResultCode::kInvalidState, "runtime render feature received an invalid builder");
        }

        const auto input = builder.find_texture(aeg::pipeline::RenderResourceId("RuntimeHostInput"));
        if (!input) {
            return input.result;
        }

        aeg::pipeline::RenderGraphTexture output = builder.state_output();
        if (!output.is_valid()) {
            TextureDesc output_desc{};
            output_desc.extent = {4, 4, 1};
            output_desc.format = Format::kRgba8Unorm;
            output_desc.capabilities = TextureCapability::kStorage;
            output_desc.memory = MemoryClass::kDeviceLocal;
            const auto created_output =
                builder.create_texture(aeg::pipeline::RenderResourceId("RuntimeRenderOutput"), output_desc,
                                       aeg::pipeline::RenderGraphResourceLifetime::kTransient);
            if (!created_output) {
                return created_output.result;
            }
            output = created_output.value;
        }

        const std::vector<aeg::pipeline::RenderGraphTexture> state_inputs(builder.state_inputs().begin(),
                                                                          builder.state_inputs().end());
        const auto pass = builder.add_compute_pass(
            "RuntimeRenderPass",
            [input, state_inputs, output](aeg::pipeline::RenderPassBuilder& pass_builder) {
                if (const Result result = pass_builder.read(input.value, ResourceUsage::kStorageRead,
                                                            PipelineStage::kCompute, ShaderStage::kCompute);
                    !result) {
                    return result;
                }
                for (const aeg::pipeline::RenderGraphTexture state_input : state_inputs) {
                    if (const Result result = pass_builder.read(state_input, ResourceUsage::kStorageRead,
                                                                PipelineStage::kCompute, ShaderStage::kCompute);
                        !result) {
                        return result;
                    }
                }
                return pass_builder.write(output, ResourceUsage::kStorageWrite, PipelineStage::kCompute,
                                          ShaderStage::kCompute);
            },
            [](aeg::pipeline::RenderPassExecutionContext&) { return Result::success(); }, QueueClass::kGraphics);
        if (!pass) {
            return pass.result;
        }

        if (!builder.state_output().is_valid()) {
            const Result export_result = builder.export_texture(output);
            if (!export_result) {
                return export_result;
            }
        }
        if (const auto* user_data = static_cast<const RuntimeFeatureUserData*>(builder.user_data());
            user_data != nullptr && user_data->fail_after_graph_build) {
            return Result::failure(ResultCode::kInvalidState, "runtime render feature requested a build failure");
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
        return stage ? Result::success()
                     : Result::failure(ResultCode::kInvalidState, "runtime render feature declaration failed");
    }
};

/** A real RenderFeature that reads state but deliberately creates no state output. */
class RuntimeStateConsumerFeature final : public aeg::pipeline::RenderFeature {
public:
    [[nodiscard]] const aeg::pipeline::FeatureDescriptor& descriptor() const noexcept override {
        static const aeg::pipeline::FeatureDescriptor kDescriptor{
            aeg::pipeline::FeatureId("RuntimeStateConsumerFeature"),
            "RuntimeStateConsumerFeature",
            FeatureDomain::kRender,
            aeg::pipeline::to_mask(FeatureDomain::kRender),
            aeg::pipeline::to_mask(PipelineScope::kFrame),
            {1, 0, 0},
            {},
        };
        return kDescriptor;
    }

protected:
    [[nodiscard]] Result setup(aeg::pipeline::RenderGraphBuilder& builder) const override {
        const auto stage = builder.add_stage({
            aeg::pipeline::StageId("RuntimeStateConsumerStage"),
            aeg::pipeline::EnginePhase::kRenderBuild,
            aeg::pipeline::ExecutionDomain::kMainThread,
            aeg::pipeline::TimePolicy::kFrame,
            false,
        });
        return stage ? Result::success()
                     : Result::failure(ResultCode::kInvalidState, "runtime state consumer declaration failed");
    }
};

class AuthoredPass final : public aeg::pipeline::RenderPass {
public:
    explicit AuthoredPass(std::vector<aeg::pipeline::RenderPassInstanceId>* setup_order = nullptr,
                          std::vector<aeg::pipeline::RenderPassInstanceId>* execute_order = nullptr)
        : setup_order_(setup_order), execute_order_(execute_order) {}

    [[nodiscard]] const aeg::pipeline::RenderPassDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }

    [[nodiscard]] Result setup(aeg::pipeline::RenderPassBuilder& builder,
                               const aeg::pipeline::RenderPassConfiguration& configuration) const override {
        const auto resource = builder.find_texture(aeg::pipeline::RenderResourceId("Runtime.AuthoredTexture"));
        if (!resource) {
            return resource.result;
        }
        const auto binding = configuration.bindings.find(77);
        if (binding == configuration.bindings.end() ||
            binding->second != aeg::pipeline::RenderResourceId("Runtime.AuthoredTexture").value()) {
            return Result::failure(ResultCode::kInvalidArgument, "authored resource binding was not preserved");
        }
        if (setup_order_ != nullptr) {
            setup_order_->push_back(configuration.instance);
        }
        builder.set_side_effects();
        return Result::success();
    }

    [[nodiscard]] Result execute(aeg::pipeline::RenderPassExecutionContext& command,
                                 const aeg::pipeline::RenderPassConfiguration& configuration) const override {
        const auto resource = command.find_texture(aeg::pipeline::RenderResourceId("Runtime.AuthoredTexture"));
        if (!resource) {
            return resource.result;
        }
        if (execute_order_ != nullptr) {
            execute_order_->push_back(configuration.instance);
        }
        return Result::success();
    }

private:
    std::vector<aeg::pipeline::RenderPassInstanceId>* setup_order_{};
    std::vector<aeg::pipeline::RenderPassInstanceId>* execute_order_{};
    aeg::pipeline::RenderPassDescriptor descriptor_{
        .type = aeg::pipeline::RenderPassTypeId("Runtime.AuthoredPass"),
        .owner_module = aeg::pipeline::ModuleId("Runtime.Tests"),
        .name = "Runtime.AuthoredPass",
        .version = {1, 0, 0},
        .graph_type = aeg::pipeline::RenderGraphPassType::kGraphics,
        .allowed_queue_mask = uint64_t{1} << static_cast<uint8_t>(QueueClass::kGraphics),
    };
};

class AuthoredPassFeature final : public aeg::pipeline::RenderFeature {
public:
    [[nodiscard]] const aeg::pipeline::FeatureDescriptor& descriptor() const noexcept override {
        static const aeg::pipeline::FeatureDescriptor kDescriptor{
            aeg::pipeline::FeatureId("Runtime.AuthoredPassFeature"),
            "Runtime.AuthoredPassFeature",
            FeatureDomain::kRender,
            aeg::pipeline::to_mask(FeatureDomain::kRender),
            aeg::pipeline::to_mask(PipelineScope::kFrame),
            {1, 0, 0},
            {},
        };
        return kDescriptor;
    }

    [[nodiscard]] Result build_render_graph(aeg::pipeline::RenderGraphBuilder& builder) const override {
        aeg::rhi::TextureDesc texture_desc{};
        texture_desc.extent = {1, 1, 1};
        texture_desc.format = aeg::rhi::Format::kRgba8Unorm;
        texture_desc.capabilities =
            aeg::rhi::TextureCapability::kSampled | aeg::rhi::TextureCapability::kColorAttachment;
        texture_desc.memory = aeg::rhi::MemoryClass::kDeviceLocal;
        if (const auto texture =
                builder.create_texture(aeg::pipeline::RenderResourceId("Runtime.AuthoredTexture"), texture_desc);
            !texture) {
            return texture.result;
        }
        return Result::success();
    }

protected:
    [[nodiscard]] Result setup(aeg::pipeline::RenderGraphBuilder&) const override { return Result::success(); }
};

class AuthoredCompositeFeature final
    : public aeg::pipeline::CompositeFeature<AuthoredPassFeature, AuthoredPassFeature> {
public:
    [[nodiscard]] const aeg::pipeline::FeatureDescriptor& descriptor() const noexcept override {
        static const aeg::pipeline::FeatureDescriptor kDescriptor{
            aeg::pipeline::FeatureId("Runtime.AuthoredPassFeature"),
            "Runtime.AuthoredCompositeFeature",
            FeatureDomain::kComposite,
            aeg::pipeline::to_mask(FeatureDomain::kRender),
            aeg::pipeline::to_mask(PipelineScope::kFrame),
            {1, 0, 0},
            {},
        };
        return kDescriptor;
    }
};

[[nodiscard]] bool verify_authored_pass_expansion(Device& device) {
    aeg::pipeline::RenderPassRegistry render_passes;
    std::vector<aeg::pipeline::RenderPassInstanceId> setup_order;
    std::vector<aeg::pipeline::RenderPassInstanceId> execute_order;
    if (const Result result =
            render_passes.register_pass([&] { return std::make_unique<AuthoredPass>(&setup_order, &execute_order); });
        !result) {
        return false;
    }

    constexpr aeg::pipeline::FeatureInstanceId kFeatureInstance("Runtime.AuthoredPassFeature.Instance");
    aeg::pipeline::PipelinePlan plan{};
    plan.pipeline = aeg::pipeline::PipelineId("Runtime.AuthoredPassPipeline");
    plan.scope = PipelineScope::kFrame;
    plan.revision = 1;
    plan.nodes.resize(2);
    plan.nodes[0].definition_stage_index = 0;
    plan.nodes[0].phase = aeg::pipeline::EnginePhase::kRenderBuild;
    plan.nodes[1].definition_stage_index = 1;
    plan.nodes[1].phase = aeg::pipeline::EnginePhase::kPresent;
    plan.dependencies = {{0, 1}};
    plan.feature_contributions.push_back({
        .feature_instance = kFeatureInstance,
        .feature = aeg::pipeline::FeatureId("Runtime.AuthoredPassFeature"),
        .part = {},
        .point = aeg::pipeline::FeaturePointId("Runtime.AuthoredPassPoint"),
        .domain = FeatureDomain::kRender,
        .first_stage_index = 0,
        .stage_count = 1,
        .point_flags = FeaturePointFlags::kMayCreateArtifacts | FeaturePointFlags::kMayCreateGraphicsPass |
                       FeaturePointFlags::kMayUseDedicatedQueue,
    });
    plan.feature_implementations.push_back({
        kFeatureInstance,
        aeg::pipeline::FeatureId("Runtime.AuthoredPassFeature"),
        {},
        std::make_shared<AuthoredPassFeature>(),
    });

    auto configuration = std::make_shared<aeg::pipeline::PipelineInstanceConfiguration>();
    configuration->pipeline = plan.pipeline;
    configuration->scope = PipelineScope::kFrame;
    configuration->features.push_back({
        .instance = kFeatureInstance,
        .feature = aeg::pipeline::FeatureId("Runtime.AuthoredPassFeature"),
        .point = aeg::pipeline::FeaturePointId("Runtime.AuthoredPassPoint"),
        .expand_authored_passes = true,
        .passes =
            {
                {.type = aeg::pipeline::RenderPassTypeId("Runtime.AuthoredPass"),
                 .instance = aeg::pipeline::RenderPassInstanceId("Runtime.AuthoredPass.First"),
                 .queue = QueueClass::kGraphics,
                 .enabled = true,
                 .resource_bindings = {{77, aeg::pipeline::RenderResourceId("Runtime.AuthoredTexture").value()}},
                 .source = "first"},
                {.type = aeg::pipeline::RenderPassTypeId("Runtime.AuthoredPass"),
                 .instance = aeg::pipeline::RenderPassInstanceId("Runtime.AuthoredPass.Disabled"),
                 .queue = QueueClass::kGraphics,
                 .enabled = false},
                {.type = aeg::pipeline::RenderPassTypeId("Runtime.AuthoredPass"),
                 .instance = aeg::pipeline::RenderPassInstanceId("Runtime.AuthoredPass.Second"),
                 .queue = QueueClass::kGraphics,
                 .enabled = true,
                 .resource_bindings = {{77, aeg::pipeline::RenderResourceId("Runtime.AuthoredTexture").value()}},
                 .source = "second"},
            },
    });

    aeg::render_graph::RenderGraph graph(device);
    aeg::render_graph::RenderGraphFeatureRuntimeDesc runtime_desc{};
    runtime_desc.configuration = configuration;
    runtime_desc.render_passes = &render_passes;
    aeg::pipeline::PipelineRegistry registry;
    aeg::render_graph::RenderGraphFeatureRuntime runtime(graph, plan, registry, runtime_desc);
    const Result build_result = runtime.build();
    const Result compile_result = build_result ? graph.compile() : build_result;
    if (!build_result || !compile_result || graph.compile_info().pass_count != 2 || setup_order.size() != 2 ||
        setup_order[0] != aeg::pipeline::RenderPassInstanceId("Runtime.AuthoredPass.First") ||
        setup_order[1] != aeg::pipeline::RenderPassInstanceId("Runtime.AuthoredPass.Second")) {
        return false;
    }

    configuration->features.front().passes.front().queue = QueueClass::kCompute;
    aeg::render_graph::RenderGraph invalid_graph(device);
    runtime_desc.configuration = configuration;
    aeg::pipeline::PipelineRegistry invalid_registry;
    aeg::render_graph::RenderGraphFeatureRuntime invalid_runtime(invalid_graph, plan, invalid_registry, runtime_desc);
    const Result invalid_result = invalid_runtime.build();
    const Result invalid_compile = invalid_graph.compile();
    if (invalid_result || invalid_result.code != ResultCode::kInvalidArgument || !invalid_compile ||
        invalid_graph.compile_info().pass_count != 0) {
        return false;
    }

    configuration->features.front().passes.front().queue = QueueClass::kGraphics;
    configuration->features.front().passes.front().ordered_after = {
        aeg::pipeline::RenderPassInstanceId("Runtime.AuthoredPass.Second")};
    aeg::render_graph::RenderGraph ordered_graph(device);
    aeg::pipeline::PipelineRegistry ordered_registry;
    aeg::render_graph::RenderGraphFeatureRuntime ordered_runtime(ordered_graph, plan, ordered_registry, runtime_desc);
    const Result ordered_build = ordered_runtime.build();
    const Result ordered_compile = ordered_build ? ordered_graph.compile() : ordered_build;
    if (!ordered_build || !ordered_compile || ordered_graph.plan().topological_order.size() != 2 ||
        ordered_graph.plan().topological_order.front() != 1) {
        return false;
    }
    configuration->features.front().passes.front().ordered_after.clear();

    const auto first_part = aeg::pipeline::make_feature_part_id(kFeatureInstance, 0);
    const auto second_part = aeg::pipeline::make_feature_part_id(kFeatureInstance, 1);
    configuration->features.front().passes.front().resolved_part = first_part;
    configuration->features.front().passes.back().resolved_part = second_part;
    plan.feature_contributions.front().part = second_part;
    plan.feature_implementations.front().part = second_part;
    plan.feature_implementations.front().implementation = std::make_shared<AuthoredCompositeFeature>();
    setup_order.clear();
    aeg::render_graph::RenderGraph composite_graph(device);
    aeg::pipeline::PipelineRegistry composite_registry;
    aeg::render_graph::RenderGraphFeatureRuntime composite_runtime(composite_graph, plan, composite_registry,
                                                                   runtime_desc);
    const Result composite_build = composite_runtime.build();
    const Result composite_compile = composite_build ? composite_graph.compile() : composite_build;
    return composite_build && composite_compile && composite_graph.compile_info().pass_count == 1 &&
           setup_order.size() == 1 &&
           setup_order.front() == aeg::pipeline::RenderPassInstanceId("Runtime.AuthoredPass.Second");
}
}  // namespace

int main() {
    if (!verify_present_flow_filtering()) {
        return 1;
    }
    if (!verify_render_state_layer_flow()) {
        return 2;
    }
    DeviceCreateDesc device_desc{};
    device_desc.application_name = "AEG RenderFeature Runtime Smoke";
    auto device_result = aeg::rhi::create_device(device_desc);
    if (!device_result) {
        return 1;
    }
    std::unique_ptr<Device> device = std::move(device_result.value);

    if (!verify_authored_pass_expansion(*device)) {
        return 2;
    }

    aeg::pipeline::PipelineRegistry registry;
    if (const Result result = registry.register_pipeline([] { return std::make_unique<RuntimeRenderPipeline>(); });
        !result) {
        return 2;
    }
    if (const Result result = registry.register_feature([] { return std::make_unique<RuntimeRenderFeature>(); });
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
        aeg::pipeline::FeatureId("RuntimeRenderFeature"), aeg::pipeline::PipelineId("RuntimeRenderPipeline"),
        PipelineScope::kFrame, aeg::pipeline::FeaturePointId("RuntimeRenderPoint"));
    if (const Result result = registry.unbind_feature(feature_instance); !result) {
        return 6;
    }
    if (const Result result = registry.unregister_feature(aeg::pipeline::FeatureId("RuntimeRenderFeature")); !result) {
        return 7;
    }

    aeg::pipeline::PipelinePlan disconnected_plan = plan;
    aeg::pipeline::PipelineNode present_node{};
    present_node.definition_stage_index = static_cast<uint32_t>(disconnected_plan.nodes.size());
    present_node.phase = aeg::pipeline::EnginePhase::kPresent;
    disconnected_plan.nodes.push_back(present_node);

    aeg::render_graph::RenderGraph graph(*device);
    TextureDesc input_desc{};
    input_desc.extent = {4, 4, 1};
    input_desc.format = Format::kRgba8Unorm;
    input_desc.capabilities =
        TextureCapability::kStorage | TextureCapability::kTransferSource | TextureCapability::kTransferDestination;
    input_desc.memory = MemoryClass::kDeviceLocal;
    const auto input_result = graph.create_texture(input_desc, aeg::render_graph::ResourceLifetime::kPersistent);
    if (!input_result) {
        return 8;
    }

    aeg::render_graph::RenderGraph disconnected_graph(*device);
    const auto disconnected_input_result =
        disconnected_graph.create_texture(input_desc, aeg::render_graph::ResourceLifetime::kPersistent);
    if (!disconnected_input_result) {
        return 9;
    }
    const std::array<aeg::render_graph::RenderGraphFeatureTextureBinding, 1> disconnected_bindings{{
        {aeg::pipeline::RenderResourceId("RuntimeHostInput"), disconnected_input_result.value},
    }};
    const aeg::render_graph::RenderGraphFeatureRuntimeDesc disconnected_runtime_desc{
        {},
        disconnected_bindings,
        nullptr,
    };
    aeg::render_graph::RenderGraphFeatureRuntime disconnected_runtime(disconnected_graph, disconnected_plan, registry,
                                                                      disconnected_runtime_desc);
    if (const Result result = disconnected_runtime.build(); !result || !disconnected_graph.compile() ||
                                                            disconnected_graph.compile_info().pass_count != 0 ||
                                                            disconnected_graph.compile_info().resource_count != 1) {
        return 10;
    }

    aeg::render_graph::RenderGraph foreign_graph(*device);
    const auto foreign_input_result =
        foreign_graph.create_texture(input_desc, aeg::render_graph::ResourceLifetime::kPersistent);
    if (!foreign_input_result) {
        return 11;
    }
    const std::array<aeg::render_graph::RenderGraphFeatureTextureBinding, 1> foreign_bindings{{
        {aeg::pipeline::RenderResourceId("RuntimeHostInput"), foreign_input_result.value},
    }};
    const aeg::render_graph::RenderGraphFeatureRuntimeDesc foreign_runtime_desc{
        {},
        foreign_bindings,
        nullptr,
    };
    aeg::render_graph::RenderGraphFeatureRuntime foreign_runtime(graph, plan, registry, foreign_runtime_desc);
    if (const Result result = foreign_runtime.build(); result || result.code != ResultCode::kNotFound) {
        return 12;
    }

    const std::array<aeg::render_graph::RenderGraphFeatureTextureBinding, 1> input_bindings{{
        {aeg::pipeline::RenderResourceId("RuntimeHostInput"), input_result.value},
    }};

    aeg::render_graph::RenderGraph state_graph(*device);
    const auto state_input_result =
        state_graph.create_texture(input_desc, aeg::render_graph::ResourceLifetime::kPersistent);
    if (!state_input_result) {
        return 13;
    }
    const std::array<aeg::render_graph::RenderGraphFeatureTextureBinding, 1> state_bindings{{
        {aeg::pipeline::RenderResourceId("RuntimeHostInput"), state_input_result.value},
    }};
    RuntimeFeatureUserData state_data{};
    aeg::render_graph::RenderGraphFeatureRuntimeDesc state_runtime_desc{
        {},
        state_bindings,
        &state_data,
        nullptr,
        {.enabled = true,
         .texture_desc = input_desc,
         .present_target_id = aeg::pipeline::RenderResourceId("RuntimeHostInput"),
         .present_extent = {4, 4, 1}},
    };
    aeg::render_graph::RenderGraphFeatureRuntime state_runtime(state_graph, plan, registry, state_runtime_desc);
    const Result state_build_result = state_runtime.build();
    const Result state_compile_result = state_build_result ? state_graph.compile() : state_build_result;
    if (!state_build_result || !state_data.observed_state_output.is_valid() ||
        state_data.observed_state_layer_count != 1 || !state_compile_result ||
        state_graph.compile_info().pass_count != 2) {
        return 14;
    }

    aeg::pipeline::PipelinePlan layered_plan = plan;
    layered_plan.nodes.resize(3);
    for (uint32_t index = 0; index < layered_plan.nodes.size(); ++index) {
        layered_plan.nodes[index].definition_stage_index = index;
        layered_plan.nodes[index].phase =
            index == 2 ? aeg::pipeline::EnginePhase::kPresent : aeg::pipeline::EnginePhase::kRenderBuild;
    }
    layered_plan.dependencies = {{0, 1}, {1, 2}};
    layered_plan.feature_contributions = {plan.feature_contributions.front(), plan.feature_contributions.front()};
    layered_plan.feature_contributions[0].first_stage_index = 0;
    layered_plan.feature_contributions[1].first_stage_index = 1;
    aeg::render_graph::RenderGraph layered_graph(*device);
    const auto layered_target =
        layered_graph.create_texture(input_desc, aeg::render_graph::ResourceLifetime::kPersistent);
    if (!layered_target) {
        return 15;
    }
    const std::array<aeg::render_graph::RenderGraphFeatureTextureBinding, 1> layered_bindings{{
        {aeg::pipeline::RenderResourceId("RuntimeHostInput"), layered_target.value},
    }};
    RuntimeFeatureUserData layered_data{};
    const aeg::render_graph::RenderGraphFeatureRuntimeDesc layered_runtime_desc{
        {},
        layered_bindings,
        &layered_data,
        nullptr,
        {.enabled = true,
         .texture_desc = input_desc,
         .present_target_id = aeg::pipeline::RenderResourceId("RuntimeHostInput"),
         .present_extent = {4, 4, 1}},
    };
    aeg::render_graph::RenderGraphFeatureRuntime layered_runtime(layered_graph, layered_plan, registry,
                                                                 layered_runtime_desc);
    const Result layered_build_result = layered_runtime.build();
    const Result layered_compile_result = layered_build_result ? layered_graph.compile() : layered_build_result;
    if (!layered_build_result || layered_data.observed_state_inputs.size() != 1 ||
        !layered_data.observed_state_inputs.front().is_valid() || !layered_compile_result ||
        layered_graph.compile_info().pass_count != 3) {
        return 16;
    }

    // State must pass through a non-writing RenderFeature.  This models a
    // readback/culling/effect Feature placed between two state-writing layers.
    aeg::pipeline::PipelinePlan forwarded_plan = layered_plan;
    forwarded_plan.nodes.resize(4);
    for (uint32_t index = 0; index < forwarded_plan.nodes.size(); ++index) {
        forwarded_plan.nodes[index].definition_stage_index = index;
        forwarded_plan.nodes[index].phase =
            index == 3 ? aeg::pipeline::EnginePhase::kPresent : aeg::pipeline::EnginePhase::kRenderBuild;
    }
    forwarded_plan.dependencies = {{0, 1}, {1, 2}, {2, 3}};
    aeg::pipeline::PipelineFeatureContributionRecord consumer_contribution = plan.feature_contributions.front();
    consumer_contribution.feature_instance = aeg::pipeline::FeatureInstanceId("RuntimeStateConsumerInstance");
    consumer_contribution.feature = aeg::pipeline::FeatureId("RuntimeStateConsumerFeature");
    consumer_contribution.part = aeg::pipeline::FeaturePartId("RuntimeStateConsumerPart");
    consumer_contribution.first_stage_index = 1;
    forwarded_plan.feature_contributions = {plan.feature_contributions.front(), consumer_contribution,
                                            plan.feature_contributions.front()};
    forwarded_plan.feature_contributions[0].first_stage_index = 0;
    forwarded_plan.feature_contributions[2].first_stage_index = 2;
    forwarded_plan.feature_implementations.push_back({consumer_contribution.feature_instance,
                                                      consumer_contribution.feature, consumer_contribution.part,
                                                      std::make_shared<RuntimeStateConsumerFeature>()});
    aeg::render_graph::RenderGraph forwarded_graph(*device);
    const auto forwarded_target =
        forwarded_graph.create_texture(input_desc, aeg::render_graph::ResourceLifetime::kPersistent);
    if (!forwarded_target) {
        return 17;
    }
    const std::array<aeg::render_graph::RenderGraphFeatureTextureBinding, 1> forwarded_bindings{{
        {aeg::pipeline::RenderResourceId("RuntimeHostInput"), forwarded_target.value},
    }};
    RuntimeFeatureUserData forwarded_data{};
    const aeg::render_graph::RenderGraphFeatureRuntimeDesc forwarded_runtime_desc{
        {},
        forwarded_bindings,
        &forwarded_data,
        nullptr,
        {.enabled = true,
         .texture_desc = input_desc,
         .present_target_id = aeg::pipeline::RenderResourceId("RuntimeHostInput"),
         .present_extent = {4, 4, 1}},
    };
    aeg::render_graph::RenderGraphFeatureRuntime forwarded_runtime(forwarded_graph, forwarded_plan, registry,
                                                                   forwarded_runtime_desc);
    const Result forwarded_build_result = forwarded_runtime.build();
    const Result forwarded_compile_result = forwarded_build_result ? forwarded_graph.compile() : forwarded_build_result;
    if (!forwarded_build_result || forwarded_data.observed_state_inputs.size() != 1 ||
        !forwarded_data.observed_state_inputs.front().is_valid() || !forwarded_compile_result ||
        forwarded_graph.compile_info().pass_count != 3) {
        return 17;
    }

    aeg::pipeline::PipelinePlan merged_plan = plan;
    merged_plan.nodes.resize(4);
    for (uint32_t index = 0; index < merged_plan.nodes.size(); ++index) {
        merged_plan.nodes[index].definition_stage_index = index;
        merged_plan.nodes[index].phase =
            index == 3 ? aeg::pipeline::EnginePhase::kPresent : aeg::pipeline::EnginePhase::kRenderBuild;
    }
    merged_plan.dependencies = {{0, 2}, {1, 2}, {2, 3}};
    merged_plan.feature_contributions = {plan.feature_contributions.front(), plan.feature_contributions.front(),
                                         plan.feature_contributions.front()};
    for (uint32_t index = 0; index < merged_plan.feature_contributions.size(); ++index) {
        merged_plan.feature_contributions[index].first_stage_index = index;
    }
    aeg::render_graph::RenderGraph merged_graph(*device);
    const auto merged_target =
        merged_graph.create_texture(input_desc, aeg::render_graph::ResourceLifetime::kPersistent);
    if (!merged_target) {
        return 17;
    }
    const std::array<aeg::render_graph::RenderGraphFeatureTextureBinding, 1> merged_bindings{{
        {aeg::pipeline::RenderResourceId("RuntimeHostInput"), merged_target.value},
    }};
    RuntimeFeatureUserData merged_data{};
    bool merged_compositor_called = false;
    const std::array<aeg::render_graph::RenderStateBlendOverride, 2> merged_blend_overrides{{
        {0, aeg::pipeline::RenderStateBlendMode::kAdd},
        {1, aeg::pipeline::RenderStateBlendMode::kAdd},
    }};
    const aeg::render_graph::RenderGraphFeatureRuntimeDesc merged_runtime_desc{
        {},
        merged_bindings,
        &merged_data,
        nullptr,
        {.enabled = true,
         .texture_desc = input_desc,
         .present_target_id = aeg::pipeline::RenderResourceId("RuntimeHostInput"),
         .present_extent = {4, 4, 1},
         .blend_overrides = merged_blend_overrides,
         .compose_terminal_states = [&merged_compositor_called](
                                        aeg::pipeline::RenderGraphBuilder& builder,
                                        std::span<const aeg::render_graph::RenderStateCompositeInput> inputs,
                                        aeg::pipeline::RenderGraphTexture destination) -> Result {
             merged_compositor_called = inputs.size() == 2 &&
                                        inputs[0].blend_mode == aeg::pipeline::RenderStateBlendMode::kAdd &&
                                        inputs[1].blend_mode == aeg::pipeline::RenderStateBlendMode::kAdd;
             if (!merged_compositor_called) {
                 return Result::failure(ResultCode::kInvalidArgument, "state merge inputs are invalid");
             }
             const auto pass = builder.add_copy_pass(
                 "RuntimeStateMerge",
                 [source = inputs.front().texture, destination](aeg::pipeline::RenderPassBuilder& pass_builder) {
                     if (const Result result = pass_builder.read(source, ResourceUsage::kCopySource,
                                                                 PipelineStage::kCopy, ShaderStage::kAll);
                         !result)
                         return result;
                     return pass_builder.write(destination, ResourceUsage::kCopyDestination, PipelineStage::kCopy,
                                               ShaderStage::kAll, {}, true);
                 },
                 [source = inputs.front().texture, destination](aeg::pipeline::RenderPassExecutionContext& execution) {
                     return execution.copy_texture(source, {aeg::rhi::Aspect::kColor, 0, 0, 1}, {}, destination,
                                                   {aeg::rhi::Aspect::kColor, 0, 0, 1}, {}, {4, 4, 1});
                 });
             return pass ? Result::success() : pass.result;
         }},
    };
    aeg::render_graph::RenderGraphFeatureRuntime merged_runtime(merged_graph, merged_plan, registry,
                                                                merged_runtime_desc);
    const Result merged_build_result = merged_runtime.build();
    const Result merged_compile_result = merged_build_result ? merged_graph.compile() : merged_build_result;
    if (!merged_build_result || merged_data.observed_state_inputs.size() != 2 ||
        !merged_data.observed_state_inputs[0].is_valid() || !merged_data.observed_state_inputs[1].is_valid() ||
        !merged_compositor_called || !merged_compile_result || merged_graph.compile_info().pass_count != 5) {
        return 18;
    }

    aeg::pipeline::PipelinePlan branching_plan = merged_plan;
    branching_plan.feature_contributions.resize(2);
    branching_plan.dependencies = {{0, 2}, {1, 2}, {2, 3}};
    aeg::render_graph::RenderGraph branching_graph(*device);
    const auto branching_target =
        branching_graph.create_texture(input_desc, aeg::render_graph::ResourceLifetime::kPersistent);
    if (!branching_target) {
        return 19;
    }
    const std::array<aeg::render_graph::RenderGraphFeatureTextureBinding, 1> branching_bindings{{
        {aeg::pipeline::RenderResourceId("RuntimeHostInput"), branching_target.value},
    }};
    bool terminal_compositor_called = false;
    const std::array<aeg::render_graph::RenderStateBlendOverride, 2> branching_blend_overrides{{
        {0, aeg::pipeline::RenderStateBlendMode::kAdd},
        {1, aeg::pipeline::RenderStateBlendMode::kAdd},
    }};
    const aeg::render_graph::RenderGraphFeatureRuntimeDesc branching_runtime_desc{
        {},
        branching_bindings,
        nullptr,
        nullptr,
        {.enabled = true,
         .texture_desc = input_desc,
         .present_target_id = aeg::pipeline::RenderResourceId("RuntimeHostInput"),
         .present_extent = {4, 4, 1},
         .blend_overrides = branching_blend_overrides,
         .compose_terminal_states = [&terminal_compositor_called](
                                        aeg::pipeline::RenderGraphBuilder& builder,
                                        std::span<const aeg::render_graph::RenderStateCompositeInput> inputs,
                                        aeg::pipeline::RenderGraphTexture destination) -> Result {
             terminal_compositor_called = inputs.size() == 2 &&
                                          inputs[0].blend_mode == aeg::pipeline::RenderStateBlendMode::kAdd &&
                                          inputs[1].blend_mode == aeg::pipeline::RenderStateBlendMode::kAdd;
             if (!terminal_compositor_called) {
                 return Result::failure(ResultCode::kInvalidArgument, "terminal compositor received invalid inputs");
             }
             const auto pass = builder.add_copy_pass(
                 "RuntimeTerminalComposite",
                 [source = inputs.front().texture, destination](aeg::pipeline::RenderPassBuilder& pass_builder) {
                     if (const Result result = pass_builder.read(source, ResourceUsage::kCopySource,
                                                                 PipelineStage::kCopy, ShaderStage::kAll);
                         !result) {
                         return result;
                     }
                     if (const Result result = pass_builder.write(destination, ResourceUsage::kCopyDestination,
                                                                  PipelineStage::kCopy, ShaderStage::kAll, {}, true);
                         !result) {
                         return result;
                     }
                     pass_builder.set_side_effects();
                     return Result::success();
                 },
                 [source = inputs.front().texture, destination](aeg::pipeline::RenderPassExecutionContext& execution) {
                     return execution.copy_texture(source, {aeg::rhi::Aspect::kColor, 0, 0, 1}, {}, destination,
                                                   {aeg::rhi::Aspect::kColor, 0, 0, 1}, {}, {4, 4, 1});
                 });
             return pass ? Result::success() : pass.result;
         }},
    };
    aeg::render_graph::RenderGraphFeatureRuntime branching_runtime(branching_graph, branching_plan, registry,
                                                                   branching_runtime_desc);
    const Result branching_build_result = branching_runtime.build();
    const Result branching_compile_result = branching_build_result ? branching_graph.compile() : branching_build_result;
    if (!branching_build_result || !terminal_compositor_called || !branching_compile_result ||
        branching_graph.compile_info().pass_count != 3) {
        return 20;
    }

    const aeg::render_graph::RenderGraphFeatureRuntimeDesc runtime_desc{
        {},
        input_bindings,
        nullptr,
    };
    aeg::render_graph::RenderGraphFeatureRuntime runtime(graph, plan, registry, runtime_desc);
    if (const Result result = runtime.build(); !result) {
        return 13;
    }
    if (const Result result = runtime.build(); result || result.code != ResultCode::kInvalidState) {
        return 14;
    }
    if (const Result result = graph.compile(); !result) {
        return 15;
    }
    if (graph.compile_info().pass_count != 1) {
        return 16;
    }
    if (graph.compile_info().culled_pass_count != 0) {
        return 17;
    }
    if (graph.compile_info().barrier_count == 0) {
        return 18;
    }

    aeg::render_graph::RenderGraph rollback_graph(*device);
    const auto rollback_input_result =
        rollback_graph.create_texture(input_desc, aeg::render_graph::ResourceLifetime::kPersistent);
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
    aeg::render_graph::RenderGraphFeatureRuntime rollback_runtime(rollback_graph, plan, registry,
                                                                  rollback_runtime_desc);
    if (const Result result = rollback_runtime.build(); result || result.code != ResultCode::kInvalidState) {
        return 18;
    }
    if (!rollback_graph.contains_texture(rollback_input_result.value)) {
        return 19;
    }
    if (const Result result = rollback_graph.compile();
        !result || rollback_graph.compile_info().resource_count != 1 || rollback_graph.compile_info().pass_count != 0) {
        return 20;
    }

    failure_data.fail_after_graph_build = false;
    if (const Result result = rollback_runtime.build(); !result) {
        return 21;
    }
    if (const Result result = rollback_graph.compile();
        !result || rollback_graph.compile_info().resource_count != 2 || rollback_graph.compile_info().pass_count != 1) {
        return 22;
    }

    aeg::render_graph::RenderGraph no_op_graph(*device);
    const auto no_op_texture = no_op_graph.create_texture(input_desc, aeg::render_graph::ResourceLifetime::kPersistent);
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
    aeg::render_graph::RenderGraphFeatureRuntime no_op_runtime(no_op_graph, no_op_plan, no_op_registry);
    if (const Result result = no_op_runtime.build(); !result || no_op_graph.plan().revision != no_op_plan_revision ||
                                                     no_op_graph.compile_info().resource_count != 1) {
        return 25;
    }

    auto retained_plan = std::make_shared<const aeg::pipeline::PipelinePlan>(plan);
    std::weak_ptr<const aeg::pipeline::PipelinePlan> weak_retained_plan = retained_plan;
    aeg::render_graph::RenderGraph retained_graph(*device);
    const auto retained_input_result =
        retained_graph.create_texture(input_desc, aeg::render_graph::ResourceLifetime::kPersistent);
    if (!retained_input_result) {
        return 26;
    }
    const std::array<aeg::render_graph::RenderGraphFeatureTextureBinding, 1> retained_bindings{{
        {aeg::pipeline::RenderResourceId("RuntimeHostInput"), retained_input_result.value},
    }};
    const aeg::render_graph::RenderGraphFeatureRuntimeDesc retained_runtime_desc{
        {},
        retained_bindings,
        nullptr,
    };
    aeg::render_graph::RenderGraphFeatureRuntime retained_runtime(retained_graph, retained_plan, registry,
                                                                  retained_runtime_desc);
    if (const Result result = retained_runtime.build(); !result || !retained_graph.compile()) {
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
