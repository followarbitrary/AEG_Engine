/**
 * @file frame_retention_smoke.cpp
 * @brief Verifies frame-slot ownership retirement with real queue submissions.
 */
#include <frame_context.hpp>
#include <array>
#include <iostream>
#include <memory>
#include <pipeline.hpp>
#include <rhi.hpp>

namespace {
aeg::foundation::Result submit_empty(aeg::render_graph::FrameContext& frame, aeg::rhi::QueueClass queue) {
    const auto command = frame.begin_command_list(queue);
    if (!command) {
        return command.result;
    }
    if (const auto result = frame.end_command_list(command.value); !result) {
        return result;
    }
    aeg::rhi::SubmitDesc desc{};
    desc.queue = queue;
    desc.command_lists = {&command.value, 1};
    return frame.submit(desc).result;
}
}  // namespace

int main() {
    using aeg::foundation::ResultCode;
    auto device = aeg::rhi::create_device({});
    if (!device) {
        return 1;
    }
    std::weak_ptr<const aeg::pipeline::PipelinePlan> weak_plan;
    {
        aeg::render_graph::FrameContext frame(*device.value, {.frames_in_flight = 2});
        auto plan = std::make_shared<const aeg::pipeline::PipelinePlan>();
        weak_plan = plan;
        if (auto result = frame.retain_until_complete(plan); result || result.code != ResultCode::kInvalidState) {
            return 2;
        }
        if (!frame.begin_frame()) {
            return 3;
        }
        if (auto result = frame.retain_until_complete({}); result || result.code != ResultCode::kInvalidArgument) {
            return 4;
        }
        // Alias pointers may be null while still carrying valid ownership.
        std::shared_ptr<const void> alias(plan, nullptr);
        if (!frame.retain_until_complete(alias)) {
            return 5;
        }
        alias.reset();
        plan.reset();
        for (auto queue :
             {aeg::rhi::QueueClass::kGraphics, aeg::rhi::QueueClass::kCompute, aeg::rhi::QueueClass::kCopy}) {
            if (!submit_empty(frame, queue)) {
                return 6;
            }
        }
        if (!frame.end_frame() || weak_plan.expired()) {
            return 7;
        }
        if (!frame.begin_frame() || weak_plan.expired() || !frame.end_frame()) {
            return 8;
        }
        if (!frame.begin_frame() || !weak_plan.expired()) {
            return 9;
        }
        plan = std::make_shared<const aeg::pipeline::PipelinePlan>();
        weak_plan = plan;
        if (!frame.retain_until_complete(plan)) {
            return 10;
        }
        plan.reset();
        if (!submit_empty(frame, aeg::rhi::QueueClass::kGraphics) || !frame.abort_frame() || !weak_plan.expired()) {
            return 11;
        }
        if (!frame.begin_frame()) {
            return 12;
        }
        plan = std::make_shared<const aeg::pipeline::PipelinePlan>();
        weak_plan = plan;
        if (!frame.retain_until_complete(plan)) {
            return 13;
        }
        plan.reset();
        if (!submit_empty(frame, aeg::rhi::QueueClass::kGraphics) || !frame.end_frame()) {
            return 14;
        }
    }
    if (!weak_plan.expired())
        return 15;
    aeg::render_graph::FrameContext drain_frame(*device.value, {.frames_in_flight = 2});
    std::weak_ptr<const aeg::pipeline::PipelinePlan> owners[2];
    for (auto& owner : owners) {
        if (!drain_frame.begin_frame())
            return 16;
        if (const auto active_drain = drain_frame.drain();
            active_drain || active_drain.code != ResultCode::kInvalidState)
            return 17;
        auto retained = std::make_shared<const aeg::pipeline::PipelinePlan>();
        owner = retained;
        if (!drain_frame.retain_until_complete(retained) ||
            !submit_empty(drain_frame, aeg::rhi::QueueClass::kGraphics) || !drain_frame.end_frame())
            return 18;
    }
    const auto number = drain_frame.frame_number();
    if (owners[0].expired() || owners[1].expired() || !drain_frame.drain() || !owners[0].expired() ||
        !owners[1].expired() || drain_frame.frame_number() != number)
        return 19;
    if (!drain_frame.drain() || !drain_frame.begin_frame() ||
        !submit_empty(drain_frame, aeg::rhi::QueueClass::kGraphics) || !drain_frame.end_frame())
        return 20;

    // A descriptor deferred in the second slot still references the first slot's view.
    // Global drain must retire descriptors from every completed slot before any view.
    aeg::rhi::TextureDesc texture_desc{};
    texture_desc.extent = {4, 4, 1};
    texture_desc.format = aeg::rhi::Format::kRgba8Unorm;
    texture_desc.capabilities =
        aeg::rhi::TextureCapability::kSampled | aeg::rhi::TextureCapability::kTransferDestination;
    const auto texture = device.value->create_texture(texture_desc);
    if (!texture) {
        std::cerr << "cross-slot texture creation: " << texture.result.message << '\n';
        return 37;
    }
    const auto view = device.value->create_texture_view({.texture = texture.value,
                                                        .kind = aeg::rhi::TextureViewKind::kSampled});
    if (!view) {
        std::cerr << "cross-slot texture view creation: " << view.result.message << '\n';
        return 38;
    }
    const std::array<aeg::rhi::BindingLayoutEntry, 1> entries{{{
        .location = {0, 0},
        .type = aeg::rhi::BindingType::kSampledTexture,
        .shader_stages = aeg::rhi::ShaderStage::kFragment,
    }}};
    const auto layout = device.value->create_binding_layout({.entries = entries});
    if (!layout) {
        std::cerr << "cross-slot binding layout creation: " << layout.result.message << '\n';
        return 39;
    }
    const auto set = device.value->create_binding_set({.layout = layout.value,
                                                       .lifetime = aeg::rhi::BindingSetLifetime::kPersistent});
    if (!set) {
        std::cerr << "cross-slot binding set creation: " << set.result.message << '\n';
        return 40;
    }
    const aeg::rhi::BindingWrite write{
        .location = {0, 0},
        .resource = {.type = aeg::rhi::BindingType::kSampledTexture, .texture_view = view.value},
    };
    if (const auto updated = device.value->update_binding_set(set.value, std::span{&write, 1}); !updated) {
        std::cerr << "cross-slot binding set update: " << updated.message << '\n';
        return 41;
    }
    aeg::render_graph::FrameContext retirement_frame(*device.value, {.frames_in_flight = 2});
    if (!retirement_frame.begin_frame() || retirement_frame.frame_slot_index() != 0 ||
        !retirement_frame.defer_destroy_texture_view(view.value) ||
        !retirement_frame.defer_destroy_texture(texture.value) ||
        !submit_empty(retirement_frame, aeg::rhi::QueueClass::kGraphics) || !retirement_frame.end_frame())
        return 41;
    if (!retirement_frame.begin_frame() || retirement_frame.frame_slot_index() != 1 ||
        !retirement_frame.defer_destroy_binding_set(set.value) ||
        !retirement_frame.defer_destroy_binding_layout(layout.value) ||
        !submit_empty(retirement_frame, aeg::rhi::QueueClass::kGraphics) || !retirement_frame.end_frame())
        return 41;
    const auto retirement_number = retirement_frame.frame_number();
    if (const auto drained = retirement_frame.drain(); !drained) {
        std::cerr << "cross-slot descriptor/view drain failed: code=" << static_cast<int>(drained.code)
                  << ", message=" << drained.message << '\n';
        return 42;
    }
    const std::array retired{
        device.value->destroy_binding_set(set.value),
        device.value->destroy_texture_view(view.value),
        device.value->destroy_texture(texture.value),
        device.value->destroy_binding_layout(layout.value),
    };
    constexpr std::array<const char*, 4> resource_names{"binding set", "texture view", "texture", "binding layout"};
    for (std::size_t index = 0; index < retired.size(); ++index) {
        if (retired[index].code != ResultCode::kNotFound) {
            std::cerr << "cross-slot drain did not retire " << resource_names[index]
                      << ": code=" << static_cast<int>(retired[index].code)
                      << ", message=" << retired[index].message << '\n';
            return 43;
        }
    }
    if (const auto drained_again = retirement_frame.drain(); !drained_again) {
        std::cerr << "repeated cross-slot descriptor/view drain failed: code=" << static_cast<int>(drained_again.code)
                  << ", message=" << drained_again.message << '\n';
        return 42;
    }
    if (retirement_frame.is_frame_active() || retirement_frame.frame_number() != retirement_number)
        return 44;

    // A failed drain must retain deferred handles until their external descriptor is released.
    const auto retry_texture = device.value->create_texture(texture_desc);
    if (!retry_texture) {
        std::cerr << "retry texture creation: " << retry_texture.result.message << '\n';
        return 45;
    }
    const auto retry_view = device.value->create_texture_view({.texture = retry_texture.value,
                                                              .kind = aeg::rhi::TextureViewKind::kSampled});
    if (!retry_view) {
        std::cerr << "retry texture view creation: " << retry_view.result.message << '\n';
        return 46;
    }
    const auto retry_layout = device.value->create_binding_layout({.entries = entries});
    if (!retry_layout) {
        std::cerr << "retry binding layout creation: " << retry_layout.result.message << '\n';
        return 47;
    }
    const auto external_set = device.value->create_binding_set({.layout = retry_layout.value,
                                                               .lifetime = aeg::rhi::BindingSetLifetime::kPersistent});
    if (!external_set) {
        std::cerr << "retry external binding set creation: " << external_set.result.message << '\n';
        return 48;
    }
    const aeg::rhi::BindingWrite retry_write{
        .location = {0, 0},
        .resource = {.type = aeg::rhi::BindingType::kSampledTexture, .texture_view = retry_view.value},
    };
    if (const auto updated = device.value->update_binding_set(external_set.value, std::span{&retry_write, 1}); !updated) {
        std::cerr << "retry external binding set update: " << updated.message << '\n';
        return 49;
    }
    aeg::render_graph::FrameContext retry_frame(*device.value, {.frames_in_flight = 2});
    if (!retry_frame.begin_frame() || !retry_frame.defer_destroy_texture_view(retry_view.value) ||
        !retry_frame.defer_destroy_texture(retry_texture.value) ||
        !submit_empty(retry_frame, aeg::rhi::QueueClass::kGraphics) || !retry_frame.end_frame())
        return 49;
    const auto retry_number = retry_frame.frame_number();
    const auto blocked = retry_frame.drain();
    if (blocked || blocked.code != ResultCode::kInvalidState) {
        std::cerr << "drain accepted a live external descriptor: code=" << static_cast<int>(blocked.code)
                  << ", message=" << blocked.message << '\n';
        return 50;
    }
    if (retry_frame.is_frame_active() || retry_frame.frame_number() != retry_number)
        return 50;
    if (const auto released = device.value->destroy_binding_set(external_set.value); !released) {
        std::cerr << "retry external binding set release: " << released.message << '\n';
        return 51;
    }
    if (const auto released = device.value->destroy_binding_layout(retry_layout.value); !released) {
        std::cerr << "retry external binding layout release: " << released.message << '\n';
        return 51;
    }
    if (const auto retried = retry_frame.drain(); !retried) {
        std::cerr << "descriptor/view drain retry failed: code=" << static_cast<int>(retried.code)
                  << ", message=" << retried.message << '\n';
        return 52;
    }
    const std::array retry_retired{
        device.value->destroy_texture_view(retry_view.value),
        device.value->destroy_texture(retry_texture.value),
    };
    for (const auto& retired_resource : retry_retired) {
        if (retired_resource.code != ResultCode::kNotFound) {
            std::cerr << "drain retry lost a failed retirement: code=" << static_cast<int>(retired_resource.code)
                      << ", message=" << retired_resource.message << '\n';
            return 53;
        }
    }
    if (retry_frame.is_frame_active() || retry_frame.frame_number() != retry_number)
        return 54;
    return 0;
}
