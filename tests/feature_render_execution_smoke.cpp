#include "feature_clock_test_fixture.hpp"
#include "../Engine/RenderGraph/private/render_graph_execution.hpp"
#include <array>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

#include "feature_render_intent.hpp"
#include "feature_render_test_fixture.hpp"
#include "feature_render_gpu_fixture.hpp"
#include "render_feature_runtime.hpp"
#include "render_feature_history.hpp"
#include "rhi.hpp"
#ifdef AEG_TEST_FEATURE_PRESENT
#include "application_platform.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vulkan/vulkan.h>
extern "C" PFN_vkQueuePresentKHR __imp_vkQueuePresentKHR;
#endif

namespace {
using namespace aeg;
using namespace aeg::feature;

using namespace render_feature_test;


#ifdef AEG_TEST_FEATURE_PRESENT
// Inject at the native API boundary, after a real present consumes the Vulkan
// semaphore. Production RHI and FrameContext keep their normal implementations.
class PresentFailure final {
public:
    PresentFailure() : original_(__imp_vkQueuePresentKHR) {
        active_ = this;
        replace(&fail_after_present);
    }
    ~PresentFailure() { replace(original_); active_ = nullptr; }
    bool called{false};
private:
    static void replace(PFN_vkQueuePresentKHR value) {
        DWORD protection{};
        if (!VirtualProtect(&__imp_vkQueuePresentKHR, sizeof(value), PAGE_READWRITE, &protection)) std::terminate();
        __imp_vkQueuePresentKHR = value;
        DWORD ignored{};
        if (!VirtualProtect(&__imp_vkQueuePresentKHR, sizeof(value), protection, &ignored)) std::terminate();
    }
    static VkResult VKAPI_CALL fail_after_present(VkQueue queue, const VkPresentInfoKHR* info) {
        active_->called = true;
        const auto result = active_->original_(queue, info);
        return result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR ? VK_ERROR_OUT_OF_DATE_KHR : result;
    }
    PFN_vkQueuePresentKHR original_{};
    inline static PresentFailure* active_{};
};

void present_attachment_only_swapchain(rhi::Device& device, FeatureRuntime& runtime,
                                      const CompiledFeatureGraph& compiled, bool inject_failure = false) {
    auto system = application::create_window_system();
    require(static_cast<bool>(system), "window system creation failed");
    platform::WindowDesc window_desc{};
    window_desc.extent = {64, 64};
    window_desc.visible = false;
    auto window = system.value->create_window(window_desc);
    require(static_cast<bool>(window), "hidden swapchain window failed");
    const auto binding = window.value->surface_binding();
    rhi::SurfaceDesc surface_desc{};
    surface_desc.native_display_handle = binding.display_handle;
    surface_desc.native_window_handle = binding.window_handle;
    const auto surface = device.create_surface(surface_desc);
    require(static_cast<bool>(surface), "Feature surface creation failed");
    rhi::SwapchainDesc swapchain_desc{};
    swapchain_desc.surface = surface.value;
    swapchain_desc.extent = {64, 64};
    require(swapchain_desc.image_capabilities == rhi::TextureCapability::kColorAttachment,
            "fixture did not use the default attachment-only swapchain");
    const auto swapchain = device.create_swapchain(swapchain_desc);
    require(static_cast<bool>(swapchain), "Feature swapchain creation failed");
    const auto info = device.get_swapchain_info(swapchain.value);
    require(static_cast<bool>(info), "Feature swapchain info failed");
    {
        render_graph::FrameContext frame(device);
        render_graph::RenderGraph graph(device);
        require(static_cast<bool>(frame.begin_frame()), "present frame begin failed");
        const auto acquired = frame.acquire_next_image(swapchain.value);
        require(static_cast<bool>(acquired), "Feature image acquisition failed");
        rhi::TextureDesc texture_desc{};
        texture_desc.extent = {info.value.extent.width, info.value.extent.height, 1};
        texture_desc.format = info.value.format;
        texture_desc.capabilities = swapchain_desc.image_capabilities;
        const auto target = graph.import_swapchain_image(acquired.value, texture_desc);
        require(static_cast<bool>(target), "Feature swapchain image import failed");
        // Match the host fallback: append Feature writes after an earlier Present pass.
        const auto previous = graph.add_pass("Host.Present", rhi::QueueClass::kGraphics,
            [texture = target.value](render_graph::PassBuilder& builder) {
                return builder.use_color_attachment(texture, rhi::LoadOp::kClear, rhi::StoreOp::kStore,
                                                    {0.f, 0.f, 0.f, 1.f}, rhi::ResourceUsage::kPresent);
            },
            [](render_graph::RenderPassCommandContext& command) {
                if (const auto result = command.begin_rendering(); !result) return result;
                return command.end_rendering();
            });
        require(static_cast<bool>(previous), "host Present pass failed");
        const auto captured = runtime.capture_render_outputs(compiled);
        require(static_cast<bool>(captured) && !captured.value.empty(), "present intent capture failed");
        auto output = captured.value.front();
        const RenderTextureReference color{RenderResourceId("Renderer.SceneColor"), 1, true};
        output.intent.textures.clear();
        output.intent.operations = {RenderClearColor{color, {1.f, 0.f, 0.f, 1.f}}};
        output.intent.final_output = color;
        render_graph::RenderFeatureHistory history(device);
        if (inject_failure) {
            const auto owners = runtime.capture_render_resource_owners(compiled);
            require(static_cast<bool>(owners) && static_cast<bool>(history.reconcile(owners.value)), "Present history owners failed");
            const RenderTextureReference retained{RenderResourceId("Present.History"), 1, false};
            output.intent.textures = {{retained.id, 1, 4, 4, RenderTextureLifetime::kHistory}};
            output.intent.operations.insert(output.intent.operations.begin(), RenderClearColor{retained, {0,0,1,1}});
        }
        const std::array bindings{render_graph::RenderGraphFeatureTextureBinding{color.id, target.value}};
        const auto outputs = std::make_shared<const std::vector<RenderFeatureOutput>>(
            std::vector<RenderFeatureOutput>{std::move(output)});
        require(static_cast<bool>(render_graph::build_render_graph_features(graph, outputs,
            {.textures = bindings}, 57, inject_failure ? &history : nullptr)),
                "attachment-only Feature Present build failed");
        if (inject_failure) {
            {
                PresentFailure failure;
                const auto executed = graph.execute(frame, {.present_acquired_frame = true});
                require(failure.called && !executed && executed.result.code == foundation::ResultCode::kOutOfDate,
                        "native Present failure injection did not reach the outlet");
            }
            require(!frame.is_frame_active() && history.size() == 0,
                    "Present failure left the history transaction/frame active");
            require(static_cast<bool>(frame.begin_frame()) && !graph.execute(frame) && !frame.is_frame_active(),
                    "Present failure permitted replay of its history transaction");
            graph.reset();
            require(static_cast<bool>(frame.drain()), "failed Present frame did not retire");
        } else {
            const auto executed = graph.execute(frame, {.present_acquired_frame = true});
            require(static_cast<bool>(executed) && executed.value.presented && executed.value.executed_pass_count == 2,
                    "Feature clear was not executed and presented");
            const auto& work = aeg::render_graph::detail::RenderGraphExecution::plan(graph).pass_plans.back().feature_work;
            require(work && work->frame_index == 57 && work->invocation.invocation_id.is_valid(),
                    "presented Feature pass lost provenance");
            const auto state = device.query_texture_state(acquired.value.image.texture);
            require(static_cast<bool>(state) && !state.value.subresource_states.empty() &&
                    state.value.subresource_states.front().usage == rhi::ResourceUsage::kPresent,
                    "last Feature write did not leave the native swapchain image in Present");
            require(static_cast<bool>(frame.end_frame()) && static_cast<bool>(frame.drain()), "present frame retirement failed");
        }
    }
    require(static_cast<bool>(device.destroy_swapchain(swapchain.value)) &&
            static_cast<bool>(device.destroy_surface(surface.value)), "present fixture cleanup failed");
}
#endif

void gbuffer_intents_commit_atomically() {
    RenderDrawGBufferScene draw{};
    draw.scene = {RenderResourceId("Scene.GBuffer"), 1};
    draw.albedo.target = {RenderResourceId("GBuffer.Albedo"), 1, true};
    draw.normal.target = {RenderResourceId("GBuffer.Normal"), 1, true};
    draw.material.target = {RenderResourceId("GBuffer.Material"), 1, true};
    draw.depth.target = {RenderResourceId("GBuffer.Depth"), 1, true};
    auto source = definition();
    source.execute = [draw](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>,
                           std::span<const uint8_t>) {
        StateDelta delta{token.state_version};
        delta.state_payload = std::vector<uint8_t>{42};
        delta.render_intent = RenderIntent{};
        delta.render_intent->operations = {draw};
        return foundation::ResultValue<StateDelta>{foundation::Result::success(), std::move(delta)};
    };
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    require(static_cast<bool>(aeg::feature::test::register_clock_test_definition(registry, source)), "GBuffer Feature registration failed");
    auto authored = document(source);
    auto compiled = FeatureGraphCompiler{}.compile(authored, registry);
    FeatureRuntime runtime(registry);
    require(compiled && runtime.publish_graph(authored, compiled.value), "GBuffer Feature publication failed");
    require(static_cast<bool>(runtime.tick(source.contract.clock.id)), "valid GBuffer intent failed owner commit");
    const auto id = compiled.value.provenance.front().instance_id;
    const auto before = runtime.find_instance(id)->state;
    require(before.payload == std::vector<uint8_t>{42}, "GBuffer intent lost its simultaneous state update");
    const auto token = runtime.invoke(id);
    require(static_cast<bool>(token), "GBuffer invalid-input invocation failed");
    for (unsigned case_index = 0; case_index < 22; ++case_index) {
        auto invalid = draw;
        switch (case_index) {
        case 0: invalid.scene.id = {}; break;
        case 1: invalid.scene.version = 0; break;
        case 2: invalid.albedo.target.id = {}; break;
        case 3: invalid.normal.target.version = 0; break;
        case 4: invalid.material.target.external = false; break;
        case 5: invalid.depth.target.external = false; break;
        case 6: invalid.albedo.load = static_cast<RenderAttachmentLoad>(255); break;
        case 7: invalid.normal.load = static_cast<RenderAttachmentLoad>(255); break;
        case 8: invalid.material.load = static_cast<RenderAttachmentLoad>(255); break;
        case 9: invalid.depth.load = static_cast<RenderAttachmentLoad>(255); break;
        case 10: invalid.albedo.clear_color[0] = std::numeric_limits<float>::quiet_NaN(); break;
        case 11: invalid.normal.clear_color[1] = std::numeric_limits<float>::infinity(); break;
        case 12: invalid.material.clear_color[2] = -std::numeric_limits<float>::infinity(); break;
        case 13: invalid.depth.clear_depth = std::numeric_limits<float>::quiet_NaN(); break;
        case 14: invalid.depth.clear_depth = 1.1f; break;
        case 15: invalid.normal.target = invalid.albedo.target; break;
        case 16: invalid.material.target = invalid.normal.target; break;
        case 17: invalid.depth.target = invalid.material.target; break;
        case 18: invalid.depth.clear_depth = -.1f; break;
        case 19: invalid.material.target = invalid.albedo.target; break;
        case 20: invalid.depth.target = invalid.albedo.target; break;
        case 21: invalid.depth.target = invalid.normal.target; break;
        }
        StateDelta rejected{token.value.state_version};
        rejected.state_payload = std::vector<uint8_t>{99};
        rejected.render_intent = RenderIntent{};
        rejected.render_intent->operations = {invalid};
        require(!runtime.complete(token.value, rejected), "malformed GBuffer intent committed");
        const auto after = runtime.capture_render_outputs(compiled.value);
        require(after && runtime.find_instance(id)->state.commit_count == before.commit_count &&
            runtime.find_instance(id)->state.payload == before.payload &&
            std::get<RenderDrawGBufferScene>(after.value.front().intent.operations.front()).normal.target.id == draw.normal.target.id,
            "invalid GBuffer intent partially changed state or accepted render work");
    }
    RenderIntent intent{};
    intent.operations = {draw};
    auto contract = source.contract;
    contract.permissions.capability_mask = to_mask(RenderIntentCapability::kDrawScene) | to_mask(RenderIntentCapability::kWriteExternal);
    require(static_cast<bool>(validate_render_intent(intent, contract)), "GBuffer Clear requires unnecessary permission");
    for (auto capability : {RenderIntentCapability::kDrawScene, RenderIntentCapability::kWriteExternal}) {
        auto denied = contract;
        denied.permissions.capability_mask &= ~to_mask(capability);
        require(!validate_render_intent(intent, denied), "GBuffer draw/write permission was not enforced");
    }
    for (unsigned role = 0; role < 4; ++role) {
        auto loading = draw;
        if (role == 0) loading.albedo.load = RenderAttachmentLoad::kLoad;
        if (role == 1) loading.normal.load = RenderAttachmentLoad::kLoad;
        if (role == 2) loading.material.load = RenderAttachmentLoad::kLoad;
        if (role == 3) loading.depth.load = RenderAttachmentLoad::kLoad;
        intent.operations = {loading};
        require(!validate_render_intent(intent, contract), "GBuffer attachment Load did not require external read permission");
        auto readable = contract;
        readable.permissions.capability_mask |= to_mask(RenderIntentCapability::kReadExternal);
        require(static_cast<bool>(validate_render_intent(intent, readable)), "valid GBuffer attachment Load was rejected");
    }
}

void deferred_lighting_intents_commit_atomically() {
    RenderDeferredLightingScene draw{};
    draw.scene = {RenderResourceId("Scene.Lighting"), 1};
    draw.albedo = {RenderResourceId("Lighting.Albedo"), 1, true};
    draw.normal = {RenderResourceId("Lighting.Normal"), 1, true};
    draw.depth = {RenderResourceId("Lighting.Depth"), 1, true};
    draw.hdr.target = {RenderResourceId("Lighting.HDR"), 1, true};
    draw.directional_shadow = RenderTextureReference{RenderResourceId("Lighting.Shadow"), 1, true};
    auto source = definition();
    source.execute = [draw](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>,
                           std::span<const uint8_t>) {
        StateDelta delta{token.state_version};
        delta.state_payload = std::vector<uint8_t>{42};
        delta.render_intent = RenderIntent{};
        delta.render_intent->operations = {draw};
        return foundation::ResultValue<StateDelta>{foundation::Result::success(), std::move(delta)};
    };
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    require(static_cast<bool>(aeg::feature::test::register_clock_test_definition(registry, source)), "lighting Feature registration failed");
    auto authored = document(source);
    auto compiled = FeatureGraphCompiler{}.compile(authored, registry);
    FeatureRuntime runtime(registry);
    require(compiled && runtime.publish_graph(authored, compiled.value), "lighting Feature publication failed");
    require(static_cast<bool>(runtime.tick(source.contract.clock.id)), "valid lighting intent failed owner commit");
    const auto id = compiled.value.provenance.front().instance_id;
    const auto before = runtime.find_instance(id)->state;
    const auto token = runtime.invoke(id);
    require(static_cast<bool>(token), "lighting invalid-input invocation failed");
    const auto reject = [&](const RenderDeferredLightingScene& invalid) {
        StateDelta delta{token.value.state_version};
        delta.state_payload = std::vector<uint8_t>{99};
        delta.render_intent = RenderIntent{};
        delta.render_intent->operations = {invalid};
        require(!runtime.complete(token.value, delta), "malformed lighting intent committed");
        const auto after = runtime.capture_render_outputs(compiled.value);
        require(after && runtime.find_instance(id)->state.commit_count == before.commit_count &&
                runtime.find_instance(id)->state.payload == before.payload &&
                std::get<RenderDeferredLightingScene>(after.value.front().intent.operations.front()).hdr.target.id == draw.hdr.target.id,
                "invalid lighting intent partially changed state or accepted work");
    };
    for (unsigned role = 0; role < 5; ++role) {
        for (unsigned defect = 0; defect < 3; ++defect) {
            auto invalid = draw;
            const std::array refs{&invalid.albedo, &invalid.normal, &invalid.depth, &invalid.hdr.target,
                                  &*invalid.directional_shadow};
            if (defect == 0) refs[role]->id = {};
            if (defect == 1) refs[role]->version = 0;
            if (defect == 2) refs[role]->external = false;
            reject(invalid);
        }
        for (unsigned other = role + 1; other < 5; ++other) {
            auto invalid = draw;
            const std::array refs{&invalid.albedo, &invalid.normal, &invalid.depth, &invalid.hdr.target,
                                  &*invalid.directional_shadow};
            *refs[role] = *refs[other];
            reject(invalid);
        }
    }
    for (unsigned defect = 0; defect < 5; ++defect) {
        auto invalid = draw;
        if (defect == 0) invalid.scene.id = {};
        if (defect == 1) invalid.scene.version = 0;
        if (defect == 2) invalid.hdr.load = static_cast<RenderAttachmentLoad>(255);
        if (defect == 3) invalid.hdr.clear_color[0] = std::numeric_limits<float>::quiet_NaN();
        if (defect == 4) invalid.hdr.clear_color[1] = std::numeric_limits<float>::infinity();
        reject(invalid);
    }
    RenderIntent intent{};
    intent.operations = {draw};
    auto contract = source.contract;
    contract.permissions.capability_mask = to_mask(RenderIntentCapability::kDrawScene) |
        to_mask(RenderIntentCapability::kWriteExternal) | to_mask(RenderIntentCapability::kReadExternal);
    require(static_cast<bool>(validate_render_intent(intent, contract)), "valid lighting permissions rejected");
    for (auto capability : {RenderIntentCapability::kDrawScene, RenderIntentCapability::kWriteExternal,
                             RenderIntentCapability::kReadExternal}) {
        auto denied = contract;
        denied.permissions.capability_mask &= ~to_mask(capability);
        require(!validate_render_intent(intent, denied), "lighting permission was not enforced");
    }
    draw.hdr.load = RenderAttachmentLoad::kLoad;
    draw.directional_shadow.reset();
    intent.operations = {draw};
    require(static_cast<bool>(validate_render_intent(intent, contract)), "valid unshadowed HDR Load was rejected");
}

void post_process_intents_commit_atomically() {
    RenderPostProcessScene draw{};
    draw.scene = {RenderResourceId("Post.Scene"), 1};
    draw.hdr = {RenderResourceId("Post.HDR"), 1, true};
    draw.depth = {RenderResourceId("Post.Depth"), 1, true};
    draw.output.target = {RenderResourceId("Post.Output"), 1, true};
    draw.settings = RenderPostProcessSettings{};
    draw.settings->exposure = .5f;
    auto source = definition();
    source.execute = [draw](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>,
                           std::span<const uint8_t>) {
        StateDelta delta{token.state_version};
        delta.state_payload = std::vector<uint8_t>{42};
        delta.render_intent = RenderIntent{};
        delta.render_intent->operations = {draw};
        return foundation::ResultValue<StateDelta>{foundation::Result::success(), std::move(delta)};
    };
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    require(static_cast<bool>(aeg::feature::test::register_clock_test_definition(registry, source)), "postprocess Feature registration failed");
    auto authored = document(source);
    auto compiled = FeatureGraphCompiler{}.compile(authored, registry);
    FeatureRuntime runtime(registry);
    require(compiled && runtime.publish_graph(authored, compiled.value), "postprocess Feature publication failed");
    require(static_cast<bool>(runtime.tick(source.contract.clock.id)), "valid postprocess intent failed owner commit");
    const auto id = compiled.value.provenance.front().instance_id;
    const auto before = runtime.find_instance(id)->state;
    const auto token = runtime.invoke(id);
    require(static_cast<bool>(token), "postprocess invocation failed");
    const auto reject = [&](const RenderPostProcessScene& invalid) {
        StateDelta delta{token.value.state_version};
        delta.state_payload = std::vector<uint8_t>{99};
        delta.render_intent = RenderIntent{};
        delta.render_intent->operations = {invalid};
        require(!runtime.complete(token.value, delta), "malformed postprocess intent committed");
        const auto after = runtime.capture_render_outputs(compiled.value);
        require(after && runtime.find_instance(id)->state.commit_count == before.commit_count &&
                runtime.find_instance(id)->state.version == before.version &&
                runtime.find_instance(id)->state.payload_revision == before.payload_revision &&
                runtime.find_instance(id)->state.payload == before.payload &&
                after.value.size() == 2 &&
                std::all_of(after.value.begin(), after.value.end(), [&](const auto& output) {
                    return output.intent.operations.size() == 1 &&
                        std::get<RenderPostProcessScene>(output.intent.operations.front()).settings->exposure == .5f &&
                        std::get<RenderPostProcessScene>(output.intent.operations.front()).output.target.id == draw.output.target.id;
                }),
                "invalid postprocess intent partially changed state or work");
    };
    for (unsigned role = 0; role < 3; ++role) {
        for (unsigned defect = 0; defect < 3; ++defect) {
            auto invalid = draw;
            const std::array refs{&invalid.hdr, &invalid.depth, &invalid.output.target};
            if (defect == 0) refs[role]->id = {};
            if (defect == 1) refs[role]->version = 0;
            if (defect == 2) refs[role]->external = false;
            reject(invalid);
        }
        for (unsigned other = role + 1; other < 3; ++other) {
            auto invalid = draw;
            const std::array refs{&invalid.hdr, &invalid.depth, &invalid.output.target};
            *refs[role] = *refs[other];
            reject(invalid);
        }
    }
    for (unsigned defect = 0; defect < 10; ++defect) {
        auto invalid = draw;
        if (defect == 0) invalid.scene.id = {};
        if (defect == 1) invalid.scene.version = 0;
        if (defect == 2) invalid.output.load = static_cast<RenderAttachmentLoad>(255);
        if (defect == 3) invalid.output.clear_color[0] = std::numeric_limits<float>::quiet_NaN();
        if (defect == 4) invalid.settings->exposure = -1;
        if (defect == 5) invalid.settings->gamma = 0;
        if (defect == 6) invalid.settings->gamma = std::numeric_limits<float>::denorm_min();
        if (defect == 7) invalid.settings->sky_zenith[0] = -1;
        if (defect == 8) invalid.settings->sky_horizon[1] = std::numeric_limits<float>::infinity();
        if (defect == 9) invalid.settings->sky_ground[2] = std::numeric_limits<float>::quiet_NaN();
        reject(invalid);
    }
    RenderIntent intent{};
    intent.operations = {draw};
    auto contract = source.contract;
    for (auto capability : {RenderIntentCapability::kDrawScene, RenderIntentCapability::kWriteExternal,
                            RenderIntentCapability::kReadExternal}) {
        auto denied = contract;
        denied.permissions.capability_mask &= ~to_mask(capability);
        require(!validate_render_intent(intent, denied), "postprocess permission was not enforced");
    }
    draw.settings.reset();
    draw.output.load = RenderAttachmentLoad::kLoad;
    intent.operations = {draw};
    require(static_cast<bool>(validate_render_intent(intent, contract)), "postprocess View defaults/Load rejected");
    StateDelta accepted{token.value.state_version};
    accepted.render_intent = intent;
    require(static_cast<bool>(runtime.complete(token.value, accepted)), "rejected postprocess intent consumed the commit token");
}

void light_list_intents_commit_atomically() {
    auto source = definition();
    source.contract.ports = {{PortId("LightList"), PortDirection::kOutput, kRenderLightListSchema, 1}};
    source.execute = [](const InvocationToken& token, const StateSnapshot&, std::span<const uint8_t>,
                        std::span<const uint8_t>) {
        RenderLightCullingScene cull{{RenderResourceId("Scene"), 1},
            {RenderResourceId("Depth"), 1, true}, {RenderResourceId("Lights"), 1, PortId("LightList")}};
        RenderDrawClusteredScene draw{};
        draw.scene = cull.scene;
        draw.light_list = {cull.output.id, cull.output.version, token.invocation_id, token.generation};
        draw.hdr.target = {RenderResourceId("HDR"), 1, true};
        draw.depth = {cull.camera_depth, RenderAttachmentLoad::kLoad, 1.f};
        StateDelta delta{token.state_version};
        delta.state_payload = std::vector<uint8_t>{42};
        delta.render_intent = RenderIntent{};
        delta.render_intent->operations = {cull, draw};
        return foundation::ResultValue<StateDelta>{foundation::Result::success(), std::move(delta)};
    };
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    require(bool(aeg::feature::test::register_clock_test_definition(registry, source)), "light-list Feature registration failed");
    auto authored = document(source);
    auto compiled = FeatureGraphCompiler{}.compile(authored, registry);
    FeatureRuntime runtime(registry);
    require(compiled && runtime.publish_graph(authored, compiled.value), "light-list Feature publication failed");
    require(bool(runtime.tick(source.contract.clock.id)), "valid light-list intent failed owner commit");
    const auto id = compiled.value.provenance.front().instance_id;
    const auto before = runtime.find_instance(id)->state;
    const auto committed = runtime.capture_render_outputs(compiled.value);
    require(committed && committed.value.size() == 2, "light-list committed outputs missing");
    const auto token = runtime.invoke(id);
    require(bool(token), "light-list rejection invocation failed");
    auto valid = committed.value.front().intent;
    auto& valid_reference = std::get<RenderDrawClusteredScene>(valid.operations[1]).light_list;
    valid_reference.producer_invocation = token.value.invocation_id;
    valid_reference.producer_generation = token.value.generation;
    for (unsigned defect = 0; defect < 27; ++defect) {
        auto invalid = valid;
        auto& cull = std::get<RenderLightCullingScene>(invalid.operations[0]);
        auto& draw = std::get<RenderDrawClusteredScene>(invalid.operations[1]);
        switch (defect) {
            case 0: cull.scene.id = {}; break;
            case 1: cull.scene.version = 0; break;
            case 2: cull.camera_depth.id = {}; break;
            case 3: cull.camera_depth.version = 0; break;
            case 4: cull.camera_depth.external = false; break;
            case 5: cull.output.id = {}; break;
            case 6: cull.output.version = 0; break;
            case 7: cull.output.export_port = PortId("Undeclared"); break;
            case 8: invalid.operations.push_back(cull); break;
            case 9: { auto duplicate = cull; ++duplicate.output.version;
                      invalid.operations.push_back(duplicate); break; }
            case 10: cull.directional_shadow = cull.camera_depth; break;
            case 11: cull.directional_shadow = RenderTextureReference{RenderResourceId("Shadow"), 1, false}; break;
            case 12: draw.scene.id = {}; break;
            case 13: draw.scene.version = 0; break;
            case 14: draw.light_list.id = {}; break;
            case 15: draw.light_list.version = 0; break;
            case 16: draw.light_list.producer_invocation = {}; break;
            case 17: draw.light_list.producer_generation = {}; break;
            case 18: draw.depth.load = RenderAttachmentLoad::kClear; break;
            case 19: draw.depth.target.external = false; break;
            case 20: draw.depth.target = draw.hdr.target; break;
            case 21: draw.hdr.load = static_cast<RenderAttachmentLoad>(255); break;
            case 22: draw.hdr.clear_color[0] = std::numeric_limits<float>::quiet_NaN(); break;
            case 23: draw.directional_shadow = draw.depth.target; break;
            case 24: draw.directional_shadow = draw.hdr.target; break;
            case 25: draw.directional_shadow = RenderTextureReference{RenderResourceId("Shadow"), 0, true}; break;
            case 26: draw.depth.clear_depth = std::numeric_limits<float>::infinity(); break;
        }
        StateDelta rejected{token.value.state_version};
        rejected.state_payload = std::vector<uint8_t>{99};
        rejected.render_intent = std::move(invalid);
        require(!runtime.complete(token.value, rejected), "malformed light-list intent committed");
        const auto after = runtime.capture_render_outputs(compiled.value);
        require(after && after.value.size() == committed.value.size() &&
            runtime.find_instance(id)->state.commit_count == before.commit_count &&
            runtime.find_instance(id)->state.version == before.version &&
            runtime.find_instance(id)->state.payload_revision == before.payload_revision &&
            runtime.find_instance(id)->state.payload == before.payload,
            "rejected light-list intent partially changed accepted state");
        for (size_t index = 0; index < after.value.size(); ++index)
            require(after.value[index].invocation == committed.value[index].invocation &&
                after.value[index].input_messages.size() == committed.value[index].input_messages.size() &&
                after.value[index].intent.operations.size() == 2 &&
                std::get<RenderDrawClusteredScene>(after.value[index].intent.operations[1]).light_list ==
                    std::get<RenderDrawClusteredScene>(committed.value[index].intent.operations[1]).light_list,
                "rejected light-list intent replaced accepted output/provenance");
    }
    for (const auto capability : {RenderIntentCapability::kCullLights, RenderIntentCapability::kReadLightList,
                                 RenderIntentCapability::kDrawScene, RenderIntentCapability::kReadExternal,
                                 RenderIntentCapability::kWriteExternal}) {
        auto denied = source.contract;
        denied.permissions.capability_mask &= ~to_mask(capability);
        require(!validate_render_intent(valid, denied), "light-list capability boundary was bypassed");
    }
    StateDelta accepted{token.value.state_version};
    accepted.state_payload = std::vector<uint8_t>{43};
    accepted.render_intent = std::move(valid);
    require(bool(runtime.complete(token.value, accepted)) && runtime.find_instance(id)->state.commit_count == before.commit_count + 1,
        "rejected light-list intents consumed the valid owner token");
}

void invalid_intent_is_an_atomic_failure(FeatureRuntime& runtime, const CompiledFeatureGraph& compiled) {
    const auto id = compiled.provenance.front().instance_id;
    const auto before = runtime.find_instance(id)->state;
    const auto captured = runtime.capture_render_outputs(compiled);
    require(static_cast<bool>(captured), "intent capture failed");
    const auto token = runtime.invoke(id);
    require(static_cast<bool>(token), "invalid intent invocation failed");
    StateDelta delta{token.value.state_version};
    delta.state_payload = std::vector<uint8_t>{99};
    delta.render_intent = captured.value.front().intent;
    std::get<RenderClearColor>(delta.render_intent->operations.front()).color[0] = std::numeric_limits<float>::quiet_NaN();
    require(!runtime.complete(token.value, delta), "invalid render intent committed");
    require(runtime.find_instance(id)->state.commit_count == before.commit_count && runtime.find_instance(id)->state.payload == before.payload,
            "invalid render intent partially changed Feature state");
    const auto after = runtime.capture_render_outputs(compiled);
    require(static_cast<bool>(after) && std::get<RenderClearColor>(after.value.front().intent.operations.front()).color[0] == 1.f,
            "invalid render intent replaced accepted GPU work");
    auto logic = definition().contract;
    logic.domains = to_mask(FeatureDomain::kLogic);
    require(!validate_render_intent(captured.value.front().intent, logic), "Logic Feature accepted a Render domain intent");
    StateDelta clear{token.value.state_version};
    clear.render_intent = RenderIntent{};
    require(static_cast<bool>(runtime.complete(token.value, clear)), "explicit empty render intent failed");
    const auto cleared = runtime.capture_render_outputs(compiled);
    require(static_cast<bool>(cleared) && cleared.value.front().intent.operations.empty(), "empty intent did not retire prior work");
    require(static_cast<bool>(runtime.tick(ClockId("Frame"))), "presentation restore tick failed");
}

void rejected_bindings_roll_back_the_whole_build(rhi::Device& device, FeatureRuntime& runtime,
                                                const CompiledFeatureGraph& compiled,
                                                unsigned first_case = 0, unsigned last_case = 12) {
    const auto captured = runtime.capture_render_outputs(compiled);
    require(static_cast<bool>(captured), "binding test capture failed");
    for (unsigned failure = first_case; failure < last_case; ++failure) {
        render_graph::RenderGraph graph(device), foreign(device);
        rhi::TextureDesc desc{};
        desc.extent = {4, 4, 1};
        desc.format = rhi::Format::kRgba8Unorm;
        desc.capabilities = rhi::TextureCapability::kTransferSource | rhi::TextureCapability::kTransferDestination;
        const auto red = graph.create_texture(desc);
        if (failure == 4) desc.extent = {2, 2, 1};
        if (failure == 8) desc.format = rhi::Format::kD32Float;
        if (failure == 9) desc.capabilities = rhi::TextureCapability::kColorAttachment;
        if (failure == 10) desc.samples = rhi::SampleCount::kX4;
        const auto blue = graph.create_texture(desc);
        require(static_cast<bool>(red) && static_cast<bool>(blue), "binding targets failed");
        std::vector<render_graph::RenderGraphFeatureTextureBinding> bindings{
            {RenderResourceId("Target.Red"), red.value, 1, captured.value[0].instance_id},
            {RenderResourceId("Target.Blue"), blue.value, 1, captured.value[1].instance_id}};
        auto outputs = captured.value;
        if (failure == 0) bindings.pop_back();
        if (failure == 1) bindings[1].version = 2;
        if (failure == 2) bindings[1].owner = outputs[0].instance_id;
        if (failure == 3) {
            const auto alien = foreign.create_texture(desc);
            require(static_cast<bool>(alien), "foreign target failed");
            bindings[1].texture = alien.value;
        }
        if (failure == 5) outputs[1].contract.permissions.capability_mask &= ~to_mask(RenderIntentCapability::kWriteExternal);
        if (failure == 6) {
            bindings[0].owner = {};
            bindings[1].texture = red.value;
            auto& copy = std::get<RenderCopyTexture>(outputs[1].intent.operations[1]);
            copy.source = {RenderResourceId("Target.Red"), 1, true};
        }
        rhi::TextureHandle native{};
        if (failure == 7 || failure == 11) {
            const auto created = device.create_texture(desc);
            require(static_cast<bool>(created), "native alias texture failed");
            native = created.value;
            const auto first = graph.import_texture(native, {}, desc);
            const auto second = graph.import_texture(native, {}, desc);
            require(static_cast<bool>(first) && static_cast<bool>(second), "native alias import failed");
            bindings[0].texture = first.value;
            bindings[0].owner = {};
            if (failure == 7) {
                bindings[1].texture = second.value;
                std::get<RenderCopyTexture>(outputs[1].intent.operations[1]).source = {RenderResourceId("Target.Red"), 1, true};
            } else {
                bindings[1].owner = {};
                const RenderTextureReference alias{RenderResourceId("Alias.Source"), 1, true};
                bindings.push_back({alias.id, second.value});
                const RenderTextureReference target{RenderResourceId("Target.Blue"), 1, true};
                outputs[0].intent.operations = {
                    RenderClearColor{{RenderResourceId("Target.Red"), 1, true}, {1.f, 0.f, 0.f, 1.f}},
                    RenderCopyTexture{alias, target, 4, 4}};
                outputs[0].intent.final_output = target;
            }
        }
        if (failure == 8) outputs[1].intent.operations = {
            RenderClearColor{{RenderResourceId("Target.Blue"), 1, true}, {1.f, 0.f, 0.f, 1.f}}};
        const unsigned baseline_resources = failure == 7 || failure == 11 ? 4 : 2;
        require(static_cast<bool>(graph.compile()) && aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).resource_count == baseline_resources, "baseline graph compile failed");
        const auto built = render_graph::build_render_graph_features(
            graph, std::make_shared<const std::vector<RenderFeatureOutput>>(outputs), {.textures = bindings}, 43);
        if (built) throw std::runtime_error("invalid render binding accepted in case " + std::to_string(failure));
        require(static_cast<bool>(graph.compile()) && aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).pass_count == 0 &&
                aeg::render_graph::detail::RenderGraphExecution::compile_info(graph).resource_count == baseline_resources, "failed Feature build left partial passes or local resources");
        graph.reset();
        if (native.is_valid()) require(static_cast<bool>(device.destroy_texture(native)), "native alias cleanup failed");
    }
}

void partial_copy_preserves_imported_contents(rhi::Device& device, FeatureRuntime& runtime,
                                            const CompiledFeatureGraph& compiled) {
    rhi::TextureDesc desc{};
    desc.extent = {8, 8, 1};
    desc.format = rhi::Format::kRgba8Unorm;
    desc.mip_levels = 2;
    desc.array_layers = 2;
    desc.capabilities = rhi::TextureCapability::kTransferSource | rhi::TextureCapability::kTransferDestination;
    const auto native = device.create_texture(desc);
    require(static_cast<bool>(native), "preserved texture creation failed");
    {
        render_graph::FrameContext frame(device);
        {
            render_graph::RenderGraph initialize(device);
            const auto texture = initialize.import_texture(native.value, {}, desc);
            require(static_cast<bool>(texture) && static_cast<bool>(frame.begin_frame()), "preserved texture import failed");
            const auto fill = initialize.add_pass("Host.FillGreen", rhi::QueueClass::kGraphics,
                [target = texture.value](render_graph::PassBuilder& pass) {
                    return pass.write(target, rhi::ResourceUsage::kCopyDestination, rhi::PipelineStage::kCopy,
                                      rhi::ShaderStage::kAll, {rhi::Aspect::kColor, 0, 2, 0, 2}, true);
                },
                [target = texture.value](render_graph::RenderPassCommandContext& command) {
                    return command.clear_texture(target, {rhi::Aspect::kColor, 0, 2, 0, 2}, {0.f, 1.f, 0.f, 1.f});
                });
            const auto ready = initialize.add_pass("Host.PreserveForNextFrame", rhi::QueueClass::kGraphics,
                [target = texture.value](render_graph::PassBuilder& pass) {
                    pass.set_side_effects();
                    return pass.read(target, rhi::ResourceUsage::kCopySource, rhi::PipelineStage::kCopy,
                                     rhi::ShaderStage::kAll, {rhi::Aspect::kColor, 0, 2, 0, 2});
                },
                [](render_graph::RenderPassCommandContext&) { return foundation::Result::success(); });
            require(static_cast<bool>(fill) && static_cast<bool>(ready) && static_cast<bool>(initialize.export_texture(texture.value)) &&
                    static_cast<bool>(initialize.execute(frame)) && static_cast<bool>(frame.end_frame()) &&
                    static_cast<bool>(frame.drain()), "preserved texture initialization failed");
        }
        render_graph::RenderGraph graph(device);
        const auto target = graph.import_texture(native.value, {}, desc);
        require(static_cast<bool>(target) && static_cast<bool>(frame.begin_frame()), "partial copy frame failed");
        const auto captured = runtime.capture_render_outputs(compiled);
        require(static_cast<bool>(captured) && !captured.value.empty(), "partial copy capture failed");
        auto output = captured.value.front();
        const RenderTextureReference destination{RenderResourceId("Target.Preserved"), 1, true};
        std::get<RenderCopyTexture>(output.intent.operations.back()).destination = destination;
        output.intent.final_output = destination;
        const std::array bindings{render_graph::RenderGraphFeatureTextureBinding{destination.id, target.value}};
        const auto outputs = std::make_shared<const std::vector<RenderFeatureOutput>>(
            std::vector<RenderFeatureOutput>{std::move(output)});
        require(static_cast<bool>(render_graph::build_render_graph_features(graph, outputs, {.textures = bindings}, 58)),
                "partial Feature copy build failed");
        require(static_cast<bool>(graph.compile()), "partial Feature copy compile failed");
        bool preserves_target = false;
        for (const auto& barrier : aeg::render_graph::detail::RenderGraphExecution::plan(graph).pass_plans.back().barriers) {
            if (barrier.resource_kind != render_graph::RenderGraphResourceKind::kTexture ||
                barrier.resource_index != target.value.index) continue;
            require(!barrier.discard_contents && barrier.subresources.base_mip == 0 &&
                    barrier.subresources.mip_count == 1 && barrier.subresources.base_layer == 0 &&
                    barrier.subresources.layer_count == 1,
                    "partial copy discards untouched pixels or transitions untouched mip/layers");
            preserves_target = true;
        }
        require(preserves_target, "partial copy omitted its imported target barrier");
        const auto allocation = frame.allocate_readback(320, 256);
        require(static_cast<bool>(allocation), "preserved pixel allocation failed");
        rhi::BufferDesc buffer_desc{};
        buffer_desc.size = allocation.value.buffer_size;
        buffer_desc.memory = rhi::MemoryClass::kReadback;
        buffer_desc.capabilities = rhi::BufferCapability::kTransferDestination;
        const auto buffer = graph.import_buffer(allocation.value.buffer, buffer_desc);
        require(static_cast<bool>(buffer), "preserved pixel buffer import failed");
        const auto readback = graph.add_pass("Host.ReadPreserved", rhi::QueueClass::kCopy,
            [texture = target.value, destination_buffer = buffer.value, offset = allocation.value.offset]
            (render_graph::PassBuilder& pass) {
                if (const auto result = pass.read(texture, rhi::ResourceUsage::kCopySource, rhi::PipelineStage::kCopy,
                                                 rhi::ShaderStage::kAll, {rhi::Aspect::kColor, 0, 1, 0, 1}); !result) return result;
                if (const auto result = pass.read(texture, rhi::ResourceUsage::kCopySource, rhi::PipelineStage::kCopy,
                                                 rhi::ShaderStage::kAll, {rhi::Aspect::kColor, 1, 1, 1, 1}); !result) return result;
                pass.set_side_effects();
                return pass.write_to(destination_buffer, rhi::ResourceUsage::kCopyDestination, rhi::PipelineStage::kCopy,
                                     rhi::ShaderStage::kAll, rhi::ResourceUsage::kHostRead, {offset, 320}, true);
            },
            [texture = target.value, destination_buffer = buffer.value, offset = allocation.value.offset]
            (render_graph::RenderPassCommandContext& command) {
                if (const auto result = command.copy_texture_to_buffer(texture, {rhi::Aspect::kColor, 0, 0, 1}, {},
                                                                       {8, 8, 1}, destination_buffer, offset, 0, 0); !result) return result;
                return command.copy_texture_to_buffer(texture, {rhi::Aspect::kColor, 1, 1, 1}, {},
                                                      {4, 4, 1}, destination_buffer, offset + 256, 0, 0);
            });
        require(static_cast<bool>(readback) && static_cast<bool>(graph.execute(frame)) &&
                static_cast<bool>(frame.end_frame()) && static_cast<bool>(frame.drain()), "preserved pixel execution failed");
        std::array<std::byte, 320> pixels{};
        require(static_cast<bool>(frame.read_readback(allocation.value, pixels)), "preserved pixel readback failed");
        for (unsigned pixel = 0; pixel < 80; ++pixel) {
            const bool red = pixel < 64 && pixel / 8 < 4 && pixel % 8 < 4;
            require(pixels[pixel * 4] == (red ? std::byte{255} : std::byte{0}) &&
                    pixels[pixel * 4 + 1] == (red ? std::byte{0} : std::byte{255}) &&
                    pixels[pixel * 4 + 2] == std::byte{0} && pixels[pixel * 4 + 3] == std::byte{255},
                    "partial copy lost pixels or an untouched mip/layer");
        }
    }
    require(static_cast<bool>(device.destroy_texture(native.value)), "preserved texture cleanup failed");
}

void composite_capture_and_generation_retirement(FeatureRegistry& registry, FeatureRuntime& runtime) {
    auto child = definition();
    const std::vector<FeatureParameterRecord> records{{ParameterId("Channel"), 1, {1}}};
    const auto params = encode_feature_parameters(records, child.contract.parameters);
    require(static_cast<bool>(params), "child parameter encoding failed");
    child.id = FeatureDefinitionId("Render.CompositeChild");
    child.contract.definition_id = child.id;
    child.contract.parameters.clear();
    child.execute = [execute = child.execute, parameters = params.value](const InvocationToken& token, const StateSnapshot& state,
                                                                       std::span<const uint8_t> input, std::span<const uint8_t>) {
        return execute(token, state, input, parameters);
    };
    require(static_cast<bool>(aeg::feature::test::register_clock_test_definition(registry, child)), "child registration failed");
    auto parent = child;
    parent.id = FeatureDefinitionId("Render.Composite");
    parent.contract.definition_id = parent.id;
    parent.contract.structure = FeatureStructure::kComposite; parent.contract.clock.work_budget_ns = 0;
    parent.contract.execution = ExecutionKind::kCall;
    parent.contract.state = StatePolicy::kStateless;
    parent.contract.children = {child.id};
    parent.execute = {};
    require(static_cast<bool>(aeg::feature::test::register_clock_test_definition(registry, parent)), "parent registration failed");
    FeatureGraphDocument authored{};
    authored.asset_uuid = "asset://render-composite";
    authored.schema_version = 1;
    authored.graph_version = 1;
    authored.nodes = {{FeatureNodeId("Parent"), parent.id, parent.version}};
    auto compiled = FeatureGraphCompiler{}.compile(authored, registry);
    require(static_cast<bool>(compiled) && static_cast<bool>(runtime.publish_graph(authored, compiled.value)), "composite publication failed");
    require(static_cast<bool>(runtime.tick(ClockId("Frame"))), "composite child execution failed");
    auto outputs = runtime.capture_render_outputs(compiled.value);
    require(static_cast<bool>(outputs) && outputs.value.size() == 1 && outputs.value[0].contract.definition_id == child.id &&
            outputs.value[0].node_id == FeatureNodeId("Parent") && outputs.value[0].instance_id != compiled.value.provenance[0].instance_id,
            "composite leaf did not retain its author node and independent owner");
    auto replacement = child;
    replacement.version = {2, 0, 0};
    replacement.contract.version = replacement.version;
    require(static_cast<bool>(runtime.replace_definition(replacement, {{1,0,0},{2,0,0},{1,0,0},{1,0,0}})), "render definition replacement failed");
    outputs = runtime.capture_render_outputs(compiled.value);
    require(static_cast<bool>(outputs) && outputs.value.empty(), "old-generation render intent survived migration");
    require(static_cast<bool>(runtime.tick(ClockId("Frame"))), "new render generation failed");
    outputs = runtime.capture_render_outputs(compiled.value);
    require(static_cast<bool>(outputs) && outputs.value.size() == 1 && outputs.value[0].invocation.generation == runtime.generation(),
            "new render generation was not captured");
    auto next = FeatureGraphCompiler{}.compile(authored, registry);
    require(static_cast<bool>(next) && static_cast<bool>(runtime.publish_graph(authored, next.value)), "render graph republication failed");
    require(!runtime.capture_render_outputs(compiled.value), "retired graph captured new-instance render work");
    outputs = runtime.capture_render_outputs(next.value);
    require(static_cast<bool>(outputs) && outputs.value.empty(), "graph republication retained old render work");
}
}

int main(int argc, char** argv) {
    try {
        gbuffer_intents_commit_atomically();
        deferred_lighting_intents_commit_atomically();
        post_process_intents_commit_atomically();
        light_list_intents_commit_atomically();
        FeatureRegistry registry{aeg::feature::test::clock_test_host()};
        const auto source = definition();
        require(static_cast<bool>(aeg::feature::test::register_clock_test_definition(registry, source)), "Feature registration failed");
        auto authored = document(source);
        auto compiled = FeatureGraphCompiler{}.compile(authored, registry);
        FeatureRuntime runtime(registry);
        require(static_cast<bool>(compiled) && static_cast<bool>(runtime.publish_graph(authored, compiled.value)), "Feature publication failed");
        require(static_cast<bool>(runtime.tick(source.contract.clock.id)), "Render Feature callback failed");
        auto device = rhi::create_device({});
        require(static_cast<bool>(device), "GPU device creation failed");
        if (argc == 3 && std::string(argv[1]) == "--binding-case") {
            const auto test_case = static_cast<unsigned>(std::stoul(argv[2]));
            rejected_bindings_roll_back_the_whole_build(*device.value, runtime, compiled.value, test_case, test_case + 1);
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--partial-copy") {
            partial_copy_preserves_imported_contents(*device.value, runtime, compiled.value);
            return 0;
        }
#ifdef AEG_TEST_FEATURE_PRESENT
        if (argc == 2 && std::string(argv[1]) == "--present-failure") {
            present_attachment_only_swapchain(*device.value, runtime, compiled.value, true);
            return 0;
        }
#endif
        execute_pixels_and_retire_owners(*device.value, runtime.capture_render_outputs(compiled.value));
        execute_pixels_and_retire_owners(*device.value, runtime.capture_render_outputs(compiled.value), true);
        execute_pixels_and_retire_owners(*device.value, runtime.capture_render_outputs(compiled.value), true, true);
#ifdef AEG_TEST_FEATURE_PRESENT
        present_attachment_only_swapchain(*device.value, runtime, compiled.value);
        present_attachment_only_swapchain(*device.value, runtime, compiled.value, true);
#endif
        invalid_intent_is_an_atomic_failure(runtime, compiled.value);
        rejected_bindings_roll_back_the_whole_build(*device.value, runtime, compiled.value);
        partial_copy_preserves_imported_contents(*device.value, runtime, compiled.value);
        composite_capture_and_generation_retirement(registry, runtime);
        std::cout << "Feature RenderGraph execution smoke passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
