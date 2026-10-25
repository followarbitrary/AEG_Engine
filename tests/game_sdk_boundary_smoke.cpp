/** @brief Verifies the actual game target facade has no engine task API. */
#if __has_include("concurrency.hpp")
#error "GameRuntime must not expose the internal Concurrency headers"
#endif
#if __has_include("pipeline.hpp")
#error "GameRuntime must not expose the internal Pipeline headers"
#endif
#if __has_include("serialization.hpp")
#error "GameRuntime must not expose the internal parameter codec dependencies"
#endif
#if __has_include("runtime_bootstrap.hpp")
#error "GameRuntime must expose only the game bootstrap facade"
#endif
#if __has_include("runtime_feature_control.hpp")
#error "GameRuntime must not expose the engine Feature control service"
#endif
#if __has_include("feature_graph_runtime_host.hpp")
#error "GameRuntime must not expose the engine FeatureGraph runtime host"
#endif
#if __has_include("rhi.hpp")
#error "Feature render intents must not expose RHI headers to GameRuntime"
#endif
#if __has_include("renderer_material_contract.hpp")
#error "GameRuntime must not expose Renderer material Shader/PSO contracts"
#endif
#if __has_include("runtime_asset_resolver.hpp")
#error "GameRuntime must not expose the Runtime renderer upload host"
#endif
#if __has_include("runtime_asset_upload_host.hpp")
#error "GameRuntime must not expose ShaderProgram/RHI upload dependencies"
#endif
#if __has_include("runtime_scene_loader.hpp")
#error "GameRuntime must not expose Runtime private headers"
#endif
#if __has_include("shader_contract.hpp")
#error "GameRuntime must not expose shader reflection contracts"
#endif
#if __has_include("feature_input.hpp")
#error "GameRuntime must not expose the engine host input admission API"
#endif
#if __has_include("feature_world_commands.hpp")
#error "GameRuntime must not expose the engine World command consumer"
#endif

#include "game_runtime_bootstrap.hpp"
#include "module_export.hpp"
#include "feature_parameters.hpp"
#include "feature_render_intent.hpp"
#include "feature_world_transform.hpp"

int main() {
#if __has_include("module_render_resources.hpp")
    return 13;
#endif
    aeg::feature::FeatureDefinition feature{};
    feature.execute = [](const aeg::feature::InvocationToken& token, const aeg::feature::StateSnapshot& state,
                          std::span<const uint8_t>, std::span<const uint8_t>) {
        aeg::feature::StateDelta delta{};
        delta.expected_version = token.state_version;
        delta.state_payload = state.payload;
        delta.state_payload->push_back(7);
        delta.render_intent = aeg::feature::RenderIntent{};
        aeg::feature::RenderDrawDepthScene shadow{};
        shadow.projection = aeg::feature::RenderDepthSceneProjection::kDirectionalShadow;
        delta.render_intent->operations.push_back(shadow);
        aeg::feature::RenderDrawScene draw{};
        draw.directional_shadow = aeg::feature::RenderTextureReference{
            aeg::feature::RenderResourceId("Renderer.ShadowDepth"), 1, true};
        delta.render_intent->operations.push_back(draw);
        aeg::feature::RenderDrawGBufferScene gbuffer{};
        gbuffer.scene = {aeg::feature::RenderResourceId("Runtime.Scene"), 1};
        gbuffer.albedo.target = {aeg::feature::RenderResourceId("Renderer.GBuffer.Albedo"), 1, true};
        gbuffer.normal.target = {aeg::feature::RenderResourceId("Renderer.GBuffer.Normal"), 1, true};
        gbuffer.material.target = {aeg::feature::RenderResourceId("Renderer.GBuffer.Material"), 1, true};
        gbuffer.depth.target = {aeg::feature::RenderResourceId("Renderer.SceneDepth"), 1, true};
        delta.render_intent->operations.push_back(gbuffer);
        aeg::feature::RenderDeferredLightingScene lighting{};
        lighting.scene = gbuffer.scene;
        lighting.albedo = gbuffer.albedo.target;
        lighting.normal = gbuffer.normal.target;
        lighting.depth = gbuffer.depth.target;
        lighting.hdr.target = {aeg::feature::RenderResourceId("Renderer.SceneLighting"), 1, true};
        lighting.directional_shadow = draw.directional_shadow;
        delta.render_intent->operations.push_back(lighting);
        aeg::feature::RenderPostProcessScene post{};
        post.scene = lighting.scene;
        post.hdr = lighting.hdr.target;
        post.depth = lighting.depth;
        post.output.target = {aeg::feature::RenderResourceId("Renderer.SceneColor"), 1, true};
        post.settings = aeg::feature::RenderPostProcessSettings{};
        post.settings->exposure = .5f;
        delta.render_intent->operations.push_back(post);
        aeg::feature::RenderLightCullingScene cull{};
        cull.scene = lighting.scene;
        cull.camera_depth = lighting.depth;
        cull.output = {aeg::feature::RenderResourceId("TileLights"), 1, aeg::feature::PortId("List")};
        delta.render_intent->operations.push_back(cull);
        aeg::feature::RenderDrawClusteredScene clustered{};
        clustered.scene = cull.scene;
        clustered.light_list = {cull.output.id, 1, token.invocation_id, token.generation};
        clustered.hdr = lighting.hdr;
        clustered.depth = {cull.camera_depth, aeg::feature::RenderAttachmentLoad::kLoad, 1.f};
        delta.render_intent->operations.push_back(clustered);
        return aeg::foundation::ResultValue<aeg::feature::StateDelta>{aeg::foundation::Result::success(), std::move(delta)};
    };
    const aeg::feature::InvocationToken invocation{aeg::feature::InvocationId(1), aeg::feature::AttemptId(2),
        aeg::feature::RuntimeGeneration(3)};
    const auto state_result = feature.execute(invocation, aeg::feature::StateSnapshot{}, {}, {});
    if (!state_result || state_result.value.state_payload != std::vector<uint8_t>{7}) return 4;
    if (std::get<aeg::feature::RenderDrawDepthScene>(state_result.value.render_intent->operations.front()).projection !=
        aeg::feature::RenderDepthSceneProjection::kDirectionalShadow) return 5;
    if (!std::get<aeg::feature::RenderDrawScene>(state_result.value.render_intent->operations[1]).directional_shadow)
        return 6;
    if (std::get<aeg::feature::RenderDrawGBufferScene>(state_result.value.render_intent->operations[2]).normal.target.id !=
        aeg::feature::RenderResourceId("Renderer.GBuffer.Normal")) return 7;
    const auto& lighting = std::get<aeg::feature::RenderDeferredLightingScene>(state_result.value.render_intent->operations[3]);
    if (lighting.hdr.target.id != aeg::feature::RenderResourceId("Renderer.SceneLighting") || !lighting.directional_shadow)
        return 8;
    const auto& post = std::get<aeg::feature::RenderPostProcessScene>(state_result.value.render_intent->operations[4]);
    if (!post.settings || post.settings->exposure != .5f || post.hdr.id != lighting.hdr.target.id) return 9;
    const auto& clustered = std::get<aeg::feature::RenderDrawClusteredScene>(state_result.value.render_intent->operations.back());
    const auto light_payload = aeg::feature::encode_render_light_list_reference(clustered.light_list);
    if (!light_payload || light_payload.value.size() != 32) return 10;
    const auto light_reference = aeg::feature::decode_render_light_list_reference(light_payload.value);
    if (!light_reference || light_reference.value != clustered.light_list ||
        light_reference.value.producer_invocation != invocation.invocation_id ||
        light_reference.value.producer_generation != invocation.generation) return 11;
    const auto typed = aeg::feature::serialize_parameter_value(int64_t{5}, aeg::feature::ParameterType::kInt32);
    if (!typed || typed.value != std::vector<uint8_t>{5,0,0,0}) return 2;
    const auto typed_value = aeg::feature::deserialize_parameter_value(typed.value, aeg::feature::ParameterType::kInt32);
    if (!typed_value || std::get<int64_t>(typed_value.value) != 5) return 3;
    const aeg::feature::WorldTransformValue before{{5.f, -2.f, 3.5f}, {0.f, 0.f, .6f, .8f}, {1.f, 2.f, 3.f}};
    const aeg::feature::WorldTransformValue after{{7.f, -4.f, -2.f}, {0.f, .6f, 0.f, .8f}, {2.f, 3.f, 4.f}};
    const auto command_bytes = aeg::feature::encode_world_transform_command({47, before, after});
    if (!command_bytes || command_bytes.value.effect != aeg::feature::SideEffectId("World.Transform") ||
        command_bytes.value.version != 1) return 14;
    const auto command = aeg::feature::decode_world_transform_command(command_bytes.value);
    if (!command || command.value.expected_world_revision != 47 || command.value.expected != before ||
        command.value.replacement != after) return 15;
    aeg::feature::WorldTransformInput observation{{47, before, .025, .125}, {9, 0, 255, 4, 0, 17}};
    const auto observation_bytes = aeg::feature::encode_world_transform_input(observation);
    if (!observation_bytes) return 16;
    const auto observation_value = aeg::feature::decode_world_transform_input(observation_bytes.value);
    observation.observation.before.translation[0] = 999.f;
    observation.channel_input.assign(1, 42);
    if (!observation_value || observation_value.value.observation.world_revision != 47 ||
        observation_value.value.observation.before != before || observation_value.value.observation.elapsed_seconds != .025 ||
        observation_value.value.observation.frame_delta_seconds != .125 ||
        observation_value.value.channel_input != std::vector<uint8_t>{9, 0, 255, 4, 0, 17}) return 17;
    const auto observation_roundtrip = aeg::feature::encode_world_transform_input(observation_value.value);
    if (!observation_roundtrip || observation_roundtrip.value != observation_bytes.value) return 18;
    aeg::module::FeatureModuleDefinition definition{};
    definition.descriptor.id = aeg::module::ModuleId("GameSdkBoundary");
    aeg::application::runtime::RuntimeBootRequest request{};
    request.headless = true;
    aeg::application::runtime::RuntimeBootstrap runtime;
    const std::vector<aeg::feature::ParameterContract> schema{{aeg::feature::ParameterId(7), 1, 1, true}};
    const std::vector<aeg::feature::FeatureParameterRecord> values{{aeg::feature::ParameterId(7), 1, {9}}};
    const auto encoded = aeg::feature::encode_feature_parameters(values, schema);
    if (!encoded) return 1;
    const auto decoded = aeg::feature::decode_feature_parameters(encoded.value, schema);
    return definition.descriptor.id.is_valid() && request.headless && runtime.shutdown() &&
           decoded && decoded.value == values ? 0 : 1;
}
