/**
 * @file feature_end_to_end_smoke.cpp
 * @brief Non-empty Editor -> Cook -> dynamic Runtime -> RenderGraph acceptance.
 */
#include "feature_clock_test_fixture.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <system_error>
#include <vector>

#include "asset_metadata.hpp"
#include "concurrency.hpp"
#include "editor_feature_graph_document.hpp"
#include "feature_graph_io.hpp"
#include "feature_module.hpp"
#include "feature_render_gpu_fixture.hpp"
#include "runtime_bootstrap.hpp"
#include "module_bundle_test_fixture.hpp"
#include "module_dynamic_feature_fixture.hpp"
#include "project_context.hpp"
#include "project_cooker.hpp"
#include "project_scaffold.hpp"
#include "runtime_feature_control.hpp"

namespace {
using namespace aeg;

constexpr std::string_view kProjectUuid = "00000000-0000-4000-8000-000000000101";
constexpr std::string_view kGraphUuid = "00000000-0000-4000-8000-000000000102";
constexpr std::string_view kSourceUuid = "00000000-0000-4000-8000-000000000103";

feature::FeatureGraphDocument make_graph(const feature::FeatureRegistry& registry) {
    using namespace feature;
    FeatureGraphDocument graph{};
    graph.asset_uuid = std::string(kGraphUuid);
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.contract_digest = registry.contract_digest();
    const auto* logic = registry.find_definition(dynamic_feature_test::kProcessLogic);
    const auto* render = registry.find_definition(FeatureDefinitionId("Render.IntentSmoke"));
    assert(logic != nullptr && render != nullptr);
    for (uint8_t channel : {uint8_t{4}, uint8_t{5}}) {
        const auto parameters = encode_feature_parameters(
            std::vector<FeatureParameterRecord>{{ParameterId("Channel"), 1, {channel}}}, logic->contract.parameters);
        assert(parameters);
        const auto logic_node = FeatureNodeId("Logic" + std::to_string(channel));
        const auto render_node = FeatureNodeId("Render" + std::to_string(channel));
        graph.nodes.push_back({logic_node, logic->id, logic->version, {}, parameters.value});
        graph.nodes.push_back({render_node, render->id, render->version, {}, parameters.value});
        graph.edges.push_back({logic_node, dynamic_feature_test::kColor, render_node,
                               dynamic_feature_test::kColor, EdgeKind::kSnapshot, ClockId("Frame"),
                               ClockId("Frame"), 1, 0, 8});
        if (channel == 4)
            graph.process_subgraphs.push_back({logic_node, {"Idle", "Active"}, {"Idle->Active", "Active->Idle"},
                                               1, "Idle", {}});
    }
    return graph;
}

foundation::Result write_graph_metadata(const std::filesystem::path& graph_path) {
    const auto source = tooling::SourceId::parse(kSourceUuid);
    const auto product = tooling::AssetId::parse(kGraphUuid);
    if (!source || !product) return foundation::invalid_argument("FeatureGraph test identities are invalid");
    tooling::AssetMeta metadata{};
    metadata.source = source.value;
    metadata.importer = "aeg.feature_graph";
    metadata.products = {{product.value, tooling::AssetKind::kFeatureGraph, "Default"}};
    return tooling::AssetMetaCodec{}.save(graph_path.string() + ".meta", metadata);
}

foundation::Result configure_project(const std::filesystem::path& project_file) {
    application::project::ProjectFileCodec codec;
    auto project = codec.load(project_file);
    if (!project) return project.result;
    project.value.config.project_uuid = std::string(kProjectUuid);
    project.value.config.target = "Fixture";
    project.value.config.module.id = "com.aeg.dynamic.fixture";
    project.value.config.module.target = "GameModule";
    return codec.save(project_file, project.value.config);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        namespace fs = std::filesystem;
        const auto root = fs::temp_directory_path() / "aeg_feature_end_to_end_smoke";
        std::error_code error;
        fs::remove_all(root, error);

        const auto scaffold = application::project::create_project(
            {root / "project", "Feature E2E", "GameModule", "empty", "1.0", "Assets/Features/Default.aegfeature"});
        assert(scaffold);
        const auto project_file = scaffold.value;
        assert(configure_project(project_file));

        module::FeatureModuleDefinition module_definition = dynamic_feature_test::definition();
        feature::FeatureRegistry registry{aeg::feature::test::clock_test_host()};
        for (const auto& definition : module_definition.feature_definitions)
            assert(aeg::feature::test::register_clock_test_definition(registry, definition));
        const auto graph_path = root / "project" / "Assets" / "Features" / "Default.aegfeature";
        const auto graph = make_graph(registry);

        // The Editor-owned document is the only authoring snapshot used by the
        // test.  Save and reopen that snapshot before Cook, preserving the
        // canonical contract digest and non-empty node/edge topology.
        application::EditorFeatureGraphDocument editor{graph, module_definition.feature_catalog};
        assert(!editor.dirty());
        assert(application::authoring::save_feature_graph_document(graph_path, editor.snapshot()));
        assert(write_graph_metadata(graph_path));
        const auto reopened = application::authoring::load_feature_graph_document(graph_path);
        assert(reopened && reopened.value.nodes.size() == 4 && reopened.value.edges.size() == 2 &&
               reopened.value.contract_digest == registry.contract_digest());

        const auto bundle = module_bundle_test::write_bundle(
            fs::absolute(argv[1]), root / "bundle", module_bundle_test::metadata_for(module_definition));
        const auto cooked = tooling::cook_project(
            {project_file, bundle.manifest_path, root / "delivery", "Debug"});
        if (!cooked) {
            std::cerr << "Feature E2E Cook failed: " << cooked.value.diagnostics.front().message << "\n";
            return 3;
        }
        assert(fs::exists(cooked.value.package) && fs::exists(cooked.value.game_manifest));

        application::runtime::RuntimeBootstrap bootstrap;
        const auto transport_ns = std::make_shared<std::atomic<uint64_t>>(0);
        const auto configured = application::runtime::RuntimeFeatureControl::set_transport_time_source(
            bootstrap, [transport_ns] { return transport_ns->load(std::memory_order_relaxed); });
        if (!configured) {
            std::cerr << "Feature E2E transport clock configuration failed: " << configured.message << "\n";
            return 11;
        }
        application::runtime::RuntimeBootRequest request{};
        request.manifest_path = root / "delivery" / "current.toml";
        request.headless = true;
        request.offscreen = true;
        double clock_seconds = 0.0;
        request.clock = {[](void* value) noexcept { return *static_cast<double*>(value); }, &clock_seconds};
        const application::runtime::RuntimeBuildIdentity identity{"b", "Fixture", "Debug"};
        const auto initialized = bootstrap.initialize(request, std::nullopt, identity);
        if (!initialized) {
            std::cerr << "Feature E2E Runtime initialize failed: " << initialized.message << "\n";
            return 7;
        }
        assert(bootstrap.startup_feature_graph_count() == 1);
        const auto active_reconfiguration = application::runtime::RuntimeFeatureControl::set_transport_time_source(
            bootstrap, [] { return uint64_t{999'999'999}; });
        assert(!active_reconfiguration && active_reconfiguration.code == foundation::ResultCode::kInvalidState);
        const auto first_tick = bootstrap.tick();
        if (!first_tick) {
            std::cerr << "Feature E2E first Runtime tick failed: " << first_tick.message << "\n";
            return 5;
        }
        // A host scheduling stall longer than the authored 8/60 s Snapshot
        // lifetime must not advance this fixture's injected transport clock.
        constexpr uint64_t snapshot_deadline_ns = 133'333'334;
        const auto stalled_at = std::chrono::steady_clock::now();
        assert(concurrency::sleep_for(200'000'000));
        assert(std::chrono::steady_clock::now() - stalled_at > std::chrono::nanoseconds(snapshot_deadline_ns));
        assert(transport_ns->load(std::memory_order_relaxed) == 0);
        // Pump real worker completions with both fixture clocks frozen.
        // Snapshot transport consumes the first Logic message on the next wave.
        bool first_wave_settled = false;
        for (unsigned spin = 0; spin != 2000; ++spin) {
            if (const auto tick = bootstrap.tick(); !tick) {
                std::cerr << "Feature E2E completion tick failed: " << tick.message << "\n";
                return 5;
            }
            first_wave_settled = true;
            for (const auto& node : graph.nodes) {
                const auto observed = application::runtime::RuntimeFeatureControl::inspect_node(
                    bootstrap, kGraphUuid, node.node_id);
                first_wave_settled = first_wave_settled && observed && !observed.value.owners.empty() &&
                    observed.value.owners.front().invocations.last_committed.has_value() &&
                    observed.value.owners.front().invocations.open_attempts.empty();
            }
            if (first_wave_settled) break;
            assert(concurrency::sleep_for(1'000'000));
        }
        if (!first_wave_settled) {
            std::cerr << "Feature E2E first DLL wave did not settle\n";
            return 6;
        }
        const auto logic_snapshot = application::runtime::RuntimeFeatureControl::inspect_node(
            bootstrap, kGraphUuid, feature::FeatureNodeId("Logic4"));
        if (!logic_snapshot || logic_snapshot.value.owners.size() != 1 ||
            logic_snapshot.value.owners.front().processes.size() != 1) {
            std::cerr << "Feature E2E Logic Process controls unavailable: owners="
                      << (logic_snapshot ? logic_snapshot.value.owners.size() : 0) << " processes="
                      << (logic_snapshot && !logic_snapshot.value.owners.empty()
                              ? logic_snapshot.value.owners.front().processes.size()
                              : 0)
                      << "\n";
            return 10;
        }
        const auto logic_process = logic_snapshot.value.owners.front().processes.front();
        application::runtime::RuntimeProcessCommand process_command{
            logic_snapshot.value.generation, std::string(kGraphUuid), feature::FeatureNodeId("Logic4"),
            logic_snapshot.value.owners.front().owner, logic_process.id, logic_process.revision,
            application::runtime::RuntimeProcessAction::kSuspend};
        assert(application::runtime::RuntimeFeatureControl::control_process(bootstrap, process_command));
        const auto suspended = application::runtime::RuntimeFeatureControl::inspect_node(
            bootstrap, kGraphUuid, feature::FeatureNodeId("Logic4"));
        assert(suspended && suspended.value.owners.front().processes.front().status == feature::ProcessStatus::kSuspended);
        process_command.expected_revision = suspended.value.owners.front().processes.front().revision;
        process_command.action = application::runtime::RuntimeProcessAction::kResume;
        assert(application::runtime::RuntimeFeatureControl::control_process(bootstrap, process_command));
        clock_seconds += 1.0 / 60.0;
        transport_ns->fetch_add(16'666'667, std::memory_order_relaxed);
        foundation::ResultValue<std::vector<feature::RenderFeatureOutput>> outputs{};
        for (unsigned spin = 0; spin != 2000; ++spin) {
            const auto tick = bootstrap.tick();
            if (!tick) {
                std::cerr << "Feature E2E Runtime tick failed: " << tick.message << "\n";
                return 5;
            }
            outputs = application::runtime::RuntimeFeatureControl::capture_render_outputs(bootstrap);
            if (!outputs) {
                std::cerr << "Feature E2E Render capture failed: " << outputs.result.message << "\n";
                return 8;
            }
            if (outputs.value.size() == 2) break;
            assert(concurrency::sleep_for(1'000'000));
        }
        if (outputs.value.size() != 2) {
            std::cerr << "Feature E2E Render capture returned " << outputs.value.size() << " outputs\n";
            return 9;
        }
        for (const auto& output : outputs.value) {
            assert(output.resource_owner.definition_digest != 0 && output.instance_id.is_valid() &&
                   output.intent.final_output.has_value());
            assert(output.input_messages.size() == 1 && output.input_messages.front().payload.size() == 1);
            const auto& timing = output.input_messages.front().source.timing;
            assert(timing && timing->origin.nanoseconds == 0 && timing->expires_at &&
                   timing->expires_at->nanoseconds == snapshot_deadline_ns);
            const auto channel = output.input_messages.front().payload.front();
            assert(channel == 4 || channel == 5);
            const auto observed = application::runtime::RuntimeFeatureControl::inspect_node(
                bootstrap, kGraphUuid, feature::FeatureNodeId("Render" + std::to_string(channel)));
            assert(observed && observed.value.owners.front().render_output &&
                   observed.value.owners.front().render_output->invocation == output.invocation);
        }
        assert(bootstrap.shutdown());

        // RenderGraph consumes the actual Runtime-owned Feature output values
        // after the dynamic module has been unloaded, proving the output is a
        // value/lease contract and not a callback into the module DLL.
        auto device = rhi::create_device({});
        assert(device);
        render_feature_test::execute_pixels_and_retire_owners(*device.value, outputs);
        fs::remove_all(root, error);
        std::cout << "feature non-empty Editor-Cook-Runtime-Render end-to-end passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << "\n";
        return 4;
    }
}
