#include "feature_clock_test_fixture.hpp"
#include <array>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "dynamic_module.hpp"
#include "module_dynamic_feature_fixture.hpp"
#include "module_bundle_test_fixture.hpp"
#include "../Engine/Module/abi/feature_module_api.h"
#include "../Engine/Module/private/dynamic_library.hpp"
#include "module_dynamic_feature_probe.hpp"
#include "feature_render_gpu_fixture.hpp"
#include "concurrency.hpp"

namespace {
using namespace aeg;
using namespace aeg::module;
using namespace aeg::feature;
using render_feature_test::require;

using module_bundle_test::write_bundle;
using module_bundle_test::metadata_for;
using dynamic_feature_test::bind_probe;
using dynamic_feature_test::Probe;
using dynamic_feature_test::ReleaseOnExit;
void negotiation_precedes_factory(const std::filesystem::path& binary) {
    auto library = aeg::module::detail::DynamicLibrary::open(binary);
    require(static_cast<bool>(library), "negotiation probe DLL open failed");
    const auto bind = library.value.symbol("aeg_dynamic_test_bind_probe");
    const auto symbol = library.value.symbol("aeg_get_module_api");
    require(static_cast<bool>(bind) && static_cast<bool>(symbol), "negotiation probe entry is missing");
    Probe probe{};
    const auto bind_fn = reinterpret_cast<AegDynamicBindProbe>(bind.value);
    bind_fn(probe.view());
    const auto entry = reinterpret_cast<AegAbiGetModuleApi>(symbol.value);
    AegAbiModuleApi output{};
    const auto invalid = entry(AEG_MODULE_ABI_MAJOR, AEG_MODULE_ABI_MINOR, sizeof(output), nullptr);
    const auto unsupported = entry(AEG_MODULE_ABI_MAJOR - 1, AEG_MODULE_ABI_MINOR, sizeof(output), &output);
    const bool passed = invalid.code == AEG_ABI_INVALID_ARGUMENT && unsupported.code == AEG_ABI_UNSUPPORTED &&
                        probe.factories == 0;
    bind_fn({});
    require(passed, "invalid ABI negotiation initialized the real DLL factory");
}
void reject_invalid_contexts(const DynamicModuleHandle& handle) {
    Probe probe{};
    bind_probe(handle)(probe.view());
    const auto table = *handle.api.features;
    const AegFeatureInvocation invocation{sizeof(AegFeatureInvocation), AEG_FEATURE_EXECUTION_API_VERSION};
    for (unsigned index = 0; index < 5; ++index) {
        auto context = table.context;
        if (index == 0) ++context.owner_cookie;
        if (index == 1) ++context.generation;
        if (index == 2) context.slot = UINT32_MAX;
        if (index == 3) ++context.kind;
        if (index == 4) context.reserved = 1;
        const AegAbiByteSink sink{&probe, Probe::write};
        AegFeatureCallbackInfo info{sizeof(info)};
        require(table.execute(table.user_data, context, dynamic_feature_test::kLogic.value(), &invocation, sink).code != AEG_ABI_OK &&
                table.validate_state(table.user_data, context, dynamic_feature_test::kLogic.value(), {}, sink).code != AEG_ABI_OK &&
                table.write_contracts(table.user_data, context, sink).code != AEG_ABI_OK &&
                table.describe_feature(table.user_data, context, 0, &info).code != AEG_ABI_OK,
                "real DLL endpoint accepted invalid context");
    }
    require(probe.executes == 0 && probe.validators == 0 && probe.writes == 0,
            "invalid context reached author callbacks or sink");
    for (unsigned index = 0; index < 6; ++index) {
        auto invalid = invocation;
        if (index == 0) invalid.struct_size = sizeof(invalid) - 1;
        if (index == 1) ++invalid.version;
        if (index == 2) invalid.reserved = 1;
        if (index == 3) invalid.input = {nullptr, 1};
        if (index == 4) invalid.current_state = {nullptr, 1};
        if (index == 5) invalid.parameters = {nullptr, AEG_FEATURE_ABI_MAX_BYTES + 1ull};
        require(table.execute(table.user_data, table.context, dynamic_feature_test::kLogic.value(), &invalid,
                              {&probe, Probe::write}).code == AEG_ABI_INVALID_ARGUMENT,
                "real DLL endpoint accepted malformed invocation views");
    }
    AegFeatureCallbackInfo info{sizeof(info)};
    require(table.describe_feature(table.user_data, table.context, 0, &info).code == AEG_ABI_OK &&
            info.definition_id == dynamic_feature_test::kLogic.value() && probe.executes == 0 && probe.writes == 0,
            "endpoint negative controls changed the valid context");
    bind_probe(handle)({});
}
void reject_mismatched_declarations(DynamicModuleLoader& loader, const std::filesystem::path& binary,
                                   const std::filesystem::path& directory) {
    for (unsigned index = 0; index < 8; ++index) {
        auto source = dynamic_feature_test::definition();
        if (index == 0) source.descriptor.name += " mismatch";
        if (index == 1) ++source.descriptor.version.patch;
        if (index == 2) source.descriptor.dependencies = {{ModuleId("unexpected.module"), {1,0,0}}};
        if (index == 3) {
            const auto shared_clock = source.feature_definitions.front().contract.clock.id;
            for (auto& definition : source.feature_definitions)
                if (definition.contract.clock.id == shared_clock) definition.contract.clock.frequency_hz = 30;
        }
        if (index == 4) source.feature_definitions.front().contract.ports.front().schema = ValueSchemaId("Other.Color");
        if (index == 5) source.feature_definitions.front().contract.state_payload.max_size = 1;
        if (index == 6) source.feature_definitions.pop_back();
        if (index == 7) {
            auto extra = source.feature_definitions.front();
            extra.id = extra.contract.definition_id = FeatureDefinitionId("Unexpected.Feature");
            source.feature_definitions.push_back(std::move(extra));
        }
        source.feature_catalog = {};
        try {
            for (const auto& definition : source.feature_definitions)
                require(static_cast<bool>(source.feature_catalog.export_definition(definition)), "mismatch fixture catalog failed");
            const auto bundle = write_bundle(binary, directory / std::to_string(index), metadata_for(source));
            DynamicModuleHandle handle{};
            require(static_cast<bool>(loader.load(bundle, handle)), "valid mismatch bundle did not load for declaration check");
            require(loader.make_feature_module(handle).result.code == foundation::ResultCode::kInvalidArgument &&
                    static_cast<bool>(loader.unload(handle)), "DLL declarations mismatched sidecar or failed import retained lease");
        } catch (const std::exception& error) {
            std::cerr << "Dynamic declaration mismatch index=" << index << " failed: " << error.what() << '\n';
            throw;
        }
    }
}
FeatureGraphDocument graph(const FeatureRegistry& registry, bool render = true) {
    FeatureGraphDocument authored{};
    authored.asset_uuid = "asset://dynamic-feature";
    authored.schema_version = 1;
    authored.graph_version = 1;
    const auto& logic = *registry.find_definition(dynamic_feature_test::kLogic);
    for (uint8_t channel : {uint8_t{1}, uint8_t{2}}) {
        const auto parameters = encode_feature_parameters(
            std::vector<FeatureParameterRecord>{{ParameterId("Channel"), 1, {channel}}}, logic.contract.parameters);
        require(static_cast<bool>(parameters), "dynamic graph parameters failed");
        authored.nodes.push_back({FeatureNodeId(channel), logic.id, logic.version, {}, parameters.value});
        if (render) {
            const auto& definition = *registry.find_definition(FeatureDefinitionId("Render.IntentSmoke"));
            authored.nodes.push_back({FeatureNodeId(channel + 10), definition.id, definition.version, {}, parameters.value});
            authored.edges.push_back({FeatureNodeId(channel), dynamic_feature_test::kColor, FeatureNodeId(channel + 10),
                dynamic_feature_test::kColor, EdgeKind::kSnapshot, ClockId("Frame"), ClockId("Frame"), 1, 0, 8});
        }
        if (!render) break;
    }
    return authored;
}
foundation::ResultValue<std::vector<RenderFeatureOutput>> execute_graph(DynamicModuleLoader& loader,
    DynamicModuleHandle& handle, ModuleRegistry& modules, FeatureRegistry& features) {
    auto imported = loader.make_feature_module(handle);
    require(static_cast<bool>(imported), "dynamic module reimport failed");
    require(static_cast<bool>(register_static_module(modules, std::move(imported.value))), "dynamic module registration failed");
    FeatureRuntime runtime(features);
    auto authored = graph(features);
    auto compiled = FeatureGraphCompiler{}.compile(authored, features);
    if (!compiled) throw std::runtime_error("dynamic graph compilation: " + compiled.result.message);
    const auto published = runtime.publish_graph(authored, compiled.value);
    if (!published) throw std::runtime_error("dynamic graph publication: " + published.message);
    require(static_cast<bool>(runtime.tick(ClockId("Frame"))), "real DLL Logic-to-Render tick failed");
    const auto captured = runtime.capture_render_outputs(compiled.value);
    require(static_cast<bool>(captured) && captured.value.size() == 2, "DLL Render outputs were not committed");
    for (const auto& output : captured.value) {
        require(output.input_messages.size() == 1 && output.input_messages.front().delivery_id.is_valid(),
                "dynamic Render did not consume a real routed receipt");
        const auto& message = output.input_messages.front();
        const auto* source = runtime.find_instance(message.source.instance);
        require(source && source->definition_id == dynamic_feature_test::kLogic && source->state.payload == std::vector<uint8_t>{1} &&
                message.source.definition_digest == source->definition_digest && message.source.invocation.generation == runtime.generation() &&
                message.source.clock == ClockId("Frame") && message.target_port == dynamic_feature_test::kColor &&
                message.schema == dynamic_feature_test::kSchema && message.payload.size() == 1,
                "dynamic receipt lost actual producer provenance");
        const auto* owner = runtime.find_instance(output.instance_id);
        require(owner && owner->state.payload == std::vector<uint8_t>{1} && owner->pending_messages.empty(),
                "DLL per-instance state or receipt consumption failed");
    }
    const auto& logic = *features.find_definition(dynamic_feature_test::kLogic);
    const auto owner_id = compiled.value.provenance.front().instance_id;
    const auto token = runtime.invoke(owner_id);
    require(static_cast<bool>(token), "dynamic illegal-state invocation failed");
    const auto* owner = runtime.find_instance(owner_id);
    const StateSnapshot snapshot{owner->state.version, owner->state.schema_version, owner->state.payload, {}};
    const std::array<uint8_t, 1> invalid{255};
    const auto delta = logic.execute(token.value, snapshot, invalid, owner->parameters);
    require(static_cast<bool>(delta) && runtime.complete(token.value, delta.value).code == foundation::ResultCode::kInvalidArgument &&
            runtime.find_instance(owner_id)->state.payload == std::vector<uint8_t>{1} &&
            runtime.find_instance(owner_id)->state.commit_count == 1,
            "real DLL state validator did not reject owner commit atomically");
    require(loader.unload(handle).code == foundation::ResultCode::kTimeout, "registered callbacks did not retain DLL");
    return captured;
}
void async_retired_dll(DynamicModuleLoader& loader, DynamicModuleHandle& old, const ModuleBundle& old_bundle,
                      const ModuleBundle& next_bundle, FeatureRegistry& registry) {
    require(static_cast<bool>(loader.load(old_bundle, old)), "old DLL async reload failed");
    auto imported = loader.make_feature_module(old);
    require(static_cast<bool>(imported) && static_cast<bool>(aeg::feature::test::register_clock_test_definition(registry, std::move(imported.value.feature_definitions.front()))),
            "old DLL async callback registration failed");
    imported = {};
    Probe probe{};
    probe.gated = true;
    bind_probe(old)(probe.view());
    DynamicModuleHandle next{};
    {
        FeatureRuntime runtime(registry);
        ReleaseOnExit release_on_exit{probe};
        auto authored = graph(registry, false);
        auto compiled = FeatureGraphCompiler{}.compile(authored, registry);
        require(static_cast<bool>(compiled) && static_cast<bool>(runtime.publish_graph(authored, compiled.value)) &&
                static_cast<bool>(runtime.enable_async_execution()) && static_cast<bool>(runtime.tick(ClockId("Frame"))),
                "real DLL async submission failed");
        require(static_cast<bool>(probe.entered.completion().wait(2'000'000'000)) &&
                probe.thread != concurrency::current_thread_id() && runtime.pending_execution_count() == 1,
                "old DLL was not blocked inside actual worker callback");
        const auto instance = compiled.value.provenance.front().instance_id;
        require(static_cast<bool>(loader.load(next_bundle, next)), "new DLL load failed");
        auto replacement = loader.make_feature_module(next);
        require(static_cast<bool>(replacement) && static_cast<bool>(runtime.replace_definition(
            replacement.value.feature_definitions.front(), {{1,0,0},{2,0,0},{1,0,0},{1,0,0}})), "new DLL callback migration failed");
        replacement = {};
        require(loader.unload(old).code == foundation::ResultCode::kTimeout,
                "old DLL unloaded while only in-flight work retained its callback");
        static_cast<void>(probe.release.complete());
        for (unsigned spin = 0; spin < 2000 && runtime.pending_execution_count(); ++spin) {
            require(static_cast<bool>(runtime.pump_completions()), "retired DLL completion pump failed");
            static_cast<void>(concurrency::sleep_for(1'000'000));
        }
        require(runtime.pending_execution_count() == 0 && runtime.find_instance(instance)->state.payload.empty() &&
                runtime.find_instance(instance)->state.commit_count == 0,
                "late old DLL result committed after replacement");
        bind_probe(old)({});
        require(static_cast<bool>(loader.unload(old)), "retired DLL code lease did not release after collection");
        require(static_cast<bool>(runtime.tick(ClockId("Frame"))), "new DLL async tick failed");
        for (unsigned spin = 0; spin < 2000 && runtime.pending_execution_count(); ++spin) {
            require(static_cast<bool>(runtime.pump_completions()), "new DLL completion pump failed");
            static_cast<void>(concurrency::sleep_for(1'000'000));
        }
        require(runtime.pending_execution_count() == 0 && runtime.find_instance(instance)->state.payload == std::vector<uint8_t>{1} &&
                runtime.find_instance(instance)->state.commit_count == 1, "new DLL callback did not commit");
        require(static_cast<bool>(runtime.tick(ClockId("Frame"))), "new DLL second async tick failed");
        for (unsigned spin = 0; spin < 2000 && runtime.pending_execution_count(); ++spin) {
            require(static_cast<bool>(runtime.pump_completions()), "new DLL second pump failed");
            static_cast<void>(concurrency::sleep_for(1'000'000));
        }
        require(runtime.find_instance(instance)->state.payload == std::vector<uint8_t>{11}, "new DLL implementation was not selected");
        require(static_cast<bool>(runtime.shutdown_execution()), "dynamic Runtime stop/join failed");
    }
    require(static_cast<bool>(registry.unregister_definition(dynamic_feature_test::kLogic)) &&
            static_cast<bool>(loader.unload(next)), "new DLL callbacks did not release after shutdown/unregistration");
}
}  // namespace
int main(int argc, char** argv) {
    try {
    using namespace aeg::module;
    aeg::feature::FeatureRegistry features{aeg::feature::test::clock_test_host()};
    ModuleRegistry modules(features);
    DynamicModuleLoader loader(modules);
    DynamicModuleHandle handle;
    const auto path = std::filesystem::temp_directory_path() / "aeg_dynamic_manifest.json";
    ModuleMetadata metadata{}; metadata.module={ModuleId{"com.aeg.dynamic.fixture"}, "Dynamic Fixture", {1,0,0},{}};
    metadata.build_id="b"; metadata.contract_digest="d";
    const auto encoded=encode_module_metadata(metadata); if (!encoded) return 1;
    { std::ofstream out(path); out << encoded.value; }
    const auto read = read_module_metadata(path);
    if (!read) return 1;
    if (validate_module_bundle(path)) { std::cerr << "metadata-only bundle compatibility is still accepted\n"; return 41; }
    ModuleBundle forged{}; forged.metadata = read.value; forged.manifest_path = path; forged.metadata_path = path;
    if (loader.load(forged, handle) || handle.loaded || handle.native_handle) {
        std::cerr << "dynamic loader accepted metadata-only/no-DLL compatibility\n"; return 42;
    }
    if (argc != 3) return 7;
    const auto fixture = std::filesystem::temp_directory_path() / "aeg_dynamic_lifetime_fixture";
    std::error_code error; std::filesystem::remove_all(fixture, error); std::filesystem::create_directories(fixture);
    const auto source_module = dynamic_feature_test::definition();
    metadata = metadata_for(source_module);
    const auto formal_bundle = write_bundle(std::filesystem::absolute(argv[1]), fixture, metadata);
    const auto binary_path = formal_bundle.binary_path;
    negotiation_precedes_factory(binary_path);
    foundation::ResultValue<ModuleBundle> bundle{foundation::Result::success(), formal_bundle};
    auto missing = bundle.value; missing.binary_path = fixture / "missing-game-module.dll";
    std::filesystem::rename(binary_path, missing.binary_path);
    if (loader.load(bundle.value, handle) || handle.loaded) return 2;
    std::filesystem::rename(missing.binary_path, binary_path);
    if (!loader.load(bundle.value, handle) || !handle.loaded || !handle.native_handle || handle.api.required_capabilities != AEG_CAP_FEATURES ||
        handle.api.abi_major != AEG_MODULE_ABI_MAJOR) return 2;
    if (loader.load(bundle.value, handle))
        return 3;
    reject_invalid_contexts(handle);
    {
        auto imported = loader.make_feature_module(handle);
        if (!imported || imported.value.feature_definitions.empty()) {
            std::cerr << "real DLL has no imported executable Features\n";
            return 10;
        }
        const auto& logic = imported.value.feature_definitions.front();
        const aeg::feature::InvocationToken token{aeg::feature::InvocationId(11), aeg::feature::AttemptId(12),
            aeg::feature::RuntimeGeneration(13), 14, aeg::feature::ProcessRevision(15), aeg::feature::StateVersion(16)};
        const aeg::feature::StateSnapshot snapshot{aeg::feature::StateVersion(17), {19, 2, 3}, {7}, std::string("run\0DLL", 7)};
        const auto parameters = aeg::feature::encode_feature_parameters(
            std::vector<aeg::feature::FeatureParameterRecord>{{aeg::feature::ParameterId("Channel"), 1, {2}}},
            logic.contract.parameters);
        if (!parameters || !logic.execute || !logic.validate_state_payload) return 11;
        const auto executed = logic.execute(token, snapshot, {}, parameters.value);
        if (!executed || executed.value.expected_version != token.state_version || executed.value.payload_revision != 14 ||
            executed.value.next_state != snapshot.current_state || executed.value.state_payload != std::vector<uint8_t>{8} ||
            executed.value.output_payload != std::vector<uint8_t>{2} || executed.value.output_messages.size() != 1) return 12;
        const auto& message = executed.value.output_messages.front();
        if (message.source_port != dynamic_feature_test::kColor || message.schema != dynamic_feature_test::kSchema ||
            message.sequence != 11 || message.causality_token != 12 || message.timestamp_ticks != 13 ||
            message.transaction_id != 17 || message.deadline_ticks != 20 || message.schema_version != 19 ||
            message.delivery_id.is_valid()) return 13;
        const std::array<uint8_t, 1> bad_state{255}, not_ready{253}, throws{254}, preserve{252}, clear{251};
        if (logic.validate_state_payload(bad_state).code != aeg::foundation::ResultCode::kInvalidArgument ||
            !logic.validate_state_payload({})) return 14;
        const auto failed = logic.execute(token, snapshot, not_ready, parameters.value);
        if (failed.result.code != aeg::foundation::ResultCode::kNotReady || failed.result.message != std::string("wait\0DLL", 8) ||
            logic.execute(token, snapshot, throws, parameters.value).result.code != aeg::foundation::ResultCode::kUnknown ||
            logic.execute(token, snapshot, preserve, parameters.value).value.state_payload.has_value() ||
            logic.execute(token, snapshot, clear, parameters.value).value.state_payload != std::vector<uint8_t>{}) return 15;
        if (loader.unload(handle).code != aeg::foundation::ResultCode::kTimeout) return 16;
    }
    {
        auto imported = loader.make_feature_module(handle);
        require(static_cast<bool>(imported), "validator-only import failed");
        auto validator = imported.value.feature_definitions.front().validate_state_payload;
        imported = {};
        require(loader.unload(handle).code == foundation::ResultCode::kTimeout &&
                static_cast<bool>(validator(std::array<uint8_t, 1>{7})),
                "standalone state-validator copy did not retain DLL code");
    }
    const auto outputs = execute_graph(loader, handle, modules, features);
    require(static_cast<bool>(modules.unregister_module(source_module.descriptor.id)), "dynamic module unregister failed");
    {
        const auto lease = loader.acquire(handle);
        if (!lease || !lease.value.valid())
            return 4;
        const auto blocked = loader.unload(handle, 1);
        if (blocked.code != aeg::foundation::ResultCode::kTimeout || !handle.loaded)
            return 5;
    }
    if (!loader.unload(handle) || handle.loaded)
        return 6;
    if (handle.native_handle || handle.api.struct_size) return 9;
    auto device = aeg::rhi::create_device({});
    require(static_cast<bool>(device), "dynamic GPU device creation failed");
    render_feature_test::execute_pixels_and_retire_owners(*device.value, outputs);
    const auto next_directory = fixture / "v2";
    const auto next_bundle = write_bundle(std::filesystem::absolute(argv[2]), next_directory,
                                         metadata_for(dynamic_feature_test::definition(2)));
    async_retired_dll(loader, handle, formal_bundle, next_bundle, features);
    reject_mismatched_declarations(loader, std::filesystem::absolute(argv[1]), fixture / "mismatched");
    std::filesystem::remove(path); std::filesystem::remove_all(fixture, error);
    std::cout << "dynamic module lifetime smoke passed\n";
    return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << "\n"; return 50; }
}
