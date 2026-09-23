/**
 * @file builtin_render_passes.cpp
 * @brief RenderPass contracts for built-in renderer variants.
 */
#include "builtin_render_passes.hpp"

#include <memory>

#include "pipeline_configuration.hpp"
#include "render_pipeline_contract.hpp"
#include "renderer_variants.hpp"

namespace aeg::renderer {
namespace {

[[nodiscard]] constexpr uint64_t queue_bit(rhi::QueueClass queue) {
    return uint64_t{1} << static_cast<uint8_t>(queue);
}

struct BuiltinPassCommands final : pipeline::RenderPassRuntimeData {
    pipeline::RenderPassTypeId type{};
    pipeline::RenderPassSetupFunction setup{};
    pipeline::RenderPassExecuteFunction execute{};
};

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
            !(descriptor_.allowed_queue_mask & queue_bit(configuration.queue)) || !configuration.parameters.empty() ||
            !configuration.bindings.empty())
            return foundation::invalid_argument("built-in renderer pass configuration is incompatible");
        const auto commands = std::dynamic_pointer_cast<const BuiltinPassCommands>(configuration.runtime_data);
        if (!commands || commands->type != descriptor_.type || !commands->setup || !commands->execute)
            return foundation::invalid_state("built-in renderer pass requires prepared frame commands");
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
foundation::ResultValue<pipeline::RenderGraphPass> add_builtin_render_pass(
    pipeline::RenderGraphBuilder& builder, const RenderFeatureContext& context, pipeline::RenderPassTypeId type,
    pipeline::RenderPassSetupFunction setup, pipeline::RenderPassExecuteFunction execute) {
    if (!builder.is_runtime() || render_feature_context(builder) != &context || !context.render_passes)
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
    if (const auto authored = builder.configuration()) {
        bool found = false;
        for (const auto& feature : authored->features) {
            if (feature.instance != builder.feature_instance_id())
                continue;
            for (const auto& pass : feature.passes) {
                if (pass.type != type)
                    continue;
                if (found)
                    return {foundation::invalid_argument("built-in renderer pass is duplicated"), {}};
                configuration->type = pass.type;
                configuration->instance = pass.instance;
                configuration->queue = pass.queue;
                configuration->enabled = pass.enabled;
                configuration->parameters = pass.parameters;
                configuration->bindings = pass.resource_bindings;
                configuration->source = pass.source;
                found = true;
            }
        }
        if (!found)
            return {foundation::not_found("built-in renderer pass is missing from feature configuration"), {}};
    }
    auto commands = std::make_shared<BuiltinPassCommands>();
    commands->type = type;
    commands->setup = std::move(setup);
    commands->execute = std::move(execute);
    configuration->runtime_data = std::move(commands);
    return pipeline::add_registered_render_pass(builder, *context.render_passes, std::move(configuration));
}

pipeline::TypeCatalogEntry make_pass_entry(size_t index) {
    const pipeline::RenderPassTypeId ids[] = {
        kShadowPassTypeId,   kGBufferPassTypeId,      kDeferredLightingPassTypeId, kSkyboxPostProcessPassTypeId,
        kZPrepassPassTypeId, kLightCullingPassTypeId, kForwardOpaquePassTypeId};
    const auto queue = index == 5 ? rhi::QueueClass::kCompute : rhi::QueueClass::kGraphics;
    const uint64_t mask = uint64_t{1} << static_cast<uint8_t>(queue);
    return {.key = {pipeline::AuthoringTypeKind::kRenderPass, ids[index].value()},
            .version = kVersion,
            .owner_module = kModule,
            .name = kPassNames[index],
            .display_name = kPassNames[index],
            .domains = kDomain,
            .scopes = kScope,
            .declaration = pipeline::PassAuthoringDeclaration{ids[index], mask}};
}
foundation::Result validate_pass_configuration(const pipeline::RenderPassConfiguration& configuration, size_t index) {
    const pipeline::RenderPassTypeId ids[] = {
        kShadowPassTypeId,   kGBufferPassTypeId,      kDeferredLightingPassTypeId, kSkyboxPostProcessPassTypeId,
        kZPrepassPassTypeId, kLightCullingPassTypeId, kForwardOpaquePassTypeId};
    return configuration.type == ids[index]
               ? foundation::Result::success()
               : foundation::invalid_argument("built-in renderer pass configuration type does not match");
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
