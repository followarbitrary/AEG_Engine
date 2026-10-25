/**
 * @file renderer_pipeline_graph_smoke.cpp
 * @brief Verifies built-in renderer variants expand into valid RenderGraph topology.
 */
#include "feature_clock_test_fixture.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <post_process.hpp>
#include <render_feature_runtime.hpp>
#include <render_graph.hpp>
#include <render_pass_execution_host.hpp>
#include <renderer.hpp>
#include <renderer_variants_internal.hpp>
#include <rhi.hpp>
#include <string>
#include <span>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "../Engine/Pipeline/private/render_pass_registry_execution.hpp"
#include "../Engine/RenderGraph/private/native_graph_build_session.hpp"
#include "../Engine/RenderGraph/private/render_graph_execution.hpp"
#include "../Engine/RenderGraph/private/render_feature_projector.hpp"
#include "native_pass_sequence.hpp"
#include "native_scene_pass_source.hpp"
#include "render_feature_context.hpp"
#include "render_feature_draw.hpp"
#include "render_feature_graph_builders.hpp"
#include "render_pipeline_graph.hpp"
#include "renderer_feature_profiles.hpp"
#include "deferred_lighting_resources.hpp"
#include "light_culling.hpp"
#include "world_scope.hpp"

#ifndef AEG_POST_PROCESS_SHADER_PACKAGE_PATH
#define AEG_POST_PROCESS_SHADER_PACKAGE_PATH ""
#endif
#ifndef AEG_DEFERRED_LIGHTING_SHADER_PACKAGE_PATH
#define AEG_DEFERRED_LIGHTING_SHADER_PACKAGE_PATH ""
#endif
#ifndef AEG_LIGHT_CULLING_SHADER_PACKAGE_PATH
#define AEG_LIGHT_CULLING_SHADER_PACKAGE_PATH ""
#endif

namespace {
using aeg::ecs::WorldHandle;
using aeg::foundation::Result;
using aeg::foundation::ResultCode;
using aeg::pipeline::RenderResourceId;
using aeg::renderer::RenderFeatureContext;
using aeg::renderer::RenderSceneSnapshot;
using aeg::renderer::RenderView;
using aeg::rhi::Format;
using aeg::rhi::MemoryClass;
using aeg::rhi::TextureCapability;
using aeg::rhi::TextureDesc;

struct EmptySceneResolver final : aeg::renderer::RenderResourceResolver {
    aeg::renderer::PostProcessResources& post;
    aeg::render_graph::FrameContext& frame;
    aeg::renderer::DeferredLightingResources* deferred{nullptr};
    aeg::renderer::LightCullingResources* culling{nullptr};
    aeg::renderer::RenderLightingResource lighting{};
    EmptySceneResolver(aeg::renderer::PostProcessResources& p, aeg::render_graph::FrameContext& f)
        : post(p), frame(f) {}
    aeg::foundation::ResultValue<aeg::renderer::RenderMeshResource> resolve_mesh(
        aeg::renderer::MeshAssetId) const override {
        return {aeg::foundation::not_found("Empty scene"), {}};
    }
    aeg::foundation::ResultValue<aeg::renderer::RenderMaterialResource> resolve_material(
        aeg::renderer::MaterialInstanceId) const override {
        return {aeg::foundation::not_found("Empty scene"), {}};
    }
    aeg::foundation::ResultValue<aeg::renderer::RenderObjectResource> resolve_object(
        const aeg::renderer::RenderInstance&, const RenderView&) const override {
        return {aeg::foundation::not_found("Empty scene"), {}};
    }
    aeg::foundation::ResultValue<aeg::renderer::RenderPostProcessResource> resolve_post_process(
        const RenderView& view) const override {
        return post.for_frame(frame, view);
    }
    aeg::foundation::ResultValue<aeg::renderer::RenderDeferredLightingResource> resolve_deferred_lighting(
        const RenderView&) const override {
        return deferred ? deferred->for_frame(frame)
                        : aeg::foundation::ResultValue<aeg::renderer::RenderDeferredLightingResource>{
                              aeg::foundation::not_found("Empty scene has no deferred program"), {}};
    }
    aeg::foundation::ResultValue<aeg::renderer::RenderLightCullingResource> resolve_light_culling(
        const RenderSceneSnapshot& scene, const aeg::renderer::RenderShadowView& shadow) const override {
        return culling ? culling->for_frame(frame, scene, shadow)
                       : aeg::foundation::ResultValue<aeg::renderer::RenderLightCullingResource>{
                             aeg::foundation::not_found("Empty scene has no culling program"), {}};
    }
    aeg::foundation::ResultValue<aeg::renderer::RenderLightingResource> resolve_lighting(
        const RenderSceneSnapshot&, const RenderView&) const override {
        return {lighting.is_valid() ? Result::success() : aeg::foundation::invalid_state("Lighting is unavailable"), lighting};
    }
};

[[nodiscard]] TextureDesc color_desc() {
    TextureDesc desc{};
    desc.extent = {8, 8, 1};
    desc.format = Format::kRgba8Unorm;
    desc.capabilities =
        TextureCapability::kSampled | TextureCapability::kColorAttachment | TextureCapability::kTransferSource;
    desc.memory = MemoryClass::kDeviceLocal;
    return desc;
}

[[nodiscard]] TextureDesc depth_desc() {
    TextureDesc desc{};
    desc.extent = {8, 8, 1};
    desc.format = Format::kD32Float;
    desc.capabilities = TextureCapability::kSampled | TextureCapability::kDepthStencil;
    desc.memory = MemoryClass::kDeviceLocal;
    return desc;
}

[[nodiscard]] RenderSceneSnapshot make_snapshot(RenderResourceId color, RenderResourceId depth) {
    RenderSceneSnapshot snapshot{};
    snapshot.frame_number = 1;
    snapshot.world_revision = 1;
    snapshot.world = WorldHandle{0, 1};
    snapshot.view.id = aeg::renderer::RenderViewId("Renderer.GraphSmokeView");
    snapshot.view.world = snapshot.world;
    snapshot.view.output_extent = {8, 8};
    snapshot.view.color_target = color;
    snapshot.view.depth_target = depth;
    snapshot.view.post_process.sky_enabled = true;
    snapshot.view.post_process.sky_zenith = snapshot.view.post_process.sky_horizon =
        snapshot.view.post_process.sky_ground = {0.25f, 0.5f, 1};
    return snapshot;
}

enum class NativeSequenceCase {
    kDefault,
    kShorter,
    kDisabledShadow,
    kOrdered,
    kDisabledFinal,
    kMissingInstance,
    kDuplicateInstance,
    kMissingOrderingSource,
    kDisabledOrderingSource,
    kSelfOrdering,
    kCyclicOrdering,
};

[[nodiscard]] bool rejected_sequence(NativeSequenceCase scenario) {
    return scenario >= NativeSequenceCase::kDisabledFinal;
}

[[nodiscard]] Result build_native_variant(aeg::render_graph::RenderGraph& graph,
    std::span<const aeg::render_graph::RenderGraphFeatureTextureBinding> bindings,
    const RenderFeatureContext& context, bool deferred) {
    using Flags = aeg::pipeline::RenderGraphBuildFlags;
    auto flags = Flags::kMayCreateResources | Flags::kMayCreateGraphicsPass | Flags::kMayWriteFinalOutput;
    if (!deferred) flags = flags | Flags::kMayCreateComputePass | Flags::kMayUseDedicatedQueue;
    const auto session = aeg::render_graph::NativeGraphBuildSession::begin(graph, bindings, flags, &context);
    if (!session) return session.result;
    if (!session.value->builder().is_runtime() || session.value->builder().build_flags() != flags ||
        session.value->builder().context_data() != &context)
        return aeg::foundation::invalid_state("fixed native session lost its actual permissions or renderer context");
    const auto result = deferred ? aeg::renderer::build_deferred_render_graph(session.value->builder(), context)
                                 : aeg::renderer::build_forward_render_graph(session.value->builder(), context);
    if (result) session.value->commit();
    return result;
}

[[nodiscard]] bool same_sequence(const aeg::renderer::NativePassSequence& left,
                                const aeg::renderer::NativePassSequence& right) {
    if (left.passes.size() != right.passes.size()) return false;
    for (size_t index = 0; index < left.passes.size(); ++index) {
        const auto& a = left.passes[index];
        const auto& b = right.passes[index];
        if (a.type != b.type || a.instance != b.instance || a.queue != b.queue || a.enabled != b.enabled ||
            a.parameters != b.parameters || a.resource_bindings != b.resource_bindings || a.source != b.source ||
            a.ordered_after != b.ordered_after) return false;
    }
    return true;
}

[[nodiscard]] bool same_work(const aeg::render_graph::RenderWorkItem& left,
                            const aeg::render_graph::RenderWorkItem& right) {
    return left.id == right.id && left.domain == right.domain && left.kind == right.kind &&
        left.feature_definition_id == right.feature_definition_id && left.owner_module == right.owner_module &&
        left.node_id == right.node_id && left.instance_id == right.instance_id && left.invocation == right.invocation &&
        left.frame_index == right.frame_index && left.operation_index == right.operation_index;
}

struct ProfilePassObservation final {
    aeg::pipeline::RenderPassConfiguration configuration{};
    aeg::render_graph::RenderSceneOperationSource source{};
    std::weak_ptr<const aeg::pipeline::RenderPassRuntimeData> commands{};
    std::weak_ptr<const aeg::pipeline::RenderPass> delegate{};
    const aeg::pipeline::RenderPassConfiguration* actual_configuration{nullptr};
    unsigned executions{0};
};

class ObservedProfilePass final : public aeg::pipeline::RenderPass {
public:
    ObservedProfilePass(std::shared_ptr<const aeg::pipeline::RenderPass> delegate,
                        std::vector<ProfilePassObservation>& observations)
        : delegate_(std::move(delegate)), observations_(observations) {}
    const aeg::pipeline::RenderPassDescriptor& descriptor() const noexcept override { return delegate_->descriptor(); }
    Result setup(aeg::pipeline::RenderPassBuilder& builder,
                 const aeg::pipeline::RenderPassConfiguration& configuration) const override {
        const auto source = aeg::renderer::detail::NativeScenePassInspection::source(configuration);
        if (!source) return source.result;
        if (const auto result = delegate_->setup(builder, configuration); !result) return result;
        auto recorded = configuration;
        recorded.runtime_data.reset();
        observations_.push_back({std::move(recorded), source.value, configuration.runtime_data, delegate_, &configuration});
        return Result::success();
    }
    Result execute(aeg::pipeline::RenderPassExecutionContext& command,
                   const aeg::pipeline::RenderPassConfiguration& configuration) const override {
        const auto source = aeg::renderer::detail::NativeScenePassInspection::source(configuration);
        if (!source) return source.result;
        const auto prepared = std::find_if(observations_.begin(), observations_.end(), [&](const auto& item) {
            return item.actual_configuration == &configuration;
        });
        if (prepared == observations_.end() || !same_work(prepared->source.work, source.value.work) ||
            prepared->source.owner != source.value.owner || prepared->source.view_key != source.value.view_key ||
            prepared->source.kind != source.value.kind || prepared->source.depth_projection != source.value.depth_projection ||
            prepared->configuration.type != configuration.type || prepared->configuration.instance != configuration.instance ||
            prepared->configuration.source != configuration.source || prepared->configuration.queue != configuration.queue ||
            prepared->configuration.enabled != configuration.enabled || prepared->configuration.parameters != configuration.parameters ||
            prepared->configuration.bindings != configuration.bindings ||
            prepared->commands.lock().get() != configuration.runtime_data.get() ||
            prepared->delegate.lock().get() != delegate_.get())
            return aeg::foundation::invalid_state("actual Feature native execution replaced its prepared source or owners");
        const auto result = delegate_->execute(command, configuration);
        if (result) ++prepared->executions;
        return result;
    }
private:
    std::shared_ptr<const aeg::pipeline::RenderPass> delegate_;
    std::vector<ProfilePassObservation>& observations_;
};

struct ProfileLighting final {
    aeg::rhi::Device& device;
    aeg::rhi::BufferHandle buffer{};
    aeg::rhi::BindingLayoutHandle layout{};
    aeg::rhi::BindingSetHandle binding{};
    ~ProfileLighting() {
        if (binding.is_valid()) (void)device.destroy_binding_set(binding);
        if (buffer.is_valid()) (void)device.destroy_buffer(buffer);
        if (layout.is_valid()) (void)device.destroy_binding_layout(layout);
    }
    [[nodiscard]] aeg::foundation::ResultValue<aeg::renderer::RenderLightingResource> initialize(
        aeg::render_graph::RenderGraph& graph) {
        using namespace aeg::rhi;
        const aeg::renderer::RenderLightingConstants constants{
            .directional_direction_intensity = {0, 0, -1, 0}, .directional_color = {0, 0, 0, 1},
            .ambient_color = {1, 1, 1, 1}};
        BufferDesc desc{};
        desc.size = sizeof(constants);
        desc.capabilities = BufferCapability::kUniform;
        desc.memory = MemoryClass::kUpload;
        desc.initial_usage = ResourceUsage::kUniformRead;
        desc.initial_stages = PipelineStage::kFragmentShader;
        desc.initial_queue = QueueClass::kGraphics;
        const auto created_buffer = device.create_buffer(desc);
        if (!created_buffer) return {created_buffer.result, {}};
        buffer = created_buffer.value;
        if (const auto result = device.write_buffer(buffer, 0, std::as_bytes(std::span{&constants, 1})); !result)
            return {result, {}};
        const std::array entries{BindingLayoutEntry{{2, 0}, BindingType::kUniformBuffer, BindingArrayMode::kSingle,
            1, ShaderStage::kFragment, BindingFlags::kNone}};
        const auto created_layout = device.create_binding_layout({entries, DebugName{"FeatureProfile.Lighting"}});
        if (!created_layout) return {created_layout.result, {}};
        layout = created_layout.value;
        const auto created_binding = device.create_binding_set({layout, BindingSetLifetime::kPersistent});
        if (!created_binding) return {created_binding.result, {}};
        binding = created_binding.value;
        BindingWrite write{};
        write.location = {2, 0};
        write.resource.type = BindingType::kUniformBuffer;
        write.resource.buffer_range = {buffer, 0, sizeof(constants)};
        if (const auto result = device.update_binding_set(binding, std::span{&write, 1}); !result) return {result, {}};
        const auto imported = graph.import_buffer(buffer, desc);
        if (!imported) return {imported.result, {}};
        return {Result::success(), {binding, 2, buffer, desc,
            {imported.value.index, imported.value.generation, imported.value.graph_id}, 0, sizeof(constants)}};
    }
};

[[nodiscard]] bool build_variant(aeg::rhi::Device& device, bool deferred, RenderResourceId color_id,
                                 RenderResourceId depth_id, uint32_t expected_passes, bool configured = false,
                                 NativeSequenceCase scenario = NativeSequenceCase::kDefault) {
    std::shared_ptr<aeg::renderer::NativePassSequence> configuration;
    if (configured) {
        configuration = std::make_shared<aeg::renderer::NativePassSequence>();
        // Deliberately reorder independent producers. The graph adapter must
        // preserve this fixed native sequence and resolve its actual instances.
        const std::array<aeg::pipeline::RenderPassTypeId, 5> order =
            deferred ? std::array<aeg::pipeline::RenderPassTypeId, 5>{aeg::renderer::kGBufferPassTypeId,
                                                                      aeg::renderer::kShadowPassTypeId,
                                                                      aeg::renderer::kDeferredLightingPassTypeId,
                                                                      aeg::renderer::kSkyboxPostProcessPassTypeId,
                                                                      aeg::pipeline::RenderPassTypeId{}}
                     : std::array<aeg::pipeline::RenderPassTypeId, 5>{
                           aeg::renderer::kZPrepassPassTypeId, aeg::renderer::kShadowPassTypeId,
                           aeg::renderer::kLightCullingPassTypeId, aeg::renderer::kForwardOpaquePassTypeId,
                           aeg::renderer::kSkyboxPostProcessPassTypeId};
        const size_t count = deferred ? 4u : 5u;
        const auto instance_prefix = deferred ? "Deferred" : "Forward";
        for (size_t index = 0; index < count; ++index) {
            const auto type = order[index];
            configuration->passes.push_back({
                .type = type,
                .instance = aeg::pipeline::RenderPassInstanceId(std::string("Renderer.GraphSmoke.") + instance_prefix +
                                                                "." + std::to_string(index)),
                .queue = type == aeg::renderer::kLightCullingPassTypeId ? aeg::rhi::QueueClass::kCompute
                                                                        : aeg::rhi::QueueClass::kGraphics,
                .enabled = true,
                .resource_bindings = {{1, 1}, {2, 2}},
                .source = std::string("fixture://native-sequence/") + instance_prefix + "/" + std::to_string(index),
            });
        }
        auto& sequence = configuration->passes;
        switch (scenario) {
            case NativeSequenceCase::kDefault:
                break;
            case NativeSequenceCase::kShorter:
                std::erase_if(sequence, [](const auto& pass) { return pass.type == aeg::renderer::kShadowPassTypeId; });
                break;
            case NativeSequenceCase::kDisabledShadow:
                sequence[1].enabled = false;
                break;
            case NativeSequenceCase::kOrdered:
                sequence.front().ordered_after = {sequence[1].instance};
                break;
            case NativeSequenceCase::kDisabledFinal:
                sequence.back().enabled = false;
                break;
            case NativeSequenceCase::kMissingInstance:
                sequence.front().instance = {};
                break;
            case NativeSequenceCase::kDuplicateInstance:
                sequence[1].instance = sequence.front().instance;
                break;
            case NativeSequenceCase::kMissingOrderingSource:
                sequence.front().ordered_after = {aeg::pipeline::RenderPassInstanceId("Renderer.MissingNativePass")};
                break;
            case NativeSequenceCase::kDisabledOrderingSource:
                sequence[1].enabled = false;
                sequence.front().ordered_after = {sequence[1].instance};
                break;
            case NativeSequenceCase::kSelfOrdering:
                sequence.front().ordered_after = {sequence.front().instance};
                break;
            case NativeSequenceCase::kCyclicOrdering:
                sequence.front().ordered_after = {sequence[1].instance};
                sequence[1].ordered_after = {sequence.front().instance};
                break;
        }
    }
    const auto sequence_before_build = configuration ? *configuration : aeg::renderer::NativePassSequence{};

    aeg::renderer::PostProcessResources post;
    if (!post.initialize(device, AEG_POST_PROCESS_SHADER_PACKAGE_PATH, Format::kRgba8Unorm))
        return false;
    aeg::render_graph::FrameContext frame(device, {.frames_in_flight = 1});
    if (!frame.begin_frame())
        return false;
    EmptySceneResolver resolver(post, frame);
    aeg::render_graph::RenderGraph graph(device);
    const auto color = graph.create_texture(color_desc(), aeg::render_graph::ResourceLifetime::kPersistent);
    const auto depth = graph.create_texture(depth_desc(), aeg::render_graph::ResourceLifetime::kPersistent);
    if (!color || !depth) {
        return false;
    }
    if (scenario == NativeSequenceCase::kDefault) {
        auto foreign_device =
            aeg::rhi::create_device({aeg::rhi::BackendType::kVulkan, "AEG Renderer Foreign Frame Smoke"});
        if (!foreign_device)
            return false;
        aeg::render_graph::FrameContext foreign_frame(*foreign_device.value, {.frames_in_flight = 1});
        if (!foreign_frame.begin_frame())
            return false;
        const auto foreign_post = post.for_frame(foreign_frame, make_snapshot(color_id, depth_id).view);
        if (foreign_post || foreign_post.result.code != ResultCode::kInvalidState) {
            std::cerr << "PostProcessResources accepted a FrameContext from a foreign RHI device\n";
            (void)foreign_frame.abort_frame();
            return false;
        }
        if (!foreign_frame.abort_frame())
            return false;
    }
    const std::array<aeg::render_graph::RenderGraphFeatureTextureBinding, 2> bindings{{
        {color_id, color.value},
        {depth_id, depth.value},
    }};
    aeg::pipeline::RenderPassRegistry passes;
    if (!aeg::renderer::register_builtin_render_passes(passes))
        return false;
    struct SetupObservation {
        aeg::pipeline::RenderPassTypeId type{};
        aeg::pipeline::RenderPassInstanceId instance{};
        std::string source{};
        aeg::rhi::QueueClass queue{};
        bool enabled{};
        aeg::foundation::ParameterBlock parameters{};
        std::unordered_map<uint64_t, uint64_t> bindings{};
        const aeg::pipeline::RenderPassConfiguration* configuration{};
        const aeg::pipeline::RenderPassRuntimeData* runtime_data{};
        std::weak_ptr<const aeg::pipeline::RenderPassRuntimeData> runtime_owner{};
        const aeg::pipeline::RenderPass* pass{};
        unsigned executions{};
    };
    std::vector<SetupObservation> observed;
    struct ObservedPass final : aeg::pipeline::RenderPass {
        std::shared_ptr<const aeg::pipeline::RenderPass> delegate;
        unsigned* setups;
        unsigned* executions;
        std::vector<SetupObservation>* observations;
        std::shared_ptr<const aeg::renderer::NativePassSequence> expected_configuration;
        Result verify_actual_configuration(const aeg::pipeline::RenderPassConfiguration& actual) const {
            if (!expected_configuration)
                return Result::success();
            for (const auto& expected : expected_configuration->passes) {
                if (expected.instance != actual.instance)
                    continue;
                if (actual.type != expected.type || actual.source != expected.source ||
                    actual.queue != expected.queue || actual.enabled != expected.enabled ||
                    actual.parameters != expected.parameters || actual.bindings != expected.resource_bindings ||
                    !actual.runtime_data)
                    return aeg::foundation::invalid_state(
                        "Actual registered pass lost its native instance/source or prepared shape");
                return Result::success();
            }
            return aeg::foundation::invalid_state("Actual registered pass was not in its native sequence");
        }
        const aeg::pipeline::RenderPassDescriptor& descriptor() const noexcept override {
            return delegate->descriptor();
        }
        static auto invalid_configurations(const aeg::pipeline::RenderPassConfiguration& config) {
            std::array invalid{config, config, config, config, config};
            invalid[0].type = aeg::pipeline::RenderPassTypeId("Renderer.UnknownPass");
            invalid[1].enabled = false;
            invalid[2].queue = config.queue == aeg::rhi::QueueClass::kCompute ? aeg::rhi::QueueClass::kGraphics
                                                                              : aeg::rhi::QueueClass::kCompute;
            invalid[3].bindings[1] = 2;
            invalid[4].parameters[1] = int64_t{7};
            return invalid;
        }
        Result setup(aeg::pipeline::RenderPassBuilder& builder,
                     const aeg::pipeline::RenderPassConfiguration& config) const override {
            ++*setups;
            if (const auto result = verify_actual_configuration(config); !result)
                return result;
            for (const auto& invalid : invalid_configurations(config)) {
                const auto result = delegate->setup(builder, invalid);
                if (result.code != ResultCode::kInvalidArgument)
                    return aeg::foundation::invalid_state("Invalid pass configuration reached setup");
            }
            auto unprepared = config;
            unprepared.runtime_data.reset();
            if (delegate->setup(builder, unprepared).code != ResultCode::kInvalidState)
                return aeg::foundation::invalid_state("Unprepared pass setup must fail");
            const auto result = delegate->setup(builder, config);
            if (!result)
                return result;
            observations->push_back({config.type, config.instance, config.source, config.queue, config.enabled,
                                     config.parameters, config.bindings, &config, config.runtime_data.get(),
                                     config.runtime_data, this, 0});
            return Result::success();
        }
        Result execute(aeg::pipeline::RenderPassExecutionContext& command,
                       const aeg::pipeline::RenderPassConfiguration& config) const override {
            ++*executions;
            if (const auto result = verify_actual_configuration(config); !result)
                return result;
            const auto setup = std::find_if(observations->begin(), observations->end(), [&](const auto& observation) {
                return observation.configuration == &config;
            });
            if (setup == observations->end() || setup->type != config.type || setup->instance != config.instance ||
                setup->source != config.source || setup->queue != config.queue || setup->enabled != config.enabled ||
                setup->parameters != config.parameters || setup->bindings != config.bindings ||
                setup->runtime_data != config.runtime_data.get() || setup->runtime_owner.expired() ||
                setup->pass != this)
                return aeg::foundation::invalid_state(
                    "Execution did not retain its actual setup configuration and owners");
            for (const auto& invalid : invalid_configurations(config))
                if (delegate->execute(command, invalid).code != ResultCode::kInvalidArgument)
                    return aeg::foundation::invalid_state("Invalid pass configuration reached execution");
            auto unprepared = config;
            unprepared.runtime_data.reset();
            if (delegate->execute(command, unprepared).code != ResultCode::kInvalidState)
                return aeg::foundation::invalid_state("Unprepared pass execution must fail");
            const auto result = delegate->execute(command, config);
            if (result)
                ++setup->executions;
            return result;
        }
    };
    constexpr std::array kPassIds{
        aeg::renderer::kShadowPassTypeId,           aeg::renderer::kGBufferPassTypeId,
        aeg::renderer::kDeferredLightingPassTypeId, aeg::renderer::kSkyboxPostProcessPassTypeId,
        aeg::renderer::kZPrepassPassTypeId,         aeg::renderer::kLightCullingPassTypeId,
        aeg::renderer::kForwardOpaquePassTypeId};
    const auto shadow_owner =
        aeg::pipeline::detail::RenderPassRegistryExecution::find_pass(passes, aeg::renderer::kShadowPassTypeId);
    if (!shadow_owner)
        return false;
    std::array<unsigned, 7> setups{}, executions{};
    for (size_t i = 0; i < kPassIds.size(); ++i) {
        const auto found = aeg::pipeline::detail::RenderPassRegistryExecution::find_pass(passes, kPassIds[i]);
        if (!found || !passes.unregister_pass(kPassIds[i]))
            return false;
        if (!passes.register_pass([&, i, owner = found.value] {
                auto pass = std::make_unique<ObservedPass>();
                pass->delegate = owner;
                pass->setups = &setups[i];
                pass->executions = &executions[i];
                pass->observations = &observed;
                pass->expected_configuration = configuration;
                return pass;
            }))
            return false;
    }
    // A borrowed native-pass host must retire as soon as its registry mutates;
    // the old fixed epoch accepted a stale host and made registry lifecycle
    // changes invisible to the graph build boundary.
    const aeg::render_graph::RenderPassExecutionHost stale_host{
        &passes, aeg::pipeline::detail::RenderPassRegistryExecution::execution_epoch(passes)};
    if (!stale_host.is_valid() || !passes.unregister_pass(aeg::renderer::kShadowPassTypeId) || stale_host.is_valid())
        return false;
    if (!passes.register_pass([owner = shadow_owner.value, &setups, &executions, &observed, configuration] {
            auto pass = std::make_unique<ObservedPass>();
            pass->delegate = owner;
            pass->setups = &setups[0];
            pass->executions = &executions[0];
            pass->observations = &observed;
            pass->expected_configuration = configuration;
            return pass;
        }))
        return false;
    const RenderSceneSnapshot snapshot = make_snapshot(color_id, depth_id);
    const RenderFeatureContext feature_context{
        &snapshot,
        &resolver,
        nullptr,
        {&passes, aeg::pipeline::detail::RenderPassRegistryExecution::execution_epoch(passes)},
        false, nullptr, configuration.get()};
    // Missing registry/types must abort the feature transaction without leaving partial passes/resources.
    if (scenario == NativeSequenceCase::kDefault) {
        aeg::pipeline::RenderPassRegistry empty_passes;
        for (const auto* unavailable : {static_cast<const aeg::pipeline::RenderPassRegistry*>(nullptr),
                                        static_cast<const aeg::pipeline::RenderPassRegistry*>(&empty_passes)}) {
            auto unavailable_context = feature_context;
            unavailable_context.pass_execution = {
                unavailable, unavailable == nullptr
                                 ? 0
                                 : aeg::pipeline::detail::RenderPassRegistryExecution::execution_epoch(*unavailable)};
            const auto failed = build_native_variant(graph, bindings, unavailable_context, deferred);
            if (failed || failed.code != (unavailable ? ResultCode::kNotFound : ResultCode::kInvalidState) ||
                !graph.compile() ||
                aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).pass_count != 0 ||
                aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).resource_count != 2)
                return false;
        }
    }
    const auto build_result = build_native_variant(graph, bindings, feature_context, deferred);
    if (rejected_sequence(scenario)) {
        // Disabling the final output fails after real registered passes were
        // prepared, so this case also proves nonempty transaction rollback.
        if (scenario == NativeSequenceCase::kDisabledFinal && observed.empty())
            return false;
        if (build_result || build_result.code != ResultCode::kInvalidArgument || !graph.compile() ||
            aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).pass_count != 0 ||
            aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).resource_count != 2 ||
            !aeg::render_graph::detail::RenderGraphExecution::plan(graph).pass_plans.empty() ||
            feature_context.native_sequence != configuration.get() ||
            !same_sequence(*configuration, sequence_before_build)) {
            std::cerr << "Invalid native sequence did not roll back: " << build_result.message << '\n';
            return false;
        }
        for (const auto& observation : observed)
            if (!observation.runtime_owner.expired())
                return false;
        return bool(frame.abort_frame()) && bool(frame.drain());
    }
    if (!build_result) {
        std::cerr << build_result.message << '\n';
        return false;
    }
    if (configuration) {
        if (feature_context.native_sequence != configuration.get() || !same_sequence(*configuration, sequence_before_build))
            return false;
        size_t index = 0;
        for (const auto& expected : configuration->passes) {
            if (!expected.enabled)
                continue;
            if (index >= observed.size() || observed[index].instance != expected.instance ||
                observed[index].type != expected.type || observed[index].source != expected.source) {
                std::cerr << "Native setup order differs from the actual declared sequence\n";
                return false;
            }
            ++index;
        }
        if (index != observed.size())
            return false;
    }
    // The graph owns resolved passes even after the host registry changes.
    for (auto id : kPassIds)
        if (!passes.unregister_pass(id))
            return false;
    const auto readback = frame.allocate_readback(8 * 8 * 4, 256);
    if (!readback)
        return false;
    aeg::rhi::BufferDesc read_desc{};
    read_desc.size = readback.value.buffer_size;
    read_desc.memory = MemoryClass::kReadback;
    read_desc.capabilities = aeg::rhi::BufferCapability::kTransferDestination;
    const auto destination = graph.import_buffer(readback.value.buffer, read_desc);
    if (!destination)
        return false;
    const auto copy = graph.add_pass(
        "Read empty sky", aeg::rhi::QueueClass::kGraphics,
        [&](aeg::render_graph::PassBuilder& pass) {
            if (const auto r =
                    pass.read(color.value, aeg::rhi::ResourceUsage::kCopySource, aeg::rhi::PipelineStage::kCopy);
                !r)
                return r;
            pass.set_side_effects();
            return pass.write(destination.value, aeg::rhi::ResourceUsage::kCopyDestination,
                              aeg::rhi::PipelineStage::kCopy);
        },
        [&](aeg::render_graph::RenderPassCommandContext& command) {
            return command.copy_texture_to_buffer(color.value, {aeg::rhi::Aspect::kColor, 0, 0, 1}, {}, {8, 8, 1},
                                                  destination.value, readback.value.offset, 0, 0);
        });
    if (!copy)
        return false;
    if (const Result result = graph.compile(); !result) {
        std::cerr << result.message << '\n';
        return false;
    }
    // Both empty variants clear their attachments without reading an unused shadow map.
    const unsigned expected_culled = scenario == NativeSequenceCase::kOrdered ||
                                             scenario == NativeSequenceCase::kShorter ||
                                             scenario == NativeSequenceCase::kDisabledShadow
                                         ? 0u
                                         : 1u;
    const unsigned active_passes =
         configuration ? static_cast<unsigned>(std::count_if(configuration->passes.begin(),
                                                             configuration->passes.end(),
                                                            [](const auto& pass) { return pass.enabled; }))
                      : expected_passes;
    if (aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).pass_count != active_passes + 1 ||
        aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).culled_pass_count != expected_culled ||
        aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).resource_count != (deferred ? 9u : 7u)) {
        std::cerr << "Variant graph passes/culled/resources: "
                  << aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).pass_count << '/'
                  << aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).culled_pass_count << '/'
                  << aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).resource_count << '\n';
        return false;
    }
    if (scenario == NativeSequenceCase::kOrdered) {
        const auto& actual_plan = aeg::render_graph::detail::RenderGraphExecution::plan(graph);
        // The later-listed independent Shadow must precede the first pass,
        // and its explicit dependency must keep its otherwise unused work live.
        if (observed.size() < 2 || observed[1].type != aeg::renderer::kShadowPassTypeId ||
            actual_plan.pass_plans.size() < 2 || !actual_plan.pass_plans[1].live ||
            actual_plan.pass_plans[1].topological_index >= actual_plan.pass_plans[0].topological_index)
            return false;
    }
    const auto execution = graph.execute(frame);
    if (!execution) {
        std::cerr << execution.result.message << '\n';
        return false;
    }
    if (execution.value.executed_pass_count != active_passes + 1 - expected_culled || !frame.end_frame() ||
        !device.wait_idle())
        return false;
    for (size_t i = 0; i < kPassIds.size(); ++i) {
        const unsigned expected =
            configuration
                ? static_cast<unsigned>(std::count_if(
                       configuration->passes.begin(), configuration->passes.end(),
                      [&](const auto& pass) { return pass.enabled && pass.type == kPassIds[i]; }))
            : deferred ? (i < 4 ? 1u : 0u)
                       : (i == 0 || i >= 3 ? 1u : 0u);
        const unsigned expected_execution = i == 0 && scenario != NativeSequenceCase::kOrdered ? 0u : expected;
        if (setups[i] != expected || executions[i] != expected_execution) {
            std::cerr << "Registered pass " << i << " setup/execute: " << setups[i] << '/' << executions[i] << '\n';
            return false;
        }
    }
    std::array<std::byte, 8 * 8 * 4> pixels{};
    if (!frame.read_readback(readback.value, pixels))
        return false;
    for (size_t pixel = 0; pixel < pixels.size(); pixel += 4) {
        if (std::abs(int(std::to_integer<unsigned>(pixels[pixel])) - 64) > 1 ||
            std::abs(int(std::to_integer<unsigned>(pixels[pixel + 1])) - 128) > 1 ||
            pixels[pixel + 2] != std::byte{255} || pixels[pixel + 3] != std::byte{255})
            return false;
    }
    if (!frame.drain())
        return false;
    for (const auto& observation : observed) {
        const auto expected_execution =
            observation.type == aeg::renderer::kShadowPassTypeId && scenario != NativeSequenceCase::kOrdered ? 0u : 1u;
        if (observation.executions != expected_execution || observation.runtime_owner.expired() || !observation.pass)
            return false;
    }
    graph.reset();
    for (const auto& observation : observed)
        if (!observation.runtime_owner.expired())
            return false;
    return true;
}

[[nodiscard]] bool build_feature_profile(aeg::rhi::Device& device, aeg::renderer::SceneRenderProfile profile) {
    using namespace aeg;
    const bool deferred = profile == renderer::SceneRenderProfile::kDeferred;
    const auto checked = [](const auto& result, const char* operation) {
        if (!result) {
            std::cerr << "Feature profile " << operation << " failed";
            if constexpr (requires { result.message; }) std::cerr << ": " << result.message;
            else if constexpr (requires { result.result.message; }) std::cerr << ": " << result.result.message;
            std::cerr << '\n';
        }
        return bool(result);
    };
    feature::FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    feature::FeatureRuntime runtime(registry);
    if (!checked(renderer::register_scene_render_profiles(registry), "registration")) return false;
    const auto document = renderer::scene_render_profile_graph(profile);
    if (!checked(document, "preset")) return false;
    auto compiled = feature::FeatureGraphCompiler{}.compile(document.value, registry);
    if (!checked(compiled, "compile") || !checked(runtime.publish_graph(document.value, compiled.value), "publication") ||
        !checked(runtime.tick(renderer::kSceneRenderProfileClock), "tick")) return false;
    const auto captured = runtime.capture_render_outputs(compiled.value);
    if (!checked(captured, "capture") || captured.value.size() != 1) return false;
    const auto& committed = captured.value.front();
    const auto* owner = runtime.find_instance(committed.instance_id);
    if (!owner || !owner->last_invocation_committed || owner->state.commit_count != 1 ||
        owner->last_invocation_committed->attempt.token != committed.invocation ||
        owner->definition_digest != committed.resource_owner.definition_digest ||
        owner->render_resource_epoch != committed.resource_owner.resource_epoch ||
        compiled.value.provenance.size() != 1 ||
        compiled.value.provenance.front().definition_id != committed.contract.definition_id ||
        compiled.value.provenance.front().owner_module != committed.owner_module ||
        compiled.value.provenance.front().node_id != committed.node_id ||
        compiled.value.provenance.front().instance_id != committed.instance_id) return false;

    renderer::PostProcessResources post;
    renderer::DeferredLightingResources deferred_resources;
    renderer::LightCullingResources culling_resources;
    if (!checked(post.initialize(device, AEG_POST_PROCESS_SHADER_PACKAGE_PATH, Format::kRgba8Unorm), "postprocess") ||
        (deferred && !checked(deferred_resources.initialize(device, AEG_DEFERRED_LIGHTING_SHADER_PACKAGE_PATH,
            renderer::kSceneLightingFormat), "deferred program")) ||
        (!deferred && !checked(culling_resources.initialize(device, AEG_LIGHT_CULLING_SHADER_PACKAGE_PATH),
            "culling program"))) return false;
    render_graph::FrameContext frame(device, {.frames_in_flight = 1});
    if (!checked(frame.begin_frame(), "frame begin")) return false;
    EmptySceneResolver resolver(post, frame);
    resolver.deferred = deferred ? &deferred_resources : nullptr;
    resolver.culling = deferred ? nullptr : &culling_resources;
    ProfileLighting lighting{device};
    render_graph::RenderGraph graph(device);
    if (deferred) {
        const auto resource = lighting.initialize(graph);
        if (!checked(resource, "lighting constants")) return false;
        resolver.lighting = resource.value;
    }

    ecs::WorldManager worlds;
    const auto world_handle = worlds.create_world({"Renderer.ActualFeatureGraph", true});
    if (!checked(world_handle, "World")) return false;
    const auto world = worlds.find(world_handle.value);
    if (!checked(world, "World lookup") || !checked(world.value->create_entity(), "World entity")) return false;
    const auto world_snapshot = world.value->commit(frame.frame_number(), 1);
    if (!checked(world_snapshot, "World commit")) return false;
    auto view = make_snapshot(RenderResourceId("Renderer.SceneColor"), RenderResourceId("Renderer.SceneDepth")).view;
    view.world = world_handle.value;
    view.view_id = 1;
    view.id = renderer::make_render_view_id(view.world, view.view_id);
    renderer::Renderer actual_renderer;
    auto scene = actual_renderer.extract(world_snapshot.value, view,
        renderer::RenderExtractionDesc{frame.frame_number(), nullptr, &resolver, true});
    if (!checked(scene, "actual Renderer extraction") || !scene.value->is_valid() ||
        !scene.value->instances.empty() || scene.value->frame_number != frame.frame_number() ||
        scene.value->world_revision != world_snapshot.value.revision() || scene.value->view.id != view.id) return false;
    auto outputs = std::make_shared<const std::vector<feature::RenderFeatureOutput>>(
        std::vector{renderer::scope_render_feature_output(committed, scene.value->view.id, true)});
    const auto expected_work = render_graph::RenderFeatureProjector{}.project(*outputs, frame.frame_number());
    if (!checked(expected_work, "actual work projection")) return false;

    const auto color = graph.create_texture(color_desc(), render_graph::ResourceLifetime::kPersistent);
    const auto depth = graph.create_texture(depth_desc(), render_graph::ResourceLifetime::kPersistent);
    const auto shadow = graph.create_texture(depth_desc(), render_graph::ResourceLifetime::kPersistent);
    auto hdr_desc = color_desc();
    hdr_desc.format = renderer::kSceneLightingFormat;
    const auto hdr = graph.create_texture(hdr_desc, render_graph::ResourceLifetime::kPersistent);
    if (!color || !depth || !shadow || !hdr) return false;
    std::vector<render_graph::RenderGraphFeatureTextureBinding> bindings{
        {feature::RenderResourceId("Renderer.SceneColor"), color.value},
        {feature::RenderResourceId("Renderer.SceneDepth"), depth.value},
        {feature::RenderResourceId("Renderer.ShadowDepth"), shadow.value},
        {feature::RenderResourceId("Renderer.SceneLighting"), hdr.value}};
    if (deferred) {
        for (const auto& [id, format] : std::array{
                 std::pair{"Renderer.GBuffer.Albedo", Format::kRgba16Float},
                 std::pair{"Renderer.GBuffer.Normal", Format::kRgba16Float},
                 std::pair{"Renderer.GBuffer.Material", Format::kRgba8Unorm}}) {
            auto desc = color_desc();
            desc.format = format;
            const auto target = graph.create_texture(desc, render_graph::ResourceLifetime::kPersistent);
            if (!target) return false;
            bindings.push_back({feature::RenderResourceId(id), target.value});
        }
    }
    constexpr std::array types{
        renderer::kShadowPassTypeId, renderer::kGBufferPassTypeId, renderer::kDeferredLightingPassTypeId,
        renderer::kSkyboxPostProcessPassTypeId, renderer::kZPrepassPassTypeId,
        renderer::kLightCullingPassTypeId, renderer::kForwardOpaquePassTypeId};
    std::vector<ProfilePassObservation> observations;
    pipeline::RenderPassRegistry passes;
    if (!checked(renderer::register_builtin_render_passes(passes), "native registry")) return false;
    for (const auto type : types) {
        const auto found = pipeline::detail::RenderPassRegistryExecution::find_pass(passes, type);
        if (!found || !passes.unregister_pass(type) || !passes.register_pass([delegate = found.value, &observations] {
                return std::make_unique<ObservedProfilePass>(delegate, observations);
            })) return false;
    }
    {
        const std::array scenes{renderer::RenderFeatureSceneBinding{renderer::kRuntimeSceneResourceId, 1,
            committed.instance_id, scene.value, &resolver}};
        const render_graph::RenderPassExecutionHost host{
            &passes, pipeline::detail::RenderPassRegistryExecution::execution_epoch(passes)};
        const renderer::RenderFeatureDrawLowerer lowerer(scenes, host);
        const auto built = render_graph::build_render_graph_features(graph, outputs,
            {.textures = bindings, .draw_lowerer = &lowerer, .current_frame_owner = &frame}, frame.frame_number());
        if (!built) {
            std::cerr << "Actual profile native graph failed: " << built.message << '\n';
            return false;
        }
    }
    if (!checked(observations.size() == (deferred ? 4u : 5u), "native setup count")) return false;
    for (const auto& observation : observations) {
        const auto& source = observation.source;
        const auto projected = std::find_if(expected_work.value.begin(), expected_work.value.end(), [&](const auto& work) {
            return work.kind == render_graph::RenderWorkItemKind::kExecute && same_work(work, source.work);
        });
        if (projected == expected_work.value.end() || source.owner != committed.resource_owner ||
            source.view_key != scene.value->view.id.value() || source.view_key != outputs->front().view_key ||
            source.work.invocation != committed.invocation || source.work.frame_index != frame.frame_number() ||
            !observation.configuration.instance.is_valid() ||
            !observation.configuration.source.starts_with("Feature.Scene/module=") || observation.commands.expired() ||
            source.work.operation_index >= outputs->front().intent.operations.size()) {
            std::cerr << "Actual native profile source differs from committed work/View\n";
            return false;
        }
        const auto& operation = outputs->front().intent.operations[source.work.operation_index];
        using Kind = render_graph::RenderSceneOperationKind;
        const bool kind_matches =
            (source.kind == Kind::kDrawDepthScene && std::holds_alternative<feature::RenderDrawDepthScene>(operation)) ||
            (source.kind == Kind::kDrawGBufferScene && std::holds_alternative<feature::RenderDrawGBufferScene>(operation)) ||
            (source.kind == Kind::kDeferredLightingScene && std::holds_alternative<feature::RenderDeferredLightingScene>(operation)) ||
            (source.kind == Kind::kPostProcessScene && std::holds_alternative<feature::RenderPostProcessScene>(operation)) ||
            (source.kind == Kind::kLightCullingScene && std::holds_alternative<feature::RenderLightCullingScene>(operation)) ||
            (source.kind == Kind::kDrawClusteredScene && std::holds_alternative<feature::RenderDrawClusteredScene>(operation));
        if (!checked(kind_matches, "native operation kind")) return false;
        if (const auto* draw = std::get_if<feature::RenderDrawDepthScene>(&operation);
            draw && !checked(source.depth_projection == draw->projection, "native depth projection")) return false;
    }
    // A new actual commit must not replace the captured source already prepared for this frame.
    if (!checked(runtime.tick(renderer::kSceneRenderProfileClock), "next tick")) return false;
    const auto newer = runtime.capture_render_outputs(compiled.value);
    owner = runtime.find_instance(committed.instance_id);
    if (!newer || newer.value.size() != 1 || newer.value.front().invocation == committed.invocation || !owner ||
        !owner->last_invocation_committed || owner->state.commit_count != 2 ||
        owner->last_invocation_committed->attempt.token != newer.value.front().invocation ||
        outputs->front().invocation != committed.invocation || outputs->front().resource_owner != committed.resource_owner)
        return false;
    for (const auto type : types) if (!passes.unregister_pass(type)) return false;

    const auto readback = frame.allocate_readback(8 * 8 * 4, 256);
    if (!readback) return false;
    rhi::BufferDesc read_desc{};
    read_desc.size = readback.value.buffer_size;
    read_desc.memory = MemoryClass::kReadback;
    read_desc.capabilities = rhi::BufferCapability::kTransferDestination;
    const auto destination = graph.import_buffer(readback.value.buffer, read_desc);
    if (!destination) return false;
    const auto copied = graph.add_pass("Read actual Feature sky", rhi::QueueClass::kGraphics,
        [target = color.value, buffer = destination.value, allocation = readback.value](render_graph::PassBuilder& pass) {
            if (const auto result = pass.read(target, rhi::ResourceUsage::kCopySource, rhi::PipelineStage::kCopy); !result)
                return result;
            pass.set_side_effects();
            return pass.write(buffer, rhi::ResourceUsage::kCopyDestination, rhi::PipelineStage::kCopy,
                rhi::ShaderStage::kAll, {allocation.offset, allocation.size});
        }, [target = color.value, buffer = destination.value, allocation = readback.value](render_graph::PassExecutionContext& command) {
            return command.copy_texture_to_buffer(target, {rhi::Aspect::kColor, 0, 0, 1}, {}, {8, 8, 1},
                buffer, allocation.offset, 0, 0);
        });
    if (!copied || !graph.compile()) return false;
    size_t attributed = 0;
    for (const auto& pass : render_graph::detail::RenderGraphExecution::plan(graph).pass_plans) {
        if (!pass.feature_work) continue;
        const auto observed = std::find_if(observations.begin(), observations.end(), [&](const auto& observation) {
            return same_work(observation.source.work, *pass.feature_work);
        });
        if (observed == observations.end()) return false;
        ++attributed;
    }
    if (!checked(attributed == observations.size(), "actual pass attribution")) return false;
    const auto executed = graph.execute(frame);
    if (!checked(executed, "execution") || !checked(frame.end_frame(), "frame end") ||
        !checked(device.wait_idle(), "GPU wait")) return false;
    for (const auto& observation : observations) {
        const auto pass = std::find_if(render_graph::detail::RenderGraphExecution::plan(graph).pass_plans.begin(),
            render_graph::detail::RenderGraphExecution::plan(graph).pass_plans.end(), [&](const auto& candidate) {
                return candidate.feature_work && same_work(*candidate.feature_work, observation.source.work);
            });
        if (pass == render_graph::detail::RenderGraphExecution::plan(graph).pass_plans.end() ||
            !checked(observation.executions == (pass->live ? 1u : 0u), "actual native execution count")) return false;
    }
    std::array<std::byte, 8 * 8 * 4> pixels{};
    if (!checked(frame.read_readback(readback.value, pixels), "readback")) return false;
    for (size_t pixel = 0; pixel < pixels.size(); pixel += 4) {
        if (std::abs(int(std::to_integer<unsigned>(pixels[pixel])) - 64) > 1 ||
            std::abs(int(std::to_integer<unsigned>(pixels[pixel + 1])) - 128) > 1 ||
            pixels[pixel + 2] != std::byte{255} || pixels[pixel + 3] != std::byte{255}) {
            std::cerr << "Actual Feature profile sky pixel differs from 64/128/255/255\n";
            return false;
        }
    }
    const std::weak_ptr<const std::vector<feature::RenderFeatureOutput>> weak_outputs = outputs;
    const std::weak_ptr<const RenderSceneSnapshot> weak_scene = scene.value;
    outputs.reset();
    scene.value.reset();
    graph.reset();
    if (!checked(!weak_outputs.expired() && !weak_scene.expired(), "captured source before drain")) return false;
    for (const auto& observation : observations)
        if (!checked(!observation.commands.expired() && !observation.delegate.expired(), "prepared owners before drain")) return false;
    if (!checked(frame.drain(), "drain") || !checked(weak_outputs.expired() && weak_scene.expired(), "captured source retirement")) return false;
    for (const auto& observation : observations)
        if (!checked(observation.commands.expired() && observation.delegate.expired(), "prepared owner retirement")) return false;
    return true;
}

}  // namespace

int main() {
    if (std::string_view(AEG_POST_PROCESS_SHADER_PACKAGE_PATH).empty() ||
        std::string_view(AEG_DEFERRED_LIGHTING_SHADER_PACKAGE_PATH).empty() ||
        std::string_view(AEG_LIGHT_CULLING_SHADER_PACKAGE_PATH).empty()) {
        std::cerr << "Renderer GPU graph test requires compiled postprocess/deferred/culling shaders\n";
        return 77;
    }
    // The variant ForwardOpaque stage must have a reusable indexed-draw entry
    // point instead of being limited to the legacy RenderPipeline binding.
    const auto opaque_builder = &aeg::renderer::build_opaque_render_graph;
    if (opaque_builder == nullptr) {
        return 11;
    }
    const auto gbuffer_builder = &aeg::renderer::build_gbuffer_render_graph;
    if (gbuffer_builder == nullptr) {
        return 12;
    }

    aeg::rhi::DeviceCreateDesc device_desc{};
    device_desc.application_name = "AEG Renderer Pipeline Graph Smoke";
    auto device = aeg::rhi::create_device(device_desc);
    if (!device) {
        return 1;
    }

    if (!build_variant(*device.value, true,
                       RenderResourceId("Renderer.SceneColor"), RenderResourceId("Renderer.SceneDepth"), 4, true)) {
        return 3;
    }
    if (!build_variant(*device.value, false,
                       RenderResourceId("Renderer.SceneColor"), RenderResourceId("Renderer.SceneDepth"), 5, true)) {
        return 4;
    }

    for (const bool deferred : {true, false}) {
        const unsigned count = deferred ? 4u : 5u;
        if (!build_variant(*device.value, deferred, RenderResourceId("Renderer.SceneColor"),
                           RenderResourceId("Renderer.SceneDepth"), count)) return 14;
        for (const auto scenario :
             {NativeSequenceCase::kShorter, NativeSequenceCase::kDisabledShadow, NativeSequenceCase::kOrdered,
              NativeSequenceCase::kDisabledFinal, NativeSequenceCase::kMissingInstance,
              NativeSequenceCase::kDuplicateInstance, NativeSequenceCase::kMissingOrderingSource,
              NativeSequenceCase::kDisabledOrderingSource, NativeSequenceCase::kSelfOrdering,
              NativeSequenceCase::kCyclicOrdering}) {
            if (!build_variant(*device.value, deferred, RenderResourceId("Renderer.SceneColor"),
                               RenderResourceId("Renderer.SceneDepth"), count, true, scenario)) {
                std::cerr << "Native sequence case failed: " << unsigned(scenario) << '\n';
                return 13;
            }
        }
    }
    for (const auto profile : {aeg::renderer::SceneRenderProfile::kDeferred, aeg::renderer::SceneRenderProfile::kForward}) {
        if (!build_feature_profile(*device.value, profile)) {
            std::cerr << "Actual committed Scene Feature profile failed: " << unsigned(profile) << '\n';
            return 15;
        }
    }

    aeg::render_graph::RenderGraph missing_graph(*device.value);
    const auto color = missing_graph.create_texture(color_desc(), aeg::render_graph::ResourceLifetime::kPersistent);
    if (!color) {
        return 6;
    }
    const std::array<aeg::render_graph::RenderGraphFeatureTextureBinding, 1> missing_bindings{{
        {RenderResourceId("Renderer.SceneColor"), color.value},
    }};
    const RenderSceneSnapshot missing_snapshot =
        make_snapshot(RenderResourceId("Renderer.SceneColor"), RenderResourceId("Renderer.SceneDepth"));
    const RenderFeatureContext missing_context{&missing_snapshot, nullptr, nullptr};
    const Result missing_result = build_native_variant(missing_graph, missing_bindings, missing_context, true);
    if (missing_result || missing_result.code != ResultCode::kNotFound || !missing_graph.compile() ||
        aeg::render_graph::detail::RenderGraphExecution::compile_info(missing_graph).pass_count != 0 ||
        aeg::render_graph::detail::RenderGraphExecution::compile_info(missing_graph).resource_count != 1) {
        return 7;
    }
    aeg::render_graph::RenderGraph foreign_graph(*device.value);
    const auto foreign_depth = foreign_graph.create_texture(depth_desc());
    if (!foreign_depth)
        return 8;
    const std::array<aeg::render_graph::RenderGraphFeatureTextureBinding, 2> foreign_bindings{{
        {RenderResourceId("Renderer.SceneColor"), color.value},
        {RenderResourceId("Renderer.SceneDepth"), foreign_depth.value},
    }};
    const auto foreign_result = build_native_variant(missing_graph, foreign_bindings, missing_context, true);
    if (foreign_result || foreign_result.code != ResultCode::kNotFound)
        return 9;
    foreign_graph.reset();
    if (build_native_variant(foreign_graph, foreign_bindings, missing_context, true))
        return 10;
    return 0;
}
