/**
 * @file frame_retention_smoke.cpp
 * @brief Verifies frame-slot ownership retirement with real queue submissions.
 */
#include <frame_context.hpp>
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
    return 0;
}
