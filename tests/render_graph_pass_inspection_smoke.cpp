/** @brief Verifies public graph snapshots and private actual Pass identity inspection. */
#include <iostream>
#include <optional>
#include <string>

#include "../Engine/RenderGraph/private/render_graph_execution.hpp"
#include "rhi.hpp"

namespace {
using aeg::foundation::Result;
using aeg::foundation::ResultCode;
using aeg::render_graph::RenderGraph;
using aeg::render_graph::detail::RenderGraphExecution;
using aeg::render_graph::detail::RenderGraphPassInspection;
using aeg::rhi::QueueClass;

auto add_observed_pass(RenderGraph& graph, std::string_view name, QueueClass queue) {
    return graph.add_pass(
        name, queue,
        [](aeg::render_graph::PassBuilder& pass) {
            pass.set_side_effects();
            return Result::success();
        },
        [](aeg::render_graph::PassExecutionContext&) { return Result::success(); });
}

int graph_inspection_is_read_only_and_owning(aeg::rhi::Device& device) {
    std::optional<aeg::render_graph::RenderGraphInspection> retained;
    uint32_t executed_callbacks = 0;
    {
        RenderGraph graph(device);
        if (graph.inspect().result.code != ResultCode::kNotReady)
            return 15;
        const auto first = graph.add_pass(
            "Inspected graph pass", QueueClass::kGraphics,
            [](aeg::render_graph::PassBuilder& pass) {
                pass.set_side_effects();
                return Result::success();
            },
            [&](aeg::render_graph::PassExecutionContext&) {
                ++executed_callbacks;
                return Result::success();
            });
        if (!first || !graph.compile())
            return 16;
        const RenderGraph& observed = graph;
        auto inspection = observed.inspect();
        if (!inspection || inspection.value.compilation.pass_count != 1 ||
            inspection.value.compilation.resource_count != 0 || inspection.value.compilation.batch_count != 1 ||
            inspection.value.plan.revision == 0 || inspection.value.plan.pass_plans.size() != 1 ||
            inspection.value.plan.pass_plans.front().pass != first.value ||
            !inspection.value.plan.pass_plans.front().live || inspection.value.plan.batches.size() != 1 ||
            inspection.value.plan.batches.front().queue != QueueClass::kGraphics || executed_callbacks != 0)
            return 17;
        retained = inspection.value;
        inspection.value.plan.pass_plans.front().live = false;
        inspection.value.plan.batches.clear();
        const auto repeated = observed.inspect();
        if (!repeated || repeated.value.plan.pass_plans.size() != 1 ||
            !repeated.value.plan.pass_plans.front().live || repeated.value.plan.batches.size() != 1 ||
            repeated.value.plan.revision != retained->plan.revision || executed_callbacks != 0)
            return 18;

        const auto second = add_observed_pass(graph, "Later graph pass", QueueClass::kGraphics);
        if (!second || observed.inspect().result.code != ResultCode::kNotReady || !graph.compile())
            return 19;
        const auto replacement = observed.inspect();
        if (!replacement || replacement.value.compilation.pass_count != 2 ||
            replacement.value.plan.pass_plans.size() != 2 || replacement.value.plan.revision <= retained->plan.revision ||
            retained->compilation.pass_count != 1 || retained->plan.pass_plans.size() != 1)
            return 20;
        if (!graph.add_dependency(first.value, second.value) || !graph.add_dependency(second.value, first.value) ||
            graph.compile() || observed.inspect().result.code != ResultCode::kNotReady)
            return 21;
        graph.clear_passes();
        if (observed.inspect().result.code != ResultCode::kNotReady || !graph.compile())
            return 22;
        const auto empty = observed.inspect();
        if (!empty || empty.value.plan.revision == 0 || empty.value.compilation.pass_count != 0 ||
            !empty.value.plan.pass_plans.empty())
            return 23;
        graph.reset();
        if (observed.inspect().result.code != ResultCode::kNotReady)
            return 24;
    }
    if (!retained || retained->compilation.pass_count != 1 || retained->plan.pass_plans.size() != 1 ||
        !retained->plan.pass_plans.front().live || retained->plan.batches.size() != 1 || executed_callbacks != 0)
        return 25;
    return 0;
}

int inspection_rejects_failed_import_refresh(aeg::rhi::Device& device) {
    RenderGraph graph(device);
    aeg::rhi::BufferDesc desc{};
    desc.size = 16;
    desc.capabilities = aeg::rhi::BufferCapability::kTransferDestination;
    const auto buffer = device.create_buffer(desc);
    if (!buffer)
        return 26;
    const auto imported = graph.import_buffer(buffer.value, desc);
    const auto compiled = imported ? graph.compile() : imported.result;
    const auto before = graph.inspect();
    const auto destroyed = device.destroy_buffer(buffer.value);
    if (!compiled || !before || before.value.compilation.resource_count != 1 || !destroyed)
        return 27;
    if (graph.compile() || graph.inspect().result.code != ResultCode::kNotReady ||
        before.value.compilation.resource_count != 1 || before.value.plan.revision == 0)
        return 28;
    return 0;
}
}  // namespace

int main() {
    const auto device = aeg::rhi::create_device({aeg::rhi::BackendType::kVulkan, "Pass inspection boundary"});
    if (!device) {
        std::cerr << device.result.message << '\n';
        return 1;
    }
    if (const int result = graph_inspection_is_read_only_and_owning(*device.value); result != 0)
        return result;
    if (const int result = inspection_rejects_failed_import_refresh(*device.value); result != 0)
        return result;
    const std::string original_name = "Actual native pass with an owning diagnostic name beyond short-string storage";
    std::optional<RenderGraphPassInspection> retained;
    {
        RenderGraph graph(*device.value);
        RenderGraph foreign_graph(*device.value);
        auto authored_name = original_name;
        const auto actual = add_observed_pass(graph, authored_name, QueueClass::kAuto);
        if (!actual || !graph.compile())
            return 2;
        authored_name.front() = 'X';
        const auto inspected = RenderGraphExecution::inspect_pass(graph, actual.value);
        if (!inspected || inspected.value.pass != actual.value || inspected.value.name != original_name ||
            inspected.value.queue != QueueClass::kGraphics)
            return 3;
        const auto& plan = RenderGraphExecution::plan(graph);
        if (plan.pass_plans.size() != 1 || plan.pass_plans.front().pass != inspected.value.pass ||
            !plan.pass_plans.front().live || plan.pass_plans.front().feature_work)
            return 4;

        const auto foreign = add_observed_pass(foreign_graph, original_name, QueueClass::kCompute);
        if (!foreign)
            return 5;
        const auto wrong_graph = RenderGraphExecution::inspect_pass(graph, foreign.value);
        if (wrong_graph || wrong_graph.result.code != ResultCode::kNotFound || !wrong_graph.value.name.empty())
            return 6;
        const auto invalid = RenderGraphExecution::inspect_pass(graph, {});
        if (invalid || invalid.result.code != ResultCode::kInvalidArgument || invalid.value.pass.is_valid())
            return 7;
        auto wrong_generation = actual.value;
        ++wrong_generation.generation;
        const auto generation = RenderGraphExecution::inspect_pass(graph, wrong_generation);
        if (generation || generation.result.code != ResultCode::kNotFound)
            return 8;

        retained = inspected.value;
        graph.clear_passes();
        const auto cleared = RenderGraphExecution::inspect_pass(graph, actual.value);
        if (cleared || cleared.result.code != ResultCode::kNotFound)
            return 9;
        const auto replacement = add_observed_pass(graph, "Replacement actual native pass", QueueClass::kCompute);
        if (!replacement || replacement.value.index != actual.value.index ||
            replacement.value.generation == actual.value.generation)
            return 10;
        const auto recycled = RenderGraphExecution::inspect_pass(graph, actual.value);
        if (recycled || recycled.result.code != ResultCode::kNotFound)
            return 11;
        const auto replacement_inspection = RenderGraphExecution::inspect_pass(graph, replacement.value);
        if (!replacement_inspection || replacement_inspection.value.pass != replacement.value ||
            replacement_inspection.value.name != "Replacement actual native pass" ||
            replacement_inspection.value.queue != QueueClass::kCompute || retained->name != original_name)
            return 12;
        graph.reset();
        const auto reset = RenderGraphExecution::inspect_pass(graph, replacement.value);
        if (reset || reset.result.code != ResultCode::kNotFound)
            return 13;
    }
    if (!retained || retained->name != original_name || retained->queue != QueueClass::kGraphics ||
        !retained->pass.is_valid())
        return 14;
    std::cout << "Actual pass inspection rejects foreign/stale handles and retains owning metadata\n";
    return 0;
}
