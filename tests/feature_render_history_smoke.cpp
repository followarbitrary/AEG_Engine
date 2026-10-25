#include "feature_clock_test_fixture.hpp"
#include "../Engine/RenderGraph/private/render_graph_execution.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>

#include "feature_render_test_fixture.hpp"
#include "render_feature_history.hpp"
#include "../Engine/RenderGraph/private/render_feature_history_access.hpp"
#include "render_feature_runtime.hpp"

namespace {
using namespace aeg;
using namespace aeg::feature;
using namespace render_feature_test;

FeatureDefinition history_definition() {
    auto result = definition();
    result.id = FeatureDefinitionId("Render.HistorySmoke");
    result.contract.definition_id = result.id;
    result.execute = [execute = result.execute](const InvocationToken& token, const StateSnapshot& state,
                                                std::span<const uint8_t> input, std::span<const uint8_t> parameters) {
        auto delta = execute(token, state, input, parameters);
        if (!delta) return delta;
        auto& intent = *delta.value.render_intent;
        intent.textures.front().lifetime = RenderTextureLifetime::kHistory;
        if (state.payload.empty()) {
            intent.operations.resize(1);
            intent.final_output = std::get<RenderClearColor>(intent.operations.front()).target;
        } else {
            intent.operations.erase(intent.operations.begin());
        }
        return delta;
    };
    return result;
}

void check_multi_history(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(19);
    }
}

void independent_runtime_histories_share_one_graph(rhi::Device& device) {
    const auto source = history_definition();
    FeatureRegistry first_registry{aeg::feature::test::clock_test_host()}, second_registry{aeg::feature::test::clock_test_host()};
    check_multi_history(aeg::feature::test::register_clock_test_definition(first_registry, source) && aeg::feature::test::register_clock_test_definition(second_registry, source),
                        "Independent history definitions failed");
    auto first_document = document(source);
    first_document.nodes.resize(1);
    auto second_document = first_document;
    const auto blue_parameters = encode_feature_parameters(
        std::vector<FeatureParameterRecord>{{ParameterId("Channel"), 1, {2}}}, source.contract.parameters);
    check_multi_history(static_cast<bool>(blue_parameters), "Independent blue history parameters failed");
    second_document.nodes.front().parameter_blob = blue_parameters.value;
    auto first_plan = FeatureGraphCompiler{}.compile(first_document, first_registry);
    auto second_plan = FeatureGraphCompiler{}.compile(second_document, second_registry);
    FeatureRuntime first_runtime(first_registry), second_runtime(second_registry);
    check_multi_history(first_plan && second_plan && first_runtime.publish_graph(first_document, first_plan.value) &&
                            second_runtime.publish_graph(second_document, second_plan.value) &&
                            first_runtime.tick(source.contract.clock.id) && second_runtime.tick(source.contract.clock.id),
                        "Independent actual Runtime history publication failed");
    const auto first_outputs = first_runtime.capture_render_outputs(first_plan.value);
    const auto second_outputs = second_runtime.capture_render_outputs(second_plan.value);
    const auto first_owners = first_runtime.capture_render_resource_owners(first_plan.value);
    const auto second_owners = second_runtime.capture_render_resource_owners(second_plan.value);
    check_multi_history(first_outputs && second_outputs && first_owners && second_owners &&
                            first_outputs.value.size() == 1 && second_outputs.value.size() == 1 &&
                            first_outputs.value.front().instance_id == FeatureInstanceId{1} &&
                            first_outputs.value.front().resource_owner == second_outputs.value.front().resource_owner &&
                            first_outputs.value.front().node_id == second_outputs.value.front().node_id &&
                            first_outputs.value.front().invocation.generation != second_outputs.value.front().invocation.generation,
                        "Independent Runtime fixture did not exercise actual local owner identity collisions");
    auto first_history = std::make_unique<render_graph::RenderFeatureHistory>(device);
    auto second_history = std::make_unique<render_graph::RenderFeatureHistory>(device);
    check_multi_history(first_history->reconcile(first_owners.value) && second_history->reconcile(second_owners.value),
                        "Independent history owner snapshots failed");
    const auto first_snapshot = std::make_shared<const std::vector<RenderFeatureOutput>>(first_outputs.value);
    const auto second_snapshot = std::make_shared<const std::vector<RenderFeatureOutput>>(second_outputs.value);
    render_graph::FrameContext frame(device, {.frames_in_flight = 2});
    render_graph::RenderGraph graph(device);
    const auto stage_both = [&] {
        check_multi_history(static_cast<bool>(render_graph::build_render_graph_features(graph, first_snapshot, {}, 1, first_history.get())),
                            "First independent history snapshot did not stage");
        const auto second = render_graph::build_render_graph_features(graph, second_snapshot, {}, 1, second_history.get());
        if (!second) {
            std::cerr << "Second independent History store was rejected: " << second.message << '\n';
            std::exit(19);
        }
    };
    stage_both();
    check_multi_history(first_history->size() == 0 && second_history->size() == 0,
                        "Unsubmitted independent history snapshots committed early");
    check_multi_history(frame.begin_frame() && graph.execute(frame) && frame.end_frame() &&
                            first_history->size() == 1 && second_history->size() == 1,
                        "Independent history snapshots did not commit together");
    const auto first_native = first_history->find_texture(FeatureInstanceId{1}, RenderResourceId("Scratch"));
    const auto second_native = second_history->find_texture(FeatureInstanceId{1}, RenderResourceId("Scratch"));
    check_multi_history(first_native && second_native && first_native.value.texture != second_native.value.texture,
                        "Independent colliding owners shared their native history allocation");
    const auto first_unchanged = [&] {
        const auto current = first_history->find_texture(FeatureInstanceId{1}, RenderResourceId("Scratch"));
        check_multi_history(current && current.value.texture == first_native.value.texture && first_history->size() == 1,
                            "Failed graph committed the first independent history snapshot");
    };
    const auto second_unchanged = [&] {
        const auto current = second_history->find_texture(FeatureInstanceId{1}, RenderResourceId("Scratch"));
        check_multi_history(current && current.value.texture == second_native.value.texture && second_history->size() == 1,
                            "Failed graph committed the second independent history snapshot");
    };
    check_multi_history(frame.begin_frame() && !graph.execute(frame) && !frame.is_frame_active(),
                        "Independent history graph replay accepted consumed snapshots");
    first_unchanged(); second_unchanged();
    graph.reset();

    check_multi_history(static_cast<bool>(frame.begin_frame()), "Independent history readback frame failed");
    const auto allocation = frame.allocate_readback(128, 256);
    check_multi_history(static_cast<bool>(allocation), "Independent history readback allocation failed");
    rhi::BufferDesc buffer_desc{};
    buffer_desc.size = allocation.value.buffer_size;
    buffer_desc.memory = rhi::MemoryClass::kReadback;
    buffer_desc.capabilities = rhi::BufferCapability::kTransferDestination;
    const auto buffer = graph.import_buffer(allocation.value.buffer, buffer_desc);
    check_multi_history(static_cast<bool>(buffer), "Independent history readback import failed");
    const std::array native{first_native.value, second_native.value};
    for (unsigned index = 0; index != native.size(); ++index) {
        rhi::TextureDesc desc{};
        desc.extent = {4, 4, 1}; desc.format = rhi::Format::kRgba8Unorm;
        desc.capabilities = rhi::TextureCapability::kTransferSource | rhi::TextureCapability::kTransferDestination;
        const auto state = device.query_texture_state(native[index].texture);
        check_multi_history(state && !state.value.subresource_states.empty(), "Independent history texture state is unavailable");
        const auto texture = graph.import_texture(native[index].texture, {}, desc,
            state.value.subresource_states.front(), native[index].lease.lock());
        check_multi_history(static_cast<bool>(texture), "Independent history texture readback import failed");
        const auto offset = allocation.value.offset + index * 64;
        const auto pass = graph.add_pass("Host.IndependentHistoryReadback", rhi::QueueClass::kCopy,
            [texture = texture.value, destination = buffer.value, offset](render_graph::PassBuilder& builder) {
                if (const auto result = builder.read(texture, rhi::ResourceUsage::kCopySource, rhi::PipelineStage::kCopy); !result) return result;
                builder.set_side_effects();
                return builder.write_to(destination, rhi::ResourceUsage::kCopyDestination, rhi::PipelineStage::kCopy,
                                         rhi::ShaderStage::kAll, rhi::ResourceUsage::kHostRead, {offset, 64}, true);
            },
            [texture = texture.value, destination = buffer.value, offset](render_graph::RenderPassCommandContext& command) {
                return command.copy_texture_to_buffer(texture, {rhi::Aspect::kColor, 0, 0, 1}, {}, {4, 4, 1}, destination, offset, 0, 0);
            });
        check_multi_history(static_cast<bool>(pass), "Independent history pixel readback pass failed");
    }
    check_multi_history(graph.execute(frame) && frame.end_frame() && frame.drain(), "Independent history pixel readback failed");
    std::array<std::byte, 128> pixels{};
    check_multi_history(static_cast<bool>(frame.read_readback(allocation.value, pixels)), "Independent history readback bytes failed");
    for (unsigned pixel = 0; pixel != 32; ++pixel) {
        const bool red = pixel < 16;
        check_multi_history(pixels[pixel * 4] == (red ? std::byte{255} : std::byte{0}) &&
                                pixels[pixel * 4 + 1] == std::byte{0} &&
                                pixels[pixel * 4 + 2] == (red ? std::byte{0} : std::byte{255}) &&
                                pixels[pixel * 4 + 3] == std::byte{255},
                            "Independent Runtime histories mixed actual red/blue GPU contents");
    }
    graph.reset();
    stage_both();
    const auto failure = graph.add_pass("Host.FailIndependentHistories", rhi::QueueClass::kGraphics,
        [](render_graph::PassBuilder& builder) { builder.set_side_effects(); return foundation::Result::success(); },
        [](render_graph::RenderPassCommandContext&) { return foundation::invalid_state("injected independent histories execution failure"); });
    check_multi_history(failure && frame.begin_frame() && !graph.execute(frame) && !frame.is_frame_active(),
                        "Failed execution accepted independent history snapshots");
    first_unchanged(); second_unchanged();
    graph.reset();
    stage_both();
    auto retirement_probe = render_graph::detail::RenderFeatureHistoryRuntimeAccess::stage(*second_history, {});
    const auto retired_domain = retirement_probe->transaction_domain();
    check_multi_history(!retired_domain.expired(), "Live history transaction has no weak store identity");
    second_history.reset();
    check_multi_history(retired_domain.expired(), "Submission observer identity kept its retired History store alive");
    check_multi_history(frame.begin_frame() && !graph.execute(frame) && !frame.is_frame_active(),
                        "Retired independent history observer allowed graph submission");
    first_unchanged();
    retirement_probe.reset();
    graph.reset();
    check_multi_history(static_cast<bool>(frame.drain()), "Independent history frame leases did not retire");
}

void same_store_partial_snapshots_still_roll_back(rhi::Device& device) {
    const auto source = history_definition();
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    check_multi_history(static_cast<bool>(aeg::feature::test::register_clock_test_definition(registry, source)), "Partial history definitions failed");
    const auto authored = document(source);
    auto plan = FeatureGraphCompiler{}.compile(authored, registry);
    FeatureRuntime runtime(registry);
    check_multi_history(plan && runtime.publish_graph(authored, plan.value) && runtime.tick(source.contract.clock.id),
                        "Partial history actual Runtime publication failed");
    const auto outputs = runtime.capture_render_outputs(plan.value);
    const auto owners = runtime.capture_render_resource_owners(plan.value);
    check_multi_history(outputs && owners && outputs.value.size() == 2, "Partial history outputs/owners are unavailable");
    const auto first = std::make_shared<const std::vector<RenderFeatureOutput>>(std::vector{outputs.value.front()});
    const auto second = std::make_shared<const std::vector<RenderFeatureOutput>>(std::vector{outputs.value.back()});
    for (const bool different_view : {false, true}) {
        render_graph::RenderFeatureHistory history(device);
        const render_graph::RenderFeatureHistoryKey first_key{101}, second_key{different_view ? 202u : 101u};
        check_multi_history(history.reconcile(first_key, owners.value) && history.reconcile(second_key, owners.value),
                            "Partial history keyed owners failed");
        const auto first_probe = render_graph::detail::RenderFeatureHistoryRuntimeAccess::stage(history, first_key);
        const auto second_probe = render_graph::detail::RenderFeatureHistoryRuntimeAccess::stage(history, second_key);
        const auto first_domain = first_probe->transaction_domain(), second_domain = second_probe->transaction_domain();
        check_multi_history(!first_domain.owner_before(second_domain) && !second_domain.owner_before(first_domain),
                            "Partial or view-key snapshots changed their actual complete-store transaction domain");
        render_graph::RenderGraph graph(device);
        render_graph::FrameContext frame(device);
        check_multi_history(static_cast<bool>(render_graph::build_render_graph_features(graph, first,
            {.history_key = first_key.value}, 1, &history)), "First partial history snapshot failed");
        check_multi_history(static_cast<bool>(graph.compile()), "First partial history graph compile failed");
        const auto before = render_graph::detail::RenderGraphExecution::compile_info(graph);
        check_multi_history(before.pass_count == 1, "First partial history graph has unexpected work");
        const auto rejected = render_graph::build_render_graph_features(graph, second,
            {.history_key = second_key.value}, 1, &history);
        check_multi_history(!rejected && graph.compile(), "Same-store partial history rejection/compile failed");
        const auto after = render_graph::detail::RenderGraphExecution::compile_info(graph);
        check_multi_history(after.pass_count == before.pass_count &&
                                after.resource_count == before.resource_count && history.size() == 0,
                            "Same-store partial snapshot was accepted or failed to roll back its graph additions");
        check_multi_history(frame.begin_frame() && graph.execute(frame) && frame.end_frame() && history.size() == 1 &&
                                history.find_texture(first_key, outputs.value.front().instance_id, RenderResourceId("Scratch")) &&
                                !history.find_texture(second_key, outputs.value.back().instance_id, RenderResourceId("Scratch")),
                            "Rejected partial snapshot overwrote the accepted complete-store transaction");
        graph.reset();
        check_multi_history(static_cast<bool>(frame.drain()), "Partial history frame leases did not retire");
    }
}

void history_isolated_by_view_key(rhi::Device& device) {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    const auto source = history_definition();
    require(static_cast<bool>(aeg::feature::test::register_clock_test_definition(registry, source)), "view history registration failed");
    const auto authored = document(source);
    auto compiled = FeatureGraphCompiler{}.compile(authored, registry);
    FeatureRuntime runtime(registry);
    require(static_cast<bool>(compiled) && static_cast<bool>(runtime.publish_graph(authored, compiled.value)) &&
            static_cast<bool>(runtime.tick(source.contract.clock.id)), "view history publication failed");
    const auto outputs = runtime.capture_render_outputs(compiled.value);
    const auto owners = runtime.capture_render_resource_owners(compiled.value);
    require(static_cast<bool>(outputs) && static_cast<bool>(owners), "view history capture failed");
    auto snapshot = std::make_shared<const std::vector<RenderFeatureOutput>>(outputs.value);
    render_graph::RenderFeatureHistory history(device);
    const render_graph::RenderFeatureHistoryKey first_view{101};
    const render_graph::RenderFeatureHistoryKey second_view{202};
    const render_graph::RenderGraphFeatureRuntimeDesc first_desc{.history_key = first_view.value};
    const render_graph::RenderGraphFeatureRuntimeDesc second_desc{.history_key = second_view.value};
    require(static_cast<bool>(history.reconcile(first_view, owners.value)), "first view history reconcile failed");
    render_graph::FrameContext frame(device);
    render_graph::RenderGraph graph(device);
    require(static_cast<bool>(frame.begin_frame()) && static_cast<bool>(render_graph::build_render_graph_features(
        graph, snapshot, first_desc, 1, &history)) && static_cast<bool>(graph.execute(frame)) &&
            static_cast<bool>(frame.end_frame()), "first view history commit failed");
    const auto first = history.find_texture(first_view, outputs.value.front().instance_id, RenderResourceId("Scratch"));
    require(static_cast<bool>(first), "first view history was not initialized");
    graph.reset();
    require(static_cast<bool>(history.reconcile(second_view, owners.value)) && static_cast<bool>(frame.begin_frame()) &&
            static_cast<bool>(render_graph::build_render_graph_features(graph, snapshot, second_desc, 2, &history)) &&
            static_cast<bool>(graph.execute(frame)) && static_cast<bool>(frame.end_frame()),
            "second view history commit failed");
    const auto second = history.find_texture(second_view, outputs.value.front().instance_id, RenderResourceId("Scratch"));
    require(static_cast<bool>(second) && second.value.texture != first.value.texture && history.size() == 4,
            "multi-view history keys shared native allocations");
    const auto default_key = history.find_texture(outputs.value.front().instance_id, RenderResourceId("Scratch"));
    require(!default_key, "unkeyed history lookup crossed a view boundary");
    graph.reset();
    require(static_cast<bool>(frame.drain()), "multi-view history frame retirement failed");
}

void history_rejects_invalid_transactions(render_graph::RenderGraph& graph, render_graph::FrameContext& frame,
    render_graph::RenderFeatureHistory& history, const RenderFeatureOutput& producer,
    std::span<const RenderResourceOwner> owners, rhi::Device& device) {
    const auto original = history.find_texture(producer.instance_id, RenderResourceId("Scratch"));
    require(static_cast<bool>(original), "history transaction fixture is uninitialized");
    const auto snapshot = [](RenderFeatureOutput output) {
        return std::make_shared<const std::vector<RenderFeatureOutput>>(std::vector{std::move(output)});
    };
    const auto unchanged = [&] {
        const auto current = history.find_texture(producer.instance_id, RenderResourceId("Scratch"));
        require(static_cast<bool>(current) && current.value.texture == original.value.texture && history.size() == 2,
                "rejected transaction replaced accepted history");
    };
    auto changed = producer;
    changed.intent.textures.front().width = 8;
    require(!render_graph::build_render_graph_features(graph, snapshot(changed), {}, 2, &history),
            "same-version history shape change was accepted");
    unchanged();
    // A changed version may replace shape, but staging must remain invisible.
    changed.intent.textures.front().version = 2;
    std::get<RenderClearColor>(changed.intent.operations.front()).target.version = 2;
    changed.intent.final_output->version = 2;
    changed.intent.operations.push_back(RenderClearColor{{RenderResourceId("Missing.External"), 1, true}, {0,1,0,1}});
    require(!render_graph::build_render_graph_features(graph, snapshot(changed), {}, 2, &history),
            "missing binding after staged history write was accepted");
    unchanged();
    require(static_cast<bool>(graph.compile()) && aeg::render_graph::detail::RenderGraphExecution::plan(graph).pass_plans.empty(), "history build rollback left passes");
    changed.intent.operations.pop_back();
    require(static_cast<bool>(render_graph::build_render_graph_features(graph, snapshot(changed), {}, 2, &history)),
            "new history version did not stage");
    unchanged();
    const auto failure = graph.add_pass("Host.FailHistory", rhi::QueueClass::kGraphics,
        [](render_graph::PassBuilder& pass) { pass.set_side_effects(); return foundation::Result::success(); },
        [](render_graph::RenderPassCommandContext&) { return foundation::invalid_state("injected execution failure"); });
    require(static_cast<bool>(failure) && static_cast<bool>(frame.begin_frame()) && !graph.execute(frame),
            "history execution failure did not reject transaction");
    unchanged();
    require(!frame.is_frame_active(), "failed history execution did not abort frame");
    graph.reset();

    // First reads, partial first writes, and export-only declarations cannot
    // turn uninitialized storage into readable history.
    render_graph::RenderFeatureHistory empty(device);
    require(static_cast<bool>(empty.reconcile(owners)), "empty history owner fixture failed");
    auto read = producer;
    const RenderTextureReference history_ref{RenderResourceId("Scratch"), 1, false};
    const RenderTextureReference probe{RenderResourceId("Probe"), 1, false};
    read.intent.textures.push_back({probe.id, 1, 4, 4});
    read.intent.operations = {RenderCopyTexture{history_ref, probe, 4, 4}};
    read.intent.final_output = probe;
    require(!render_graph::build_render_graph_features(graph, snapshot(read), {}, 2, &empty),
            "uninitialized history read was accepted");
    read.intent.operations = {RenderClearColor{probe, {0,1,0,1}}, RenderCopyTexture{probe, history_ref, 2, 2}};
    read.intent.final_output = history_ref;
    require(!render_graph::build_render_graph_features(graph, snapshot(read), {}, 2, &empty),
            "uninitialized partial history write was accepted");
    read.intent.operations.clear();
    require(!render_graph::build_render_graph_features(graph, snapshot(read), {}, 2, &empty),
            "uninitialized history export was accepted");
    require(empty.size() == 0, "rejected first read/write initialized history");
    require(static_cast<bool>(render_graph::build_render_graph_features(graph, snapshot(producer), {}, 2, &empty)),
            "history stale-plan fixture did not build");
    empty.clear();
    require(static_cast<bool>(frame.begin_frame()) && !graph.execute(frame) && !frame.is_frame_active() && empty.size() == 0,
            "retired store accepted a stale graph submission");
    graph.reset();
}

void history_survives_graph_reset_and_retires_with_frames(rhi::Device& device) {
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    const auto source = history_definition();
    require(static_cast<bool>(aeg::feature::test::register_clock_test_definition(registry, source)), "history Feature registration failed");
    auto authored = document(source);
    auto compiled = FeatureGraphCompiler{}.compile(authored, registry);
    FeatureRuntime runtime(registry);
    require(static_cast<bool>(compiled) && static_cast<bool>(runtime.publish_graph(authored, compiled.value)),
            "history graph publication failed");
    render_graph::RenderFeatureHistory history(device);
    render_graph::FrameContext frame(device, {.frames_in_flight = 2});
    render_graph::RenderGraph graph(device);
    require(static_cast<bool>(runtime.tick(source.contract.clock.id)), "history producer tick failed");
    auto owners = runtime.capture_render_resource_owners(compiled.value);
    auto captured = runtime.capture_render_outputs(compiled.value);
    require(static_cast<bool>(owners) && static_cast<bool>(captured) && captured.value.size() == 2 &&
            static_cast<bool>(history.reconcile(owners.value)), "history owners were not captured");
    auto outputs = std::make_shared<const std::vector<RenderFeatureOutput>>(captured.value);
    require(static_cast<bool>(frame.begin_frame()), "history producer frame failed");
    const auto first_slot = frame.frame_slot_index();
    require(static_cast<bool>(render_graph::build_render_graph_features(graph, outputs, {}, 1, &history)),
            "history producer build failed");
    require(history.size() == 0, "unsubmitted history was published");
    require(!render_graph::build_render_graph_features(graph, outputs, {}, 1, &history),
            "one graph accepted independent history transactions for the same store");
    require(static_cast<bool>(graph.execute(frame)) && static_cast<bool>(frame.end_frame()), "history producer execute failed");
    std::array<render_graph::RenderFeatureHistoryTexture, 2> native{};
    for (unsigned index = 0; index < native.size(); ++index) {
        const auto found = history.find_texture(captured.value[index].instance_id, RenderResourceId("Scratch"));
        require(static_cast<bool>(found), "submitted history is missing");
        native[index] = found.value;
    }
    require(native[0].texture != native[1].texture, "same local history ID shared native allocations between instances");
    graph.reset();
    outputs.reset();
    history_rejects_invalid_transactions(graph, frame, history, captured.value.front(), owners.value, device);
    require(static_cast<bool>(runtime.tick(source.contract.clock.id)), "history consumer tick failed");
    captured = runtime.capture_render_outputs(compiled.value);
    require(static_cast<bool>(captured) && captured.value.size() == 2, "history consumer capture failed");
    const RenderTextureReference patch{RenderResourceId("Patch"), 1, false};
    const RenderTextureReference retained{RenderResourceId("Scratch"), 1, false};
    captured.value.front().intent.textures.push_back({patch.id, 1, 2, 2});
    captured.value.front().intent.operations.insert(captured.value.front().intent.operations.begin(),
        {RenderClearColor{patch, {0,1,0,1}}, RenderCopyTexture{patch, retained, 2, 2}});
    outputs = std::make_shared<const std::vector<RenderFeatureOutput>>(captured.value);
    require(static_cast<bool>(frame.begin_frame()) && first_slot != frame.frame_slot_index(),
            "consumer did not use another frame slot");
    rhi::TextureDesc target_desc{};
    target_desc.extent = {4, 4, 1};
    target_desc.format = rhi::Format::kRgba8Unorm;
    target_desc.capabilities = rhi::TextureCapability::kTransferSource | rhi::TextureCapability::kTransferDestination;
    std::array<render_graph::RenderGraphTexture, 2> targets{};
    for (auto& target : targets) {
        const auto created = graph.create_texture(target_desc);
        require(static_cast<bool>(created), "history consumer target failed");
        target = created.value;
    }
    const std::array bindings{
        render_graph::RenderGraphFeatureTextureBinding{RenderResourceId("Target.Red"), targets[0]},
        render_graph::RenderGraphFeatureTextureBinding{RenderResourceId("Target.Blue"), targets[1]}};
    require(static_cast<bool>(render_graph::build_render_graph_features(graph, outputs, {.textures = bindings}, 2, &history)),
            "history consumer build failed");
    const auto allocation = frame.allocate_readback(128, 256);
    require(static_cast<bool>(allocation), "history readback allocation failed");
    rhi::BufferDesc buffer_desc{};
    buffer_desc.size = allocation.value.buffer_size;
    buffer_desc.memory = rhi::MemoryClass::kReadback;
    buffer_desc.capabilities = rhi::BufferCapability::kTransferDestination;
    const auto buffer = graph.import_buffer(allocation.value.buffer, buffer_desc);
    require(static_cast<bool>(buffer), "history readback import failed");
    for (unsigned index = 0; index < targets.size(); ++index) {
        const auto offset = allocation.value.offset + index * 64;
        const auto readback = graph.add_pass("Host.HistoryReadback", rhi::QueueClass::kCopy,
            [texture = targets[index], destination = buffer.value, offset](render_graph::PassBuilder& pass) {
                if (const auto result = pass.read(texture, rhi::ResourceUsage::kCopySource, rhi::PipelineStage::kCopy); !result) return result;
                pass.set_side_effects();
                return pass.write_to(destination, rhi::ResourceUsage::kCopyDestination, rhi::PipelineStage::kCopy,
                                     rhi::ShaderStage::kAll, rhi::ResourceUsage::kHostRead, {offset, 64}, true);
            },
            [texture = targets[index], destination = buffer.value, offset](render_graph::RenderPassCommandContext& command) {
                return command.copy_texture_to_buffer(texture, {rhi::Aspect::kColor, 0, 0, 1}, {}, {4, 4, 1}, destination, offset, 0, 0);
            });
        require(static_cast<bool>(readback), "history readback pass failed");
    }
    require(static_cast<bool>(graph.execute(frame)) && static_cast<bool>(frame.end_frame()), "history consumer execute failed");
    auto latest = native;
    const auto rewritten = history.find_texture(captured.value.front().instance_id, retained.id);
    require(static_cast<bool>(rewritten) && rewritten.value.texture != native.front().texture,
            "partial history write mutated previously submitted allocation");
    latest.front() = rewritten.value;
    const std::array leases{native[0], native[1], latest[0]};
    require(static_cast<bool>(frame.begin_frame()) && !graph.execute(frame) && !frame.is_frame_active(),
            "submitted history graph can replay without a fresh transaction");
    graph.reset();
    outputs.reset();
    auto peer = source;
    peer.id = FeatureDefinitionId("Render.UnrelatedHistory");
    peer.contract.definition_id = peer.id;
    require(static_cast<bool>(aeg::feature::test::register_clock_test_definition(registry, peer)), "unrelated history fixture registration failed");
    peer.version = {2, 0, 0};
    peer.contract.version = peer.version;
    require(static_cast<bool>(runtime.replace_definition(peer, {{1,0,0},{2,0,0},{1,0,0},{1,0,0}})),
            "unrelated definition migration failed");
    owners = runtime.capture_render_resource_owners(compiled.value);
    require(static_cast<bool>(owners) && static_cast<bool>(history.reconcile(owners.value)) && history.size() == 2,
            "unrelated migration erased initialized history");
    for (unsigned index = 0; index < native.size(); ++index) {
        const auto found = history.find_texture(captured.value[index].instance_id, RenderResourceId("Scratch"));
        require(static_cast<bool>(found) && found.value.texture == latest[index].texture, "unrelated migration replaced native history");
    }
    auto replacement = source;
    replacement.version = {2, 0, 0};
    replacement.contract.version = replacement.version;
    require(static_cast<bool>(runtime.replace_definition(replacement, {{1,0,0},{2,0,0},{1,0,0},{1,0,0}})),
            "history owner migration failed");
    owners = runtime.capture_render_resource_owners(compiled.value);
    require(static_cast<bool>(owners) && static_cast<bool>(history.reconcile(owners.value)) && history.size() == 0,
            "affected migration did not retire history");
    for (const auto& texture : leases)
        require(!texture.lease.expired() && static_cast<bool>(device.query_texture_state(texture.texture)),
                "native history retired before submitted frame leases");
    require(static_cast<bool>(frame.drain()), "history frames did not drain");
    std::array<std::byte, 128> pixels{};
    require(static_cast<bool>(frame.read_readback(allocation.value, pixels)), "history pixel readback failed");
    for (unsigned pixel = 0; pixel < 32; ++pixel) {
        const bool red = pixel < 16;
        const bool green = red && pixel / 4 < 2 && pixel % 4 < 2;
        require(pixels[pixel * 4] == (red && !green ? std::byte{255} : std::byte{0}) &&
                pixels[pixel * 4 + 1] == (green ? std::byte{255} : std::byte{0}) &&
                pixels[pixel * 4 + 2] == (red ? std::byte{0} : std::byte{255}) && pixels[pixel * 4 + 3] == std::byte{255},
                "previous-frame GPU contents did not survive reset/slot change");
    }
    for (const auto& texture : leases)
        require(texture.lease.expired() && !device.query_texture_state(texture.texture), "native history leaked after frame retirement");
}

void history_follows_process_lifecycle(rhi::Device& device) {
    auto source = history_definition();
    source.contract.execution = ExecutionKind::kProcess;
    source.contract.state = StatePolicy::kProcessState;
    source.contract.has_continuation = true;
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    require(static_cast<bool>(aeg::feature::test::register_clock_test_definition(registry, source)), "Process history registration failed");
    const auto authored = document(source);
    auto compiled = FeatureGraphCompiler{}.compile(authored, registry);
    FeatureRuntime runtime(registry);
    require(static_cast<bool>(compiled) && static_cast<bool>(runtime.publish_graph(authored, compiled.value)) &&
            static_cast<bool>(runtime.tick(source.contract.clock.id)), "Process history publication/tick failed");
    auto captured = runtime.capture_render_outputs(compiled.value);
    auto owners = runtime.capture_render_resource_owners(compiled.value);
    render_graph::RenderFeatureHistory history(device);
    render_graph::FrameContext frame(device);
    render_graph::RenderGraph graph(device);
    require(static_cast<bool>(captured) && captured.value.size() == 2 && static_cast<bool>(owners) &&
            static_cast<bool>(history.reconcile(owners.value)), "Process history capture failed");
    // Persistent writes root their passes without a presentation export.
    for (auto& output : captured.value) output.intent.final_output.reset();
    const auto outputs = std::make_shared<const std::vector<RenderFeatureOutput>>(captured.value);
    require(static_cast<bool>(frame.begin_frame()) && static_cast<bool>(render_graph::build_render_graph_features(
        graph, outputs, {}, 1, &history)) && static_cast<bool>(graph.execute(frame)) &&
        static_cast<bool>(frame.end_frame()) && history.size() == 2, "unexported history writes were culled");
    const auto first = history.find_texture(captured.value[0].instance_id, RenderResourceId("Scratch"));
    const auto second = history.find_texture(captured.value[1].instance_id, RenderResourceId("Scratch"));
    require(static_cast<bool>(first) && static_cast<bool>(second), "Process native history missing");
    const auto process = runtime.process(captured.value.front().instance_id);
    require(static_cast<bool>(process) && static_cast<bool>(runtime.suspend(process.value)), "history Process suspend failed");
    owners = runtime.capture_render_resource_owners(compiled.value);
    require(static_cast<bool>(owners) && static_cast<bool>(history.reconcile(owners.value)) && history.size() == 2,
            "suspension erased retained history");
    require(static_cast<bool>(runtime.resume(process.value)) && static_cast<bool>(runtime.cancel(process.value)),
            "history Process resume/cancel failed");
    owners = runtime.capture_render_resource_owners(compiled.value);
    require(static_cast<bool>(owners) && static_cast<bool>(history.reconcile(owners.value)) && history.size() == 1 &&
            !first.value.lease.expired(), "targeted cancellation did not retire only its owner");
    graph.reset();
    require(static_cast<bool>(frame.drain()) && first.value.lease.expired() && !second.value.lease.expired(),
            "cancelled history lease did not follow frame retirement");
    auto versioned = captured.value.back();
    versioned.intent.textures.front().version = 2;
    versioned.intent.textures.front().width = 8;
    std::get<RenderClearColor>(versioned.intent.operations.front()).target.version = 2;
    const auto upgraded = std::make_shared<const std::vector<RenderFeatureOutput>>(std::vector{versioned});
    require(static_cast<bool>(frame.begin_frame()) && static_cast<bool>(render_graph::build_render_graph_features(
        graph, upgraded, {}, 2, &history)) && static_cast<bool>(graph.execute(frame)) &&
        static_cast<bool>(frame.end_frame()), "new-version history shape replacement failed");
    const auto current = history.find_texture(versioned.instance_id, RenderResourceId("Scratch"));
    require(static_cast<bool>(current) && current.value.texture != second.value.texture,
            "new history version reused the retired native allocation");
    graph.reset();
    require(second.value.lease.expired(), "full history replacement retained unused previous storage");
    auto stale = captured.value.back();
    const RenderTextureReference probe{RenderResourceId("Version.Probe"), 1, false};
    stale.intent.textures.push_back({probe.id, 1, 4, 4});
    stale.intent.operations = {RenderCopyTexture{{RenderResourceId("Scratch"), 1, false}, probe, 4, 4}};
    stale.intent.final_output = probe;
    require(!render_graph::build_render_graph_features(graph,
        std::make_shared<const std::vector<RenderFeatureOutput>>(std::vector{stale}), {}, 3, &history),
        "old history version read the replacement allocation");
    auto empty = captured.value.back();
    empty.intent = {};
    const auto retired = std::make_shared<const std::vector<RenderFeatureOutput>>(std::vector{empty});
    require(static_cast<bool>(frame.begin_frame()) && static_cast<bool>(render_graph::build_render_graph_features(
        graph, retired, {}, 2, &history)) && history.size() == 1 && static_cast<bool>(graph.execute(frame)) &&
        static_cast<bool>(frame.end_frame()) && history.size() == 0, "empty intent did not release history after submission");
    graph.reset();
    require(static_cast<bool>(frame.drain()) && current.value.lease.expired(), "empty intent leaked native history");
}
}

int main() {
    try {
        auto device = aeg::rhi::create_device({});
        require(static_cast<bool>(device), "history GPU creation failed");
        independent_runtime_histories_share_one_graph(*device.value);
        same_store_partial_snapshots_still_roll_back(*device.value);
        history_isolated_by_view_key(*device.value);
        history_survives_graph_reset_and_retires_with_frames(*device.value);
        history_follows_process_lifecycle(*device.value);
        std::cout << "Feature GPU history smoke passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
