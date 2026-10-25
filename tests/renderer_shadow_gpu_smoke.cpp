/** @brief Checks actual caster occlusion in both builtin renderer variants. */
#include "feature_clock_test_fixture.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>
#include <variant>

#include "../Engine/Pipeline/private/render_pass_registry_execution.hpp"
#include "../Engine/RenderGraph/private/render_graph_execution.hpp"
#include "../Engine/Shader/private/shader_pipeline_layout_access.hpp"
#include "concurrency.hpp"
#include "deferred_lighting_resources.hpp"
#include "feature_render_test_fixture.hpp"
#include "render_scene_provenance_fixture.hpp"
#include "feature_runtime.hpp"
#include "light_culling.hpp"
#include "native_scene_pass_source.hpp"
#include "post_process.hpp"
#include "render_feature_draw.hpp"
#include "render_feature_runtime.hpp"
#include "render_graph.hpp"
#include "render_scene_lowerer.hpp"
#include "renderer_variants_internal.hpp"
#include "renderer_feature_profiles.hpp"
#include "resource_uploader.hpp"
#include "runtime_asset_resolver.hpp"
#include "runtime_asset_upload_host.hpp"
#include "shader_contract.hpp"
#include "shadow_rendering.hpp"

namespace {
using namespace aeg;
constexpr uint32_t kSize = 128;
constexpr pipeline::RenderResourceId kColor{"ShadowTest.Color"}, kDepth{"ShadowTest.Depth"}, kLight{"ShadowTest.Light"};
constexpr pipeline::RenderResourceId kShadow{"ShadowTest.Shadow"}, kColor2{"ShadowTest.Color2"},
    kDepth2{"ShadowTest.Depth2"};
constexpr pipeline::RenderResourceId kNormal{"GBufferTest.Normal"}, kMaterial{"GBufferTest.Material"};
constexpr pipeline::RenderResourceId kHdr{"LightingTest.HDR"}, kHdr2{"LightingTest.HDR2"};
constexpr pipeline::RenderResourceId kPost{"PostTest.Output"}, kPost2{"PostTest.Output2"};
constexpr std::array kNativePassTypes{renderer::kShadowPassTypeId,           renderer::kGBufferPassTypeId,
                                      renderer::kDeferredLightingPassTypeId, renderer::kSkyboxPostProcessPassTypeId,
                                      renderer::kZPrepassPassTypeId,         renderer::kLightCullingPassTypeId,
                                      renderer::kForwardOpaquePassTypeId};

pipeline::RenderPassTypeId operation_pass_type(const feature::RenderOperation& operation) {
    return std::visit(
        [](const auto& actual) -> pipeline::RenderPassTypeId {
            using Operation = std::decay_t<decltype(actual)>;
            if constexpr (std::is_same_v<Operation, feature::RenderDrawScene> ||
                          std::is_same_v<Operation, feature::RenderDrawClusteredScene>)
                return renderer::kForwardOpaquePassTypeId;
            else if constexpr (std::is_same_v<Operation, feature::RenderDrawGBufferScene>)
                return renderer::kGBufferPassTypeId;
            else if constexpr (std::is_same_v<Operation, feature::RenderDeferredLightingScene>)
                return renderer::kDeferredLightingPassTypeId;
            else if constexpr (std::is_same_v<Operation, feature::RenderPostProcessScene>)
                return renderer::kSkyboxPostProcessPassTypeId;
            else if constexpr (std::is_same_v<Operation, feature::RenderLightCullingScene>)
                return renderer::kLightCullingPassTypeId;
            else if constexpr (std::is_same_v<Operation, feature::RenderDrawDepthScene>)
                return actual.projection == feature::RenderDepthSceneProjection::kCamera ? renderer::kZPrepassPassTypeId
                                                                                         : renderer::kShadowPassTypeId;
            else
                return {};
        },
        operation);
}

struct NativePassRecord {
    pipeline::RenderPassConfiguration configuration{};  // Runtime data is reset; this is diagnostic metadata only.
    render_graph::RenderSceneOperationSource source{};
    const pipeline::RenderPassConfiguration* identity{};
    const pipeline::RenderPassRuntimeData* prepared_identity{};
    std::weak_ptr<const pipeline::RenderPassRuntimeData> prepared{};
    std::weak_ptr<const pipeline::RenderPass> delegate{};
    unsigned executions{};
};
struct NativePassObservations {
    const std::vector<feature::RenderFeatureOutput>* expected{nullptr};
    uint64_t frame{};
    std::vector<NativePassRecord> records{};
    std::array<unsigned, kNativePassTypes.size()> positive_setups{}, positive_executions{};
    unsigned foreign_setup_trials{}, foreign_execute_trials{};
    unsigned other_owner_setup{}, other_owner_execute{}, other_operation_setup{}, other_operation_execute{};
};

foundation::Result verify_native_operation(const pipeline::RenderPassConfiguration& configuration,
                                           const render_graph::RenderSceneOperationSource& source,
                                           const NativePassObservations& observation) {
    if (!configuration.instance.is_valid() || configuration.instance.value() == configuration.type.value() ||
        configuration.source.empty() || !configuration.enabled || !configuration.parameters.empty() ||
        !configuration.bindings.empty() || !configuration.runtime_data ||
        configuration.queue != (configuration.type == renderer::kLightCullingPassTypeId ? rhi::QueueClass::kCompute
                                                                                        : rhi::QueueClass::kGraphics))
        return foundation::invalid_state("registered native operation lost actual immutable configuration");
    if (!observation.expected)
        return foundation::Result::success();
    const auto found = std::find_if(observation.expected->begin(), observation.expected->end(),
                                    [&](const auto& output) { return output.instance_id == source.work.instance_id; });
    if (found == observation.expected->end() || source.owner != found->resource_owner ||
        source.work.domain != feature::FeatureDomain::kRender ||
        source.work.kind != render_graph::RenderWorkItemKind::kExecute || !source.work.id.is_valid() ||
        source.work.feature_definition_id != found->contract.definition_id ||
        source.work.owner_module != found->owner_module || source.work.node_id != found->node_id ||
        source.work.invocation != found->invocation || source.work.frame_index != observation.frame ||
        source.view_key != found->view_key || source.work.operation_index >= found->intent.operations.size() ||
        operation_pass_type(found->intent.operations[source.work.operation_index]) != configuration.type)
        return foundation::invalid_state("registered native operation differs from current captured Feature output");
    const auto& actual = found->intent.operations[source.work.operation_index];
    const auto kind = std::visit(
        [](const auto& operation) {
            using Operation = std::decay_t<decltype(operation)>;
            using Kind = render_graph::RenderSceneOperationKind;
            if constexpr (std::is_same_v<Operation, feature::RenderDrawScene>)
                return Kind::kDrawScene;
            else if constexpr (std::is_same_v<Operation, feature::RenderDrawClusteredScene>)
                return Kind::kDrawClusteredScene;
            else if constexpr (std::is_same_v<Operation, feature::RenderDrawDepthScene>)
                return Kind::kDrawDepthScene;
            else if constexpr (std::is_same_v<Operation, feature::RenderDrawGBufferScene>)
                return Kind::kDrawGBufferScene;
            else if constexpr (std::is_same_v<Operation, feature::RenderDeferredLightingScene>)
                return Kind::kDeferredLightingScene;
            else if constexpr (std::is_same_v<Operation, feature::RenderPostProcessScene>)
                return Kind::kPostProcessScene;
            else if constexpr (std::is_same_v<Operation, feature::RenderLightCullingScene>)
                return Kind::kLightCullingScene;
            else
                return Kind::kClearColor;
        },
        actual);
    if (source.kind != kind || (std::holds_alternative<feature::RenderDrawDepthScene>(actual) &&
                                source.depth_projection != std::get<feature::RenderDrawDepthScene>(actual).projection))
        return foundation::invalid_state("registered native operation selected another actual Scene role");
    const auto projected = render_graph::RenderFeatureProjector{}.project(*observation.expected, observation.frame);
    if (!projected) return projected.result;
    const auto expected_work = std::find_if(projected.value.begin(), projected.value.end(), [&](const auto& work) {
        return work.id == source.work.id && work.kind == source.work.kind && work.domain == source.work.domain &&
            work.feature_definition_id == source.work.feature_definition_id && work.owner_module == source.work.owner_module &&
            work.node_id == source.work.node_id && work.instance_id == source.work.instance_id &&
            work.invocation == source.work.invocation && work.frame_index == source.work.frame_index &&
            work.operation_index == source.work.operation_index;
    });
    if (expected_work == projected.value.end())
        return foundation::invalid_state("registered native operation differs from its actual projected WorkID or full token");
    return foundation::Result::success();
}

foundation::Result scope_current_scene_capture(std::vector<feature::RenderFeatureOutput>& outputs,
    const feature::FeatureRuntime& runtime, renderer::RenderViewId view) {
    if (!view.is_valid() || outputs.empty()) return foundation::invalid_state("Scene capture has no actual current View or output");
    for (auto& output : outputs) {
        const auto* instance = runtime.find_instance(output.instance_id);
        if (!instance || !instance->last_invocation_committed ||
            instance->last_invocation_committed->attempt.token != output.invocation ||
            output.resource_owner.instance_id != output.instance_id || output.resource_owner.node_id != output.node_id ||
            output.resource_owner.definition_digest != instance->definition_digest ||
            output.resource_owner.resource_epoch != instance->render_resource_epoch)
            return foundation::invalid_state("Scene capture differs from its actual committed Runtime owner and invocation");
        output = renderer::scope_render_feature_output(output, view, true);
    }
    return foundation::Result::success();
}

class ObservedNativePass final : public pipeline::RenderPass {
public:
    ObservedNativePass(std::shared_ptr<const pipeline::RenderPass> delegate, NativePassObservations& observation)
        : delegate_(std::move(delegate)), observation_(observation) {}
    const pipeline::RenderPassDescriptor& descriptor() const noexcept override { return delegate_->descriptor(); }
    foundation::Result setup(pipeline::RenderPassBuilder& builder,
                             const pipeline::RenderPassConfiguration& configuration) const override {
        const auto source = renderer::detail::NativeScenePassInspection::source(configuration);
        if (!source)
            return source.result;
        if (const auto result = verify_native_operation(configuration, source.value, observation_); !result)
            return result;
        if (observation_.expected) {
            for (const auto& other : observation_.records) {
                const auto prepared = other.prepared.lock();
                if (!prepared || other.configuration.type != configuration.type ||
                    (other.configuration.instance == configuration.instance &&
                     other.configuration.source == configuration.source))
                    continue;
                auto replaced = configuration;
                replaced.runtime_data = prepared;
                ++observation_.foreign_setup_trials;
                if (other.source.work.instance_id != source.value.work.instance_id)
                    ++observation_.other_owner_setup;
                else if (other.source.work.operation_index != source.value.work.operation_index)
                    ++observation_.other_operation_setup;
                if (delegate_->setup(builder, replaced).code != foundation::ResultCode::kInvalidArgument)
                    return foundation::invalid_state("native setup accepted another actual prepared operation");
                break;
            }
        }
        const auto result = delegate_->setup(builder, configuration);
        if (!result)
            return result;
        if (observation_.expected) {
            auto metadata = configuration;
            metadata.runtime_data.reset();
            observation_.records.push_back({std::move(metadata), source.value, &configuration,
                                            configuration.runtime_data.get(), configuration.runtime_data, delegate_,
                                            0});
        }
        return result;
    }
    foundation::Result execute(pipeline::RenderPassExecutionContext& command,
                               const pipeline::RenderPassConfiguration& configuration) const override {
        const auto source = renderer::detail::NativeScenePassInspection::source(configuration);
        if (!source)
            return source.result;
        if (const auto result = verify_native_operation(configuration, source.value, observation_); !result)
            return result;
        const auto record = std::find_if(observation_.records.begin(), observation_.records.end(),
                                         [&](const auto& value) { return value.identity == &configuration; });
        if (record == observation_.records.end() || record->prepared_identity != configuration.runtime_data.get() ||
            record->prepared.expired() || record->delegate.expired() ||
            record->source.work.invocation != source.value.work.invocation ||
            record->source.owner != source.value.owner || record->source.work.id != source.value.work.id ||
            record->configuration.instance != configuration.instance ||
            record->configuration.source != configuration.source || record->configuration.type != configuration.type ||
            record->configuration.queue != configuration.queue ||
            record->configuration.enabled != configuration.enabled ||
            record->configuration.parameters != configuration.parameters ||
            record->configuration.bindings != configuration.bindings)
            return foundation::invalid_state("native execution lost its actual setup identity or owners");
        for (const auto& other : observation_.records) {
            const auto prepared = other.prepared.lock();
            if (!prepared || other.configuration.type != configuration.type ||
                (other.configuration.instance == configuration.instance &&
                 other.configuration.source == configuration.source))
                continue;
            auto replaced = configuration;
            replaced.runtime_data = prepared;
            ++observation_.foreign_execute_trials;
            if (other.source.work.instance_id != source.value.work.instance_id)
                ++observation_.other_owner_execute;
            else if (other.source.work.operation_index != source.value.work.operation_index)
                ++observation_.other_operation_execute;
            if (delegate_->execute(command, replaced).code != foundation::ResultCode::kInvalidArgument)
                return foundation::invalid_state("native execution accepted another actual prepared operation");
            break;
        }
        const auto result = delegate_->execute(command, configuration);
        if (result)
            ++record->executions;
        return result;
    }

private:
    std::shared_ptr<const pipeline::RenderPass> delegate_;
    NativePassObservations& observation_;
};

foundation::Result register_observed_native_passes(pipeline::RenderPassRegistry& passes,
                                                   NativePassObservations& observation) {
    if (const auto result = renderer::register_builtin_render_passes(passes); !result)
        return result;
    for (const auto type : kNativePassTypes) {
        const auto found = pipeline::detail::RenderPassRegistryExecution::find_pass(passes, type);
        if (!found)
            return found.result;
        if (const auto result = passes.unregister_pass(type); !result)
            return result;
        if (const auto result = passes.register_pass([owner = found.value, &observation] {
                return std::make_unique<ObservedNativePass>(owner, observation);
            });
            !result)
            return result;
    }
    return foundation::Result::success();
}

foundation::Result verify_native_graph(const render_graph::RenderGraph& graph, NativePassObservations& observation) {
    if (!observation.expected)
        return foundation::invalid_state("native graph has no current captured output oracle");
    std::array<unsigned, kNativePassTypes.size()> expected{};
    for (const auto& output : *observation.expected)
        for (const auto& operation : output.intent.operations) {
            const auto type = operation_pass_type(operation);
            const auto found = std::find(kNativePassTypes.begin(), kNativePassTypes.end(), type);
            if (found != kNativePassTypes.end())
                ++expected[static_cast<size_t>(found - kNativePassTypes.begin())];
        }
    std::array<unsigned, kNativePassTypes.size()> actual{};
    for (const auto& record : observation.records) {
        const auto found = std::find(kNativePassTypes.begin(), kNativePassTypes.end(), record.configuration.type);
        if (found == kNativePassTypes.end())
            return foundation::invalid_state("native pass used an unknown fixed role");
        const auto index = static_cast<size_t>(found - kNativePassTypes.begin());
        ++actual[index];
        const auto live = std::any_of(
            render_graph::detail::RenderGraphExecution::plan(graph).pass_plans.begin(),
            render_graph::detail::RenderGraphExecution::plan(graph).pass_plans.end(), [&](const auto& pass) {
                return pass.live && pass.feature_work &&
                       pass.feature_work->kind == render_graph::RenderWorkItemKind::kExecute &&
                       pass.feature_work->id == record.source.work.id;
            });
        if (record.executions != (live ? 1u : 0u))
            return foundation::invalid_state("native execution count differs from actual live Feature work");
        ++observation.positive_setups[index];
        observation.positive_executions[index] += record.executions;
    }
    return actual == expected
               ? foundation::Result::success()
               : foundation::invalid_state("captured Scene operations bypassed their actual native registered roles");
}

struct ClusteredFixtureResources {
    renderer::RenderLightCullingResource lights{};
    size_t resolve_calls{0};
    size_t compute_calls{0};
    std::vector<rhi::BindingSetHandle> forward_sets{};
    rhi::TextureViewHandle camera_view{};
    std::vector<rhi::TextureViewHandle> shadow_views{};
    rhi::BufferHandle clusters{};
};
/** Test seam forwards to the real lowerer; it neither allocates light lists nor records draws. */
class ObservedSceneLowerer final : public render_graph::RenderSceneDrawLowerer {
public:
    explicit ObservedSceneLowerer(const render_graph::RenderSceneDrawLowerer& native) : native_(native) {}
    mutable std::shared_ptr<const render_graph::RenderLightListLease> saved{}, fresh{};
    mutable pipeline::RenderGraphBuffer saved_witness{}, fresh_witness{};
    mutable size_t consumer_calls{0};
    size_t fail_consumer{0};
    bool return_saved{false}, consume_saved{false}, create_witness{false};
    renderer::RenderMaterialResource* material{nullptr};
    const renderer::RenderMaterialResource* replacement_material{nullptr};
    size_t replacement_consumer{0};
    mutable size_t material_replacements{0};
    feature::RenderResourceId contract_probe_scene{};
    bool probe_material_replacement{false};
    mutable size_t contract_probe_calls{0}, contract_probe_successes{0};
    std::shared_ptr<const void> depth_snapshot_override{};
    std::optional<feature::RenderDepthSceneProjection> depth_override_projection{};
    mutable unsigned depth_override_calls{0};
    foundation::ResultValue<std::shared_ptr<const void>> lower(pipeline::RenderGraphBuilder& builder,
        const feature::RenderDrawDepthScene& draw, const render_graph::RenderSceneDrawTargets& targets,
        const feature::RenderResourceOwner& owner, uint64_t frame) const override {
        auto result = native_.lower(builder, draw, targets, owner, frame);
        if (result && depth_snapshot_override && (!depth_override_projection || draw.projection == *depth_override_projection)) {
            result.value = depth_snapshot_override;
            ++depth_override_calls;
        }
        return result;
    }
#define FORWARD_SCENE_LOWER(Type) \
    foundation::ResultValue<std::shared_ptr<const void>> lower(pipeline::RenderGraphBuilder& builder, \
        const feature::Type& draw, const render_graph::RenderSceneDrawTargets& targets, \
        const feature::RenderResourceOwner& owner, uint64_t frame) const override { \
        const auto result = native_.lower(builder, draw, targets, owner, frame); \
        if (contract_probe_scene.is_valid() && draw.scene.id == contract_probe_scene) { \
            ++contract_probe_calls; \
            if (result) { \
                ++contract_probe_successes; \
                return {foundation::invalid_argument("test contract probe accepted invalid resource; forced rollback"), {}}; \
            } \
        } \
        return result; \
    }
    FORWARD_SCENE_LOWER(RenderDrawScene)
    FORWARD_SCENE_LOWER(RenderDrawGBufferScene)
    FORWARD_SCENE_LOWER(RenderDeferredLightingScene)
    FORWARD_SCENE_LOWER(RenderPostProcessScene)
#undef FORWARD_SCENE_LOWER
    foundation::ResultValue<std::shared_ptr<const render_graph::RenderLightListLease>> lower(
        pipeline::RenderGraphBuilder& builder, const feature::RenderLightCullingScene& draw,
        const render_graph::RenderSceneDrawTargets& targets, const feature::RenderResourceOwner& owner,
        const render_graph::RenderFeatureBuildScope& scope, const render_graph::RenderLightListProduction& production) const override {
        if (create_witness) {
            rhi::BufferDesc description{};
            description.size = 4;
            description.capabilities = rhi::BufferCapability::kStorage;
            const auto witness = builder.create_buffer(description);
            if (!witness) return {witness.result, {}};
            fresh_witness = witness.value;
        }
        auto result = native_.lower(builder, draw, targets, owner, scope, production);
        if (result) {
            fresh = result.value;
            if (!saved) { saved = fresh; saved_witness = fresh_witness; }
            if (return_saved) result.value = saved;
        }
        return result;
    }
    foundation::Result lower(pipeline::RenderGraphBuilder& builder, const feature::RenderDrawClusteredScene& draw,
        const render_graph::RenderSceneDrawTargets& targets, const feature::RenderResourceOwner& owner,
        const render_graph::RenderFeatureBuildScope& scope, const render_graph::RenderLightListLease& list) const override {
        ++consumer_calls;
        struct RestoreMaterial final {
            renderer::RenderMaterialResource* target{};
            renderer::RenderMaterialResource original{};
            ~RestoreMaterial() { if (target) *target = std::move(original); }
        } restore;
        if (material && replacement_material && consumer_calls == replacement_consumer) {
            restore.target = material;
            restore.original = *material;
            *material = *replacement_material;
            ++material_replacements;
        }
        const bool probe = probe_material_replacement && restore.target;
        if (probe) ++contract_probe_calls;
        const auto result = native_.lower(builder, draw, targets, owner, scope, consume_saved ? *saved : list);
        if (result && probe) {
            ++contract_probe_successes;
            return foundation::invalid_argument("test contract probe accepted invalid resource; forced rollback");
        }
        if (result && consumer_calls == fail_consumer) return foundation::invalid_argument("injected failure after actual consumer lowering");
        return result;
    }
private:
    const render_graph::RenderSceneDrawLowerer& native_;
};
feature::RenderDrawGBufferScene gbuffer_draw(feature::RenderSceneReference scene, feature::RenderAttachmentLoad load) {
    using namespace feature;
    RenderDrawGBufferScene draw{};
    draw.scene = scene;
    draw.albedo = {{RenderResourceId(kColor.value()), 1, true}, load, {.125f, .25f, .375f, .5f}};
    draw.normal = {{RenderResourceId(kNormal.value()), 1, true}, load, {.75f, .5f, .25f, .125f}};
    draw.material = {{RenderResourceId(kMaterial.value()), 1, true}, load, {.25f, .5f, .75f, 1.f}};
    draw.depth = {{RenderResourceId(kDepth.value()), 1, true}, load, .75f};
    if (load == RenderAttachmentLoad::kLoad) {
        draw.albedo.clear_color = draw.normal.clear_color = draw.material.clear_color = {1.f, 0.f, 0.f, 0.f};
        draw.depth.clear_depth = .25f;
    }
    return draw;
}
float half_value(uint16_t value) {
    const float sign = value & 0x8000 ? -1.f : 1.f;
    const auto exponent = (value >> 10) & 31;
    const auto mantissa = value & 1023;
    if (exponent == 31)
        return sign * (mantissa ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity());
    return exponent ? sign * std::ldexp(1.f + mantissa / 1024.f, exponent - 15)
                    : sign * std::ldexp(static_cast<float>(mantissa), -24);
}
bool check(const foundation::Result& r) {
    if (!r)
        std::cerr << r.message << '\n';
    return bool(r);
}
rhi::BufferDesc upload_desc(uint64_t size, rhi::BufferCapability capability, rhi::ResourceUsage usage) {
    rhi::BufferDesc desc{};
    desc.size = size;
    desc.capabilities = capability;
    desc.memory = rhi::MemoryClass::kUpload;
    desc.initial_usage = usage;
    desc.initial_queue = rhi::QueueClass::kGraphics;
    return desc;
}
struct Resolver final : renderer::RenderResourceResolver {
    rhi::Device& device;
    render_graph::FrameContext& frame;
    renderer::ShadowResources& shadows;
    renderer::DeferredLightingResources& deferred;
    renderer::LightCullingResources& culling;
    renderer::PostProcessResources& post;
    feature::RenderPostProcessSettings post_settings{};
    bool runtime_post{false}, fail_post_inputs{false};
    uint8_t post_failure{0};
    mutable bool attachment_probe_preconditions{true};
    mutable size_t attachment_probe_mutations{0};
    mutable std::vector<renderer::RenderPostProcessResource> post_resources{};
    mutable std::vector<rhi::BindingSetHandle> post_input_sets{};
    renderer::RenderMeshResource mesh{};
    renderer::RenderMaterialResource material{};
    ClusteredFixtureResources* observed_culling{nullptr};
    std::array<std::pair<const renderer::RenderSceneSnapshot*, ClusteredFixtureResources*>, 2> observed_culling_by_scene{};
    mutable size_t culling_resolutions{0};
    uint8_t culling_failure{0};
    std::optional<renderer::RenderLightCullingResource> substituted_culling{};
    std::optional<renderer::RenderMaterialResource> rejected_material{};
    renderer::RenderLightingResource lighting{};
    bool runtime_lighting{false};
    uint8_t deferred_failure{0};
    bool fail_deferred_inputs{false};
    mutable renderer::RenderDeferredLightingResource last_deferred{};
    mutable std::vector<rhi::BindingSetHandle> deferred_input_sets{};
    rhi::BindingLayoutHandle object_layout{};
    application::runtime::RuntimeAssetResolver objects;
    uint8_t shadow_failure{0};
    mutable renderer::RenderShadowResource last_shadow{};
    mutable renderer::RenderLightCullingResource last_culling{};
    mutable renderer::RenderPostProcessResource last_post{};
    Resolver(rhi::Device& d, render_graph::FrameContext& f, renderer::ShadowResources& s,
             renderer::DeferredLightingResources& l, renderer::LightCullingResources& c,
             renderer::PostProcessResources& p)
        : device(d), frame(f), shadows(s), deferred(l), culling(c), post(p) {}
    foundation::ResultValue<renderer::RenderPostProcessResource> resolve_post_process(
        const renderer::RenderView& view) const override {
        const bool rejected = view.id == renderer::RenderViewId("Rejected.Post.View");
        if (rejected && post_failure == 1)
            return {foundation::not_ready("test PostProcess unavailable"), {}};
        auto result = post.for_frame(frame, view, runtime_post ? view.post_process : post_settings);
        if (result && rejected) {
            if (post_failure == 2) result.value.pipeline = {};
            if (post_failure == 3) result.value.prepare_inputs = {};
            if (post_failure == 4) result.value.constants = {};
            if (post_failure == 5) result.value.constants_desc.size = 1;
            if (post_failure == 6) result.value.constants_desc.capabilities = rhi::BufferCapability::kVertex;
            if (post_failure == 7) result.value.attachment_contract.color_format = rhi::Format::kRgba16Float;
            if (post_failure == 8) result.value.attachment_contract.samples = rhi::SampleCount::kX4;
            if (post_failure == 9) result.value.attachment_contract.depth_stencil_format = rhi::Format::kD32Float;
            if (post_failure == 10) result.value.attachment_contract.additional_color_formats[0] = rhi::Format::kRgba8Unorm;
            if (post_failure == 11) result.value.attachment_contract.additional_color_formats[1] = rhi::Format::kRgba16Float;
            if (post_failure == 12) {
                auto& profile = result.value.profile_contract;
                profile.attachment.color_format = rhi::Format::kRgba16Float;
                attachment_probe_preconditions = attachment_probe_preconditions &&
                    profile.is_valid(renderer::RendererProfile::kPostProcess) &&
                    profile.attachment.is_single_color_attachment() &&
                    result.value.attachment_contract.is_single_color_attachment() &&
                    profile.attachment != result.value.attachment_contract;
                ++attachment_probe_mutations;
            }
        }
        if (result && runtime_post && result.value.prepare_inputs) {
            const auto prepare = result.value.prepare_inputs;
            result.value.prepare_inputs = [this, prepare](rhi::TextureViewHandle color, rhi::TextureViewHandle depth) {
                if (fail_post_inputs)
                    return foundation::ResultValue<rhi::BindingSetHandle>{foundation::not_ready("test PostProcess execute failure"), {}};
                const auto set = prepare(color, depth);
                if (set) post_input_sets.push_back(set.value);
                return set;
            };
            post_resources.push_back(result.value);
        }
        if (result)
            last_post = result.value;
        return result;
    }
    foundation::ResultValue<renderer::RenderMeshResource> resolve_mesh(renderer::MeshAssetId) const override {
        return {foundation::Result::success(), mesh};
    }
    foundation::ResultValue<renderer::RenderMaterialResource> resolve_material(
        renderer::MaterialInstanceId id) const override {
        return {foundation::Result::success(), id == renderer::MaterialInstanceId("Rejected.GBuffer.Material") && rejected_material
            ? *rejected_material : material};
    }
    foundation::ResultValue<renderer::RenderLightingResource> resolve_lighting(
        const renderer::RenderSceneSnapshot& snapshot, const renderer::RenderView& view) const override {
        if (runtime_lighting) {
            const bool rejected = view.id == renderer::RenderViewId("Rejected.Lighting.View");
            if (rejected && deferred_failure == 15) return {foundation::not_ready("test lighting unavailable"), {}};
            auto result = objects.resolve_lighting(snapshot, view);
            if (result && rejected) {
                if (deferred_failure == 16) result.value.lighting_size = 1;
                if (deferred_failure == 17) result.value.lighting_buffer_desc.capabilities = rhi::BufferCapability::kVertex;
                if (deferred_failure == 18) result.value.binding_group = 3;
                if (deferred_failure == 19) result.value.lighting_buffer_desc.size = 1;
                if (deferred_failure == 20) result.value.lighting_resource.index = 999999;
            }
            return result;
        }
        return {foundation::Result::success(), lighting};
    }
    foundation::ResultValue<renderer::RenderDeferredLightingResource> resolve_deferred_lighting(
        const renderer::RenderView& view) const override {
        const bool rejected = view.id == renderer::RenderViewId("Rejected.Lighting.View");
        if (rejected && deferred_failure == 1) return {foundation::not_ready("test DeferredLighting unavailable"), {}};
        auto result = deferred.for_frame(frame);
        if (result && rejected) {
            if (deferred_failure == 2) result.value.pipeline = {};
            if (deferred_failure == 3) result.value.prepare_inputs = {};
            if (deferred_failure == 4) result.value.attachment_contract.color_format = rhi::Format::kRgba8Unorm;
            if (deferred_failure == 5) result.value.attachment_contract.samples = rhi::SampleCount::kX4;
            if (deferred_failure == 6) result.value.attachment_contract.depth_stencil_format = rhi::Format::kD32Float;
            if (deferred_failure == 7) result.value.attachment_contract.additional_color_formats[0] = rhi::Format::kRgba16Float;
            if (deferred_failure == 8) result.value.attachment_contract.additional_color_formats[1] = rhi::Format::kRgba8Unorm;
            if (deferred_failure == 9) result.value.shadow_pipeline = {};
            if (deferred_failure == 10) result.value.shadow_pipeline_contract.color_format = rhi::Format::kRgba8Unorm;
            if (deferred_failure == 11) result.value.shadow_pipeline_contract.samples = rhi::SampleCount::kX4;
            if (deferred_failure == 12) result.value.shadow_pipeline_contract.depth_stencil_format = rhi::Format::kD32Float;
            if (deferred_failure == 13) result.value.shadow_pipeline_contract.additional_color_formats[0] = rhi::Format::kRgba16Float;
            if (deferred_failure == 14) result.value.shadow_pipeline_contract.additional_color_formats[1] = rhi::Format::kRgba8Unorm;
            if (deferred_failure == 21 || deferred_failure == 22) {
                const bool shadowed = deferred_failure == 22;
                auto& profile = shadowed ? result.value.shadow_profile_contract : result.value.profile_contract;
                const auto& attachment = shadowed ? result.value.shadow_pipeline_contract : result.value.attachment_contract;
                profile.attachment.color_format = rhi::Format::kRgba8Unorm;
                attachment_probe_preconditions = attachment_probe_preconditions &&
                    profile.is_valid(renderer::RendererProfile::kDeferredLighting) &&
                    profile.attachment.is_single_color_attachment() && attachment.is_single_color_attachment() &&
                    profile.attachment != attachment;
                ++attachment_probe_mutations;
            }
        }
        if (result && runtime_lighting && result.value.prepare_inputs) {
            result.value.prepare_inputs = [this, prepare = result.value.prepare_inputs](const renderer::RenderDeferredLightingInputs& inputs) {
                if (fail_deferred_inputs)
                    return foundation::ResultValue<rhi::BindingSetHandle>{foundation::not_ready("test DeferredLighting input preparation failed"), {}};
                const auto prepared = prepare(inputs);
                if (prepared) deferred_input_sets.push_back(prepared.value);
                return prepared;
            };
        }
        if (result && runtime_lighting) last_deferred = result.value;
        return result;
    }
    foundation::ResultValue<renderer::RenderShadowResource> resolve_shadow(
        const renderer::RenderShadowView& shadow) const override {
        if (shadow_failure == 1) return {foundation::not_ready("test shadow resource unavailable"), {}};
        auto result = shadows.for_frame(frame, shadow.constants);
        if (result && shadow_failure == 2) result.value.prepare_inputs = {};
        if (result && shadow_failure == 3) result.value.buffer_desc.size = 1;
        if (result && shadow_failure == 4) result.value.buffer_desc.capabilities = rhi::BufferCapability::kVertex;
        if (result)
            last_shadow = result.value;
        return result;
    }
    foundation::ResultValue<renderer::RenderLightCullingResource> resolve_light_culling(
        const renderer::RenderSceneSnapshot& scene, const renderer::RenderShadowView& shadow) const override {
        auto result = substituted_culling
            ? foundation::ResultValue<renderer::RenderLightCullingResource>{foundation::Result::success(), *substituted_culling}
            : culling.for_frame(frame, scene, shadow);
        if (result) switch (culling_failure) {
        case 1: result.value.constants_desc.size = 1; break;
        case 2: result.value.lights = result.value.constants; break;
        case 3: result.value.cluster_bytes += 4; break;
        case 4: result.value.shadow_enabled = !result.value.shadow_enabled; break;
        case 5: result.value.pipeline = {}; break;
        case 6: result.value.prepare_forward = {}; break;
        case 7: ++result.value.tile_count_x; break;
        case 8: result.value.lights_desc.initial_stages = rhi::PipelineStage::kCopy; break;
        default: break;
        }
        if (result) {
            last_culling = result.value;
            ++culling_resolutions;
            auto* observed = observed_culling;
            for (const auto& [source, bucket] : observed_culling_by_scene) if (source == &scene) observed = bucket;
            if (observed && result.value.prepare_compute && result.value.prepare_forward) {
                const auto compute = result.value.prepare_compute, forward = result.value.prepare_forward;
                ++observed->resolve_calls;
                observed->lights = result.value;
                result.value.prepare_compute = [observed, compute](rhi::TextureViewHandle camera, rhi::BufferHandle clusters) {
                    auto prepared = compute(camera, clusters);
                    if (prepared) { ++observed->compute_calls; observed->clusters = clusters; observed->camera_view = camera; }
                    return prepared;
                };
                result.value.prepare_forward = [observed, forward, shadow_enabled = result.value.shadow_enabled](rhi::TextureViewHandle map, rhi::BufferHandle clusters) {
                    if (map.is_valid() != shadow_enabled || clusters != observed->clusters)
                        return foundation::ResultValue<rhi::BindingSetHandle>{foundation::invalid_state("consumer used another list or shadow"), {}};
                    auto prepared = forward(map, clusters);
                    if (prepared) { observed->forward_sets.push_back(prepared.value); observed->shadow_views.push_back(map); }
                    return prepared;
                };
            }
        }
        return result;
    }
    foundation::ResultValue<renderer::RenderObjectResource> resolve_object(
        const renderer::RenderInstance& instance, const renderer::RenderView& view) const override {
        return objects.resolve_object(instance, view);
    }
};
}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--clustered-reflection") {
        const auto shadow = shader::load_shader_program(AEG_FORWARD_CLUSTERED_SHADER_PACKAGE_PATH);
        const auto plain = shader::load_shader_program(AEG_FORWARD_CLUSTERED_UNSHADOWED_SHADER_PACKAGE_PATH);
        if (!shadow || !plain) return 33;
        const auto binding = [](const shader::ShaderProgram& program, uint16_t group, uint16_t slot) {
            const auto& resources = program.contract.reflection.resources;
            return std::find_if(resources.begin(), resources.end(), [&](const auto& resource) {
                return resource.group == group && resource.binding == slot;
            }) != resources.end();
        };
        if (!binding(shadow.value, 3, 0) || binding(plain.value, 3, 0)) {
            std::cerr << "unshadowed clustered entry still reflects the shadow descriptor\n";
            return 34;
        }
        for (uint16_t slot : {uint16_t{1}, uint16_t{2}, uint16_t{3}})
            if (!binding(shadow.value, 3, slot) || !binding(plain.value, 3, slot)) return 35;
        for (const auto& expected : shadow.value.contract.reflection.resources) {
            if (expected.group == 3 && expected.binding == 0) continue;
            const auto& resources = plain.value.contract.reflection.resources;
            const auto actual = std::find_if(resources.begin(), resources.end(), [&](const auto& resource) {
                return resource.group == expected.group && resource.binding == expected.binding;
            });
            if (actual == resources.end() || actual->type != expected.type || actual->array_mode != expected.array_mode ||
                actual->array_count != expected.array_count || actual->shader_stages != expected.shader_stages ||
                actual->flags != expected.flags) return 36;
        }
        if (plain.value.contract.reflection.resources.size() + 1 != shadow.value.contract.reflection.resources.size()) return 37;
        std::cout << "actual clustered entry reflection preserves all bindings except the removed shadow texture\n";
        return 0;
    }
    const bool gbuffer_mode = argc > 1 && std::string_view(argv[1]) == "--feature-gbuffer";
    const bool post_feature_mode = argc > 1 && std::string_view(argv[1]) == "--feature-post-process";
    const bool lighting_mode = post_feature_mode || (argc > 1 && std::string_view(argv[1]) == "--feature-deferred");
    const bool gbuffer_path = gbuffer_mode || lighting_mode;
    const bool shadow_feature_mode = argc > 1 && std::string_view(argv[1]) == "--feature-intent";
    const bool clustered_shadow_wave_mode = argc > 1 && std::string_view(argv[1]) == "--feature-clustered-shadow-publication";
    const bool clustered_shadow_cancel_mode = argc > 1 && std::string_view(argv[1]) == "--feature-clustered-shadow-process-cancel";
    const bool clustered_shadow_process_mode = clustered_shadow_cancel_mode ||
        (argc > 1 && std::string_view(argv[1]) == "--feature-clustered-shadow-process-suspend");
    const bool clustered_shadow_timing_mode = clustered_shadow_wave_mode || clustered_shadow_process_mode;
    const bool clustered_wave_mode = clustered_shadow_wave_mode ||
        (argc > 1 && std::string_view(argv[1]) == "--feature-clustered-publication");
    const bool clustered_shadow_mode = clustered_shadow_timing_mode ||
        (argc > 1 && std::string_view(argv[1]) == "--feature-clustered-shadow");
    const uint32_t final_wave_iteration = clustered_shadow_wave_mode ? 2u : 6u;
    const bool clustered_pair_mode = argc > 1 && std::string_view(argv[1]) == "--feature-clustered-two-producers";
    const bool clustered_cancel_mode = clustered_shadow_cancel_mode ||
        (argc > 1 && std::string_view(argv[1]) == "--feature-clustered-process-cancel");
    const bool clustered_process_mode = clustered_cancel_mode || clustered_shadow_process_mode ||
        (argc > 1 && std::string_view(argv[1]) == "--feature-clustered-process-suspend");
    const bool unshadowed_mode = argc > 1 && std::string_view(argv[1]) == "--clustered-unshadowed";
    const bool clustered_feature_mode = unshadowed_mode || clustered_wave_mode || clustered_process_mode || clustered_shadow_mode || clustered_pair_mode ||
        (argc > 1 && std::string_view(argv[1]) == "--feature-clustered");
    const bool feature_mode = shadow_feature_mode || gbuffer_path || clustered_feature_mode;
    const bool scene_profile_mode = !feature_mode;
    const bool post_process = argc > 1 && (std::string_view(argv[1]) == "--post-process" ||
                                           std::string_view(argv[1]) == "--deferred-post-process");
    const bool deferred_mode = argc > 1 && (std::string_view(argv[1]) == "--deferred" ||
                                            std::string_view(argv[1]) == "--deferred-post-process");
    const bool clustered_hdr = clustered_feature_mode;
    const bool local_lights = (clustered_hdr && !clustered_shadow_mode) || (argc > 1 && std::string_view(argv[1]) == "--local-lights");
    const auto created = rhi::create_device({rhi::BackendType::kVulkan, "Shadow pixels"});
    if (!created)
        return 1;
    auto& device = *created.value;
    shader::ShaderManager shaders(device);
    renderer::DeferredLightingResources deferred;
    renderer::ShadowResources shadows;
    renderer::LightCullingResources culling;
    renderer::PostProcessResources post;
    if (!check(post.initialize(device, AEG_POST_PROCESS_SHADER_PACKAGE_PATH, rhi::Format::kRgba8Unorm)))
        return 32;
    if (!check(deferred.initialize(device, AEG_DEFERRED_LIGHTING_SHADER_PACKAGE_PATH, renderer::kSceneLightingFormat,
                                   AEG_DEFERRED_SHADOW_SHADER_PACKAGE_PATH)))
        return 2;
    render_graph::FrameContext frame(device, {.frames_in_flight = 2});
    render_graph::RenderGraph graph(device);
    resource::ResourceUploader uploader(device);
    Resolver resolver(device, frame, shadows, deferred, culling, post);
    resolver.runtime_lighting = lighting_mode;
    const std::array<rhi::VertexBindingDesc, 1> bindings{{{0, 36, rhi::VertexInputRate::kVertex}}};
    const std::array<rhi::VertexAttributeDesc, 3> attributes{{{0, 0, rhi::Format::kRgba32Float, 0},
                                                              {1, 0, rhi::Format::kRg32Float, 16},
                                                              {2, 0, rhi::Format::kRgb32Float, 24}}};
    const std::array colors{rhi::Format::kRgba8Unorm};
    rhi::GraphicsPipelineState graphics{};
    graphics.vertex_bindings = bindings;
    graphics.vertex_attributes = attributes;
    graphics.cull_mode = rhi::CullMode::kNone;
    graphics.depth_test_enable = true;
    graphics.depth_write_enable = true;
    graphics.depth_stencil_format = rhi::Format::kD32Float;
    graphics.color_formats = colors;
    const auto opaque_program = shader::load_shader_program(AEG_OPAQUE_UNLIT_SHADER_PACKAGE_PATH);
    const auto shadow_program = shader::load_shader_program(AEG_FORWARD_SHADOW_SHADER_PACKAGE_PATH);
    const auto gbuffer_program = shader::load_shader_program(AEG_GBUFFER_MESH_SHADER_PACKAGE_PATH);
    const auto depth_program = shader::load_shader_program(AEG_DEPTH_MESH_SHADER_PACKAGE_PATH);
    const auto clustered_program = shader::load_shader_program(AEG_FORWARD_CLUSTERED_SHADER_PACKAGE_PATH);
    const auto unshadowed_program = shader::load_shader_program(AEG_FORWARD_CLUSTERED_UNSHADOWED_SHADER_PACKAGE_PATH);
    if (!opaque_program || !shadow_program || !gbuffer_program || !depth_program || !clustered_program)
        return 3;
    const auto opaque_layout = shaders.build_pipeline_layout(opaque_program.value);
    const auto shadow_layout = shaders.build_pipeline_layout(shadow_program.value);
    const auto gbuffer_layout = shaders.build_pipeline_layout(gbuffer_program.value);
    const auto depth_layout = shaders.build_pipeline_layout(depth_program.value);
    const auto clustered_layout = shaders.build_pipeline_layout(clustered_program.value);
    if (!unshadowed_program) return 4;
    const auto unshadowed_layout = shaders.build_pipeline_layout(unshadowed_program.value);
    if (!opaque_layout || !shadow_layout || !gbuffer_layout || !depth_layout || !clustered_layout)
        return 4;
    const auto clustered_inputs = shaders.binding_layout(clustered_layout.value, 3);
    if (!unshadowed_layout) return 4;
    const auto unshadowed_inputs = shaders.binding_layout(unshadowed_layout.value, 3);
    if (!clustered_inputs || !unshadowed_inputs ||
        !check(culling.initialize(device, AEG_LIGHT_CULLING_SHADER_PACKAGE_PATH, clustered_inputs.value, unshadowed_inputs.value)))
        return 4;
    const auto shadow_inputs = shaders.binding_layout(shadow_layout.value, 3);
    if (!shadow_inputs || !check(shadows.initialize(device, shadow_inputs.value)))
        return 4;
    if (argc == 1) {
        auto foreign_device_result = rhi::create_device({rhi::BackendType::kVulkan,
                                                          "Renderer Foreign Profile Frame Smoke"});
        if (!foreign_device_result)
            return 6;
        render_graph::FrameContext foreign_frame(*foreign_device_result.value, {.frames_in_flight = 1});
        if (!foreign_frame.begin_frame())
            return 6;
        renderer::RenderShadowConstants foreign_constants{};
        foreign_constants.parameters = {1.0f, 1.0f, 0.0f, 0.0f};
        const auto foreign_deferred = deferred.for_frame(foreign_frame);
        const auto foreign_shadow = shadows.for_frame(foreign_frame, foreign_constants);
        const bool deferred_rejected = !foreign_deferred &&
            foreign_deferred.result.code == foundation::ResultCode::kInvalidState;
        const bool shadow_rejected = !foreign_shadow &&
            foreign_shadow.result.code == foundation::ResultCode::kInvalidState;
        if (!foreign_frame.abort_frame())
            return 6;
        if (!deferred_rejected || !shadow_rejected) {
            std::cerr << "Renderer profile resources accepted a FrameContext from a foreign RHI device\n";
            return 6;
        }
    }
    const auto opaque =
        shaders.create_pipeline(opaque_program.value, opaque_layout.value, {rhi::PipelineType::kGraphics, graphics});
    const auto shadow_opaque =
        shaders.create_pipeline(shadow_program.value, shadow_layout.value, {rhi::PipelineType::kGraphics, graphics});
    const renderer::RenderMaterialShaderContract material_shader_contract{
        renderer::kRenderMaterialShaderContractVersion,
        shader::detail::ShaderPipelineLayoutAccess::contract_hash(opaque_layout.value),
        shader::detail::ShaderPipelineLayoutAccess::cache_epoch(opaque_layout.value)};
    const renderer::RenderMaterialShaderContract shadow_shader_contract{
        renderer::kRenderMaterialShaderContractVersion,
        shader::detail::ShaderPipelineLayoutAccess::contract_hash(shadow_layout.value),
        shader::detail::ShaderPipelineLayoutAccess::cache_epoch(shadow_layout.value)};
    const std::array hdr_colors{renderer::kSceneLightingFormat};
    graphics.color_formats = hdr_colors;
    const auto clustered = shaders.create_pipeline(clustered_program.value, clustered_layout.value,
                                                   {rhi::PipelineType::kGraphics, graphics});
    const auto unshadowed = shaders.create_pipeline(unshadowed_program.value, unshadowed_layout.value,
                                                   {rhi::PipelineType::kGraphics, graphics});
    const renderer::RenderMaterialAttachmentContract clustered_contract{graphics.color_formats.front(), graphics.samples,
        graphics.depth_stencil_format};
    const renderer::RendererProfilePsoContract clustered_profile_contract{
        {renderer::kRendererProfileContractVersion, renderer::RendererProfile::kForwardClustered,
         shader::detail::ShaderPipelineLayoutAccess::contract_hash(clustered_layout.value)},
        clustered_contract};
    const renderer::RendererProfilePsoContract unshadowed_profile_contract{
        {renderer::kRendererProfileContractVersion, renderer::RendererProfile::kForwardClustered,
         shader::detail::ShaderPipelineLayoutAccess::contract_hash(unshadowed_layout.value)},
        clustered_contract};
    const std::array mrt{rhi::Format::kRgba16Float, rhi::Format::kRgba16Float, rhi::Format::kRgba8Unorm};
    graphics.color_formats = mrt;
    const auto gbuffer =
        shaders.create_pipeline(gbuffer_program.value, gbuffer_layout.value, {rhi::PipelineType::kGraphics, graphics});
    const renderer::RenderMaterialAttachmentContract gbuffer_attachment_contract{
        graphics.color_formats[0], graphics.samples, graphics.depth_stencil_format,
        {graphics.color_formats[1], graphics.color_formats[2]}};
    const auto gbuffer_cache_epoch = shader::detail::ShaderPipelineLayoutAccess::cache_epoch(gbuffer_layout.value);
    const renderer::RendererProfilePsoContract gbuffer_profile_contract{
        {renderer::kRendererProfileContractVersion, renderer::RendererProfile::kGBuffer,
         shader::detail::ShaderPipelineLayoutAccess::contract_hash(gbuffer_layout.value), gbuffer_cache_epoch},
        gbuffer_attachment_contract, gbuffer_cache_epoch};
    graphics.color_formats = {};
    graphics.vertex_attributes = std::span{attributes.data(), 1};
    const auto depth_pipeline =
        shaders.create_pipeline(depth_program.value, depth_layout.value, {rhi::PipelineType::kGraphics, graphics});
    const renderer::RenderMaterialAttachmentContract depth_attachment_contract{
        rhi::Format::kUnknown, graphics.samples, graphics.depth_stencil_format};
    const auto depth_cache_epoch = shader::detail::ShaderPipelineLayoutAccess::cache_epoch(depth_layout.value);
    const renderer::RendererProfilePsoContract depth_profile_contract{
        {renderer::kRendererProfileContractVersion, renderer::RendererProfile::kShadow,
         shader::detail::ShaderPipelineLayoutAccess::contract_hash(depth_layout.value), depth_cache_epoch},
        depth_attachment_contract, depth_cache_epoch};
    if (!opaque || !shadow_opaque || !gbuffer || !depth_pipeline || !clustered || !unshadowed)
        return 5;
    resolver.object_layout = shaders.binding_layout(opaque_layout.value, 0).value;
    const auto material_layout = shaders.binding_layout(opaque_layout.value, 1).value;
    const auto lighting_layout = shaders.binding_layout(opaque_layout.value, 2).value;
    application::runtime::RuntimeAssetResolver::UploadContext upload_context{
        .device = &device, .frame_context = &frame, .graph = &graph, .uploader = &uploader,
        .object_binding_layout = resolver.object_layout, .lighting_binding_layout = lighting_layout};
    resolver.objects.set_upload_context(upload_context);
    const std::array<std::array<float, 9>, 4> vertices{{{-1, -1, 0, 1, 0, 0, 0, 0, 1},
                                                        {1, -1, 0, 1, 1, 0, 0, 0, 1},
                                                        {1, 1, 0, 1, 1, 1, 0, 0, 1},
                                                        {-1, 1, 0, 1, 0, 1, 0, 0, 1}}};
    const std::array<uint16_t, 6> indices{0, 1, 2, 0, 2, 3};
    const auto vertex_desc =
        upload_desc(sizeof(vertices), rhi::BufferCapability::kVertex, rhi::ResourceUsage::kVertexInput);
    const auto index_desc =
        upload_desc(sizeof(indices), rhi::BufferCapability::kIndex, rhi::ResourceUsage::kIndexInput);
    const auto vertex = device.create_buffer(vertex_desc), index = device.create_buffer(index_desc);
    if (!vertex || !index || !check(device.write_buffer(vertex.value, 0, std::as_bytes(std::span{vertices}))) ||
        !check(device.write_buffer(index.value, 0, std::as_bytes(std::span{indices}))))
        return 6;
    resolver.mesh = {.vertex_buffer = vertex.value,
                     .vertex_buffer_desc = vertex_desc,
                     .index_buffer = index.value,
                     .index_buffer_desc = index_desc,
                     .vertex_stride = 36,
                     .vertex_count = 4,
                     .index_count = 6};
    const auto sampler = device.create_sampler({});
    if (!sampler)
        return 7;
    pipeline::RenderPassRegistry passes;
    NativePassObservations native_observation{};
    renderer::RenderSceneSnapshot scene{};
    scene.world = {0, 1};
    scene.world_revision = 1;
    scene.view.world = scene.world;
    scene.view.id = renderer::RenderViewId("ShadowTest.Camera");
    scene.view.output_extent = {kSize, kSize};
    scene.view.color_target = kColor;
    scene.view.depth_target = kDepth;
    scene.view.camera.view = math::look_at({0, 0, 3}, {0, 0, 0}, {0, 1, 0});
    scene.view.camera.projection = math::orthographic(-1, 1, -1, 1, 0.1f, 5);
    scene.instances = {{.object = renderer::RenderObjectId(1),
                        .mesh = renderer::MeshAssetId(1),
                        .material = renderer::MaterialInstanceId(1),
                        .world_transform = math::scale({0.9f, 0.9f, 1}),
                        .bounds_radius = 1.42f},
                       {.object = renderer::RenderObjectId(2),
                        .mesh = renderer::MeshAssetId(1),
                        .material = renderer::MaterialInstanceId(1),
                        .world_transform = math::translation({-0.45f, 0, 0.6f}) * math::scale({0.15f, 0.15f, 1}),
                        .bounds_radius = 0.22f}};
    scene.lights = {{.light = renderer::RenderLightId(1),
                     .type = renderer::RenderLightType::kDirectional,
                     .direction = {0.5f, 0, -1},
                     .intensity = 0.8f}};
    if (local_lights) {
        scene.instances.resize(1);
        scene.lights[0].intensity = 0;
        scene.lights.push_back(
            {.light = renderer::RenderLightId(2), .position = {-0.15f, 0, 0.4f}, .color = {1, 0, 0}, .range = 0.6f});
        scene.lights.push_back({.light = renderer::RenderLightId(3),
                                .type = renderer::RenderLightType::kSpot,
                                .position = {0.4f, 0, 0.5f},
                                .direction = {0, 0, -1},
                                .color = {0, 0, 1},
                                .range = 1,
                                .inner_cone_cosine = 0.99f,
                                .outer_cone_cosine = 0.95f});
    }
    const auto base_scene = scene;
    size_t iteration = 0;
    bool lighting_execution_failure_tested = false;
    feature::FeatureRegistry features{aeg::feature::test::clock_test_host()};
    concurrency::CompletionSource wave_consumer_started, wave_consumer_release;
    feature::FeatureRuntime feature_runtime(features);
    struct ReleaseConsumer final {
        concurrency::CompletionSource& gate;
        ~ReleaseConsumer() { static_cast<void>(gate.complete()); }
    } release_consumer{wave_consumer_release};
    auto producer = render_feature_test::definition();
    producer.id = feature::FeatureDefinitionId(gbuffer_path ? "GBuffer.Producer" : "Shadow.Producer");
    producer.contract.definition_id = producer.id;
    producer.contract.parameters.clear();
    producer.contract.ports = {{feature::PortId("Out"), feature::PortDirection::kOutput,
        feature::ValueSchemaId(lighting_mode ? "Lighting.SceneReferences" : gbuffer_mode ? "GBuffer.SceneMrtReferences" : "Shadow.SceneTextureReference"), 1}};
    producer.execute = [gbuffer_path, lighting_mode](const feature::InvocationToken& token, const feature::StateSnapshot&,
        std::span<const uint8_t>, std::span<const uint8_t>) {
        using namespace feature;
        if (gbuffer_path) {
            auto draw = gbuffer_draw({RenderResourceId(lighting_mode ? "Lighting.Geometry.Scene" : "GBuffer.Producer.Scene"), 1}, RenderAttachmentLoad::kClear);
            if (lighting_mode) draw.depth.clear_depth = 1.f;
            StateDelta delta{token.state_version};
            delta.render_intent = RenderIntent{};
            delta.render_intent->operations = {draw};
            const std::array<uint64_t, 10> references{RenderResourceId("Shadow.Scene").value(), 1,
                draw.albedo.target.id.value(), draw.albedo.target.version, draw.normal.target.id.value(), draw.normal.target.version,
                draw.material.target.id.value(), draw.material.target.version, draw.depth.target.id.value(), draw.depth.target.version};
            delta.output_payload.resize(sizeof(references));
            std::memcpy(delta.output_payload.data(), references.data(), sizeof(references));
            if (lighting_mode) {
                RenderDrawDepthScene shadow{draw.scene, {{RenderResourceId(kShadow.value()), 1, true}}};
                shadow.projection = RenderDepthSceneProjection::kDirectionalShadow;
                delta.render_intent->operations.push_back(shadow);
                const std::array<uint64_t, 2> second_scene{RenderResourceId("Lighting.Second.Scene").value(), 1};
                delta.output_payload.resize(sizeof(references) + sizeof(second_scene));
                std::memcpy(delta.output_payload.data() + sizeof(references), second_scene.data(), sizeof(second_scene));
            }
            return foundation::ResultValue<StateDelta>{foundation::Result::success(), std::move(delta)};
        }
        const RenderTextureReference map{RenderResourceId(kShadow.value()), 1, true};
        RenderDrawDepthScene draw{{RenderResourceId("Shadow.Scene"), 1}, {map}};
        draw.projection = RenderDepthSceneProjection::kDirectionalShadow;
        StateDelta delta{token.state_version};
        delta.render_intent = RenderIntent{};
        delta.render_intent->operations = {draw}; // Consumer reads, rather than a producer export, anchor the map.
        const std::array<uint64_t, 4> references{draw.scene.id.value(), draw.scene.version, map.id.value(), map.version};
        delta.output_payload.resize(sizeof(references));
        std::memcpy(delta.output_payload.data(), references.data(), sizeof(references));
        return foundation::ResultValue<StateDelta>{foundation::Result::success(), std::move(delta)};
    };
    auto consumer = render_feature_test::definition();
    consumer.id = feature::FeatureDefinitionId(lighting_mode ? "Lighting.Consumer" : gbuffer_mode ? "GBuffer.Load" : "Shadow.Consumer");
    consumer.contract.definition_id = consumer.id;
    consumer.contract.ports = {{feature::PortId("In"), feature::PortDirection::kInput,
        feature::ValueSchemaId(lighting_mode ? "Lighting.SceneReferences" : gbuffer_mode ? "GBuffer.SceneMrtReferences" : "Shadow.SceneTextureReference"), 1}};
    if (post_feature_mode) consumer.contract.ports.push_back({feature::PortId("Out"), feature::PortDirection::kOutput,
        feature::ValueSchemaId("Post.SceneReferences"), 1});
    consumer.execute = [schema = consumer.contract.parameters, &iteration, gbuffer_mode, lighting_mode, post_feature_mode](const feature::InvocationToken& token,
        const feature::StateSnapshot&, std::span<const uint8_t> input, std::span<const uint8_t> parameters) {
        using namespace feature;
        if (lighting_mode) {
            std::array<uint64_t, 12> references{};
            if (input.size() != sizeof(references))
                return foundation::ResultValue<StateDelta>{foundation::invalid_argument("lighting producer input absent"), {}};
            std::memcpy(references.data(), input.data(), sizeof(references));
            const auto decoded = decode_feature_parameters(parameters, schema);
            if (!decoded) return foundation::ResultValue<StateDelta>{decoded.result, {}};
            const bool second = decoded.value.front().payload.front() == 2;
            RenderDeferredLightingScene draw{};
            draw.scene = {RenderResourceId(references[second ? 10 : 0]), references[second ? 11 : 1]};
            draw.albedo = {RenderResourceId(references[2]), references[3], true};
            draw.normal = {RenderResourceId(references[4]), references[5], true};
            draw.depth = {RenderResourceId(references[8]), references[9], true};
            draw.hdr.target = {RenderResourceId((second ? kHdr2 : kHdr).value()), 1, true};
            if (iteration != 0) draw.directional_shadow = RenderTextureReference{RenderResourceId(kShadow.value()), 1, true};
            StateDelta delta{token.state_version};
            delta.render_intent = RenderIntent{};
            delta.render_intent->operations = {draw};
            if (!second && iteration == 0) {
                draw.hdr.load = RenderAttachmentLoad::kLoad;
                delta.render_intent->operations.push_back(draw);
            }
            if (post_feature_mode) {
                const std::array<uint64_t, 6> post_refs{draw.scene.id.value(), draw.scene.version,
                    draw.hdr.target.id.value(), draw.hdr.target.version, draw.depth.id.value(), draw.depth.version};
                delta.output_payload.resize(sizeof(post_refs));
                std::memcpy(delta.output_payload.data(), post_refs.data(), sizeof(post_refs));
            }
            return foundation::ResultValue<StateDelta>{foundation::Result::success(), std::move(delta)};
        }
        if (gbuffer_mode) {
            std::array<uint64_t, 10> references{};
            if (input.size() != sizeof(references))
                return foundation::ResultValue<StateDelta>{foundation::invalid_argument("GBuffer producer input absent"), {}};
            std::memcpy(references.data(), input.data(), sizeof(references));
            const auto decoded = decode_feature_parameters(parameters, schema);
            if (!decoded) return foundation::ResultValue<StateDelta>{decoded.result, {}};
            auto draw = gbuffer_draw({RenderResourceId(references[0]), references[1]}, RenderAttachmentLoad::kLoad);
            draw.albedo.target = {RenderResourceId(references[2]), references[3], true};
            draw.normal.target = {RenderResourceId(references[4]), references[5], true};
            draw.material.target = {RenderResourceId(references[6]), references[7], true};
            draw.depth.target = {RenderResourceId(references[8]), references[9], true};
            if (decoded.value.front().payload.front() == 1 && iteration >= 4) {
                if (iteration < 7) {
                    const std::array attachments{&draw.albedo, &draw.normal, &draw.material};
                    attachments[iteration - 4]->load = RenderAttachmentLoad::kClear;
                } else draw.depth.load = RenderAttachmentLoad::kClear;
            }
            StateDelta delta{token.state_version};
            delta.render_intent = RenderIntent{};
            delta.render_intent->operations = {draw};
            if (decoded.value.front().payload.front() == 1 && iteration == 0)
                delta.render_intent->operations.push_back(draw);
            return foundation::ResultValue<StateDelta>{foundation::Result::success(), std::move(delta)};
        }
        std::array<uint64_t, 4> references{};
        if (input.size() != sizeof(references))
            return foundation::ResultValue<StateDelta>{foundation::invalid_argument("shadow producer input absent"), {}};
        std::memcpy(references.data(), input.data(), sizeof(references));
        const auto decoded = decode_feature_parameters(parameters, schema);
        if (!decoded) return foundation::ResultValue<StateDelta>{decoded.result, {}};
        const bool second = decoded.value.front().payload.front() == 2;
        RenderDrawScene draw{{RenderResourceId(references[0]), references[1]},
            {RenderResourceId((second ? kColor2 : kColor).value()), 1, true}};
        draw.depth = RenderDepthAttachment{{RenderResourceId((second ? kDepth2 : kDepth).value()), 1, true}};
        if (iteration != 0) draw.directional_shadow = RenderTextureReference{RenderResourceId(references[2]), references[3], true};
        if (iteration == 4) draw.clear_color = {.2f, .3f, .4f, 1.f};
        StateDelta delta{token.state_version};
        delta.render_intent = RenderIntent{};
        delta.render_intent->operations = {draw};
        if (!second && iteration == 1) delta.render_intent->operations.push_back(draw);
        delta.render_intent->final_output = draw.target;
        return foundation::ResultValue<StateDelta>{foundation::Result::success(), std::move(delta)};
    };
    if (clustered_feature_mode) {
        producer.id = feature::FeatureDefinitionId("Clustered.CullProducer");
        producer.contract.definition_id = producer.id;
        producer.contract.ports = {{feature::PortId("Out"), feature::PortDirection::kOutput, feature::kRenderLightListSchema, 1}};
        if (clustered_process_mode) {
            producer.contract.execution = feature::ExecutionKind::kProcess;
            producer.contract.state = feature::StatePolicy::kProcessState;
            producer.contract.has_continuation = true;
        }
        producer.execute = [clustered_wave_mode, clustered_process_mode, clustered_shadow_mode](const feature::InvocationToken& token, const feature::StateSnapshot&,
            std::span<const uint8_t>, std::span<const uint8_t>) {
            using namespace feature;
            const RenderSceneReference scene{RenderResourceId("Shadow.Scene"), 1};
            const RenderTextureReference depth{RenderResourceId(kDepth.value()), 1, true};
            RenderDrawDepthScene draw{scene, {depth}};
            draw.depth.clear_depth = 1.f;
            RenderLightCullingScene cull{scene, depth, {RenderResourceId("Clustered.Shared.List"),
                clustered_wave_mode || clustered_process_mode ? token.state_version.value() : 1, PortId("Out")}};
            StateDelta delta{token.state_version};
            delta.render_intent = RenderIntent{};
            delta.render_intent->operations = {draw};
            if (clustered_shadow_mode) {
                auto shadow = draw;
                shadow.projection = RenderDepthSceneProjection::kDirectionalShadow;
                shadow.depth.target = {RenderResourceId(kShadow.value()), 1, true};
                cull.directional_shadow = shadow.depth.target;
                delta.render_intent->operations.push_back(shadow);
            }
            delta.render_intent->operations.push_back(cull);
            const auto encoded = encode_render_light_list_reference({cull.output.id, cull.output.version, token.invocation_id, token.generation});
            if (!encoded) return foundation::ResultValue<StateDelta>{encoded.result, {}};
            delta.output_payload = encoded.value;
            return foundation::ResultValue<StateDelta>{foundation::Result::success(), std::move(delta)};
        };
        consumer.id = feature::FeatureDefinitionId("Clustered.ForwardConsumer");
        consumer.contract.definition_id = consumer.id;
        consumer.contract.ports = {{feature::PortId("In"), feature::PortDirection::kInput, feature::kRenderLightListSchema, 1}};
        consumer.execute = [schema = consumer.contract.parameters, clustered_wave_mode, clustered_process_mode, clustered_shadow_mode, clustered_shadow_timing_mode, &iteration, &wave_consumer_started,
            &wave_consumer_release](const feature::InvocationToken& token,
            const feature::StateSnapshot&, std::span<const uint8_t> input, std::span<const uint8_t> parameters) {
            using namespace feature;
            // A stopped producer does not erase a consumer's independently committed intent/receipt.
            if (clustered_process_mode && input.empty())
                return foundation::ResultValue<StateDelta>{foundation::Result::success(), StateDelta{token.state_version}};
            const auto list = decode_render_light_list_reference(input);
            const auto decoded = decode_feature_parameters(parameters, schema);
            if (!list || !decoded) return foundation::ResultValue<StateDelta>{!list ? list.result : decoded.result, {}};
            const bool second = decoded.value.front().payload.front() == 2;
            if (clustered_wave_mode && second && list.value.version == 2) {
                static_cast<void>(wave_consumer_started.complete());
                static_cast<void>(wave_consumer_release.completion().wait());
            }
            RenderDrawClusteredScene draw{};
            draw.scene = {RenderResourceId("Shadow.Scene"), 1};
            draw.light_list = list.value;
            draw.hdr.target = {RenderResourceId((second ? kColor2 : kColor).value()), 1, true};
            draw.hdr.clear_color = clustered_wave_mode && list.value.version == 2 ?
                std::array{.4f, .2f, .6f, 1.f} : std::array{.2f, .3f, .4f, 1.f};
            draw.depth.target = {RenderResourceId(kDepth.value()), 1, true};
            draw.depth.load = RenderAttachmentLoad::kLoad;
            if (clustered_shadow_mode) draw.directional_shadow = RenderTextureReference{RenderResourceId(kShadow.value()), 1, true};
            StateDelta delta{token.state_version};
            delta.render_intent = RenderIntent{};
            delta.render_intent->operations = {draw};
            if (clustered_shadow_mode && !clustered_shadow_timing_mode && second && (iteration == 1 || iteration == 2)) {
                RenderDrawDepthScene preserving{};
                preserving.scene = draw.scene;
                preserving.projection = iteration == 1 ? RenderDepthSceneProjection::kDirectionalShadow : RenderDepthSceneProjection::kCamera;
                preserving.depth.target = iteration == 1 ? *draw.directional_shadow : draw.depth.target;
                preserving.depth.load = RenderAttachmentLoad::kLoad;
                preserving.depth.clear_depth = .25f; // Load ignores this value and preserves the actual producer contents.
                delta.render_intent->operations.push_back(preserving);
            }
            return foundation::ResultValue<StateDelta>{foundation::Result::success(), std::move(delta)};
        };
    }
    auto document = render_feature_test::document(consumer);
    auto post_consumer = consumer;
    if (post_feature_mode) {
        post_consumer.id = feature::FeatureDefinitionId("PostProcess.Consumer");
        post_consumer.contract.definition_id = post_consumer.id;
        post_consumer.contract.ports = {{feature::PortId("In"), feature::PortDirection::kInput,
            feature::ValueSchemaId("Post.SceneReferences"), 1}};
        post_consumer.execute = [schema = post_consumer.contract.parameters, &iteration](const feature::InvocationToken& token,
            const feature::StateSnapshot&, std::span<const uint8_t> input, std::span<const uint8_t> parameters) {
            using namespace feature;
            std::array<uint64_t, 6> refs{};
            if (input.size() != sizeof(refs))
                return foundation::ResultValue<StateDelta>{foundation::invalid_argument("PostProcess producer input absent"), {}};
            std::memcpy(refs.data(), input.data(), sizeof(refs));
            const auto decoded = decode_feature_parameters(parameters, schema);
            if (!decoded) return foundation::ResultValue<StateDelta>{decoded.result, {}};
            const bool second = decoded.value.front().payload.front() == 2;
            RenderPostProcessScene draw{};
            draw.scene = {RenderResourceId(refs[0]), refs[1]};
            draw.hdr = {RenderResourceId(refs[2]), refs[3], true};
            draw.depth = {RenderResourceId(refs[4]), refs[5], true};
            draw.output.target = {RenderResourceId((second ? kPost2 : kPost).value()), 1, true};
            draw.settings = RenderPostProcessSettings{};
            draw.settings->sky_zenith = draw.settings->sky_horizon = draw.settings->sky_ground = {.25f, .5f, 1.f};
            draw.settings->sky_enabled = iteration != 2;
            draw.settings->tone_mapping = true;
            draw.settings->exposure = second ? .5f : 1.f;
            draw.settings->gamma = iteration == 2 ? 2.f : 1.f;
            if (!second && iteration == 3) draw.settings.reset(); // Immutable View defaults.
            StateDelta delta{token.state_version};
            delta.render_intent = RenderIntent{};
            delta.render_intent->operations = {draw};
            if (!second) {
                draw.output.load = RenderAttachmentLoad::kLoad;
                delta.render_intent->operations.push_back(draw);
            }
            return foundation::ResultValue<StateDelta>{foundation::Result::success(), std::move(delta)};
        };
        const auto authored_post = render_feature_test::document(post_consumer);
        for (size_t index = 0; index < authored_post.nodes.size(); ++index) {
            auto node = authored_post.nodes[index];
            node.node_id = feature::FeatureNodeId(index + 3);
            document.nodes.push_back(std::move(node));
            document.edges.push_back({feature::FeatureNodeId(index + 1), feature::PortId("Out"),
                feature::FeatureNodeId(index + 3), feature::PortId("In"), feature::EdgeKind::kData,
                consumer.contract.clock.id, post_consumer.contract.clock.id, 1, 1, 0});
        }
    }
    if (clustered_pair_mode) {
        document.nodes.insert(document.nodes.begin(), {{feature::FeatureNodeId("Shadow.A"), producer.id, producer.version},
            {feature::FeatureNodeId("Shadow.B"), producer.id, producer.version}});
    } else document.nodes.insert(document.nodes.begin(), {feature::FeatureNodeId("Shadow"), producer.id, producer.version});
    for (uint64_t channel : {1ull, 2ull}) document.edges.push_back({clustered_pair_mode ?
        feature::FeatureNodeId(channel == 1 ? "Shadow.A" : "Shadow.B") : feature::FeatureNodeId("Shadow"), feature::PortId("Out"),
        feature::FeatureNodeId(channel), feature::PortId("In"), feature::EdgeKind::kData,
        producer.contract.clock.id, consumer.contract.clock.id, 1, 1, 0});
    feature::CompiledFeatureGraph feature_plan{};
    if (feature_mode) {
        if (!check(aeg::feature::test::register_clock_test_definition(features, producer)) || !check(aeg::feature::test::register_clock_test_definition(features, consumer))) return 34;
        if (post_feature_mode && !check(aeg::feature::test::register_clock_test_definition(features, post_consumer))) return 34;
        auto compiled = feature::FeatureGraphCompiler{}.compile(document, features);
        if (!compiled || !check(feature_runtime.publish_graph(document, compiled.value))) return 34;
        feature_plan = compiled.value;
    } else {
        if (!check(renderer::register_scene_render_profiles(features))) return 74;
        const auto scene_document = renderer::scene_render_profile_graph(deferred_mode
            ? renderer::SceneRenderProfile::kDeferred : renderer::SceneRenderProfile::kForward);
        if (!scene_document) return 74;
        auto compiled = feature::FeatureGraphCompiler{}.compile(scene_document.value, features);
        if (!compiled || !check(feature_runtime.publish_graph(scene_document.value, compiled.value))) return 74;
        feature_plan = compiled.value;
    }
    std::array<feature::FeatureInstanceId, 4> paired_instances{}; // PA, PB, CA, CB.
    if (clustered_pair_mode) {
        const std::array nodes{feature::FeatureNodeId("Shadow.A"), feature::FeatureNodeId("Shadow.B"),
            feature::FeatureNodeId(1), feature::FeatureNodeId(2)};
        for (size_t index = 0; index < nodes.size(); ++index) {
            const auto found = std::find_if(feature_plan.provenance.begin(), feature_plan.provenance.end(),
                [&](const auto& item) { return item.node_id == nodes[index]; });
            if (found == feature_plan.provenance.end()) return 68;
            paired_instances[index] = found->instance_id;
        }
    }
    const auto pump_wave_until = [&](const auto& condition) {
        for (unsigned spin = 0; spin != 2000 && !condition(); ++spin) {
            if (!check(feature_runtime.pump_completions())) return false;
            static_cast<void>(concurrency::sleep_for(1'000'000));
        }
        return condition();
    };
    if (clustered_wave_mode) {
        if (!check(feature_runtime.tick(producer.contract.clock.id)) || !check(feature_runtime.enable_async_execution()) ||
            !check(feature_runtime.tick(producer.contract.clock.id))) return 65;
        if (!pump_wave_until([&] { return wave_consumer_started.completion().is_complete() &&
            feature_runtime.find_instance(feature_plan.provenance[0].instance_id)->state.commit_count == 2 &&
            feature_runtime.find_instance(feature_plan.provenance[1].instance_id)->state.commit_count == 2; })) return 65;
        if (feature_runtime.find_instance(feature_plan.provenance[2].instance_id)->state.commit_count != 1 ||
            feature_runtime.render_publication_status(producer.contract.clock.id).code != foundation::ResultCode::kNotReady) return 65;
    }
    std::array<int, 6> dark_pixels{}, lit_pixels{}, blue_pixels{}, background_pixels{};
    foundation::Result legacy_scene_provenance = foundation::Result::success();
    size_t legacy_scene_geometry = 0;
    size_t contract_probe_count = 0;
    bool contract_probes_valid = true;
    bool lighting_attachment_probes_observed = false, post_attachment_probe_observed = false;
    const auto observe_contract_rejection = [&](std::string_view name, const foundation::Result& rejected,
        const ObservedSceneLowerer& observed, bool preconditions, bool lease_released, bool rolled_back,
        std::string_view expected_error) {
        const bool native_rejected = observed.contract_probe_calls == 1 && observed.contract_probe_successes == 0;
        const bool passed = preconditions && native_rejected && !rejected &&
            (rejected.code == foundation::ResultCode::kInvalidArgument || rejected.code == foundation::ResultCode::kInvalidState) &&
            rejected.message.find(expected_error) != std::string::npos && lease_released && rolled_back;
        ++contract_probe_count;
        contract_probes_valid = contract_probes_valid && passed;
        std::cout << "Render contract probe=" << name << " preconditions=" << preconditions
            << " native-calls=" << observed.contract_probe_calls << " native-accepted=" << observed.contract_probe_successes
            << " consumers=" << observed.consumer_calls << " replacements=" << observed.material_replacements
            << " lease-released=" << lease_released << " rollback=" << rolled_back << " passed=" << passed
            << " result=" << rejected.message << '\n';
    };
    std::array<std::vector<std::byte>, 4> gbuffer_reference;
    std::optional<renderer::RenderLightCullingResource> retired_culling;
    std::optional<render_graph::RenderFeatureBuildScope> previous_shadow_scope;
    std::optional<feature::InvocationToken> previous_shadow_invocation;
    for (; iteration != (clustered_pair_mode ? 2u : clustered_cancel_mode ? 1u : clustered_shadow_wave_mode ? 3u : clustered_shadow_process_mode ? 2u : clustered_wave_mode ? 7u : gbuffer_mode ? 8u : feature_mode || clustered_hdr ? 6u : 3u);) {
        if (!check(frame.begin_frame()))
            return 10;
        if (feature_mode || scene_profile_mode) {
            native_observation.expected = nullptr;
            native_observation.records.clear();
            native_observation.frame = frame.frame_number();
            if (!check(register_observed_native_passes(passes, native_observation))) return 70;
        }
        if (feature_mode || clustered_hdr) scene = base_scene;
        if (gbuffer_mode) {
            std::reverse(scene.instances.begin(), scene.instances.end());
            if (iteration == 1 || iteration == 2 || iteration >= 4) scene.instances.clear();
        }
        scene.frame_number = frame.frame_number();
        if (post_feature_mode) {
            scene.view.post_process.exposure = .75f;
            scene.view.post_process.tone_mapping = true;
            resolver.runtime_post = true;
            resolver.post_resources.clear();
            resolver.post_input_sets.clear();
        }
        scene.lights.front().casts_shadow = iteration != 0;
        if (shadow_feature_mode && iteration == 3) scene.lights.front().casts_shadow = false;
        if (shadow_feature_mode && iteration == 4) { scene.instances.clear(); scene.shadow_casters.clear(); }
        if (shadow_feature_mode && iteration == 5) scene.lights.clear();
        const size_t clustered_case = clustered_shadow_timing_mode ? 1 : clustered_wave_mode && iteration == final_wave_iteration ? 1 : iteration;
        if (clustered_shadow_mode) {
            scene.lights.front().casts_shadow = clustered_shadow_timing_mode || iteration == 1 || iteration == 2;
            if (!clustered_shadow_timing_mode && iteration == 2) {
                scene.lights.front().direction = {2, 0, -1};
                scene.shadow_casters = scene.instances;
                scene.shadow_casters[1].world_transform =
                    math::translation({-1.35f, 0, .6f}) * math::scale({.15f, .15f, 1});
                scene.instances.resize(1);
            }
            if (iteration == 4) { scene.instances.clear(); scene.shadow_casters.clear(); }
            if (iteration == 5) scene.lights.clear();
        } else if (clustered_hdr) {
            scene.lights.front().casts_shadow = false;
            scene.lights.front().direction = {0, 0, -1};
            scene.lights.front().intensity = !clustered_pair_mode && clustered_case == 0 ? .8f : 0.f;
            scene.lights[1].intensity = clustered_pair_mode || clustered_case == 1 || clustered_case == 3 ? 1.f : 0.f;
            scene.lights[2].intensity = !clustered_pair_mode && (clustered_case == 2 || clustered_case == 3) ? 1.f : 0.f;
            if (clustered_case == 3) { scene.lights[1].position.x = 100; scene.lights[2].direction = {0, 0, 1}; }
            if (clustered_case == 4) scene.lights.clear();
            if (clustered_case == 5) { scene.instances.clear(); scene.shadow_casters.clear(); }
        } else if (post_process) {
            scene.instances.resize(1);
            scene.lights.front().casts_shadow = false;
            scene.lights.front().intensity = 4;
            scene.lights.front().direction = {0, 0, -1};  // Analytic HDR input: 4 direct + 0.1 ambient.
            resolver.post_settings.sky_enabled = true;
            resolver.post_settings.tone_mapping = true;
            resolver.post_settings.sky_zenith = resolver.post_settings.sky_horizon =
                resolver.post_settings.sky_ground = {0.25f, 0.5f, 1};
            resolver.post_settings.exposure = iteration == 1 ? 0.5f : 1;
            resolver.post_settings.gamma = iteration == 2 ? 2 : 1;
        } else if (local_lights) {
            scene.lights[1].intensity = scene.lights[2].intensity = iteration == 0 ? 0.0f : 1.0f;
            if (iteration == 2) {
                scene.lights[1].position.x = 100;
                scene.lights[2].direction = {0, 0, 1};
            }
        } else if (!gbuffer_path && iteration == 2) {
            // The caster is outside the camera, but its shadow still crosses the visible receiver.
            scene.lights.front().direction = {2, 0, -1};
            scene.shadow_casters = scene.instances;
            scene.shadow_casters[1].world_transform =
                math::translation({-1.35f, 0, 0.6f}) * math::scale({0.15f, 0.15f, 1});
            scene.instances.resize(1);
        }
        if (lighting_mode) {
            scene.lights.front().direction = {0, 0, -1};
            scene.lights.front().intensity = 4;
            scene.lights.front().casts_shadow = iteration == 1;
            if (iteration == 1) {
                scene.shadow_casters = scene.instances;
                scene.instances.resize(1);
            }
            if (iteration == 3) scene.lights.clear();
            if (iteration == 4) { scene.instances.clear(); scene.shadow_casters.clear(); }
            if (iteration == 5) scene.lights.front().direction = {0, 0, 1};
            resolver.deferred_input_sets.clear();
        }
        rhi::TextureDesc color_desc{};
        color_desc.extent = {kSize, kSize, 1};
        color_desc.format = gbuffer_path || clustered_hdr ? rhi::Format::kRgba16Float : rhi::Format::kRgba8Unorm;
        color_desc.capabilities = rhi::TextureCapability::kColorAttachment | rhi::TextureCapability::kTransferSource;
        if (lighting_mode) color_desc.capabilities |= rhi::TextureCapability::kSampled;
        const auto color = graph.create_texture(color_desc);
        auto depth_desc = color_desc;
        depth_desc.format = rhi::Format::kD32Float;
        depth_desc.capabilities = rhi::TextureCapability::kDepthStencil | rhi::TextureCapability::kSampled;
        if (gbuffer_mode) depth_desc.capabilities |= rhi::TextureCapability::kTransferSource;
        const auto depth = graph.create_texture(depth_desc);
        const auto shadow = shadow_feature_mode || lighting_mode || clustered_shadow_mode ? graph.create_texture(depth_desc) : foundation::ResultValue<render_graph::RenderGraphTexture>{};
        const auto second_color = shadow_feature_mode || clustered_hdr ? graph.create_texture(color_desc) : foundation::ResultValue<render_graph::RenderGraphTexture>{};
        const auto second_depth = shadow_feature_mode || clustered_pair_mode ? graph.create_texture(depth_desc) : foundation::ResultValue<render_graph::RenderGraphTexture>{};
        const auto normal = gbuffer_path ? graph.create_texture(color_desc) : foundation::ResultValue<render_graph::RenderGraphTexture>{};
        auto material_desc = color_desc;
        material_desc.format = rhi::Format::kRgba8Unorm;
        const auto material = gbuffer_path ? graph.create_texture(material_desc) : foundation::ResultValue<render_graph::RenderGraphTexture>{};
        const auto hdr = lighting_mode ? graph.create_texture(color_desc) : foundation::ResultValue<render_graph::RenderGraphTexture>{};
        const auto second_hdr = lighting_mode ? graph.create_texture(color_desc) : foundation::ResultValue<render_graph::RenderGraphTexture>{};
        auto output_desc = color_desc;
        output_desc.format = rhi::Format::kRgba8Unorm;
        const auto post_output = post_feature_mode ? graph.create_texture(output_desc) : foundation::ResultValue<render_graph::RenderGraphTexture>{};
        const auto second_post_output = post_feature_mode ? graph.create_texture(output_desc) : foundation::ResultValue<render_graph::RenderGraphTexture>{};
        if (post_feature_mode && (!post_output || !second_post_output)) return 54;
        auto white_desc = color_desc;
        white_desc.extent = {1, 1, 1};
        white_desc.capabilities = rhi::TextureCapability::kColorAttachment | rhi::TextureCapability::kSampled;
        const auto white = graph.create_texture(white_desc);
        if (!color || !depth || !white)
            return 11;
        if ((shadow_feature_mode && (!shadow || !second_color || !second_depth)) || (gbuffer_mode && (!normal || !material))) return 35;
        if (lighting_mode && (!shadow || !normal || !material || !hdr || !second_hdr)) return 35;
        // Graph views are allocated at compile; material descriptors are prepared by a preceding pass.
        const auto tint_desc =
            upload_desc(sizeof(math::Vec4), rhi::BufferCapability::kUniform, rhi::ResourceUsage::kUniformRead);
        const auto tint = device.create_buffer(tint_desc);
        const auto lighting_constants = renderer::make_render_lighting_constants(scene.lights, {0.1f, 0.1f, 0.1f});
        const auto light_desc = upload_desc(sizeof(renderer::RenderLightingConstants), rhi::BufferCapability::kUniform,
                                            rhi::ResourceUsage::kUniformRead);
        const auto light = lighting_mode ? foundation::ResultValue<rhi::BufferHandle>{} : device.create_buffer(light_desc);
        const auto material_set = device.create_binding_set({material_layout, rhi::BindingSetLifetime::kPersistent});
        const auto light_set = lighting_mode ? foundation::ResultValue<rhi::BindingSetHandle>{}
            : device.create_binding_set({lighting_layout, rhi::BindingSetLifetime::kPersistent});
        if (!tint || !material_set || (!lighting_mode && (!light || !light_set || !lighting_constants)))
            return 12;
        const math::Vec4 tint_value = gbuffer_path ? math::Vec4{.25f, .5f, .75f, .5f} : math::Vec4{1, 1, 1, 1};
        if (!check(device.write_buffer(tint.value, 0, std::as_bytes(std::span{&tint_value, 1}))) ||
            (!lighting_mode && !check(device.write_buffer(light.value, 0, std::as_bytes(std::span{&lighting_constants.value, 1})))))
            return 13;
        const auto light_graph = lighting_mode ? foundation::ResultValue<render_graph::RenderGraphBuffer>{}
            : graph.import_buffer(light.value, light_desc);
        const auto tint_graph = graph.import_buffer(tint.value, tint_desc);
        if ((!lighting_mode && !light_graph) || !tint_graph)
            return 14;
        rhi::BindingWrite light_write{};
        light_write.location = {2, 0};
        light_write.resource.type = rhi::BindingType::kUniformBuffer;
        light_write.resource.buffer_range = {light.value, 0, sizeof(renderer::RenderLightingConstants)};
        if (!lighting_mode && !check(device.update_binding_set(light_set.value, std::span{&light_write, 1})))
            return 15;
        resolver.lighting = {light_set.value,
                             2,
                             light.value,
                             light_desc,
                             {light_graph.value.index, light_graph.value.generation, light_graph.value.graph_id},
                             0,
                             sizeof(renderer::RenderLightingConstants)};
        resolver.material = {.pipeline = opaque.value,
                             .binding_set = material_set.value,
                             .material_texture = {white.value.index, white.value.generation, white.value.graph_id},
                             .material_parameter_buffer = tint.value,
                             .material_parameter_buffer_desc = tint_desc,
                             .material_parameter_resource = {tint_graph.value.index, tint_graph.value.generation,
                                                             tint_graph.value.graph_id},
                             .material_parameter_size = sizeof(math::Vec4),
                             .gbuffer_pipeline = gbuffer.value,
                             .depth_pipeline = depth_pipeline.value,
                             .shadow_pipeline = shadow_opaque.value,
                             .clustered_pipeline = clustered.value};
        resolver.material.attachment_contract = {rhi::Format::kRgba8Unorm, rhi::SampleCount::kX1, rhi::Format::kD32Float};
        resolver.material.shader_contract = material_shader_contract;
        resolver.material.depth_pipeline_contract = depth_attachment_contract;
        resolver.material.depth_profile_contract = depth_profile_contract;
        resolver.material.variant_cache_epoch = depth_cache_epoch;
        resolver.material.shadow_pipeline_contract = resolver.material.attachment_contract;
        resolver.material.shadow_shader_contract = shadow_shader_contract;
        resolver.material.shadow_pipeline_cache_epoch = depth_cache_epoch;
        resolver.material.gbuffer_pipeline_contract = gbuffer_attachment_contract;
        resolver.material.gbuffer_profile_contract = gbuffer_profile_contract;
        resolver.material.clustered_pipeline_contract = clustered_contract;
        resolver.material.clustered_profile_contract = clustered_profile_contract;
        resolver.material.unshadowed_clustered_variant = renderer::RenderMaterialPipelineVariant{
            unshadowed.value, clustered_contract, {}, unshadowed_profile_contract};
        const auto clear = graph.add_pass(
            "White material", rhi::QueueClass::kGraphics,
            [&](render_graph::PassBuilder& pass) {
                return pass.use_color_attachment(white.value, rhi::LoadOp::kClear, rhi::StoreOp::kStore, {1, 1, 1, 1},
                                                 rhi::ResourceUsage::kSampledRead);
            },
            [&](render_graph::RenderPassCommandContext& command) {
                if (auto r = command.begin_rendering(); !r)
                    return r;
                return command.end_rendering();
            });
        if (!clear)
            return 16;
        const auto binding = graph.add_pass(
            "Material inputs", rhi::QueueClass::kGraphics,
            [&](render_graph::PassBuilder& pass) {
                pass.set_side_effects();
                return pass.read(white.value, rhi::ResourceUsage::kSampledRead, rhi::PipelineStage::kFragmentShader);
            },
            [&](render_graph::RenderPassCommandContext& command) {
                const auto view = command.resolve_texture_view(white.value);
                if (!view)
                    return view.result;
                std::array<rhi::BindingWrite, 3> writes{};
                writes[0].location = {1, 0};
                writes[0].resource.type = rhi::BindingType::kSampledTexture;
                writes[0].resource.texture_view = view.value;
                writes[1].location = {1, 1};
                writes[1].resource.type = rhi::BindingType::kSampler;
                writes[1].resource.sampler = sampler.value;
                writes[2].location = {1, 2};
                writes[2].resource.type = rhi::BindingType::kUniformBuffer;
                writes[2].resource.buffer_range = {tint.value, 0, sizeof(math::Vec4)};
                return device.update_binding_set(material_set.value, writes);
            });
        if (!binding)
            return 17;
        auto snapshot = std::make_shared<const renderer::RenderSceneSnapshot>(scene);
        std::weak_ptr<const renderer::RenderSceneSnapshot> scene_lease = snapshot;
        std::shared_ptr<const std::vector<feature::RenderFeatureOutput>> outputs;
        std::weak_ptr<const void> output_lease;
        rhi::TextureHandle aliased_native{};
        std::vector<renderer::RenderFeatureSceneBinding> scenes{{scene_profile_mode ? renderer::kRuntimeSceneResourceId
            : feature::RenderResourceId("Shadow.Scene"), 1, {}, snapshot, &resolver}};
        ClusteredFixtureResources clustered_fixture{};
        ClusteredFixtureResources second_clustered_fixture{};
        std::array<std::weak_ptr<const render_graph::RenderLightListLease>, 2> paired_bundle_leases{};
        bool paired_lifetimes_overlap = false;
        std::weak_ptr<const render_graph::RenderLightListLease> shadow_bundle_lease;
        render_graph::RenderGraphPass unused_shadow_pass{};
        render_graph::RenderGraphTexture unused_shadow{};
        std::shared_ptr<const renderer::RenderSceneSnapshot> producer_snapshot;
        std::weak_ptr<const renderer::RenderSceneSnapshot> producer_lease;
        std::shared_ptr<const renderer::RenderSceneSnapshot> second_snapshot;
        std::weak_ptr<const renderer::RenderSceneSnapshot> second_scene_lease;
        if (unshadowed_mode) {
            const auto sentinel = graph.create_texture(depth_desc);
            if (!sentinel || !second_color) return 58;
            unused_shadow = sentinel.value;
            const auto sentinel_pass = graph.add_pass("Unused shadow sentinel", rhi::QueueClass::kGraphics,
                [target = sentinel.value](render_graph::PassBuilder& pass) {
                    return pass.use_depth_stencil_attachment(target, rhi::LoadOp::kClear, rhi::StoreOp::kStore, 1.f, 0);
                }, [](render_graph::RenderPassCommandContext& command) {
                    if (const auto result = command.begin_rendering(); !result) return result;
                    return command.end_rendering();
                });
            if (!sentinel_pass) return 58;
            unused_shadow_pass = sentinel_pass.value;
        }
        if (clustered_pair_mode) {
            if (!second_depth) return 68;
            auto second_scene = scene;
            second_scene.lights[1].intensity = 0;
            second_scene.lights[2].intensity = 1;
            second_snapshot = std::make_shared<const renderer::RenderSceneSnapshot>(std::move(second_scene));
            second_scene_lease = second_snapshot;
            scenes.clear();
            for (size_t index = 0; index < paired_instances.size(); ++index)
                scenes.push_back({feature::RenderResourceId("Shadow.Scene"), 1, paired_instances[index],
                    index % 2 == 0 ? snapshot : second_snapshot, &resolver});
            resolver.observed_culling_by_scene = {{{snapshot.get(), &clustered_fixture}, {second_snapshot.get(), &second_clustered_fixture}}};
        }
        if (gbuffer_mode) {
            auto source_scene = base_scene;
            source_scene.frame_number = frame.frame_number();
            std::reverse(source_scene.instances.begin(), source_scene.instances.end());
            if (iteration == 2 || iteration == 3) source_scene.instances.clear();
            producer_snapshot = std::make_shared<const renderer::RenderSceneSnapshot>(std::move(source_scene));
            producer_lease = producer_snapshot;
            scenes.push_back({feature::RenderResourceId("GBuffer.Producer.Scene"), 1, {}, producer_snapshot, &resolver});
        }
        if (lighting_mode) {
            producer_snapshot = snapshot;
            producer_lease = producer_snapshot;
            scenes.push_back({feature::RenderResourceId("Lighting.Geometry.Scene"), 1, {}, producer_snapshot, &resolver});
            auto consumer_scene = scene;
            if (consumer_scene.shadow_casters.empty()) consumer_scene.shadow_casters = consumer_scene.instances;
            consumer_scene.instances.clear();
            snapshot = std::make_shared<const renderer::RenderSceneSnapshot>(consumer_scene);
            scene_lease = snapshot;
            scenes[0].snapshot = snapshot;
            if (!consumer_scene.lights.empty()) consumer_scene.lights.front().intensity = 2;
            second_snapshot = std::make_shared<const renderer::RenderSceneSnapshot>(std::move(consumer_scene));
            second_scene_lease = second_snapshot;
            scenes.push_back({feature::RenderResourceId("Lighting.Second.Scene"), 1, {}, second_snapshot, &resolver});
        }
        if (clustered_feature_mode) {
            resolver.observed_culling = clustered_pair_mode ? nullptr : &clustered_fixture;
            resolver.culling_resolutions = 0;
            std::vector<render_graph::RenderGraphFeatureTextureBinding> textures{
                {kColor, color.value}, {kColor2, second_color.value}, {kDepth, depth.value}};
            if (clustered_pair_mode) {
                textures.pop_back();
                for (size_t index = 0; index < paired_instances.size(); ++index)
                    textures.push_back({kDepth, index % 2 == 0 ? depth.value : second_depth.value, 1, paired_instances[index]});
            }
            if (clustered_shadow_mode) {
                if (!shadow) return 67;
                textures.push_back({kShadow, shadow.value});
            }
            if (clustered_wave_mode && iteration == final_wave_iteration) {
                static_cast<void>(wave_consumer_release.complete());
                if (!pump_wave_until([&] { return feature_runtime.find_instance(feature_plan.provenance[2].instance_id)->state.commit_count == 2; }) ||
                    !check(feature_runtime.render_publication_status(producer.contract.clock.id))) return 65;
            }
            if (!clustered_wave_mode && !check(feature_runtime.tick(producer.contract.clock.id))) return 63;
            auto captured = feature_runtime.capture_render_outputs(feature_plan);
            if (!captured || captured.value.size() != (clustered_pair_mode ? 4u : 3u)) return 63;
            if (unshadowed_mode && !check(scope_current_scene_capture(captured.value, feature_runtime, snapshot->view.id))) return 74;
            if (clustered_pair_mode) {
                std::vector<feature::RenderFeatureOutput> ordered;
                for (const auto instance : paired_instances) {
                    const auto found = std::find_if(captured.value.begin(), captured.value.end(),
                        [&](const auto& item) { return item.instance_id == instance; });
                    if (found == captured.value.end()) return 68;
                    ordered.push_back(std::move(*found));
                }
                captured.value = std::move(ordered);
                const auto& first_list = std::get<feature::RenderLightCullingScene>(captured.value[0].intent.operations[1]).output;
                const auto& second_list = std::get<feature::RenderLightCullingScene>(captured.value[1].intent.operations[1]).output;
                if (first_list.id != second_list.id || first_list.version != second_list.version ||
                    captured.value[0].invocation.invocation_id == captured.value[1].invocation.invocation_id) return 68;
                for (size_t pair = 0; pair < 2; ++pair) {
                    const auto& source = captured.value[pair];
                    const auto& target = captured.value[pair + 2];
                    if (source.contract.definition_id != producer.id || target.contract.definition_id != consumer.id ||
                        target.input_messages.size() != 1) return 68;
                    const auto& message = target.input_messages.front();
                    const auto decoded = feature::decode_render_light_list_reference(message.payload);
                    const auto& used = std::get<feature::RenderDrawClusteredScene>(target.intent.operations.front()).light_list;
                    if (!decoded || used != decoded.value || message.source.instance != source.instance_id ||
                        message.source.invocation != source.invocation || message.source.definition_digest != source.resource_owner.definition_digest ||
                        message.source.resource_epoch != source.resource_owner.resource_epoch || message.source.clock != source.contract.clock.id ||
                        message.kind != feature::EdgeKind::kData || message.schema != feature::kRenderLightListSchema || message.schema_version != 1 ||
                        message.source_port != feature::PortId("Out") || message.target_port != feature::PortId("In") ||
                        !message.delivery_id.is_valid() || used.producer_invocation != source.invocation.invocation_id ||
                        used.producer_generation != source.invocation.generation || used.id != first_list.id || used.version != first_list.version) return 68;
                }
                if (captured.value[2].input_messages.front().delivery_id == captured.value[3].input_messages.front().delivery_id) return 68;
            }
            if (clustered_process_mode) {
                const auto& source = captured.value.front();
                if (source.contract.definition_id != producer.id ||
                    source.invocation != feature_runtime.find_instance(source.instance_id)->render_output->invocation ||
                    !check(feature_runtime.render_publication_status(producer.contract.clock.id)) ||
                    (iteration != 0 && source.invocation.state_version.value() < 3)) return 66;
                for (size_t index = 1; index < captured.value.size(); ++index) {
                    const auto& target = captured.value[index];
                    const auto& reference = std::get<feature::RenderDrawClusteredScene>(target.intent.operations.front()).light_list;
                    if (reference.producer_invocation != source.invocation.invocation_id ||
                        reference.producer_generation != source.invocation.generation ||
                        target.input_messages.size() != 1 ||
                        target.input_messages.front().source.invocation != source.invocation ||
                        target.input_messages.front().source.instance != source.instance_id) return 66;
                }
            }
            if (clustered_wave_mode) {
                const auto expected_wave = iteration == final_wave_iteration ? 1u : 0u;
                const auto& source = captured.value.front();
                if (source.invocation.input_version != expected_wave ||
                    std::get<feature::RenderLightCullingScene>(source.intent.operations.back()).output.version != expected_wave + 1) return 65;
                for (size_t index = 1; index != captured.value.size(); ++index) {
                    const auto& target = captured.value[index];
                    const auto& list = std::get<feature::RenderDrawClusteredScene>(target.intent.operations.front()).light_list;
                    if (target.invocation.input_version != expected_wave || list.producer_invocation != source.invocation.invocation_id ||
                        list.producer_generation != source.invocation.generation || list.version != expected_wave + 1 ||
                        target.input_messages.size() != 1 || target.input_messages.front().source.invocation != source.invocation ||
                        target.input_messages.front().source.instance != source.instance_id) return 65;
                }
            }
            if (!clustered_pair_mode || iteration == 1) std::reverse(captured.value.begin(), captured.value.end());
            outputs = std::make_shared<const std::vector<feature::RenderFeatureOutput>>(std::move(captured.value));
            output_lease = outputs;
            const renderer::RenderFeatureDrawLowerer lowerer(scenes, {&passes, pipeline::detail::RenderPassRegistryExecution::execution_epoch(passes)});
            std::string_view rejection_case{"captured producer shape"};
            const auto clustered_failure = [&](int line, size_t consumers = std::numeric_limits<size_t>::max()) {
                std::cerr << "Clustered fixture assertion failed: line=" << line << " frame=" << iteration
                    << " case=" << rejection_case << " culling-resolutions=" << resolver.culling_resolutions;
                if (consumers != std::numeric_limits<size_t>::max()) std::cerr << " native-consumers=" << consumers;
                std::cerr << '\n';
                return 64;
            };
            if (!clustered_pair_mode && ((*outputs)[2].contract.definition_id != producer.id ||
                (*outputs)[2].intent.operations.size() != (clustered_shadow_mode ? 3u : 2u))) return clustered_failure(__LINE__);
            if (iteration == 0 || (!clustered_pair_mode && iteration == 1)) {
                const auto alternate_depth = graph.create_texture(depth_desc);
                const auto culling_normal = graph.create_texture(color_desc);
                auto material_description = color_desc;
                material_description.format = rhi::Format::kRgba8Unorm;
                const auto culling_material = graph.create_texture(material_description);
                rejection_case = "negative texture controls";
                if (!alternate_depth || !culling_normal || !culling_material) return clustered_failure(__LINE__);
                if (!check(graph.compile())) return clustered_failure(__LINE__);
                const auto baseline = aeg::render_graph::detail::RenderGraphExecution::compile_info(graph);
                const auto reject = [&](const std::vector<feature::RenderFeatureOutput>& candidate,
                    std::span<const renderer::RenderFeatureSceneBinding> scene_bindings,
                    std::span<const render_graph::RenderGraphFeatureTextureBinding> bindings,
                    const render_graph::RenderSceneDrawLowerer* supplied = nullptr, std::string_view required_error = {},
                    std::optional<foundation::ResultCode> required_code = {}) {
                    const renderer::RenderFeatureDrawLowerer native(scene_bindings, {&passes, pipeline::detail::RenderPassRegistryExecution::execution_epoch(passes)});
                    const auto failed = render_graph::build_render_graph_features(graph,
                        std::make_shared<const std::vector<feature::RenderFeatureOutput>>(candidate),
                        {.textures = bindings, .draw_lowerer = supplied ? supplied : &native, .current_frame_owner = &frame},
                        frame.frame_number());
                    if (failed || (failed.code != foundation::ResultCode::kInvalidState &&
                        failed.code != foundation::ResultCode::kInvalidArgument) || !check(graph.compile()) ||
                        (required_code && failed.code != *required_code) ||
                        (!required_error.empty() && failed.message.find(required_error) == std::string::npos) ||
                        aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).pass_count != baseline.pass_count ||
                        aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).resource_count != baseline.resource_count ||
                        aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).culled_pass_count != baseline.culled_pass_count) {
                        std::cerr << "typed bundle accepted invalid production or failed rollback: " << rejection_case << ": " << failed.message << '\n';
                        return false;
                    }
                    return true;
                };
                if (clustered_pair_mode) {
                    rejection_case = "real receipt reference mismatch";
                    auto wrong_reference = *outputs;
                    std::get<feature::RenderDrawClusteredScene>(wrong_reference[2].intent.operations[0]).light_list =
                        std::get<feature::RenderDrawClusteredScene>(wrong_reference[3].intent.operations[0]).light_list;
                    ObservedSceneLowerer receipt_mismatch(lowerer);
                    const auto before_receipt = resolver.culling_resolutions;
                    if (!reject(wrong_reference, scenes, textures, &receipt_mismatch, {}, foundation::ResultCode::kInvalidArgument) ||
                        receipt_mismatch.consumer_calls != 0 || resolver.culling_resolutions != before_receipt) return 68;
                    auto wrong_depth = textures;
                    rejection_case = "other producer camera depth";
                    const auto consumer_depth = std::find_if(wrong_depth.begin(), wrong_depth.end(), [&](const auto& binding) {
                        return binding.id == kDepth && binding.owner == paired_instances[2];
                    });
                    if (consumer_depth == wrong_depth.end()) return 68;
                    consumer_depth->texture = second_depth.value;
                    ObservedSceneLowerer depth_mismatch(lowerer);
                    if (!reject(*outputs, scenes, wrong_depth, &depth_mismatch, "clustered draw camera depth") ||
                        depth_mismatch.consumer_calls != 1) return 68;
                    auto cloned_scene = scenes;
                    rejection_case = "consumer canonical snapshot clone";
                    cloned_scene[2].snapshot = std::make_shared<const renderer::RenderSceneSnapshot>(*snapshot);
                    const renderer::RenderFeatureDrawLowerer cloned_lowerer(cloned_scene, {&passes, pipeline::detail::RenderPassRegistryExecution::execution_epoch(passes)});
                    ObservedSceneLowerer snapshot_mismatch(cloned_lowerer);
                    if (!reject(*outputs, cloned_scene, textures, &snapshot_mismatch, "clustered draw camera depth") ||
                        snapshot_mismatch.consumer_calls != 1) return 68;
                    ObservedSceneLowerer returned_other_producer(lowerer);
                    rejection_case = "returned other producer lease";
                    returned_other_producer.return_saved = true;
                    if (!reject(*outputs, scenes, textures, &returned_other_producer) || returned_other_producer.consumer_calls != 0 ||
                        !returned_other_producer.saved || !returned_other_producer.fresh ||
                        returned_other_producer.saved->production().owner.instance_id != paired_instances[0] ||
                        returned_other_producer.fresh->production().owner.instance_id != paired_instances[1]) return 68;
                    ObservedSceneLowerer consumed_other_producer(lowerer);
                    rejection_case = "consumed other producer lease";
                    consumed_other_producer.consume_saved = true;
                    if (!reject(*outputs, scenes, textures, &consumed_other_producer, "clustered draw camera depth") ||
                        consumed_other_producer.consumer_calls != 2) return 68;
                    ObservedSceneLowerer strong_aba(lowerer);
                    rejection_case = "rollback after both native consumers";
                    strong_aba.fail_consumer = 2; strong_aba.create_witness = true;
                    if (!reject(*outputs, scenes, textures, &strong_aba, "injected failure after actual consumer lowering") ||
                        strong_aba.consumer_calls != 2 || !strong_aba.saved || !strong_aba.fresh) return 68;
                    resolver.substituted_culling = clustered_fixture.lights;
                    rejection_case = "other snapshot native resource origin";
                    ObservedSceneLowerer origin_mismatch(lowerer);
                    const auto before_origin = resolver.culling_resolutions;
                    const bool wrong_origin_rejected = reject(*outputs, scenes, textures, &origin_mismatch,
                        "light culling resource has another frame, device, snapshot or native payload");
                    resolver.substituted_culling.reset();
                    if (!wrong_origin_rejected || origin_mismatch.consumer_calls != 0 || !origin_mismatch.saved ||
                        origin_mismatch.saved->production().owner.instance_id != paired_instances[0] ||
                        resolver.culling_resolutions != before_origin + 2) return 68;
                    strong_aba.consumer_calls = 0; strong_aba.return_saved = true;
                    rejection_case = "retained lease build ABA";
                    if (!reject(*outputs, scenes, textures, &strong_aba) || strong_aba.consumer_calls != 0 ||
                        strong_aba.saved->scope().identity == strong_aba.fresh->scope().identity ||
                        strong_aba.saved_witness != strong_aba.fresh_witness) return 68;
                    clustered_fixture = {};
                    second_clustered_fixture = {};
                } else if (iteration == 0) {
                    for (unsigned depth_failure = 0; depth_failure < 3; ++depth_failure) {
                        auto invalid = *outputs;
                        auto& operations = invalid[2].intent.operations;
                        if (depth_failure == 0) operations.erase(operations.begin()); // Host D32 is not a production.
                        if (depth_failure == 1) std::get<feature::RenderDrawDepthScene>(operations[0]).depth.load = feature::RenderAttachmentLoad::kLoad;
                        if (depth_failure == 2) std::get<feature::RenderDrawDepthScene>(operations[0]).projection = feature::RenderDepthSceneProjection::kDirectionalShadow;
                        rejection_case = depth_failure == 0 ? "missing camera depth production" : depth_failure == 1
                            ? "unproduced depth Load" : "camera replaced by directional depth";
                        if (!reject(invalid, scenes, textures)) return clustered_failure(__LINE__);
                    }
                    const auto clone = std::make_shared<const renderer::RenderSceneSnapshot>(*snapshot);
                    std::vector<renderer::RenderFeatureSceneBinding> owned_scenes;
                    for (size_t index = 0; index < outputs->size(); ++index)
                        owned_scenes.push_back({feature::RenderResourceId("Shadow.Scene"), 1, (*outputs)[index].instance_id,
                            index == 1 ? clone : snapshot, &resolver});
                    rejection_case = "equal-value consumer snapshot clone";
                    if (!reject(*outputs, owned_scenes, textures)) return clustered_failure(__LINE__); // Equal scene values do not share production identity.
                    std::vector<render_graph::RenderGraphFeatureTextureBinding> owned_depths{{kColor, color.value}, {kColor2, second_color.value}};
                    for (size_t index = 0; index < outputs->size(); ++index)
                        owned_depths.push_back({kDepth, index == 1 ? alternate_depth.value : depth.value, 1, (*outputs)[index].instance_id});
                    rejection_case = "other owner camera depth handle";
                    if (!reject(*outputs, scenes, owned_depths)) return clustered_failure(__LINE__);
                    ObservedSceneLowerer wrong_depth_snapshot(lowerer);
                    wrong_depth_snapshot.depth_snapshot_override = clone;
                    rejection_case = "lowerer returned cloned depth snapshot";
                    if (!reject(*outputs, scenes, textures, &wrong_depth_snapshot)) return clustered_failure(__LINE__);
                    auto overwritten = *outputs;
                    auto overwrite = std::get<feature::RenderDrawDepthScene>(overwritten[2].intent.operations[0]);
                    overwrite.projection = feature::RenderDepthSceneProjection::kDirectionalShadow;
                    overwritten[0].intent.operations.push_back(overwrite);
                    ObservedSceneLowerer overwritten_lowerer(lowerer);
                    rejection_case = "directional depth overwrite after first actual consumer";
                    if (!reject(overwritten, scenes, textures, &overwritten_lowerer) || overwritten_lowerer.consumer_calls != 2)
                        return clustered_failure(__LINE__, overwritten_lowerer.consumer_calls);
                    for (uint8_t failure = 1; failure <= 8; ++failure) {
                        resolver.culling_failure = failure;
                        rejection_case = "invalid resolved culling payload";
                        if (!reject(*outputs, scenes, textures)) return clustered_failure(__LINE__);
                    }
                    resolver.culling_failure = 0;
                    rejection_case = "other canonical snapshot culling resource origin";
                    const auto other_snapshot = culling.for_frame(frame, *clone, {});
                    if (!other_snapshot) return clustered_failure(__LINE__);
                    resolver.substituted_culling = other_snapshot.value;
                    if (!reject(*outputs, scenes, textures)) return clustered_failure(__LINE__);
                    resolver.substituted_culling.reset();
                    rejection_case = "other FrameContext culling resource origin";
                    render_graph::FrameContext other_frame(device, {.frames_in_flight = 1});
                    if (!check(other_frame.begin_frame()) || other_frame.frame_number() != frame.frame_number()) return clustered_failure(__LINE__);
                    const auto foreign = culling.for_frame(other_frame, *snapshot, {});
                    if (!foreign) return clustered_failure(__LINE__);
                    resolver.substituted_culling = foreign.value;
                    if (!reject(*outputs, scenes, textures)) return clustered_failure(__LINE__);
                    resolver.substituted_culling.reset();
                    if (!check(other_frame.abort_frame()) || !check(other_frame.drain())) return clustered_failure(__LINE__);
                    rejection_case = "foreign RHI device resource origin";
                    const auto another_device = rhi::create_device({rhi::BackendType::kVulkan, "Origin device control"});
                    if (!another_device) return clustered_failure(__LINE__);
                    render_graph::FrameContext foreign_frame(*another_device.value, {.frames_in_flight = 1});
                    if (!check(foreign_frame.begin_frame())) return clustered_failure(__LINE__);
                    const auto wrong_device = culling.for_frame(foreign_frame, *snapshot, {});
                    if (wrong_device || wrong_device.result.code != foundation::ResultCode::kInvalidState) {
                        (void)foreign_frame.abort_frame();
                        (void)foreign_frame.drain();
                        return clustered_failure(__LINE__);
                    }
                    const auto wrong_deferred = deferred.for_frame(foreign_frame);
                    if (wrong_deferred || wrong_deferred.result.code != foundation::ResultCode::kInvalidState) {
                        std::cerr << "DeferredLightingResources accepted a FrameContext from a foreign RHI device\n";
                        (void)foreign_frame.abort_frame();
                        (void)foreign_frame.drain();
                        return 70;
                    }
                    renderer::RenderShadowConstants foreign_constants{};
                    foreign_constants.parameters = {1.0f, 1.0f, 0.0f, 0.0f};
                    const auto wrong_shadow = shadows.for_frame(foreign_frame, foreign_constants);
                    if (wrong_shadow || wrong_shadow.result.code != foundation::ResultCode::kInvalidState) {
                        std::cerr << "ShadowResources accepted a FrameContext from a foreign RHI device\n";
                        (void)foreign_frame.abort_frame();
                        (void)foreign_frame.drain();
                        return 71;
                    }
                    if (!check(foreign_frame.abort_frame()) || !check(foreign_frame.drain())) return clustered_failure(__LINE__);
                    // Strong A remains alive through B/C; a witness verifies numeric graph-handle recurrence.
                    rejection_case = "actual strong build identity ABA";
                    ObservedSceneLowerer aba(lowerer);
                    aba.create_witness = true;
                    aba.fail_consumer = 2;
                    if (!reject(*outputs, scenes, textures, &aba) || !aba.saved || aba.consumer_calls != 2) return 65;
                    aba.consumer_calls = 0;
                    aba.return_saved = true;
                    if (!reject(*outputs, scenes, textures, &aba) || aba.consumer_calls != 0 || !aba.fresh ||
                        aba.saved->scope().identity == aba.fresh->scope().identity ||
                        aba.saved->scope().frame_index != aba.fresh->scope().frame_index ||
                        aba.saved->scope().graph_id != aba.fresh->scope().graph_id ||
                        aba.saved_witness != aba.fresh_witness) return 65;
                    aba.return_saved = false;
                    aba.consume_saved = true;
                    aba.consumer_calls = 0;
                    if (!reject(*outputs, scenes, textures, &aba) || aba.consumer_calls != 1) return 65;
                    aba.consume_saved = false;
                    aba.consumer_calls = 0;
                    auto preserved = *outputs;
                    auto load = std::get<feature::RenderDrawDepthScene>(preserved[2].intent.operations[0]);
                    load.depth.load = feature::RenderAttachmentLoad::kLoad;
                    preserved[2].intent.operations.insert(preserved[2].intent.operations.begin() + 1, load);
                    if (!reject(preserved, scenes, textures, &aba) || aba.consumer_calls != 2) return 65;
                    auto gbuffer_production = *outputs;
                    gbuffer_production[2].intent.operations[0] = gbuffer_draw(
                        {feature::RenderResourceId("Shadow.Scene"), 1}, feature::RenderAttachmentLoad::kClear);
                    auto gbuffer_bindings = std::vector<render_graph::RenderGraphFeatureTextureBinding>(textures.begin(), textures.end());
                    gbuffer_bindings.push_back({kNormal, culling_normal.value});
                    gbuffer_bindings.push_back({kMaterial, culling_material.value});
                    aba.consumer_calls = 0;
                    if (!reject(gbuffer_production, scenes, gbuffer_bindings, &aba) || aba.consumer_calls != 2) return 65;
                    auto& loaded_gbuffer = std::get<feature::RenderDrawGBufferScene>(gbuffer_production[2].intent.operations[0]);
                    loaded_gbuffer.depth.load = feature::RenderAttachmentLoad::kLoad;
                    aba.consumer_calls = 0;
                    if (!reject(gbuffer_production, scenes, gbuffer_bindings, &aba) || aba.consumer_calls != 0) return 65;
                    if (!clustered_shadow_mode) {
                        // Optional, disabled shadow still needs matching directional depth production.
                        auto with_shadow = *outputs;
                        const feature::RenderTextureReference shadow_reference{feature::RenderResourceId(kShadow.value()), 1, true};
                        std::get<feature::RenderLightCullingScene>(with_shadow[2].intent.operations[1]).directional_shadow = shadow_reference;
                        for (size_t index = 0; index < 2; ++index)
                            std::get<feature::RenderDrawClusteredScene>(with_shadow[index].intent.operations[0]).directional_shadow = shadow_reference;
                        auto shadow_bindings = std::vector<render_graph::RenderGraphFeatureTextureBinding>(textures.begin(), textures.end());
                        shadow_bindings.push_back({kShadow, alternate_depth.value});
                        if (!reject(with_shadow, scenes, shadow_bindings)) return 65;
                        auto shadow_draw = std::get<feature::RenderDrawDepthScene>(with_shadow[2].intent.operations[0]);
                        shadow_draw.projection = feature::RenderDepthSceneProjection::kDirectionalShadow;
                        shadow_draw.depth.target = shadow_reference;
                        with_shadow[2].intent.operations.insert(with_shadow[2].intent.operations.begin() + 1, shadow_draw);
                        aba.consumer_calls = 0;
                        if (!reject(with_shadow, scenes, shadow_bindings, &aba) || aba.consumer_calls != 2) return 65;
                        shadow_bindings.back().texture = depth.value;
                        if (!reject(with_shadow, scenes, shadow_bindings)) return 65;
                    }
                } else {
                    rejection_case = "retired prior frame culling payload";
                    if (!retired_culling) return clustered_failure(__LINE__);
                    resolver.substituted_culling = retired_culling;
                    if (!reject(*outputs, scenes, textures)) return clustered_failure(__LINE__);
                    resolver.substituted_culling.reset();
                    if (clustered_shadow_mode) {
                        const auto fit = renderer::make_directional_shadow_view(*snapshot);
                        if (!fit || !fit.value.enabled) return 67;
                        auto overwritten = *outputs;
                        auto shadow_draw = std::get<feature::RenderDrawDepthScene>(overwritten[2].intent.operations[1]);
                        shadow_draw.depth.clear_depth = .25f;
                        overwritten[0].intent.operations.push_back(shadow_draw);
                        ObservedSceneLowerer observe_overwrite(lowerer);
                        if (!reject(overwritten, scenes, textures, &observe_overwrite) || observe_overwrite.consumer_calls != 2) {
                            std::cerr << "cull shadow contents were replaced before the second consumer\n";
                            return 67;
                        }
                        overwritten = *outputs;
                        auto camera_draw = std::get<feature::RenderDrawDepthScene>(overwritten[2].intent.operations[0]);
                        camera_draw.depth.clear_depth = .25f;
                        overwritten[0].intent.operations.push_back(camera_draw);
                        ObservedSceneLowerer observe_camera_overwrite(lowerer);
                        if (!reject(overwritten, scenes, textures, &observe_camera_overwrite) ||
                            observe_camera_overwrite.consumer_calls != 2) return 67;
                        for (unsigned defect = 0; defect != 3; ++defect) {
                            auto invalid = *outputs;
                            auto& operations = invalid[2].intent.operations;
                            if (defect == 0) operations.erase(operations.begin() + 1);
                            if (defect == 1) std::get<feature::RenderDrawDepthScene>(operations[1]).projection = feature::RenderDepthSceneProjection::kCamera;
                            if (defect == 2) std::get<feature::RenderDrawDepthScene>(operations[1]).depth.load = feature::RenderAttachmentLoad::kLoad;
                            if (!reject(invalid, scenes, textures)) return 67;
                        }
                        const auto clone = std::make_shared<const renderer::RenderSceneSnapshot>(*snapshot);
                        ObservedSceneLowerer shadow_snapshot(lowerer);
                        shadow_snapshot.depth_snapshot_override = clone;
                        shadow_snapshot.depth_override_projection = feature::RenderDepthSceneProjection::kDirectionalShadow;
                        if (!reject(*outputs, scenes, textures, &shadow_snapshot) || shadow_snapshot.depth_override_calls != 1) return 67;
                        auto aliased_shadow = textures;
                        aliased_shadow.back().texture = depth.value;
                        if (!reject(*outputs, scenes, aliased_shadow)) return 67;
                        auto wrong_consumer_shadow = textures;
                        wrong_consumer_shadow.push_back({kShadow, alternate_depth.value, 1, (*outputs)[0].instance_id});
                        if (!reject(*outputs, scenes, wrong_consumer_shadow)) return 67;
                        const auto valid_material = resolver.material;
                        for (unsigned defect = 0; defect != 6; ++defect) {
                            auto invalid_material = valid_material;
                            switch (defect) {
                            case 0: invalid_material.clustered_pipeline = {}; break;
                            case 1: invalid_material.clustered_pipeline_contract.color_format = rhi::Format::kRgba8Unorm; break;
                            case 2: invalid_material.clustered_pipeline_contract.samples = rhi::SampleCount::kX4; break;
                            case 3: invalid_material.clustered_pipeline_contract.depth_stencil_format = rhi::Format::kUnknown; break;
                            case 4: invalid_material.clustered_pipeline_contract.additional_color_formats[0] = rhi::Format::kRgba8Unorm; break;
                            case 5: invalid_material.clustered_pipeline_contract.additional_color_formats[1] = rhi::Format::kRgba8Unorm; break;
                            }
                            resolver.material = invalid_material;
                            const auto before = resolver.culling_resolutions;
                            const bool rejected = reject(*outputs, scenes, textures, nullptr, "opaque clustered material");
                            resolver.material = valid_material;
                            if (!rejected || resolver.culling_resolutions != before + 1) return 67;
                        }
                        ObservedSceneLowerer enabled_aba(lowerer);
                        enabled_aba.create_witness = true; enabled_aba.fail_consumer = 2;
                        if (!reject(*outputs, scenes, textures, &enabled_aba) || !enabled_aba.saved || enabled_aba.consumer_calls != 2) return 67;
                        enabled_aba.consumer_calls = 0; enabled_aba.return_saved = true;
                        if (!reject(*outputs, scenes, textures, &enabled_aba) || enabled_aba.consumer_calls != 0 || !enabled_aba.fresh ||
                            enabled_aba.saved->scope().identity == enabled_aba.fresh->scope().identity ||
                            enabled_aba.saved_witness != enabled_aba.fresh_witness) return 67;
                    }
                }
                resolver.culling_resolutions = 0;
            }
            if (unshadowed_mode && iteration == 0) {
                if (!check(graph.compile())) return 62;
                const auto baseline = aeg::render_graph::detail::RenderGraphExecution::compile_info(graph);
                for (unsigned failure = 0; failure < 6; ++failure) {
                    auto bad = resolver.material;
                    switch (failure) {
                    case 0: bad.unshadowed_clustered_variant.reset(); break;
                    case 1: bad.unshadowed_clustered_variant->pipeline = {}; break;
                    case 2: bad.unshadowed_clustered_variant->attachment_contract.color_format = rhi::Format::kRgba8Unorm; break;
                    case 3: bad.unshadowed_clustered_variant->attachment_contract.samples = rhi::SampleCount::kX4; break;
                    case 4: bad.unshadowed_clustered_variant->attachment_contract.depth_stencil_format = rhi::Format::kUnknown; break;
                    case 5: bad.unshadowed_clustered_variant->attachment_contract.additional_color_formats[0] = rhi::Format::kRgba8Unorm; break;
                    }
                    ObservedSceneLowerer rejected(lowerer);
                    rejected.material = &resolver.material;
                    rejected.replacement_material = &bad;
                    rejected.replacement_consumer = 2;
                    const auto failed = render_graph::build_render_graph_features(graph, outputs,
                        {.textures = textures, .draw_lowerer = &rejected, .current_frame_owner = &frame}, frame.frame_number());
                    if (failed || failed.code != foundation::ResultCode::kInvalidArgument ||
                        rejected.consumer_calls != 2 || rejected.material_replacements != 1 || !check(graph.compile()) ||
                        aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).pass_count != baseline.pass_count ||
                        aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).resource_count != baseline.resource_count ||
                        aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).culled_pass_count != baseline.culled_pass_count)
                        return 62;
                }
                resolver.culling_resolutions = 0;
            }
            if ((unshadowed_mode && iteration == 0) ||
                (clustered_shadow_mode && !clustered_shadow_timing_mode && iteration == 1)) {
                const bool shadowed = clustered_shadow_mode;
                const auto shadow_view = renderer::make_directional_shadow_view(*snapshot);
                auto bad = resolver.material;
                ++bad.variant_cache_epoch;
                if (!shadowed && !bad.unshadowed_clustered_variant) return 75;
                const auto& profile = shadowed ? bad.clustered_profile_contract
                    : bad.unshadowed_clustered_variant->profile_contract;
                const auto& attachment = shadowed ? bad.clustered_pipeline_contract
                    : bad.unshadowed_clustered_variant->attachment_contract;
                const bool preconditions = shadow_view && shadow_view.value.enabled == shadowed && bad.is_valid() &&
                    profile.is_valid(renderer::RendererProfile::kForwardClustered) &&
                    profile.attachment.is_single_color_attachment() && attachment.is_single_color_attachment() &&
                    profile.attachment == attachment && profile.cache_epoch != bad.variant_cache_epoch &&
                    profile.shader.cache_epoch == profile.cache_epoch &&
                    (shadowed || (bad.unshadowed_clustered_variant->cache_epoch == profile.cache_epoch &&
                        bad.unshadowed_clustered_variant->cache_epoch != bad.variant_cache_epoch));
                if (!preconditions || !check(graph.compile())) return 75;
                const auto baseline = aeg::render_graph::detail::RenderGraphExecution::compile_info(graph);
                auto invalid_outputs = std::make_shared<const std::vector<feature::RenderFeatureOutput>>(*outputs);
                std::weak_ptr<const void> failed_lease = invalid_outputs;
                ObservedSceneLowerer observed(lowerer);
                observed.material = &resolver.material;
                observed.replacement_material = &bad;
                observed.replacement_consumer = 2;
                observed.probe_material_replacement = true;
                // A successful invalid draw is recorded before forcing transaction rollback, so every
                // new case and the subsequent real GPU recovery frame can still be observed on RED.
                const auto rejected = render_graph::build_render_graph_features(graph, invalid_outputs,
                    {.textures = textures, .draw_lowerer = &observed, .current_frame_owner = &frame}, frame.frame_number());
                invalid_outputs.reset();
                observed.saved.reset();
                observed.fresh.reset();
                if (!check(graph.compile())) return 75;
                const auto& after = aeg::render_graph::detail::RenderGraphExecution::compile_info(graph);
                const bool rolled_back = after.pass_count == baseline.pass_count && after.resource_count == baseline.resource_count &&
                    after.culled_pass_count == baseline.culled_pass_count;
                observe_contract_rejection(shadowed ? "clustered-shadow-owner-epoch" : "clustered-unshadowed-owner-epoch",
                    rejected, observed, preconditions && observed.consumer_calls == 2 && observed.material_replacements == 1,
                    failed_lease.expired(), rolled_back, "epoch");
                if (!rolled_back) return 75;
                resolver.culling_resolutions = 0;
            }
            if (clustered_shadow_mode && (clustered_shadow_timing_mode || iteration == 1 || iteration == 2)) {
                resolver.material.unshadowed_clustered_variant.reset();
                resolver.material.shadow_pipeline = {};
                resolver.material.shadow_pipeline_contract = {};
            }
            ObservedSceneLowerer observed_lowerer(lowerer);
            native_observation.records.clear();
            native_observation.expected = outputs.get();
            if (!check(render_graph::build_render_graph_features(graph, outputs,
                {.textures = textures, .draw_lowerer = clustered_pair_mode || clustered_shadow_timing_mode ?
                    static_cast<const render_graph::RenderSceneDrawLowerer*>(&observed_lowerer) : &lowerer, .current_frame_owner = &frame}, frame.frame_number()))) return 63;
            if (unshadowed_mode) {
                const auto constants = graph.import_buffer(clustered_fixture.lights.constants, clustered_fixture.lights.constants_desc);
                const auto lights = graph.import_buffer(clustered_fixture.lights.lights, clustered_fixture.lights.lights_desc);
                if (!constants || !lights) return 58;
                clustered_fixture.lights.constants_graph = {constants.value.index, constants.value.generation, constants.value.graph_id};
                clustered_fixture.lights.lights_graph = {lights.value.index, lights.value.generation, lights.value.graph_id};
            }
            if (resolver.culling_resolutions != (clustered_pair_mode ? 2u : 1u) || !check(graph.compile())) return 63;
            if (clustered_pair_mode) {
                if (!observed_lowerer.saved || !observed_lowerer.fresh || observed_lowerer.saved == observed_lowerer.fresh ||
                    observed_lowerer.saved->scope() != observed_lowerer.fresh->scope() ||
                    observed_lowerer.saved->production().owner == observed_lowerer.fresh->production().owner) return 68;
                for (const auto& bundle : {observed_lowerer.saved, observed_lowerer.fresh}) {
                    const auto& actual = bundle->production();
                    if (actual.output_index >= outputs->size() || actual.operation_index != 1 ||
                        actual.owner != (*outputs)[actual.output_index].resource_owner ||
                        actual.invocation != (*outputs)[actual.output_index].invocation) return 68;
                }
                paired_bundle_leases = {observed_lowerer.saved, observed_lowerer.fresh};
            }
            if (clustered_shadow_timing_mode) {
                if (!observed_lowerer.saved || observed_lowerer.saved != observed_lowerer.fresh || observed_lowerer.consumer_calls != 2) return 69;
                const auto& bundle = *observed_lowerer.saved;
                const auto& production = bundle.production();
                if (production.output_index != 2 || production.operation_index != 2 ||
                    production.owner != (*outputs)[2].resource_owner || production.invocation != (*outputs)[2].invocation ||
                    !std::get<feature::RenderLightCullingScene>((*outputs)[2].intent.operations.back()).directional_shadow || bundle.scope().frame_index != frame.frame_number() || !bundle.scope().identity) return 69;
                if (previous_shadow_scope && (bundle.scope().identity == previous_shadow_scope->identity ||
                    bundle.scope().frame_index == previous_shadow_scope->frame_index)) return 69;
                if (previous_shadow_invocation && ((clustered_shadow_wave_mode && iteration == 1) ?
                    production.invocation != *previous_shadow_invocation : production.invocation == *previous_shadow_invocation)) return 69;
                previous_shadow_scope = bundle.scope(); // Retain only the strong build identity, not GPU/snapshot leases.
                previous_shadow_invocation = production.invocation;
                shadow_bundle_lease = observed_lowerer.saved;
            }
            for (const auto& pass : aeg::render_graph::detail::RenderGraphExecution::plan(graph).pass_plans) {
                if (!pass.feature_work) continue;
                const auto& work = *pass.feature_work;
                const auto source = std::find_if(outputs->begin(), outputs->end(), [&](const auto& item) { return item.instance_id == work.instance_id; });
                if (source == outputs->end() || work.operation_index >= source->intent.operations.size() ||
                    work.invocation != source->invocation || work.frame_index != frame.frame_number()) return 63;
            }
        } else if (feature_mode) {
            std::vector textures{
                render_graph::RenderGraphFeatureTextureBinding{kColor, color.value},
                render_graph::RenderGraphFeatureTextureBinding{kDepth, depth.value},
                render_graph::RenderGraphFeatureTextureBinding{kShadow, shadow.value},
                render_graph::RenderGraphFeatureTextureBinding{kColor2, second_color.value},
                render_graph::RenderGraphFeatureTextureBinding{kDepth2, second_depth.value}};
            if (gbuffer_mode) textures = {{kColor, color.value}, {kNormal, normal.value}, {kMaterial, material.value}, {kDepth, depth.value}};
            if (lighting_mode) textures = {{kColor, color.value}, {kNormal, normal.value}, {kMaterial, material.value},
                {kDepth, depth.value}, {kShadow, shadow.value}, {kHdr, hdr.value}, {kHdr2, second_hdr.value}};
            if (post_feature_mode) {
                textures.push_back({kPost, post_output.value});
                textures.push_back({kPost2, second_post_output.value});
            }
            if (!check(feature_runtime.tick(producer.contract.clock.id))) return 36;
            auto captured = feature_runtime.capture_render_outputs(feature_plan);
            if (!captured || captured.value.size() != (post_feature_mode ? 5u : 3u)) return 36;
            outputs = std::make_shared<const std::vector<feature::RenderFeatureOutput>>(std::move(captured.value));
            output_lease = outputs;
            if (lighting_mode && iteration == 0) {
                std::array<render_graph::RenderGraphTexture, 35> incompatible{};
                for (size_t index = 0; index < incompatible.size(); ++index) {
                    const size_t role = index / 7;
                    auto desc = role == 2 || role == 4 ? depth_desc : color_desc;
                    switch (index % 7) {
                    case 0: desc.format = role == 2 || role == 4 ? rhi::Format::kRgba16Float : rhi::Format::kRgba8Unorm;
                        desc.capabilities = rhi::TextureCapability::kColorAttachment | rhi::TextureCapability::kSampled; break;
                    case 1: desc.capabilities = role == 3 ? rhi::TextureCapability::kSampled : rhi::TextureCapability::kTransferSource; break;
                    case 2: ++desc.extent.width; break;
                    case 3: desc.mip_levels = 2; break;
                    case 4: desc.array_layers = 2; break;
                    case 5: desc.samples = rhi::SampleCount::kX4; break;
                    case 6: desc.dimension = rhi::TextureDimension::kTex3D; desc.extent.depth = 2; break;
                    }
                    const auto target = graph.create_texture(desc);
                    if (!target) return 50;
                    incompatible[index] = target.value;
                }
                const auto native = device.create_texture(color_desc);
                if (!native) return 50;
                aliased_native = native.value;
                const auto alias_a = graph.import_texture(aliased_native, {}, color_desc);
                const auto alias_b = graph.import_texture(aliased_native, {}, color_desc);
                if (!alias_a || !alias_b || !check(graph.compile())) return 50;
                const auto baseline = aeg::render_graph::detail::RenderGraphExecution::compile_info(graph);
                for (size_t test_case = 0; test_case < 101; ++test_case) {
                    auto invalid_outputs = std::make_shared<std::vector<feature::RenderFeatureOutput>>(*outputs);
                    auto& output = *std::find_if(invalid_outputs->begin(), invalid_outputs->end(), [](const auto& item) {
                        return item.node_id == feature::FeatureNodeId(2);
                    });
                    output.intent.operations.push_back(output.intent.operations.back());
                    auto& bad = std::get<feature::RenderDeferredLightingScene>(output.intent.operations.back());
                    bad.directional_shadow = feature::RenderTextureReference{feature::RenderResourceId(kShadow.value()), 1, true};
                    const std::array refs{&bad.albedo, &bad.normal, &bad.depth, &bad.hdr.target, &*bad.directional_shadow};
                    auto bindings = textures;
                    auto invalid_scenes = scenes;
                    resolver.deferred_failure = 0;
                    if (test_case < incompatible.size()) {
                        refs[test_case / 7]->id = feature::RenderResourceId("Invalid.Lighting.Texture");
                        bindings.push_back({pipeline::RenderResourceId("Invalid.Lighting.Texture"), incompatible[test_case]});
                    } else if (test_case < 45) {
                        size_t pair = 35;
                        for (size_t first = 0; first < refs.size(); ++first)
                            for (size_t second = first + 1; second < refs.size(); ++second)
                                if (pair++ == test_case) *refs[second] = *refs[first];
                    } else if (test_case >= 47 && test_case < 67) {
                        bad.scene.id = feature::RenderResourceId("Rejected.Lighting.Scene");
                        auto rejected_scene = std::make_shared<renderer::RenderSceneSnapshot>(*snapshot);
                        rejected_scene->view.id = renderer::RenderViewId("Rejected.Lighting.View");
                        resolver.deferred_failure = static_cast<uint8_t>(test_case - 46);
                        if (resolver.deferred_failure >= 9 && resolver.deferred_failure <= 14)
                            rejected_scene->lights.front().casts_shadow = true;
                        invalid_scenes.push_back({bad.scene.id, 1, {}, rejected_scene, &resolver});
                    } else if (test_case >= 83 && test_case < 88) {
                        refs[test_case - 83]->version = 2;
                    } else if (test_case >= 88 && test_case < 93) {
                        const std::array<size_t, 5> positions{0, 1, 3, 6, 4};
                        bindings[positions[test_case - 88]].owner = outputs->front().instance_id;
                    } else if (test_case >= 93 && test_case < 98) {
                        refs[test_case - 93]->id = feature::RenderResourceId("Missing.Lighting.Texture");
                    } else switch (test_case) {
                    case 45: bindings[1].texture = bindings[0].texture; break;
                    case 46:
                        bad.albedo.id = feature::RenderResourceId("Alias.Lighting.A");
                        bad.normal.id = feature::RenderResourceId("Alias.Lighting.B");
                        bindings.push_back({pipeline::RenderResourceId("Alias.Lighting.A"), alias_a.value});
                        bindings.push_back({pipeline::RenderResourceId("Alias.Lighting.B"), alias_b.value}); break;
                    case 67: break; // Missing lowerer below.
                    case 68: bad.scene.id = feature::RenderResourceId("Missing.Lighting.Scene"); break;
                    case 69: bad.scene.version = 2; break;
                    case 70: invalid_scenes.back().owner = outputs->front().instance_id; break;
                    case 71: case 72: case 73: case 74: case 100: {
                        bad.scene.id = feature::RenderResourceId("Invalid.Lighting.Scene");
                        auto rejected_scene = std::make_shared<renderer::RenderSceneSnapshot>(*snapshot);
                        if (test_case == 71) ++rejected_scene->frame_number;
                        if (test_case == 74) ++rejected_scene->view.output_extent.width;
                        if (test_case == 100) rejected_scene->world = {};
                        invalid_scenes.push_back({bad.scene.id, 1, {}, test_case == 72 ? nullptr : rejected_scene,
                            test_case == 73 ? nullptr : &resolver}); break;
                    }
                    case 75: output.contract.permissions.capability_mask &= ~feature::to_mask(feature::RenderIntentCapability::kReadExternal); break;
                    case 76: output.contract.permissions.capability_mask &= ~feature::to_mask(feature::RenderIntentCapability::kDrawScene); break;
                    case 77: output.contract.permissions.capability_mask &= ~feature::to_mask(feature::RenderIntentCapability::kWriteExternal); break;
                    case 78: bad.hdr.clear_color[0] = std::numeric_limits<float>::quiet_NaN(); break;
                    case 79: bad.hdr.load = static_cast<feature::RenderAttachmentLoad>(255); break;
                    case 80: output.intent.final_output = bad.hdr.target; break;
                    case 81: output.intent.operations.push_back(feature::RenderCopyTexture{bad.albedo, bad.hdr.target, kSize, kSize}); break;
                    case 82: output.intent.operations.push_back(feature::RenderClearColor{bad.hdr.target, {0, 0, 0, 1}}); break;
                    case 98: bad.albedo.external = false; break;
                    case 99: invalid_scenes.push_back(invalid_scenes.back()); break;
                    }
                    const renderer::RenderFeatureDrawLowerer invalid_lowerer(invalid_scenes, {&passes, pipeline::detail::RenderPassRegistryExecution::execution_epoch(passes)});
                    std::weak_ptr<const void> failed_lease = invalid_outputs;
                    const auto rejected = render_graph::build_render_graph_features(graph, invalid_outputs,
                        {.textures = bindings, .draw_lowerer = test_case == 67 ? nullptr : &invalid_lowerer}, frame.frame_number());
                    resolver.deferred_failure = 0;
                    invalid_outputs.reset();
                    if (rejected || !failed_lease.expired() || !check(graph.compile()) ||
                        aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).pass_count != baseline.pass_count ||
                        aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).resource_count != baseline.resource_count ||
                        aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).culled_pass_count != baseline.culled_pass_count) {
                        std::cerr << "DeferredLighting rollback case=" << test_case << '\n'; return 51;
                    }
                }
                for (uint8_t failure : {uint8_t{21}, uint8_t{22}}) {
                    if (lighting_attachment_probes_observed) break;
                    const bool shadowed = failure == 22;
                    auto invalid_outputs = std::make_shared<std::vector<feature::RenderFeatureOutput>>(*outputs);
                    auto& output = *std::find_if(invalid_outputs->begin(), invalid_outputs->end(), [](const auto& item) {
                        return item.node_id == feature::FeatureNodeId(2);
                    });
                    output.intent.operations.push_back(output.intent.operations.back());
                    auto& bad = std::get<feature::RenderDeferredLightingScene>(output.intent.operations.back());
                    bad.directional_shadow = feature::RenderTextureReference{feature::RenderResourceId(kShadow.value()), 1, true};
                    bad.scene.id = feature::RenderResourceId("Rejected.Lighting.Scene");
                    auto rejected_scene = std::make_shared<renderer::RenderSceneSnapshot>(*snapshot);
                    rejected_scene->view.id = renderer::RenderViewId("Rejected.Lighting.View");
                    rejected_scene->lights.front().casts_shadow = shadowed;
                    const auto shadow_view = renderer::make_directional_shadow_view(*rejected_scene);
                    if (!shadow_view || shadow_view.value.enabled != shadowed) return 75;
                    auto invalid_scenes = scenes;
                    invalid_scenes.push_back({bad.scene.id, 1, {}, rejected_scene, &resolver});
                    const renderer::RenderFeatureDrawLowerer invalid_lowerer(invalid_scenes,
                        {&passes, pipeline::detail::RenderPassRegistryExecution::execution_epoch(passes)});
                    ObservedSceneLowerer observed(invalid_lowerer);
                    observed.contract_probe_scene = bad.scene.id;
                    resolver.deferred_failure = failure;
                    resolver.attachment_probe_preconditions = true;
                    resolver.attachment_probe_mutations = 0;
                    std::weak_ptr<const void> failed_lease = invalid_outputs;
                    const auto rejected = render_graph::build_render_graph_features(graph, invalid_outputs,
                        {.textures = textures, .draw_lowerer = &observed}, frame.frame_number());
                    resolver.deferred_failure = 0;
                    invalid_outputs.reset();
                    if (!check(graph.compile())) return 75;
                    const auto& after = aeg::render_graph::detail::RenderGraphExecution::compile_info(graph);
                    const bool rolled_back = after.pass_count == baseline.pass_count && after.resource_count == baseline.resource_count &&
                        after.culled_pass_count == baseline.culled_pass_count;
                    observe_contract_rejection(shadowed ? "deferred-shadow-profile-attachment" : "deferred-base-profile-attachment",
                        rejected, observed, resolver.attachment_probe_preconditions && resolver.attachment_probe_mutations == 1,
                        failed_lease.expired(), rolled_back, "attachment contract");
                    if (!rolled_back) return 75;
                }
                lighting_attachment_probes_observed = true;
            }
            if (gbuffer_mode && iteration == 0) {
                std::array<render_graph::RenderGraphTexture, 28> incompatible{};
                for (size_t index = 0; index < incompatible.size(); ++index) {
                    const size_t role = index / 7;
                    auto desc = role == 3 ? depth_desc : color_desc;
                    if (role == 2) desc.format = rhi::Format::kRgba8Unorm;
                    switch (index % 7) {
                        case 0: desc.format = role < 2 ? rhi::Format::kRgba8Unorm : rhi::Format::kRgba16Float;
                            if (role == 3) desc.capabilities = rhi::TextureCapability::kColorAttachment; break;
                        case 1: desc.capabilities = rhi::TextureCapability::kSampled; break;
                        case 2: ++desc.extent.width; break;
                        case 3: desc.mip_levels = 2; break;
                        case 4: desc.array_layers = 2; break;
                        case 5: desc.samples = rhi::SampleCount::kX4; break;
                        case 6: desc.dimension = rhi::TextureDimension::kTex3D; desc.extent.depth = 2; break;
                    }
                    const auto target = graph.create_texture(desc);
                    if (!target) return 45;
                    incompatible[index] = target.value;
                }
                const auto native = device.create_texture(color_desc);
                if (!native) return 45;
                aliased_native = native.value;
                const auto alias_first = graph.import_texture(aliased_native, {}, color_desc);
                const auto alias_second = graph.import_texture(aliased_native, {}, color_desc);
                if (!alias_first || !alias_second || alias_first.value == alias_second.value || !check(graph.compile())) return 45;
                const auto baseline = aeg::render_graph::detail::RenderGraphExecution::compile_info(graph);
                const std::array<std::array<size_t, 2>, 6> pairs{{{0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}}};
                for (size_t test_case = 0; test_case < 90; ++test_case) {
                    auto invalid_outputs = std::make_shared<std::vector<feature::RenderFeatureOutput>>(*outputs);
                    auto& output = invalid_outputs->back();
                    output.intent.operations.push_back(output.intent.operations.back());
                    auto& bad = std::get<feature::RenderDrawGBufferScene>(output.intent.operations.back());
                    const std::array targets{&bad.albedo.target, &bad.normal.target, &bad.material.target, &bad.depth.target};
                    auto bindings = textures;
                    auto invalid_scenes = scenes;
                    auto invalid_material = resolver.material;
                    if ((test_case >= 35 && test_case <= 40) || test_case >= 69) {
                        bad.scene.id = feature::RenderResourceId("Rejected.GBuffer.Material.Scene");
                        auto invalid_snapshot = std::make_shared<renderer::RenderSceneSnapshot>(*snapshot);
                        for (auto& instance : invalid_snapshot->instances)
                            instance.material = renderer::MaterialInstanceId("Rejected.GBuffer.Material");
                        invalid_scenes.push_back({bad.scene.id, 1, {}, invalid_snapshot, &resolver});
                    }
                    if (test_case < 28) {
                        const size_t role = test_case / 7;
                        targets[role]->id = feature::RenderResourceId("Invalid.GBuffer.Attachment");
                        bindings.push_back({pipeline::RenderResourceId("Invalid.GBuffer.Attachment"), incompatible[test_case]});
                    } else if (test_case < 34) {
                        const auto pair = pairs[test_case - 28];
                        *targets[pair[1]] = *targets[pair[0]];
                    } else if (test_case >= 52 && test_case < 56) {
                        targets[test_case - 52]->version = 2;
                    } else if (test_case >= 56 && test_case < 60) {
                        bindings[test_case - 56].owner = outputs->front().instance_id;
                    } else if (test_case >= 60 && test_case < 64) {
                        targets[test_case - 60]->id = feature::RenderResourceId("Missing.GBuffer.Attachment");
                    } else switch (test_case) {
                        case 34: bindings[1].texture = color.value; break;
                        case 35: invalid_material.gbuffer_pipeline = {}; break;
                        case 36: invalid_material.gbuffer_pipeline_contract.color_format = rhi::Format::kRgba8Unorm; break;
                        case 37: invalid_material.gbuffer_pipeline_contract.additional_color_formats[0] = rhi::Format::kRgba8Unorm; break;
                        case 38: invalid_material.gbuffer_pipeline_contract.additional_color_formats[1] = rhi::Format::kRgba16Float; break;
                        case 39: invalid_material.gbuffer_pipeline_contract.samples = rhi::SampleCount::kX4; break;
                        case 40: invalid_material.gbuffer_pipeline_contract.depth_stencil_format = rhi::Format::kUnknown; break;
                        case 41: break; // Missing lowerer below.
                        case 42: bad.scene.id = feature::RenderResourceId("Missing.GBuffer.Scene"); break;
                        case 43: bad.scene.version = 2; break;
                        case 44: invalid_scenes[0].owner = outputs->front().instance_id; break;
                        case 45: case 46: case 47: case 48: {
                            bad.scene.id = feature::RenderResourceId("Invalid.GBuffer.Scene");
                            auto invalid_snapshot = std::make_shared<renderer::RenderSceneSnapshot>(*snapshot);
                            if (test_case == 45) ++invalid_snapshot->frame_number;
                            if (test_case == 48) ++invalid_snapshot->view.output_extent.width;
                            invalid_scenes.push_back({bad.scene.id, 1, {}, test_case == 47 ? nullptr : invalid_snapshot,
                                test_case == 46 ? nullptr : &resolver});
                            break;
                        }
                        case 49: output.contract.permissions.capability_mask &= ~feature::to_mask(feature::RenderIntentCapability::kDrawScene); break;
                        case 50: output.contract.permissions.capability_mask &= ~feature::to_mask(feature::RenderIntentCapability::kWriteExternal); break;
                        case 51: output.contract.permissions.capability_mask &= ~feature::to_mask(feature::RenderIntentCapability::kReadExternal); break;
                        case 64: output.intent.operations.push_back(feature::RenderClearColor{bad.albedo.target, {0, 0, 0, 1}}); break;
                        case 65: output.intent.operations.push_back(feature::RenderCopyTexture{bad.albedo.target, bad.normal.target, kSize, kSize}); break;
                        case 66: output.intent.final_output = bad.albedo.target; break;
                        case 67: bad.depth.clear_depth = std::numeric_limits<float>::quiet_NaN(); break;
                        case 68:
                            bad.albedo.target.id = feature::RenderResourceId("Alias.GBuffer.Albedo");
                            bad.normal.target.id = feature::RenderResourceId("Alias.GBuffer.Normal");
                            bindings.push_back({pipeline::RenderResourceId("Alias.GBuffer.Albedo"), alias_first.value});
                            bindings.push_back({pipeline::RenderResourceId("Alias.GBuffer.Normal"), alias_second.value});
                            break;
                        case 69: invalid_material.gbuffer_profile_contract = {}; break;
                        case 70: invalid_material.gbuffer_profile_contract.shader.profile = renderer::RendererProfile::kShadow; break;
                        case 71: invalid_material.gbuffer_profile_contract.shader.version = 0; break;
                        case 72: ++invalid_material.gbuffer_profile_contract.shader.version; break;
                        case 73: invalid_material.gbuffer_profile_contract.shader.layout_hash = 0; break;
                        case 74: invalid_material.gbuffer_profile_contract.shader.cache_epoch = 0; break;
                        case 75: invalid_material.gbuffer_profile_contract.cache_epoch = 0; break;
                        case 76: ++invalid_material.gbuffer_profile_contract.cache_epoch; break;
                        case 77:
                            ++invalid_material.gbuffer_profile_contract.shader.cache_epoch;
                            ++invalid_material.gbuffer_profile_contract.cache_epoch;
                            break;
                        case 78: invalid_material.variant_cache_epoch = 0; break;
                        case 79: invalid_material.gbuffer_profile_contract.attachment.color_format = rhi::Format::kRgba8Unorm; break;
                        case 80: invalid_material.gbuffer_profile_contract.attachment.additional_color_formats[0] = rhi::Format::kRgba8Unorm; break;
                        case 81: invalid_material.gbuffer_profile_contract.attachment.additional_color_formats[1] = rhi::Format::kRgba16Float; break;
                        case 82: invalid_material.gbuffer_profile_contract.attachment.samples = rhi::SampleCount::kX4; break;
                        case 83: invalid_material.gbuffer_profile_contract.attachment.depth_stencil_format = rhi::Format::kUnknown; break;
                        case 84: std::swap(invalid_material.gbuffer_profile_contract.attachment.additional_color_formats[0],
                                          invalid_material.gbuffer_profile_contract.attachment.additional_color_formats[1]); break;
                        case 85:
                            std::swap(invalid_material.gbuffer_profile_contract.attachment.additional_color_formats[0],
                                      invalid_material.gbuffer_profile_contract.attachment.additional_color_formats[1]);
                            invalid_material.gbuffer_pipeline_contract = invalid_material.gbuffer_profile_contract.attachment;
                            break;
                        case 86: invalid_material.gbuffer_profile_contract.attachment.additional_color_formats[0] = rhi::Format::kUnknown; break;
                        case 87: invalid_material.gbuffer_profile_contract.attachment.additional_color_formats[1] = rhi::Format::kUnknown; break;
                        case 88: invalid_material.gbuffer_profile_contract.attachment.color_format = rhi::Format::kUnknown; break;
                        case 89: invalid_material.gbuffer_profile_contract.attachment.depth_stencil_format = rhi::Format::kRgba16Float; break;
                    }
                    const renderer::RenderFeatureDrawLowerer invalid_lowerer(invalid_scenes, {&passes, pipeline::detail::RenderPassRegistryExecution::execution_epoch(passes)});
                    resolver.rejected_material = invalid_material;
                    std::weak_ptr<const void> failed_lease = invalid_outputs;
                    const auto rejected = render_graph::build_render_graph_features(graph, invalid_outputs,
                        {.textures = bindings, .draw_lowerer = test_case == 41 ? nullptr : &invalid_lowerer}, frame.frame_number());
                    resolver.rejected_material.reset();
                    invalid_outputs.reset();
                    if (rejected || !failed_lease.expired() || !check(graph.compile())) {
                        std::cerr << "Invalid GBuffer accepted or leaked: " << test_case << '\n';
                        return 45;
                    }
                    const auto after = aeg::render_graph::detail::RenderGraphExecution::compile_info(graph);
                    if (after.pass_count != baseline.pass_count || after.resource_count != baseline.resource_count ||
                        after.culled_pass_count != baseline.culled_pass_count || after.batch_count != baseline.batch_count ||
                        after.transient_buffer_slot_count != baseline.transient_buffer_slot_count ||
                        after.transient_texture_slot_count != baseline.transient_texture_slot_count ||
                        after.barrier_count != baseline.barrier_count || after.barrier_point_count != baseline.barrier_point_count ||
                        after.queue_dependency_count != baseline.queue_dependency_count) {
                        std::cerr << "GBuffer rejection leaked native graph work: " << test_case << '\n';
                        return 45;
                    }
                }
            }
            if (shadow_feature_mode && iteration == 1) {
                std::array<render_graph::RenderGraphTexture, 7> incompatible{};
                for (uint32_t index = 0; index < incompatible.size(); ++index) {
                    auto desc = depth_desc;
                    switch (index) {
                        case 0: desc.format = rhi::Format::kRgba8Unorm;
                            desc.capabilities = rhi::TextureCapability::kColorAttachment | rhi::TextureCapability::kSampled; break;
                        case 1: desc.capabilities = rhi::TextureCapability::kDepthStencil; break;
                        case 2: ++desc.extent.width; break;
                        case 3: desc.mip_levels = 2; break;
                        case 4: desc.array_layers = 2; break;
                        case 5: desc.samples = rhi::SampleCount::kX4; break;
                        case 6: desc.dimension = rhi::TextureDimension::kTex3D; desc.extent.depth = 2; break;
                    }
                    const auto target = graph.create_texture(desc);
                    if (!target) return 37;
                    incompatible[index] = target.value;
                }
                if (!check(graph.compile())) return 37;
                const auto baseline = aeg::render_graph::detail::RenderGraphExecution::compile_info(graph);
                for (uint32_t test_case = 0; test_case != 40; ++test_case) {
                    auto invalid_outputs = std::make_shared<std::vector<feature::RenderFeatureOutput>>(*outputs);
                    auto& output = invalid_outputs->back();
                    auto draw = std::get<feature::RenderDrawScene>(output.intent.operations.back());
                    output.intent.operations.push_back(draw); // Valid producer and consumers precede the rejected operation.
                    auto& bad_draw = std::get<feature::RenderDrawScene>(output.intent.operations.back());
                    auto bindings = textures;
                    auto invalid_scenes = scenes;
                    const auto saved_material = resolver.material;
                    auto invalid_material = resolver.material;
                    if (test_case >= 26) {
                        bad_draw.scene.id = feature::RenderResourceId("Rejected.Shadow.Material.Scene");
                        auto invalid_snapshot = std::make_shared<renderer::RenderSceneSnapshot>(*snapshot);
                        for (auto& instance : invalid_snapshot->instances)
                            instance.material = renderer::MaterialInstanceId("Rejected.GBuffer.Material");
                        invalid_scenes.push_back({bad_draw.scene.id, 1, {}, invalid_snapshot, &resolver});
                    }
                    switch (test_case) {
                        case 0: output.contract.permissions.capability_mask &= ~feature::to_mask(feature::RenderIntentCapability::kReadExternal); break;
                        case 1: bad_draw.directional_shadow->external = false; break;
                        case 2: bad_draw.directional_shadow->id = {}; break;
                        case 3: bad_draw.directional_shadow->version = 0; break;
                        case 4: bad_draw.directional_shadow->version = 2; break;
                        case 5: bad_draw.directional_shadow->id = feature::RenderResourceId("Missing.Shadow"); break;
                        case 6: bindings[2].owner = outputs->front().instance_id; break;
                        case 7: bad_draw.directional_shadow = bad_draw.depth->target; break;
                        case 8: bindings[2].texture = second_depth.value; break;
                        case 9: bindings[2].texture = second_color.value; break;
                        case 10: resolver.material.shadow_pipeline = {}; break;
                        case 11: resolver.material.shadow_pipeline_contract.color_format = rhi::Format::kBgra8Unorm; break;
                        case 12: resolver.material.shadow_pipeline_contract.samples = rhi::SampleCount::kX4; break;
                        case 13: resolver.material.shadow_pipeline_contract.depth_stencil_format = rhi::Format::kUnknown; break;
                        case 14: resolver.shadow_failure = 1; break;
                        case 15:
                            bad_draw.scene.id = feature::RenderResourceId("Invalid.Consumer.Scene");
                            invalid_scenes.push_back({bad_draw.scene.id, 1, {}, snapshot, nullptr});
                            break;
                        case 23: resolver.shadow_failure = 2; break;
                        case 24: resolver.shadow_failure = 3; break;
                        case 25: resolver.shadow_failure = 4; break;
                        case 26: invalid_material.shadow_shader_contract = {}; break;
                        case 27: invalid_material.shadow_shader_contract.version = 0; break;
                        case 28: ++invalid_material.shadow_shader_contract.version; break;
                        case 29: invalid_material.shadow_shader_contract.layout_hash = 0; break;
                        case 30: invalid_material.shadow_shader_contract.cache_epoch = 0; break;
                        case 31: invalid_material.shadow_pipeline_cache_epoch = 0; break;
                        case 32: ++invalid_material.shadow_pipeline_cache_epoch; break;
                        case 33: invalid_material.variant_cache_epoch = 0; break;
                        case 34: invalid_material.shadow_pipeline_contract.additional_color_formats[0] = rhi::Format::kRgba8Unorm; break;
                        case 35: invalid_material.shadow_pipeline_contract.additional_color_formats[1] = rhi::Format::kRgba8Unorm; break;
                        case 36: invalid_material.shadow_pipeline_contract.color_format = rhi::Format::kUnknown; break;
                        case 37: invalid_material.shadow_pipeline_contract.depth_stencil_format = rhi::Format::kRgba16Float; break;
                        case 38: invalid_material.shadow_pipeline_contract.samples = static_cast<rhi::SampleCount>(0); break;
                        case 39: invalid_material.shader_contract = {}; break;
                        default:
                            bad_draw.directional_shadow->id = feature::RenderResourceId("Invalid.Shadow");
                            break;
                    }
                    std::vector<render_graph::RenderGraphFeatureTextureBinding> invalid_bindings(bindings.begin(), bindings.end());
                    if (test_case >= 16 && test_case <= 22)
                        invalid_bindings.push_back({pipeline::RenderResourceId("Invalid.Shadow"), incompatible[test_case - 16]});
                    const renderer::RenderFeatureDrawLowerer invalid_lowerer(invalid_scenes, {&passes, pipeline::detail::RenderPassRegistryExecution::execution_epoch(passes)});
                    if (test_case >= 26) resolver.rejected_material = invalid_material;
                    std::weak_ptr<const void> failed_lease = invalid_outputs;
                    const auto rejected = render_graph::build_render_graph_features(graph, invalid_outputs,
                        {.textures = invalid_bindings, .draw_lowerer = &invalid_lowerer}, frame.frame_number());
                    resolver.material = saved_material;
                    resolver.rejected_material.reset();
                    resolver.shadow_failure = 0;
                    invalid_outputs.reset();
                    if (rejected || !failed_lease.expired() || !check(graph.compile())) {
                        std::cerr << "Invalid shadow consumer accepted or leaked: " << test_case << '\n';
                        return 37;
                    }
                    const auto after = aeg::render_graph::detail::RenderGraphExecution::compile_info(graph);
                    if (after.pass_count != baseline.pass_count || after.resource_count != baseline.resource_count ||
                        after.culled_pass_count != baseline.culled_pass_count || after.batch_count != baseline.batch_count ||
                        after.transient_buffer_slot_count != baseline.transient_buffer_slot_count ||
                        after.transient_texture_slot_count != baseline.transient_texture_slot_count ||
                        after.barrier_count != baseline.barrier_count || after.barrier_point_count != baseline.barrier_point_count ||
                        after.queue_dependency_count != baseline.queue_dependency_count) {
                        std::cerr << "Shadow material rejection leaked native graph work: " << test_case << '\n';
                        return 37;
                    }
                }
                // Each bad depth operation follows genuine shadow production and its
                // color consumers. Rejection must undo that complete native graph work.
                for (const auto projection : {feature::RenderDepthSceneProjection::kCamera,
                                             feature::RenderDepthSceneProjection::kDirectionalShadow}) {
                    for (uint32_t test_case = 0; test_case != 19; ++test_case) {
                        auto invalid_outputs = std::make_shared<std::vector<feature::RenderFeatureOutput>>(*outputs);
                        auto& output = invalid_outputs->back();
                        feature::RenderDrawDepthScene bad{
                            {feature::RenderResourceId("Rejected.Depth.Profile.Scene"), 1},
                            {{feature::RenderResourceId(projection == feature::RenderDepthSceneProjection::kCamera
                                ? kDepth.value() : kShadow.value()), 1, true}, feature::RenderAttachmentLoad::kClear, 1.f}};
                        bad.projection = projection;
                        output.intent.operations.push_back(bad);
                        auto invalid_snapshot = std::make_shared<renderer::RenderSceneSnapshot>(*snapshot);
                        for (auto& instance : invalid_snapshot->instances)
                            instance.material = renderer::MaterialInstanceId("Rejected.GBuffer.Material");
                        for (auto& caster : invalid_snapshot->shadow_casters)
                            caster.material = renderer::MaterialInstanceId("Rejected.GBuffer.Material");
                        auto invalid_scenes = scenes;
                        invalid_scenes.push_back({bad.scene.id, 1, {}, invalid_snapshot, &resolver});
                        auto invalid_material = resolver.material;
                        switch (test_case) {
                            case 0: invalid_material.depth_profile_contract = {}; break;
                            case 1: invalid_material.depth_profile_contract.shader.profile = renderer::RendererProfile::kForwardClustered; break;
                            case 2: invalid_material.depth_profile_contract.shader.version = 0; break;
                            case 3: ++invalid_material.depth_profile_contract.shader.version; break;
                            case 4: invalid_material.depth_profile_contract.shader.layout_hash = 0; break;
                            case 5: invalid_material.depth_profile_contract.shader.cache_epoch = 0; break;
                            case 6: invalid_material.depth_profile_contract.cache_epoch = 0; break;
                            case 7: ++invalid_material.depth_profile_contract.cache_epoch; break;
                            case 8: invalid_material.depth_profile_contract.attachment.color_format = rhi::Format::kRgba8Unorm; break;
                            case 9: invalid_material.depth_profile_contract.attachment.samples = rhi::SampleCount::kX4; break;
                            case 10: invalid_material.depth_profile_contract.attachment.depth_stencil_format = rhi::Format::kUnknown; break;
                            case 11: invalid_material.depth_profile_contract.attachment.additional_color_formats[0] = rhi::Format::kRgba8Unorm; break;
                            case 12: invalid_material.depth_profile_contract.attachment.additional_color_formats[1] = rhi::Format::kRgba8Unorm; break;
                            case 13: invalid_material.depth_pipeline_contract.color_format = rhi::Format::kRgba8Unorm; break;
                            case 14: invalid_material.depth_pipeline_contract.samples = rhi::SampleCount::kX4; break;
                            case 15: invalid_material.depth_pipeline_contract.depth_stencil_format = rhi::Format::kUnknown; break;
                            case 16: invalid_material.depth_pipeline_contract.additional_color_formats[0] = rhi::Format::kRgba8Unorm; break;
                            case 17: invalid_material.depth_pipeline_contract.additional_color_formats[1] = rhi::Format::kRgba8Unorm; break;
                            case 18:
                                ++invalid_material.depth_profile_contract.shader.cache_epoch;
                                ++invalid_material.depth_profile_contract.cache_epoch;
                                break;
                        }
                        resolver.rejected_material = invalid_material;
                        const renderer::RenderFeatureDrawLowerer invalid_lowerer(invalid_scenes, {&passes, pipeline::detail::RenderPassRegistryExecution::execution_epoch(passes)});
                        std::weak_ptr<const void> failed_lease = invalid_outputs;
                        const auto rejected = render_graph::build_render_graph_features(graph, invalid_outputs,
                            {.textures = textures, .draw_lowerer = &invalid_lowerer}, frame.frame_number());
                        resolver.rejected_material.reset();
                        invalid_outputs.reset();
                        if (rejected || rejected.code != foundation::ResultCode::kInvalidArgument ||
                            !failed_lease.expired() || !check(graph.compile())) {
                            std::cerr << "Invalid depth profile accepted or retained output: projection="
                                      << static_cast<unsigned>(projection) << " case=" << test_case << '\n';
                            return 70;
                        }
                        const auto after = aeg::render_graph::detail::RenderGraphExecution::compile_info(graph);
                        if (after.pass_count != baseline.pass_count || after.resource_count != baseline.resource_count ||
                            after.culled_pass_count != baseline.culled_pass_count || after.batch_count != baseline.batch_count ||
                            after.transient_buffer_slot_count != baseline.transient_buffer_slot_count ||
                            after.transient_texture_slot_count != baseline.transient_texture_slot_count ||
                            after.barrier_count != baseline.barrier_count || after.barrier_point_count != baseline.barrier_point_count ||
                            after.queue_dependency_count != baseline.queue_dependency_count) {
                            std::cerr << "Depth profile rejection leaked native graph work: projection="
                                      << static_cast<unsigned>(projection) << " case=" << test_case << '\n';
                            return 70;
                        }
                    }
                }
            }
            if (post_feature_mode && iteration == 0) {
                std::array<render_graph::RenderGraphTexture, 21> incompatible{};
                for (size_t index = 0; index < incompatible.size(); ++index) {
                    const size_t role = index / 7;
                    auto desc = role == 0 ? color_desc : role == 1 ? depth_desc : output_desc;
                    switch (index % 7) {
                    case 0: desc.format = role == 2 ? rhi::Format::kRgba16Float : rhi::Format::kRgba8Unorm; break;
                    case 1: desc.capabilities = role == 0 ? rhi::TextureCapability::kColorAttachment
                        : role == 1 ? rhi::TextureCapability::kDepthStencil : rhi::TextureCapability::kSampled; break;
                    case 2: ++desc.extent.width; break;
                    case 3: desc.mip_levels = 2; break;
                    case 4: desc.array_layers = 2; break;
                    case 5: desc.samples = rhi::SampleCount::kX4; break;
                    case 6: desc.dimension = rhi::TextureDimension::kTex3D; desc.extent.depth = 2; break;
                    }
                    const auto texture = graph.create_texture(desc);
                    if (!texture) return 57;
                    incompatible[index] = texture.value;
                }
                if (!check(graph.compile())) return 57;
                const auto baseline = aeg::render_graph::detail::RenderGraphExecution::compile_info(graph);
                for (size_t test_case = 0; test_case < 55; ++test_case) {
                    auto invalid_outputs = std::make_shared<std::vector<feature::RenderFeatureOutput>>(*outputs);
                    auto& output = *std::find_if(invalid_outputs->begin(), invalid_outputs->end(), [](const auto& item) {
                        return item.node_id == feature::FeatureNodeId(4);
                    });
                    output.intent.operations.push_back(output.intent.operations.back());
                    auto& bad = std::get<feature::RenderPostProcessScene>(output.intent.operations.back());
                    const std::array refs{&bad.hdr, &bad.depth, &bad.output.target};
                    auto bindings = textures;
                    auto invalid_scenes = scenes;
                    const auto original_scene = bad.scene;
                    auto rejected_scene = std::make_shared<renderer::RenderSceneSnapshot>(*second_snapshot);
                    rejected_scene->view.id = renderer::RenderViewId("Rejected.Post.View");
                    bad.scene.id = feature::RenderResourceId("Rejected.Post.Scene");
                    invalid_scenes.push_back({bad.scene.id, 1, {}, rejected_scene, &resolver});
                    resolver.post_failure = 0;
                    if (test_case < 21) {
                        refs[test_case / 7]->id = feature::RenderResourceId("Invalid.Post.Texture");
                        bindings.push_back({pipeline::RenderResourceId("Invalid.Post.Texture"), incompatible[test_case]});
                    } else if (test_case < 24) {
                        const std::array pairs{std::array{0u, 1u}, std::array{0u, 2u}, std::array{1u, 2u}};
                        const auto pair = pairs[test_case - 21];
                        *refs[pair[0]] = *refs[pair[1]];
                    } else if (test_case < 35) resolver.post_failure = static_cast<uint8_t>(test_case - 23);
                    else if (test_case < 38) refs[test_case - 35]->version = 2;
                    else if (test_case < 41) refs[test_case - 38]->id = feature::RenderResourceId("Missing.Post.Texture");
                    else switch (test_case) {
                        case 41: bad.scene.version = 2; break;
                        case 42: invalid_scenes.back().owner = outputs->front().instance_id; break;
                        case 43: rejected_scene->frame_number += 1; break;
                        case 44: invalid_scenes.back().snapshot.reset(); break;
                        case 45: invalid_scenes.back().resolver = nullptr; break;
                        case 46: invalid_scenes.push_back(invalid_scenes.back()); break;
                        case 47: bad.scene.id = feature::RenderResourceId("Missing.Post.Scene"); break;
                        case 48: bindings[6].owner = std::find_if(outputs->begin(), outputs->end(), [](const auto& item) {
                            return item.node_id == feature::FeatureNodeId(2);
                        })->instance_id; break; // Lighting producer may write; foreign PP consumer must fail.
                        case 49: bindings[6].texture = depth.value; break; // Distinct logical IDs, same graph resource.
                        case 50: bad.settings->exposure = std::numeric_limits<float>::infinity(); break;
                        case 51: bad.settings->gamma = -1.f; break;
                        case 52: output.contract.permissions.capability_mask &= ~feature::to_mask(feature::RenderIntentCapability::kReadExternal); break;
                        case 53: rejected_scene->world = {}; break;
                        case 54: rejected_scene->view.output_extent.width += 1; break;
                    }
                    // Only the final operation has rejected View/service metadata; prior PP uses the real scene.
                    if (bad.scene.id == original_scene.id) return 57;
                    const renderer::RenderFeatureDrawLowerer invalid_lowerer(invalid_scenes, {&passes, pipeline::detail::RenderPassRegistryExecution::execution_epoch(passes)});
                    std::weak_ptr<const std::vector<feature::RenderFeatureOutput>> failed_lease = invalid_outputs;
                    const auto rejected = render_graph::build_render_graph_features(graph, invalid_outputs,
                        {.textures = bindings, .draw_lowerer = &invalid_lowerer}, frame.frame_number());
                    resolver.post_failure = 0;
                    invalid_outputs.reset();
                    if (rejected || !failed_lease.expired() || !check(graph.compile()) ||
                        aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).pass_count != baseline.pass_count ||
                        aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).resource_count != baseline.resource_count ||
                        aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).culled_pass_count != baseline.culled_pass_count) {
                        std::cerr << "Invalid PostProcess accepted or leaked: " << test_case << '\n';
                        return 57;
                    }
                }
                if (!post_attachment_probe_observed) {
                    auto invalid_outputs = std::make_shared<std::vector<feature::RenderFeatureOutput>>(*outputs);
                    auto& output = *std::find_if(invalid_outputs->begin(), invalid_outputs->end(), [](const auto& item) {
                        return item.node_id == feature::FeatureNodeId(4);
                    });
                    output.intent.operations.push_back(output.intent.operations.back());
                    auto& bad = std::get<feature::RenderPostProcessScene>(output.intent.operations.back());
                    auto rejected_scene = std::make_shared<renderer::RenderSceneSnapshot>(*second_snapshot);
                    rejected_scene->view.id = renderer::RenderViewId("Rejected.Post.View");
                    bad.scene.id = feature::RenderResourceId("Rejected.Post.Scene");
                    auto invalid_scenes = scenes;
                    invalid_scenes.push_back({bad.scene.id, 1, {}, rejected_scene, &resolver});
                    const renderer::RenderFeatureDrawLowerer invalid_lowerer(invalid_scenes,
                        {&passes, pipeline::detail::RenderPassRegistryExecution::execution_epoch(passes)});
                    ObservedSceneLowerer observed(invalid_lowerer);
                    observed.contract_probe_scene = bad.scene.id;
                    resolver.post_failure = 12;
                    resolver.attachment_probe_preconditions = true;
                    resolver.attachment_probe_mutations = 0;
                    std::weak_ptr<const void> failed_lease = invalid_outputs;
                    const auto rejected = render_graph::build_render_graph_features(graph, invalid_outputs,
                        {.textures = textures, .draw_lowerer = &observed}, frame.frame_number());
                    resolver.post_failure = 0;
                    invalid_outputs.reset();
                    if (!check(graph.compile())) return 75;
                    const auto& after = aeg::render_graph::detail::RenderGraphExecution::compile_info(graph);
                    const bool rolled_back = after.pass_count == baseline.pass_count && after.resource_count == baseline.resource_count &&
                        after.culled_pass_count == baseline.culled_pass_count;
                    observe_contract_rejection("post-process-profile-attachment", rejected, observed,
                        resolver.attachment_probe_preconditions && resolver.attachment_probe_mutations == 1,
                        failed_lease.expired(), rolled_back, "attachment contract");
                    if (!rolled_back) return 75;
                    post_attachment_probe_observed = true;
                }
            }
            const renderer::RenderFeatureDrawLowerer lowerer(scenes, {&passes, pipeline::detail::RenderPassRegistryExecution::execution_epoch(passes)});
            resolver.post_resources.clear();
            if (shadow_feature_mode && (iteration == 0 || iteration == 3)) {
                // The real color draw must ignore an unusable, unselected shadow variant.
                resolver.material.shadow_shader_contract = {};
                resolver.material.shadow_pipeline_cache_epoch = 0;
                resolver.material.shadow_pipeline_contract = {rhi::Format::kUnknown, rhi::SampleCount::kX4,
                    rhi::Format::kRgba16Float, {rhi::Format::kRgba8Unorm, rhi::Format::kRgba8Unorm}};
            }
            native_observation.records.clear();
            native_observation.expected = outputs.get();
            if (!check(render_graph::build_render_graph_features(graph, outputs,
                {.textures = textures, .draw_lowerer = &lowerer}, frame.frame_number())) || !check(graph.compile())) return 38;
            std::vector<bool> live(outputs->size());
            std::vector<std::array<uint32_t, 2>> operation_counts(outputs->size());
            for (const auto& pass : aeg::render_graph::detail::RenderGraphExecution::plan(graph).pass_plans) {
                if (!pass.feature_work) continue;
                const auto& work = *pass.feature_work;
                const auto source = std::find_if(outputs->begin(), outputs->end(), [&](const auto& output) { return work.instance_id == output.instance_id; });
                if (source == outputs->end() || work.feature_definition_id != source->contract.definition_id ||
                    work.owner_module != source->owner_module || work.node_id != source->node_id ||
                    work.invocation.invocation_id != source->invocation.invocation_id ||
                    work.invocation.generation != source->invocation.generation ||
                    work.operation_index >= source->intent.operations.size() || work.frame_index != frame.frame_number()) {
                    std::cerr << "Feature provenance mismatch frame=" << iteration << '\n';
                    return 39;
                }
                if (gbuffer_mode) {
                    if (work.kind != render_graph::RenderWorkItemKind::kExecute || work.operation_index >= 2) return 39;
                    ++operation_counts[static_cast<size_t>(source - outputs->begin())][work.operation_index];
                }
                if (post_feature_mode && source->contract.definition_id == post_consumer.id) {
                    if (work.kind != render_graph::RenderWorkItemKind::kExecute || work.operation_index >= 2) return 39;
                    ++operation_counts[static_cast<size_t>(source - outputs->begin())][work.operation_index];
                }
                live[static_cast<size_t>(source - outputs->begin())] = live[static_cast<size_t>(source - outputs->begin())] || pass.live;
            }
            if (post_feature_mode) {
                if (resolver.post_resources.size() != 3) return 56;
                const auto resource_count = aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).resource_count;
                std::array<render_graph::RenderGraphBuffer, 3> constants{};
                for (size_t index = 0; index < constants.size(); ++index) {
                    const auto& resource = resolver.post_resources[index];
                    const auto imported = graph.import_buffer(resource.constants, resource.constants_desc);
                    const auto repeated = graph.import_buffer(resource.constants, resource.constants_desc);
                    if (!imported || !repeated || imported.value != repeated.value) return 56;
                    constants[index] = imported.value;
                    for (size_t previous = 0; previous < index; ++previous)
                        if (constants[previous] == constants[index]) return 56;
                }
                if (!check(graph.compile()) || aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).resource_count != resource_count) return 56;
                for (size_t index = 0; index < outputs->size(); ++index)
                    if ((*outputs)[index].contract.definition_id == post_consumer.id)
                        for (size_t operation = 0; operation < operation_counts[index].size(); ++operation)
                            if (operation_counts[index][operation] != (operation < (*outputs)[index].intent.operations.size() ? 1u : 0u)) return 39;
            }
            if (gbuffer_mode) {
                for (size_t index = 0; index < outputs->size(); ++index)
                    for (size_t operation = 0; operation < operation_counts[index].size(); ++operation)
                        if (operation_counts[index][operation] != (operation < (*outputs)[index].intent.operations.size() ? 1u : 0u)) return 39;
                if (std::any_of(live.begin(), live.end(), [](bool value) { return value; })) return 39;
            } else if (lighting_mode) {
                // Persistent lighting uploads are side effects; the actual fullscreen/MRT
                // Graphics work must still be culled until an HDR consumer is added.
                for (const auto& pass : aeg::render_graph::detail::RenderGraphExecution::plan(graph).pass_plans)
                    if (pass.feature_work && pass.live &&
                        (pass.batch_index >= aeg::render_graph::detail::RenderGraphExecution::plan(graph).batches.size() ||
                         aeg::render_graph::detail::RenderGraphExecution::plan(graph).batches[pass.batch_index].queue != rhi::QueueClass::kCopy)) return 39;
            } else if (!live[1] || !live[2] || ((iteration == 1 || iteration == 2) && !live[0])) return 39;
        } else {
            if (!check(feature_runtime.tick(renderer::kSceneRenderProfileClock))) return 74;
            auto captured = feature_runtime.capture_render_outputs(feature_plan);
            if (!captured || captured.value.size() != 1 ||
                !check(scope_current_scene_capture(captured.value, feature_runtime, snapshot->view.id))) return 74;
            outputs = std::make_shared<const std::vector<feature::RenderFeatureOutput>>(std::move(captured.value));
            output_lease = outputs;
            std::vector<render_graph::RenderGraphFeatureTextureBinding> textures{
                {feature::RenderResourceId("Renderer.SceneColor"), color.value},
                {feature::RenderResourceId("Renderer.SceneDepth"), depth.value}};
            const auto add_target = [&](const char* name, rhi::Format format, rhi::TextureCapability capabilities) {
                auto description = color_desc;
                description.format = format;
                description.capabilities = capabilities;
                description.name = rhi::DebugName{name};
                const auto target = graph.create_texture(description);
                if (!target) return target.result;
                textures.push_back({feature::RenderResourceId(name), target.value});
                return foundation::Result::success();
            };
            if (!check(add_target("Renderer.ShadowDepth", rhi::Format::kD32Float,
                    rhi::TextureCapability::kDepthStencil | rhi::TextureCapability::kSampled)) ||
                !check(add_target("Renderer.SceneLighting", renderer::kSceneLightingFormat,
                    rhi::TextureCapability::kColorAttachment | rhi::TextureCapability::kSampled))) return 74;
            if (deferred_mode) {
                for (const auto name : {"Renderer.GBuffer.Albedo", "Renderer.GBuffer.Normal", "Renderer.GBuffer.Material"}) {
                    const auto format = std::string_view(name) == "Renderer.GBuffer.Material"
                        ? rhi::Format::kRgba8Unorm : rhi::Format::kRgba16Float;
                    if (!check(add_target(name, format,
                        rhi::TextureCapability::kColorAttachment | rhi::TextureCapability::kSampled))) return 74;
                }
            }
            const renderer::RenderFeatureDrawLowerer lowerer(scenes,
                {&passes, pipeline::detail::RenderPassRegistryExecution::execution_epoch(passes)});
            native_observation.expected = outputs.get();
            if (!check(render_graph::build_render_graph_features(graph, outputs,
                {.textures = textures, .draw_lowerer = &lowerer, .current_frame_owner = &frame}, frame.frame_number()))) return 74;
        }
        const auto add_readback = [&](render_graph::RenderGraphTexture source, uint32_t bytes_per_pixel = 4,
            rhi::Aspect aspect = rhi::Aspect::kColor) -> foundation::ResultValue<render_graph::FrameAllocation> {
            const uint64_t byte_count = kSize * kSize * bytes_per_pixel;
            const auto allocation = frame.allocate_readback(byte_count, 256);
            if (!allocation) return allocation;
            rhi::BufferDesc desc{};
            desc.size = allocation.value.buffer_size;
            desc.capabilities = rhi::BufferCapability::kTransferDestination;
            desc.memory = rhi::MemoryClass::kReadback;
            desc.initial_usage = rhi::ResourceUsage::kHostRead;
            desc.initial_stages = rhi::PipelineStage::kHost;
            desc.initial_queue = rhi::QueueClass::kCopy;
            const auto destination = graph.import_buffer(allocation.value.buffer, desc);
            if (!destination) return {destination.result, {}};
            render_graph::RenderGraphPass prepared{};
            if (aspect == rhi::Aspect::kDepth) {
                // The frame allocator initializes host buffers on Copy. Establish GPU ownership there
                // before the depth copy, which requires Graphics without maintenance10.
                const auto upload = frame.allocate_upload(byte_count, 256);
                if (!upload) return {upload.result, {}};
                const std::vector<std::byte> zeros(static_cast<size_t>(byte_count));
                if (const auto result = frame.write_upload(upload.value, zeros); !result) return {result, {}};
                auto upload_description = desc;
                upload_description.size = upload.value.buffer_size;
                upload_description.capabilities = rhi::BufferCapability::kTransferSource;
                upload_description.memory = rhi::MemoryClass::kUpload;
                upload_description.initial_usage = rhi::ResourceUsage::kHostWrite;
                const auto staged = graph.import_buffer(upload.value.buffer, upload_description);
                if (!staged) return {staged.result, {}};
                const auto prepare = graph.add_pass("Initialize depth readback", rhi::QueueClass::kCopy,
                    [staged = staged.value, destination = destination.value, source_offset = upload.value.offset,
                        offset = allocation.value.offset, byte_count](render_graph::PassBuilder& pass) {
                        if (const auto result = pass.read(staged, rhi::ResourceUsage::kCopySource, rhi::PipelineStage::kCopy,
                            rhi::ShaderStage::kAll, {source_offset, byte_count}); !result) return result;
                        return pass.write(destination, rhi::ResourceUsage::kCopyDestination, rhi::PipelineStage::kCopy,
                            rhi::ShaderStage::kAll, {offset, byte_count});
                    },
                    [staged = staged.value, destination = destination.value, source_offset = upload.value.offset,
                        offset = allocation.value.offset, byte_count](render_graph::RenderPassCommandContext& command) {
                        return command.copy_buffer(staged, destination, source_offset, offset, byte_count);
                    });
                if (!prepare) return {prepare.result, {}};
                prepared = prepare.value;
            }
            const auto copy = graph.add_pass("Attachment pixels",
                aspect == rhi::Aspect::kDepth ? rhi::QueueClass::kGraphics : rhi::QueueClass::kCopy,
                [source, destination = destination.value, aspect, offset = allocation.value.offset, byte_count](render_graph::PassBuilder& pass) {
                    if (const auto result = pass.read(source, rhi::ResourceUsage::kCopySource, rhi::PipelineStage::kCopy,
                        rhi::ShaderStage::kAll, {aspect, 0, 1, 0, 1}); !result) return result;
                    pass.set_side_effects();
                    return pass.write(destination, rhi::ResourceUsage::kCopyDestination, rhi::PipelineStage::kCopy,
                        rhi::ShaderStage::kAll, {offset, byte_count});
                },
                [source, destination = destination.value, offset = allocation.value.offset, aspect](render_graph::RenderPassCommandContext& command) {
                    return command.copy_texture_to_buffer(source, {aspect, 0, 0, 1}, {},
                        {kSize, kSize, 1}, destination, offset, 0, 0);
                });
            if (copy && prepared.is_valid())
                if (const auto result = graph.add_dependency(prepared, copy.value); !result) return {result, {}};
            return {copy ? foundation::Result::success() : copy.result, allocation.value};
        };
        const auto readback = add_readback(post_feature_mode ? post_output.value : lighting_mode ? hdr.value : color.value,
            (gbuffer_path && !post_feature_mode) || clustered_hdr ? 8 : 4);
        if (!readback) return 19;
        const auto second_readback = post_feature_mode ? add_readback(second_post_output.value) : lighting_mode ? add_readback(second_hdr.value, 8)
            : shadow_feature_mode || clustered_hdr ? add_readback(second_color.value, clustered_hdr ? 8 : 4)
                : foundation::ResultValue<render_graph::FrameAllocation>{};
        std::array<render_graph::FrameAllocation, 4> mrt_readbacks{};
        if (gbuffer_mode) {
            const auto normals = add_readback(normal.value, 8);
            const auto materials = add_readback(material.value);
            const auto depths = add_readback(depth.value, 4, rhi::Aspect::kDepth);
            if (!normals || !materials || !depths) return 43;
            mrt_readbacks = {readback.value, normals.value, materials.value, depths.value};
        }
        if (!readback || ((shadow_feature_mode || lighting_mode || clustered_hdr) && !second_readback) || !check(graph.compile())) return 19;
        if (clustered_pair_mode) {
            std::array<uint32_t, 6> topology;
            topology.fill(std::numeric_limits<uint32_t>::max());
            for (const auto& pass : aeg::render_graph::detail::RenderGraphExecution::plan(graph).pass_plans) {
                if (!pass.live || !pass.feature_work) continue;
                const auto& work = *pass.feature_work;
                const auto owner = std::find(paired_instances.begin(), paired_instances.end(), work.instance_id);
                const auto source = std::find_if(outputs->begin(), outputs->end(), [&](const auto& item) {
                    return item.instance_id == work.instance_id;
                });
                if (owner == paired_instances.end() || source == outputs->end() ||
                    work.feature_definition_id != source->contract.definition_id || work.owner_module != source->owner_module ||
                    work.node_id != source->node_id || work.invocation != source->invocation || work.frame_index != frame.frame_number() ||
                    pass.batch_index >= aeg::render_graph::detail::RenderGraphExecution::plan(graph).batches.size() ||
                    work.operation_index >= source->intent.operations.size()) return 68;
                const auto expected_queue = work.kind == render_graph::RenderWorkItemKind::kExecute &&
                    std::holds_alternative<feature::RenderLightCullingScene>(source->intent.operations[work.operation_index])
                    ? rhi::QueueClass::kCompute : rhi::QueueClass::kGraphics;
                if (aeg::render_graph::detail::RenderGraphExecution::plan(graph).batches[pass.batch_index].queue != expected_queue) return 68;
                const auto index = static_cast<size_t>(owner - paired_instances.begin());
                if (work.operation_index >= (index < 2 ? 2u : 1u)) return 68;
                topology[index < 2 ? index * 2 + work.operation_index : index + 2] = pass.topological_index;
            }
            if (std::find(topology.begin(), topology.end(), std::numeric_limits<uint32_t>::max()) != topology.end()) return 68;
            paired_lifetimes_overlap = std::max(topology[1], topology[3]) < std::min(topology[4], topology[5]);
            if (iteration == 0 && !paired_lifetimes_overlap) return 68;
        }
        if (clustered_shadow_mode && (clustered_shadow_timing_mode || iteration == 1 || iteration == 2)) {
            std::array<bool, 5> live_work{};
            bool shadow_fragment_read = false;
            bool camera_compute_read = false;
            bool cluster_compute_write = false;
            for (const auto& pass : aeg::render_graph::detail::RenderGraphExecution::plan(graph).pass_plans) {
                if (!pass.live || !pass.feature_work) continue;
                const auto& work = *pass.feature_work;
                const auto source = std::find_if(outputs->begin(), outputs->end(), [&](const auto& item) {
                    return item.instance_id == work.instance_id;
                });
                if (source == outputs->end() || work.feature_definition_id != source->contract.definition_id ||
                    work.node_id != source->node_id || work.owner_module != source->owner_module ||
                    work.invocation != source->invocation || work.frame_index != frame.frame_number() ||
                    pass.batch_index >= aeg::render_graph::detail::RenderGraphExecution::plan(graph).batches.size()) {
                    std::cerr << "enabled shadow live work lost Feature provenance\n"; return 67;
                }
                const auto queue = aeg::render_graph::detail::RenderGraphExecution::plan(graph).batches[pass.batch_index].queue;
                if (source == outputs->begin() + 2 && work.operation_index < 3) {
                    const auto expected_queue = work.operation_index == 2 && work.kind == render_graph::RenderWorkItemKind::kExecute
                        ? rhi::QueueClass::kCompute : rhi::QueueClass::kGraphics;
                    if (queue != expected_queue) {
                        std::cerr << "enabled shadow producer queue, op=" << work.operation_index << '\n'; return 67;
                    }
                    live_work[work.operation_index] = true;
                    if (work.operation_index == 2) for (const auto& barrier : pass.barriers) {
                        if (barrier.phase != render_graph::RenderGraphBarrierPhase::kBeforePass ||
                            !rhi::has_flag(barrier.after.pipeline_stages, rhi::PipelineStage::kCompute)) continue;
                        if (barrier.resource_kind == render_graph::RenderGraphResourceKind::kTexture &&
                            barrier.resource_index == depth.value.index && barrier.resource_generation == depth.value.generation &&
                            barrier.after.usage == rhi::ResourceUsage::kSampledRead) camera_compute_read = true;
                        if (barrier.resource_kind == render_graph::RenderGraphResourceKind::kBuffer &&
                            barrier.after.usage == rhi::ResourceUsage::kStorageWrite) cluster_compute_write = true;
                    }
                } else if (source != outputs->begin() + 2 && work.operation_index == 0) {
                    if (queue != rhi::QueueClass::kGraphics) return 67;
                    live_work[3 + static_cast<size_t>(source - outputs->begin())] = true;
                    for (const auto& barrier : pass.barriers) {
                        if (barrier.resource_kind == render_graph::RenderGraphResourceKind::kTexture &&
                            barrier.resource_index == shadow.value.index && barrier.resource_generation == shadow.value.generation &&
                            barrier.phase == render_graph::RenderGraphBarrierPhase::kBeforePass &&
                            barrier.after.usage == rhi::ResourceUsage::kSampledRead &&
                            rhi::has_flag(barrier.after.pipeline_stages, rhi::PipelineStage::kFragmentShader)) shadow_fragment_read = true;
                    }
                }
            }
            if (!shadow_fragment_read || !camera_compute_read || !cluster_compute_write ||
                !std::all_of(live_work.begin(), live_work.end(), [](bool live) { return live; })) {
                std::cerr << "enabled shadow liveness, frame=" << iteration << " fragment=" << shadow_fragment_read
                    << " compute=" << camera_compute_read << cluster_compute_write << " work=";
                for (const auto live : live_work) std::cerr << live;
                std::cerr << '\n'; return 67;
            }
        }
        if (unshadowed_mode) {
            bool sentinel_culled = false;
            bool empty_cull_record = iteration != 5;
            for (const auto& pass : aeg::render_graph::detail::RenderGraphExecution::plan(graph).pass_plans) {
                if (pass.pass == unused_shadow_pass) sentinel_culled = !pass.live;
                if (iteration == 5 && pass.feature_work &&
                    std::any_of(native_observation.records.begin(), native_observation.records.end(), [&](const auto& record) {
                        return record.configuration.type == renderer::kLightCullingPassTypeId &&
                            record.source.work.id == pass.feature_work->id;
                    })) {
                    empty_cull_record = true;
                    if (pass.live) return 59;
                }
                if (!pass.live) continue;
                for (const auto& barrier : pass.barriers) {
                    if (barrier.resource_kind == render_graph::RenderGraphResourceKind::kTexture &&
                        barrier.resource_index == unused_shadow.index && barrier.resource_generation == unused_shadow.generation) return 59;
                    if (iteration == 5 && barrier.resource_kind == render_graph::RenderGraphResourceKind::kBuffer &&
                        ((barrier.after.usage == rhi::ResourceUsage::kStorageRead ||
                          barrier.after.usage == rhi::ResourceUsage::kStorageWrite ||
                          barrier.after.usage == rhi::ResourceUsage::kStorageReadWrite) ||
                         (barrier.resource_index == clustered_fixture.lights.constants_graph.index &&
                          barrier.resource_generation == clustered_fixture.lights.constants_graph.generation) ||
                         (barrier.resource_index == clustered_fixture.lights.lights_graph.index &&
                          barrier.resource_generation == clustered_fixture.lights.lights_graph.generation))) {
                        std::cerr << "empty draw retained light-list dependency, case=" << iteration << '\n';
                        return 59;
                    }
                }
            }
            if (!sentinel_culled || !empty_cull_record) return 59;
        }
        if (gbuffer_path) {
            std::vector<bool> live(outputs->size());
            for (const auto& pass : aeg::render_graph::detail::RenderGraphExecution::plan(graph).pass_plans) {
                if (!pass.feature_work || !pass.live) continue;
                if (lighting_mode && (pass.batch_index >= aeg::render_graph::detail::RenderGraphExecution::plan(graph).batches.size() ||
                    aeg::render_graph::detail::RenderGraphExecution::plan(graph).batches[pass.batch_index].queue != rhi::QueueClass::kGraphics)) continue;
                for (size_t index = 0; index < outputs->size(); ++index)
                    if (pass.feature_work->instance_id == (*outputs)[index].instance_id) live[index] = true;
            }
            if (!std::all_of(live.begin(), live.end(), [](bool value) { return value; })) {
                std::cerr << "HDR consumers did not retain every Feature: " << live[0] << live[1] << live[2] << '\n';
                return 39;
            }
        }
        const auto retire_fixture = [&]() -> foundation::Result {
            if (aliased_native.is_valid())
                if (const auto result = frame.defer_destroy_texture(aliased_native); !result) return result;
            if (const auto result = frame.defer_destroy_binding_set(material_set.value); !result) return result;
            if (const auto result = frame.defer_destroy_buffer(tint.value); !result) return result;
            if (!lighting_mode) {
                if (const auto result = frame.defer_destroy_binding_set(light_set.value); !result) return result;
                if (const auto result = frame.defer_destroy_buffer(light.value); !result) return result;
            }
            return foundation::Result::success();
        };
        if (feature_mode || scene_profile_mode) {
            // All commands were resolved already. Registry retirement must not
            // invalidate their actual immutable setup data or native pass owner.
            const render_graph::RenderPassExecutionHost retired{&passes,
                pipeline::detail::RenderPassRegistryExecution::execution_epoch(passes)};
            for (const auto type : kNativePassTypes) if (!check(passes.unregister_pass(type))) return 70;
            if (retired.is_valid()) return 70;
        }
        // Execution failure aborts the frame itself; register native retirement beforehand.
        if (!check(retire_fixture())) return 21;
        if (lighting_mode && !lighting_execution_failure_tested) {
            resolver.fail_deferred_inputs = !post_feature_mode;
            resolver.fail_post_inputs = post_feature_mode;
            const auto failed = graph.execute(frame);
            resolver.fail_deferred_inputs = false;
            resolver.fail_post_inputs = false;
            if (failed || failed.result.code != foundation::ResultCode::kNotReady || frame.is_frame_active()) return 52;
            graph.reset();
            outputs.reset();
            snapshot.reset();
            producer_snapshot.reset();
            second_snapshot.reset();
            scenes.clear();
            native_observation.expected = nullptr;
            if (!check(frame.abort_frame()) || !check(frame.drain()) || !output_lease.expired() ||
                !scene_lease.expired() || !producer_lease.expired() || !second_scene_lease.expired()) return 52;
            for (const auto& record : native_observation.records)
                if (!record.prepared.expired() || !record.delegate.expired()) return 71;
            lighting_execution_failure_tested = true;
            continue; // Rebuild this same analytic case in a fresh frame after actual failure retirement.
        }
        const auto executed = graph.execute(frame);
        if (!executed) {
            check(executed.result);
            return 20;
        }
        if (!check(verify_native_graph(graph, native_observation))) return 70;
        if (clustered_pair_mode && (executed.value.submitted_batch_count == 0 || executed.value.draw_call_count < 4 ||
            executed.value.queue_completions.empty() || !executed.value.completion.is_valid())) return 68;
        if (clustered_process_mode && iteration == 0) {
            const auto source = std::find_if(feature_plan.provenance.begin(), feature_plan.provenance.end(),
                [&](const auto& item) { return item.definition_id == producer.id; });
            if (source == feature_plan.provenance.end()) return 66;
            const auto process = feature_runtime.process(source->instance_id);
            if (!process || !check(feature_runtime.dispatch_process(source->instance_id))) return 66;
            const auto produced = feature_runtime.find_instance(source->instance_id)->render_output->invocation;
            std::array<feature::MessageDeliveryId, 2> deliveries{};
            size_t target_index = 0;
            for (const auto& item : feature_plan.provenance) if (item.definition_id == consumer.id) {
                const auto* target = feature_runtime.find_instance(item.instance_id);
                if (target_index == deliveries.size() || target->pending_messages.size() != 1 ||
                    target->pending_messages.front().source.invocation != produced) return 66;
                deliveries[target_index++] = target->pending_messages.front().delivery_id;
            }
            if (target_index != 2 || !deliveries[0].is_valid() || deliveries[0] == deliveries[1]) return 66;
            if (!check(clustered_cancel_mode ? feature_runtime.cancel(process.value) : feature_runtime.suspend(process.value)) ||
                !check(feature_runtime.tick(producer.contract.clock.id))) return 66;
            const auto waiting = feature_runtime.capture_render_outputs(feature_plan);
            if (!waiting || !waiting.value.empty() ||
                feature_runtime.render_publication_status(producer.contract.clock.id).code != foundation::ResultCode::kNotReady) return 66;
            target_index = 0;
            for (const auto& item : feature_plan.provenance) if (item.definition_id == consumer.id) {
                const auto* target = feature_runtime.find_instance(item.instance_id);
                if (!target->pending_messages.empty() || !target->render_output ||
                    target->render_output->input_messages.size() != 1 ||
                    target->render_output->input_messages.front().delivery_id != deliveries[target_index++] ||
                    target->render_output->input_messages.front().source.invocation != produced) return 66;
            }
            // The already submitted A graph owns its immutable cohort until the normal frame drain.
            if (output_lease.expired() || scene_lease.expired()) return 66;
        }
        if (!check(frame.end_frame()) || !check(device.wait_idle()))
            return 21;
        std::array<std::byte, kSize * kSize * 4> pixels{};
        if (!gbuffer_path && !clustered_hdr && !check(frame.read_readback(readback.value, pixels)))
            return 22;
        if (clustered_hdr) {
            const bool empty_clustered = clustered_shadow_mode ? clustered_case == 4 : clustered_case == 5;
            if (!clustered_pair_mode && (empty_clustered ? (clustered_fixture.compute_calls != 0 || !clustered_fixture.forward_sets.empty())
                : (clustered_fixture.compute_calls != 1 || clustered_fixture.forward_sets.size() != 2 ||
                   clustered_fixture.forward_sets[0] == clustered_fixture.forward_sets[1]))) return 60;
            if (clustered_pair_mode) {
                for (const auto* bucket : {&clustered_fixture, &second_clustered_fixture}) {
                    if (bucket->resolve_calls != 1 || bucket->compute_calls != 1 || bucket->forward_sets.size() != 1 ||
                        !bucket->camera_view.is_valid() || !bucket->clusters.is_valid() ||
                        bucket->shadow_views.size() != 1 || bucket->shadow_views.front().is_valid()) return 68;
                }
                if (clustered_fixture.lights.constants == second_clustered_fixture.lights.constants ||
                    clustered_fixture.lights.lights == second_clustered_fixture.lights.lights ||
                    clustered_fixture.forward_sets.front() == second_clustered_fixture.forward_sets.front() ||
                    (paired_lifetimes_overlap && (clustered_fixture.clusters == second_clustered_fixture.clusters ||
                        clustered_fixture.camera_view == second_clustered_fixture.camera_view))) return 68;
                std::cout << "equal List frame=" << iteration << " overlapping=" << paired_lifetimes_overlap
                    << " clusters_reused=" << (clustered_fixture.clusters == second_clustered_fixture.clusters)
                    << " camera_reused=" << (clustered_fixture.camera_view == second_clustered_fixture.camera_view) << '\n';
            }
            if (clustered_shadow_mode && !empty_clustered) {
                const bool enabled = clustered_case == 1 || clustered_case == 2;
                if (resolver.last_culling.shadow_enabled != enabled || clustered_fixture.shadow_views.size() != 2 ||
                    clustered_fixture.shadow_views[0].is_valid() != enabled ||
                    clustered_fixture.shadow_views[1] != clustered_fixture.shadow_views[0] ||
                    (enabled && clustered_fixture.shadow_views[0] == clustered_fixture.camera_view)) return 67;
            }
            std::array<std::vector<uint16_t>, 2> samples;
            for (size_t consumer = 0; consumer < samples.size(); ++consumer) {
                samples[consumer].resize(kSize * kSize * 4);
                if (!check(frame.read_readback(consumer == 0 ? readback.value : second_readback.value,
                    std::as_writable_bytes(std::span{samples[consumer]})))) return 61;
                for (const auto value : samples[consumer]) if (!std::isfinite(half_value(value))) return 61;
                for (const uint32_t x : {54u, 90u, 0u}) {
                    std::array<float, 4> expected{.1f, .1f, .1f, 1.f};
                    if (clustered_pair_mode) {
                        if (x == 0) expected = {.2f, .3f, .4f, 1.f};
                        else if (consumer == 0 && x == 54) expected[0] = .211000959f;
                        else if (consumer == 1 && x == 90) expected[2] = .349612184f;
                    } else if (clustered_shadow_mode) {
                        if (x == 0 || empty_clustered) expected = clustered_wave_mode && iteration == final_wave_iteration ?
                            std::array{.4f, .2f, .6f, 1.f} : std::array{.2f, .3f, .4f, 1.f};
                        else if (clustered_case != 5 && !((clustered_case == 1 || clustered_case == 2) && x == 54)) {
                            const float direct = clustered_case == 2 ? .1f + .8f / std::sqrt(5.f) : .1f + .8f / std::sqrt(1.25f);
                            expected = {direct, direct, direct, 1.f};
                        }
                    } else if (x == 0 || clustered_case == 5) expected = clustered_wave_mode && iteration == final_wave_iteration ?
                        std::array{.4f, .2f, .6f, 1.f} : std::array{.2f, .3f, .4f, 1.f};
                    else if (clustered_case == 0) expected = {.9f, .9f, .9f, 1.f};
                    else if (clustered_case == 1 && x == 54) expected[0] = .211000959f;
                    else if (clustered_case == 2 && x == 90) expected[2] = .349612184f;
                    for (size_t channel = 0; channel < expected.size(); ++channel) {
                        const float actual = half_value(samples[consumer][(64 * kSize + x) * 4 + channel]);
                        if (std::abs(actual - expected[channel]) > .001f) {
                            std::cerr << "clustered HDR case=" << iteration << " consumer=" << consumer << " x=" << x
                                << " channel=" << channel << " actual=" << actual << " expected=" << expected[channel] << '\n';
                            return 61;
                        }
                    }
                }
            }
            if (clustered_pair_mode ? samples[0] == samples[1] : samples[0] != samples[1]) return 61;
        } else if (gbuffer_mode) {
            std::array<std::vector<std::byte>, 4> values;
            for (size_t attachment = 0; attachment < values.size(); ++attachment) {
                values[attachment].resize(kSize * kSize * (attachment < 2 ? 8 : 4));
                if (!check(frame.read_readback(mrt_readbacks[attachment], values[attachment]))) return 44;
            }
            const auto clear = gbuffer_draw({}, feature::RenderAttachmentLoad::kClear);
            const bool empty = iteration == 2;
            for (size_t pixel = 0; pixel < kSize * kSize; ++pixel) {
                for (size_t attachment = 0; attachment < 2; ++attachment) {
                    const auto& expected = attachment == 0 ? clear.albedo.clear_color : clear.normal.clear_color;
                    for (size_t channel = 0; channel < 4; ++channel) {
                        uint16_t bits{};
                        std::memcpy(&bits, values[attachment].data() + (pixel * 4 + channel) * 2, 2);
                        const float value = half_value(bits);
                        if (!std::isfinite(value) || (empty && std::abs(value - expected[channel]) > .0001f)) return 44;
                    }
                }
                float depth_value{};
                std::memcpy(&depth_value, values[3].data() + pixel * 4, 4);
                if (!std::isfinite(depth_value) || (empty && std::abs(depth_value - .75f) > .00001f)) return 44;
                if (empty)
                    for (size_t channel = 0; channel < 4; ++channel)
                        if (std::abs(static_cast<int>(std::to_integer<unsigned>(values[2][pixel * 4 + channel])) -
                            std::array{64, 128, 191, 255}[channel]) > 1) return 44;
            }
            for (const uint32_t x : {35u, 90u, 0u}) {
                const size_t pixel = x ? 64 * kSize + x : 0;
                const bool geometry = !empty && x != 0;
                const std::array expected_albedo = iteration == 4 ? std::array{1.f, 0.f, 0.f, 0.f}
                    : geometry ? std::array{.25f, .5f, .75f, .5f} : clear.albedo.clear_color;
                const std::array expected_normal = iteration == 5 ? std::array{1.f, 0.f, 0.f, 0.f}
                    : geometry ? std::array{.5f, .5f, 1.f, 1.f} : clear.normal.clear_color;
                for (size_t channel = 0; channel < 4; ++channel) {
                    for (size_t attachment = 0; attachment < 2; ++attachment) {
                        uint16_t bits{};
                        std::memcpy(&bits, values[attachment].data() + (pixel * 4 + channel) * 2, 2);
                        if (std::abs(half_value(bits) - (attachment == 0 ? expected_albedo : expected_normal)[channel]) > .0001f)
                            return 45;
                    }
                    const auto expected = iteration == 6 ? std::array{255, 0, 0, 0}
                        : geometry ? std::array{128, 0, 0, 255} : std::array{64, 128, 191, 255};
                    if (std::abs(static_cast<int>(std::to_integer<unsigned>(values[2][pixel * 4 + channel])) - expected[channel]) > 1)
                        return 45;
                }
                float value{};
                std::memcpy(&value, values[3].data() + pixel * 4, 4);
                const float expected = iteration == 7 ? .25f : geometry ? (x == 35 ? 23.f / 49.f : 29.f / 49.f) : .75f;
                if (std::abs(value - expected) > .00001f) return 45;
            }
            if (iteration == 0) gbuffer_reference = values;
            else if (iteration >= 4) {
                auto expected = gbuffer_reference;
                const size_t role = iteration - 4;
                const std::array<uint16_t, 4> red_half{0x3c00, 0, 0, 0};
                const std::array<uint8_t, 4> red_bytes{255, 0, 0, 0};
                const float clear_depth = .25f;
                for (size_t pixel = 0; pixel < kSize * kSize; ++pixel) {
                    if (role < 2) std::memcpy(expected[role].data() + pixel * 8, red_half.data(), 8);
                    else if (role == 2) std::memcpy(expected[role].data() + pixel * 4, red_bytes.data(), 4);
                    else std::memcpy(expected[role].data() + pixel * 4, &clear_depth, 4);
                }
                if (values != expected) return 46;
            } else if (!empty && values != gbuffer_reference) return 46;
        }
        if (post_feature_mode) {
            const std::array allocations{readback.value, second_readback.value};
            for (size_t consumer_index = 0; consumer_index < allocations.size(); ++consumer_index) {
                std::vector<std::byte> bytes(kSize * kSize * 4);
                if (!check(frame.read_readback(allocations[consumer_index], bytes))) return 55;
                const float exposure = consumer_index == 1 ? .5f : iteration == 3 ? .75f : 1.f;
                const float gamma = iteration == 2 ? 2.f : 1.f;
                for (uint32_t x : {35u, 90u, 0u}) {
                    const bool geometry = x != 0 && iteration != 4;
                    const bool direct = geometry && iteration != 3 && iteration != 5 && !(iteration == 1 && x == 35);
                    const float illumination = .05f + (direct ? consumer_index == 0 ? 4.f : 2.f : 0.f);
                    const bool sky = !geometry && iteration != 2 && !(consumer_index == 0 && iteration == 3);
                    const std::array expected_hdr = geometry
                        ? std::array{.25f * illumination, .5f * illumination, .75f * illumination, .5f}
                        : sky ? std::array{.25f, .5f, 1.f, 1.f} : std::array{0.f, 0.f, 0.f, 0.f};
                    const size_t pixel = x ? 64 * kSize + x : 0;
                    for (size_t channel = 0; channel < 4; ++channel) {
                        const float linear = std::max(0.f, expected_hdr[channel]) * exposure;
                        const float expected = channel == 3 ? expected_hdr[channel] : std::pow(linear / (1.f + linear), 1.f / gamma);
                        const int expected_byte = static_cast<int>(std::lround(expected * 255.f));
                        const int actual = static_cast<int>(std::to_integer<unsigned>(bytes[pixel * 4 + channel]));
                        if (!std::isfinite(expected) || std::abs(actual - expected_byte) > 2) {
                            std::cerr << "PostProcess pixel mismatch frame=" << iteration << " consumer=" << consumer_index
                                      << " x=" << x << " channel=" << channel << " actual=" << actual << " expected=" << expected_byte << '\n';
                            return 55;
                        }
                    }
                }
            }
            if (resolver.post_resources.size() != 3 || resolver.post_input_sets.size() != 3) return 56;
            for (size_t first = 0; first < 3; ++first)
                for (size_t second = first + 1; second < 3; ++second)
                    if (resolver.post_resources[first].constants == resolver.post_resources[second].constants ||
                        resolver.post_input_sets[first] == resolver.post_input_sets[second]) return 56;
        }
        if (lighting_mode && !post_feature_mode) {
            const std::array allocations{readback.value, second_readback.value};
            for (size_t consumer_index = 0; consumer_index < allocations.size(); ++consumer_index) {
                std::vector<std::byte> hdr_pixels(kSize * kSize * 8);
                if (!check(frame.read_readback(allocations[consumer_index], hdr_pixels))) return 48;
                for (size_t component = 0; component < kSize * kSize * 4; ++component) {
                    uint16_t bits{};
                    std::memcpy(&bits, hdr_pixels.data() + component * 2, 2);
                    if (!std::isfinite(half_value(bits))) return 48;
                }
                for (uint32_t x : {35u, 90u, 0u}) {
                    const bool geometry = x != 0 && iteration != 4;
                    const bool direct = geometry && iteration != 3 && iteration != 5 && !(iteration == 1 && x == 35);
                    const float illumination = .05f + (direct ? consumer_index == 0 ? 4.f : 2.f : 0.f);
                    const std::array expected = geometry
                        ? std::array{.25f * illumination, .5f * illumination, .75f * illumination, .5f}
                        : std::array{0.f, 0.f, 0.f, 0.f};
                    const size_t pixel = x ? 64 * kSize + x : 0;
                    for (size_t channel = 0; channel < expected.size(); ++channel) {
                        uint16_t bits{};
                        std::memcpy(&bits, hdr_pixels.data() + (pixel * 4 + channel) * 2, 2);
                        // Bound quantization by one binary16 step at the independent oracle's magnitude.
                        const float tolerance = expected[channel] == 0.f ? 0.f
                            : std::max(.00001f, std::ldexp(1.f, std::ilogb(std::abs(expected[channel])) - 10));
                        if (std::abs(half_value(bits) - expected[channel]) > tolerance) {
                            std::cerr << "HDR mismatch frame=" << iteration << " consumer=" << consumer_index
                                      << " x=" << x << " channel=" << channel << " actual=" << half_value(bits)
                                      << " expected=" << expected[channel] << '\n';
                            return 48;
                        }
                    }
                }
            }
            if (resolver.deferred_input_sets.size() != (iteration == 0 ? 3u : 2u)) return 49;
            for (size_t first = 0; first < resolver.deferred_input_sets.size(); ++first)
                for (size_t second = first + 1; second < resolver.deferred_input_sets.size(); ++second)
                    if (resolver.deferred_input_sets[first] == resolver.deferred_input_sets[second]) return 49;
        }
        if (shadow_feature_mode) {
            std::array<std::byte, kSize * kSize * 4> second_pixels{};
            if (!check(frame.read_readback(second_readback.value, second_pixels)) || pixels != second_pixels) return 40;
            for (size_t offset = 0; offset < pixels.size(); offset += 4)
                if (std::to_integer<unsigned>(pixels[offset + 3]) != 255 ||
                    (iteration == 4 && std::to_integer<unsigned>(pixels[offset]) != 51)) return 40;
        }
        if (scene_profile_mode || unshadowed_mode) {
            if (!outputs || outputs->empty()) return 73;
            const auto provenance = render_feature_test::check_scene_draw_provenance(graph, *outputs, frame.frame_number());
            if (!provenance && legacy_scene_provenance) legacy_scene_provenance = provenance.result;
            if (!scene.instances.empty()) {
                legacy_scene_geometry += provenance.value;
                if ((provenance.value == 0 || executed.value.draw_call_count == 0) && legacy_scene_provenance)
                    legacy_scene_provenance = foundation::invalid_state("actual Scene geometry provenance or GPU draw count was vacuous");
            }
            for (const auto& output : *outputs)
                if (output.view_key != scene.view.id.value() && legacy_scene_provenance)
                    legacy_scene_provenance = foundation::invalid_state("actual Scene capture lost its current extracted View");
        }
        if (feature_mode || scene_profile_mode) {
            graph.reset();
            native_observation.expected = nullptr;
            for (const auto& record : native_observation.records)
                if (record.prepared.expired() || record.delegate.expired()) return 71;
            outputs.reset();
            snapshot.reset();
            scenes.clear();
            producer_snapshot.reset();
            second_snapshot.reset();
            if (output_lease.expired()) return 41;
            if (clustered_shadow_timing_mode && shadow_bundle_lease.expired()) return 69;
            if (scene_lease.expired()) return 41;
            if (gbuffer_path && producer_lease.expired()) return 47;
            if ((lighting_mode || clustered_pair_mode) && second_scene_lease.expired()) return 47;
            if (clustered_pair_mode && (paired_bundle_leases[0].expired() || paired_bundle_leases[1].expired())) return 68;
        }
        if (!gbuffer_path && !clustered_hdr) {
            dark_pixels[iteration] = std::to_integer<unsigned>(pixels[(64 * kSize + 54) * 4]);
            lit_pixels[iteration] = std::to_integer<unsigned>(pixels[(64 * kSize + 90) * 4]);
            blue_pixels[iteration] = std::to_integer<unsigned>(pixels[(64 * kSize + 90) * 4 + 2]);
            background_pixels[iteration] = std::to_integer<unsigned>(pixels[0]);
        }
        if (!check(frame.drain()))
            return 23;
        if (feature_mode || scene_profile_mode) for (const auto& record : native_observation.records)
            if (!record.prepared.expired() || !record.delegate.expired()) return 71;
        if (clustered_feature_mode) retired_culling = resolver.last_culling;
        if (!scene_lease.expired()) return 41;
        if (!output_lease.expired()) return 41;
        if (clustered_shadow_timing_mode && !shadow_bundle_lease.expired()) return 69;
        if (gbuffer_path && !producer_lease.expired()) return 47;
        if ((lighting_mode || clustered_pair_mode) && !second_scene_lease.expired()) return 47;
        if (clustered_pair_mode) {
            if (!paired_bundle_leases[0].expired() || !paired_bundle_leases[1].expired()) return 68;
            for (const auto* bucket : {&clustered_fixture, &second_clustered_fixture}) {
                const auto stale = bucket->lights.prepare_forward({}, {});
                if (stale || stale.result.code != foundation::ResultCode::kInvalidState) return 68;
            }
            resolver.observed_culling_by_scene = {};
        }
        if ((!feature_mode || post_feature_mode) && !clustered_hdr) {
            const auto stale_post = resolver.last_post.prepare_inputs({}, {});
            if (stale_post || stale_post.result.code != foundation::ResultCode::kInvalidState) return 33;
        }
        if ((shadow_feature_mode && (iteration == 1 || iteration == 2)) || (iteration != 0 && deferred_mode && !post_process)) {
            const auto stale = resolver.last_shadow.prepare_inputs({});
            if (stale || stale.result.code != foundation::ResultCode::kInvalidState)
                return 27;
        }
        if (!deferred_mode && (!feature_mode || clustered_feature_mode)) {
            const auto stale = resolver.last_culling.prepare_forward({}, {});
            if (stale || stale.result.code != foundation::ResultCode::kInvalidState)
                return 30;
        }
        if (shadows.for_frame(frame, {}))
            return 28;
        if (lighting_mode) {
            const auto stale = resolver.last_deferred.prepare_inputs({});
            if (stale || stale.result.code != foundation::ResultCode::kInvalidState) return 53;
        }
        graph.reset();
        if (clustered_process_mode && iteration == 0) {
            if (!check(frame.begin_frame()) || !check(feature_runtime.tick(producer.contract.clock.id))) return 66;
            auto waiting = feature_runtime.capture_render_outputs(feature_plan);
            if (!waiting || !waiting.value.empty() ||
                feature_runtime.render_publication_status(producer.contract.clock.id).code != foundation::ResultCode::kNotReady) return 66;
            const auto culling_calls = resolver.culling_resolutions;
            if (!check(render_graph::build_render_graph_features(graph,
                std::make_shared<const std::vector<feature::RenderFeatureOutput>>(std::move(waiting.value)),
                {.current_frame_owner = &frame}, frame.frame_number())) || !check(graph.compile()) ||
                aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).pass_count != 0 || aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).resource_count != 0 ||
                resolver.culling_resolutions != culling_calls || !aeg::render_graph::detail::RenderGraphExecution::plan(graph).pass_plans.empty()) return 66;
            const auto empty_execution = graph.execute(frame);
            if (!empty_execution || !check(frame.end_frame()) || !check(frame.drain())) return 66;
            graph.reset();
            if (!clustered_cancel_mode) {
                const auto source = std::find_if(feature_plan.provenance.begin(), feature_plan.provenance.end(),
                    [&](const auto& item) { return item.definition_id == producer.id; });
                const auto process = feature_runtime.process(source->instance_id);
                if (!process || !check(feature_runtime.resume(process.value))) return 66;
                const auto still_waiting = feature_runtime.capture_render_outputs(feature_plan);
                if (!still_waiting || !still_waiting.value.empty() ||
                    feature_runtime.render_publication_status(producer.contract.clock.id).code != foundation::ResultCode::kNotReady) return 66;
            }
        }
        ++iteration;
    }
    if (feature_mode && (native_observation.foreign_setup_trials == 0 || native_observation.foreign_execute_trials == 0))
        return 72;
    if (post_feature_mode && (native_observation.other_owner_setup == 0 || native_observation.other_owner_execute == 0 ||
        native_observation.other_operation_setup == 0 || native_observation.other_operation_execute == 0)) return 72;
    if (feature_mode) {
        if (gbuffer_path && native_observation.positive_executions[1] == 0) return 70;
        if (lighting_mode && native_observation.positive_executions[2] == 0) return 70;
        if (post_feature_mode && native_observation.positive_executions[3] == 0) return 70;
        if (clustered_feature_mode && (native_observation.positive_executions[4] == 0 ||
            native_observation.positive_executions[5] == 0 || native_observation.positive_executions[6] == 0)) return 70;
        if (shadow_feature_mode && (native_observation.positive_executions[0] == 0 || native_observation.positive_executions[6] == 0)) return 70;
        if (clustered_shadow_mode && native_observation.positive_executions[0] == 0) return 70;
    }
    if (!gbuffer_path && !clustered_hdr) std::cout << "Shadow samples: " << dark_pixels[0] << ',' << dark_pixels[1] << ',' << dark_pixels[2]
              << "; lit: " << lit_pixels[0] << ',' << lit_pixels[1] << ',' << lit_pixels[2] << '\n';
    if (post_feature_mode) {
        std::cout << "Feature GBuffer to Lighting to PostProcess data edges, isolated settings and final LDR pixels passed\n";
    } else if (lighting_mode) {
        std::cout << "Feature GBuffer to HDR lighting, same-frame constants and shadow pixels passed\n";
    } else if (gbuffer_mode) {
        std::cout << "GBuffer albedo, normal, material, depth, Clear/Load and Feature chain pixels passed\n";
    } else if (shadow_feature_mode) {
        constexpr std::array expected_dark{208, 26, 26, 208, 51, 26};
        constexpr std::array expected_lit{208, 208, 117, 208, 51, 26};
        for (size_t sample = 0; sample < expected_dark.size(); ++sample)
            if (std::abs(dark_pixels[sample] - expected_dark[sample]) > 3 ||
                std::abs(lit_pixels[sample] - expected_lit[sample]) > 3) return 42;
    } else if (post_process) {
        std::cout << "Sky samples: " << background_pixels[0] << ',' << background_pixels[1] << ','
                  << background_pixels[2] << '\n';
        if (std::abs(dark_pixels[0] - 205) > 3 || std::abs(dark_pixels[1] - 171) > 3 ||
            std::abs(dark_pixels[2] - 229) > 3 || std::abs(background_pixels[0] - 51) > 2 ||
            std::abs(background_pixels[1] - 28) > 2 || std::abs(background_pixels[2] - 114) > 2)
            return 31;
    } else if (clustered_pair_mode) {
        std::cout << "Two independent Feature light-list producers with equal public IDs: real receipts, owner snapshots, both capture orders, Point/Spot HDR pixels and frame retirement passed\n";
    } else if (clustered_shadow_timing_mode) {
        std::cout << "Effective directional shadow Feature " << (clustered_shadow_wave_mode ? "complete A/B waves" :
            clustered_cancel_mode ? "Process cancel" : "Process suspend/resume")
            << ": actual shadow views/work, per-owner receipts, dark/lit HDR and normal frame leases passed\n";
    } else if (clustered_shadow_mode) {
        std::cout << "Independent Feature directional depth/culling and two HDR consumers: central/off-camera shadow pixels, native descriptors and retirement passed\n";
    } else if (clustered_process_mode) {
        std::cout << "Feature LightList Process " << (clustered_cancel_mode ? "cancel" : "suspend/resume")
            << ": real pending receipts, empty GPU work, submitted cohort leases and HDR pixels passed\n";
    } else if (clustered_hdr) {
        std::cout << (clustered_feature_mode ? "Independent Feature light-list producer and two consumers: " : "Shared clustered recorder: ")
                  << "directional/point/spot/empty HDR pixels and no shadow binding passed\n";
    } else if (local_lights) {
        std::cout << "Spot samples: " << blue_pixels[0] << ',' << blue_pixels[1] << ',' << blue_pixels[2] << '\n';
        if (dark_pixels[0] < 23 || dark_pixels[0] > 27 || dark_pixels[1] < 45 || dark_pixels[2] < 23 ||
            dark_pixels[2] > 27 || blue_pixels[0] < 23 || blue_pixels[0] > 27 || blue_pixels[1] < 75 ||
            blue_pixels[2] < 23 || blue_pixels[2] > 27 || lit_pixels[1] < 23 || lit_pixels[1] > 27)
            return 29;
    } else if (dark_pixels[0] < 170 || dark_pixels[1] > 45 || dark_pixels[1] < 20 || dark_pixels[2] > 45 ||
               dark_pixels[2] < 20 || std::abs(lit_pixels[0] - lit_pixels[1]) > 3 || lit_pixels[2] < 95)
        return 24;
    if ((scene_profile_mode || unshadowed_mode) && (!legacy_scene_provenance || legacy_scene_geometry == 0)) {
        std::cerr << "After successful native frames and verified pixels: " << legacy_scene_provenance.message
                  << "; actual live Scene geometry=" << legacy_scene_geometry << '\n';
        return 73;
    }
    if (!check(resolver.objects.retire_gpu_resources()) ||
        !check(device.destroy_sampler(sampler.value)) || !check(device.destroy_buffer(vertex.value)) ||
        !check(device.destroy_buffer(index.value)))
        return 25;
    for (auto pipeline : {opaque.value, shadow_opaque.value, gbuffer.value, depth_pipeline.value, clustered.value, unshadowed.value})
        if (!check(device.destroy_pipeline(pipeline)))
            return 26;
    const size_t expected_contract_probes = lighting_mode ? (post_feature_mode ? 3u : 2u)
        : unshadowed_mode || (clustered_shadow_mode && !clustered_shadow_timing_mode) ? 1u : 0u;
    if (!contract_probes_valid || contract_probe_count != expected_contract_probes) {
        std::cerr << "Render contract rejection failed after real GPU recovery pixels and retirement: observed="
            << contract_probe_count << " expected=" << expected_contract_probes << '\n';
        return 75;
    }
    if (!gbuffer_path && !clustered_hdr) std::cout << "Shadowed receiver, ambient, lit receiver and off-camera caster passed\n";
}
