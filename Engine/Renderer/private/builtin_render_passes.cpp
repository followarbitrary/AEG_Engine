/**
 * @file builtin_render_passes.cpp
 * @brief RenderPass contracts for built-in renderer variants.
 */
#include "builtin_render_passes.hpp"
#include "native_scene_pass_source.hpp"

#include <memory>
#include <optional>

#include "render_pipeline_contract.hpp"
#include "../../Pipeline/private/render_pass_execution.hpp"
#include "renderer_variants_internal.hpp"

namespace aeg::renderer {
namespace {

[[nodiscard]] constexpr uint64_t queue_bit(rhi::QueueClass queue) {
    return uint64_t{1} << static_cast<uint8_t>(queue);
}

struct BuiltinPassCommands final : pipeline::RenderPassRuntimeData {
    pipeline::RenderPassTypeId type{};
    pipeline::RenderPassInstanceId instance{};
    rhi::QueueClass queue{rhi::QueueClass::kGraphics};
    bool enabled{true};
    foundation::ParameterBlock parameters{};
    std::unordered_map<uint64_t, uint64_t> bindings{};
    std::string source{};
    std::optional<render_graph::RenderSceneOperationSource> operation{};
    pipeline::RenderPassSetupFunction setup{};
    pipeline::RenderPassExecuteFunction execute{};
};

bool source_matches_type(const render_graph::RenderSceneOperationSource& source, pipeline::RenderPassTypeId type) {
    using Kind = render_graph::RenderSceneOperationKind;
    switch (source.kind) {
    case Kind::kDrawScene: case Kind::kDrawClusteredScene: return type == kForwardOpaquePassTypeId;
    case Kind::kDrawGBufferScene: return type == kGBufferPassTypeId;
    case Kind::kDeferredLightingScene: return type == kDeferredLightingPassTypeId;
    case Kind::kPostProcessScene: return type == kSkyboxPostProcessPassTypeId;
    case Kind::kLightCullingScene: return type == kLightCullingPassTypeId;
    case Kind::kDrawDepthScene:
        if (source.depth_projection == feature::RenderDepthSceneProjection::kCamera) return type == kZPrepassPassTypeId;
        if (source.depth_projection == feature::RenderDepthSceneProjection::kDirectionalShadow) return type == kShadowPassTypeId;
        return false;
    default: return false;
    }
}

bool prepared_identity_matches(const BuiltinPassCommands& commands, const pipeline::RenderPassConfiguration& configuration) {
    return commands.type == configuration.type && commands.instance == configuration.instance &&
        commands.instance.is_valid() && commands.queue == configuration.queue && commands.enabled == configuration.enabled &&
        commands.parameters == configuration.parameters && commands.bindings == configuration.bindings &&
        commands.source == configuration.source && (!commands.operation || source_matches_type(*commands.operation, commands.type));
}

pipeline::RenderPassInstanceId scene_pass_instance(pipeline::RenderPassTypeId type,
    const render_graph::RenderSceneOperationSource& source, uint64_t actual_view) {
    uint64_t hash = 14695981039346656037ull;
    const auto mix = [&hash](uint64_t value) {
        for (unsigned byte = 0; byte != 8; ++byte) {
            hash ^= (value >> (byte * 8)) & 0xffu;
            hash *= 1099511628211ull;
        }
    };
    mix(pipeline::RenderPassInstanceId("Renderer.NativeSceneOperation").value());
    mix(type.value()); mix(source.work.id.value()); mix(source.view_key); mix(actual_view);
    mix(source.owner.definition_digest); mix(source.owner.resource_epoch);
    mix(static_cast<uint64_t>(source.kind)); mix(static_cast<uint64_t>(source.depth_projection));
    mix(0); // One fixed primary native recorder for each Scene operation.
    return pipeline::RenderPassInstanceId(hash == 0 ? 1 : hash);
}

std::string scene_pass_source(const render_graph::RenderSceneOperationSource& source, uint64_t actual_view) {
    const auto& work = source.work;
    const auto& token = work.invocation;
    return "Feature.Scene/module=" + std::to_string(work.owner_module.value()) +
        "/definition=" + std::to_string(work.feature_definition_id.value()) + "/node=" + std::to_string(work.node_id.value()) +
        "/instance=" + std::to_string(work.instance_id.value()) + "/invocation=" + std::to_string(token.invocation_id.value()) +
        "/attempt=" + std::to_string(token.attempt_id.value()) + "/generation=" + std::to_string(token.generation.value()) +
        "/input=" + std::to_string(token.input_version) + "/process=" + std::to_string(token.process_revision.value()) +
        "/state=" + std::to_string(token.state_version.value()) + "/frame=" + std::to_string(work.frame_index) +
        "/operation=" + std::to_string(work.operation_index) + "/view=" + std::to_string(source.view_key) +
        "/native-view=" + std::to_string(actual_view) + "/digest=" + std::to_string(source.owner.definition_digest) +
        "/epoch=" + std::to_string(source.owner.resource_epoch);
}

class BuiltinRenderPass final : public pipeline::RenderPass {
public:
    BuiltinRenderPass(pipeline::RenderPassTypeId type, const char* name, pipeline::RenderGraphPassType graph_type,
                      rhi::QueueClass queue)
        : descriptor_{
              .type = type,
              .owner_module = pipeline::ModuleId("Renderer"),
              .name = name,
              .version = {1, 0, 0},
              .graph_type = graph_type,
              .allowed_queue_mask = queue_bit(queue),
          } {}

    [[nodiscard]] const pipeline::RenderPassDescriptor& descriptor() const noexcept override { return descriptor_; }

    [[nodiscard]] foundation::Result setup(pipeline::RenderPassBuilder& builder,
                                           const pipeline::RenderPassConfiguration& configuration) const override {
        if (const auto result = validate(configuration); !result)
            return result;
        return std::static_pointer_cast<const BuiltinPassCommands>(configuration.runtime_data)->setup(builder);
    }

    [[nodiscard]] foundation::Result execute(pipeline::RenderPassExecutionContext& command,
                                             const pipeline::RenderPassConfiguration& configuration) const override {
        if (const auto result = validate(configuration); !result)
            return result;
        return std::static_pointer_cast<const BuiltinPassCommands>(configuration.runtime_data)->execute(command);
    }

private:
    [[nodiscard]] foundation::Result validate(const pipeline::RenderPassConfiguration& configuration) const {
        if (configuration.type != descriptor_.type || !configuration.enabled ||
            configuration.queue > rhi::QueueClass::kCopy ||
            !(descriptor_.allowed_queue_mask & queue_bit(configuration.queue)) || !configuration.parameters.empty())
            return foundation::invalid_argument("built-in renderer pass configuration is incompatible");
        for (const auto& binding : configuration.bindings) {
            if (binding.first != binding.second)
                return foundation::invalid_argument("built-in renderer pass resource rebinding is unsupported");
        }
        const auto commands = std::dynamic_pointer_cast<const BuiltinPassCommands>(configuration.runtime_data);
        if (!commands || commands->type != descriptor_.type || !commands->setup || !commands->execute)
            return foundation::invalid_state("built-in renderer pass requires prepared frame commands");
        if (!prepared_identity_matches(*commands, configuration))
            return foundation::invalid_argument("built-in renderer pass configuration does not match its prepared identity");
        return foundation::Result::success();
    }
    pipeline::RenderPassDescriptor descriptor_{};
};

[[nodiscard]] std::unique_ptr<pipeline::RenderPass> make_graphics_pass(pipeline::RenderPassTypeId id,
                                                                       const char* name) {
    return std::make_unique<BuiltinRenderPass>(id, name, pipeline::RenderGraphPassType::kGraphics,
                                               rhi::QueueClass::kGraphics);
}

}  // namespace

namespace detail {
foundation::ResultValue<render_graph::RenderSceneOperationSource> NativeScenePassInspection::source(
    const pipeline::RenderPassConfiguration& configuration) {
    const auto commands = std::dynamic_pointer_cast<const BuiltinPassCommands>(configuration.runtime_data);
    if (!commands || !prepared_identity_matches(*commands, configuration))
        return {foundation::invalid_state("native Scene pass has no matching prepared configuration"), {}};
    if (!commands->operation) return {foundation::not_found("native pass has no Feature Scene operation source"), {}};
    return {foundation::Result::success(), *commands->operation};
}

foundation::ResultValue<pipeline::RenderGraphPass> add_builtin_render_pass(
    pipeline::RenderGraphBuilder& builder, const RenderFeatureContext& context, pipeline::RenderPassTypeId type,
    pipeline::RenderPassSetupFunction setup, pipeline::RenderPassExecuteFunction execute,
    pipeline::RenderPassInstanceId authored_instance) {
    if (!builder.is_runtime() || !context.pass_execution.is_valid() ||
        (!context.registered_native && render_feature_context(builder) != &context))
        return {foundation::invalid_state("built-in renderer pass requires its host RenderPassRegistry"), {}};
    bool known = false;
    for (const auto* name : kPassNames)
        known |= type == pipeline::RenderPassTypeId(name);
    if (!known || !setup || !execute)
        return {foundation::invalid_argument("built-in renderer pass commands or type are invalid"), {}};
    auto configuration = std::make_shared<pipeline::RenderPassConfiguration>();
    configuration->type = type;
    configuration->instance = pipeline::RenderPassInstanceId(type.value());
    configuration->queue = type == kLightCullingPassTypeId ? rhi::QueueClass::kCompute : rhi::QueueClass::kGraphics;
    if (context.registered_native) {
        const auto* source = context.operation_source;
        if (!context.is_valid() || !source || !source_matches_type(*source, type) || authored_instance.is_valid() ||
            context.native_sequence ||
            source->work.kind != render_graph::RenderWorkItemKind::kExecute ||
            source->work.domain != feature::FeatureDomain::kRender || !source->work.id.is_valid() ||
            !source->work.feature_definition_id.is_valid() || !source->work.owner_module.is_valid() ||
            source->owner.instance_id != source->work.instance_id || source->owner.node_id != source->work.node_id ||
            source->owner.definition_digest == 0 || source->owner.resource_epoch == 0 ||
            !source->work.invocation.invocation_id.is_valid() || !source->work.invocation.generation.is_valid() ||
            source->work.frame_index != context.scene_snapshot->frame_number ||
            (source->view_key != 0 && source->view_key != context.scene_snapshot->view.id.value()))
            return {foundation::invalid_state("registered native Scene pass has another actual operation source"), {}};
        configuration->instance = scene_pass_instance(type, *source, context.scene_snapshot->view.id.value());
        configuration->source = scene_pass_source(*source, context.scene_snapshot->view.id.value());
    } else if (const auto* sequence = context.native_sequence) {
        bool found = false;
        for (const auto& pass : sequence->passes) {
                const bool matches_instance = authored_instance.is_valid() && pass.instance == authored_instance;
                if ((!authored_instance.is_valid() && pass.type != type) ||
                    (authored_instance.is_valid() && !matches_instance))
                    continue;
                if (found)
                    return {foundation::invalid_argument("built-in renderer pass instance is duplicated"), {}};
                if (pass.type != type)
                    return {foundation::invalid_argument("built-in renderer pass instance type does not match"), {}};
                configuration->type = pass.type;
                configuration->instance = pass.instance;
                configuration->queue = pass.queue;
                configuration->enabled = pass.enabled;
                configuration->parameters = pass.parameters;
                configuration->bindings = pass.resource_bindings;
                configuration->source = pass.source;
                found = true;
        }
        if (!found)
            return {foundation::not_found("built-in renderer pass is missing from its native sequence"), {}};
    }
    auto commands = std::make_shared<BuiltinPassCommands>();
    commands->type = type;
    commands->instance = configuration->instance;
    commands->queue = configuration->queue;
    commands->enabled = configuration->enabled;
    commands->parameters = configuration->parameters;
    commands->bindings = configuration->bindings;
    commands->source = configuration->source;
    if (context.registered_native) commands->operation = *context.operation_source;
    commands->setup = std::move(setup);
    commands->execute = std::move(execute);
    configuration->runtime_data = std::move(commands);
    const auto native_instance = configuration->instance;
    const auto added = pipeline::add_registered_render_pass(builder, *context.pass_execution.registry,
                                                           std::move(configuration));
    if (added && context.native_sequence) {
        if (const auto remembered = builder.remember_native_pass(native_instance, added.value); !remembered)
            return {remembered, {}};
    }
    return added;
}

}  // namespace detail

foundation::Result register_builtin_render_passes(pipeline::RenderPassRegistry& registry) {
    struct PassRegistration final {
        pipeline::RenderPassTypeId id;
        const char* name;
        pipeline::RenderGraphPassType graph_type;
        rhi::QueueClass queue;
    };
    constexpr PassRegistration kPasses[] = {
        {kShadowPassTypeId, "Renderer.ShadowPass", pipeline::RenderGraphPassType::kGraphics,
         rhi::QueueClass::kGraphics},
        {kGBufferPassTypeId, "Renderer.GBufferPass", pipeline::RenderGraphPassType::kGraphics,
         rhi::QueueClass::kGraphics},
        {kDeferredLightingPassTypeId, "Renderer.DeferredLightingPass", pipeline::RenderGraphPassType::kGraphics,
         rhi::QueueClass::kGraphics},
        {kSkyboxPostProcessPassTypeId, "Renderer.SkyboxPostProcessPass", pipeline::RenderGraphPassType::kGraphics,
         rhi::QueueClass::kGraphics},
        {kZPrepassPassTypeId, "Renderer.ZPrepassPass", pipeline::RenderGraphPassType::kGraphics,
         rhi::QueueClass::kGraphics},
        {kLightCullingPassTypeId, "Renderer.LightCullingPass", pipeline::RenderGraphPassType::kCompute,
         rhi::QueueClass::kCompute},
        {kForwardOpaquePassTypeId, "Renderer.ForwardOpaquePass", pipeline::RenderGraphPassType::kGraphics,
         rhi::QueueClass::kGraphics},
    };
    for (const PassRegistration& pass : kPasses) {
        const foundation::Result result = registry.register_pass([pass]() -> std::unique_ptr<pipeline::RenderPass> {
            return pass.graph_type == pipeline::RenderGraphPassType::kGraphics
                       ? make_graphics_pass(pass.id, pass.name)
                       : std::make_unique<BuiltinRenderPass>(pass.id, pass.name, pass.graph_type, pass.queue);
        });
        if (!result) {
            return result;
        }
    }
    return foundation::Result::success();
}

}  // namespace aeg::renderer
