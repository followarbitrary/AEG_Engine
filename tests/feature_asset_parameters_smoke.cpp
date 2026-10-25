#include "feature_clock_test_fixture.hpp"
#include <filesystem>
#include <iostream>
#include <utility>

#include "asset_package.hpp"
#include "cooked_asset.hpp"
#include "feature_graph.hpp"

namespace {
using namespace aeg;

bool require(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

feature::FeatureDefinition definition(std::string_view name, std::string uuid, int& calls) {
    feature::FeatureDefinition result{};
    result.id = feature::FeatureDefinitionId{name};
    result.version = {1, 0, 0};
    auto& contract = result.contract;
    contract.definition_id = result.id;
    contract.version = result.version;
    contract.domains = feature::to_mask(feature::FeatureDomain::kLogic);
    contract.scope.id = feature::ScopeId{"World"};
    contract.clock = aeg::feature::make_periodic_clock_contract(feature::ClockId{"Simulation"}, 60, 4, feature::LateDataPolicy::kQueue,
        aeg::feature::DeadlinePolicy::kObserve, contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    contract.permissions.version = 1;
    feature::ParameterMetadata metadata{};
    metadata.type = feature::ParameterType::kAssetReference;
    metadata.default_value = foundation::AssetReference{std::move(uuid)};
    metadata.constraint = feature::AssetConstraint{false, foundation::AssetKind::kTexture};
    contract.parameters = {{feature::ParameterId{1}, 1, 16, false, metadata}};
    result.execute = [&calls](const feature::InvocationToken& token, const feature::StateSnapshot&,
                              std::span<const uint8_t>, std::span<const uint8_t>) {
        ++calls;
        return foundation::ResultValue<feature::StateDelta>{foundation::Result::success(), {token.state_version, 0}};
    };
    return result;
}

feature::FeatureGraphDocument graph_for(const feature::FeatureDefinition& definition) {
    feature::FeatureCatalog catalog;
    if (!catalog.export_definition(definition)) return {};
    feature::FeatureGraphDocument graph{};
    graph.asset_uuid = "30000000-0000-0000-0000-000000000001";
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.contract_digest = catalog.contract_digest();
    graph.nodes = {{feature::FeatureNodeId{1}, definition.id, definition.version, {}, {}}};
    return graph;
}
bool rejects_missing_resolver() {
    int calls{};
    auto executable = definition("Logic.Texture", "10000000-0000-0000-0000-000000000001", calls);
    feature::FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    if (!require(static_cast<bool>(aeg::feature::test::register_clock_test_definition(registry, executable)), "asset parameter definition was rejected")) return false;
    auto graph = graph_for(executable);
    auto compiled = feature::FeatureGraphCompiler{}.compile(graph, registry);
    if (!require(static_cast<bool>(compiled), "asset parameter graph did not compile")) return false;
    feature::FeatureRuntime runtime{registry};
    const auto generation = runtime.generation();
    if (!require(!runtime.publish_graph(graph, compiled.value),
                 "typed asset parameter published without an asset resolver") ||
        !require(runtime.generation() == generation && calls == 0,
                 "asset validation failure changed the live generation or entered a callback")) return false;
    const auto expanded = registry.expand(executable.id, {});
    if (!expanded) return false;
    const auto instance = runtime.activate(expanded.value, executable.contract.scope.id);
    return require(instance && !runtime.dispatch_call(instance.value) && calls == 0,
                   "direct dispatch bypassed the required asset resolver");
}

bool validates_packaged_assets(const std::filesystem::path& root) {
    const auto texture_id = foundation::AssetId::parse("10000000-0000-0000-0000-000000000001").value;
    const auto mesh_id = foundation::AssetId::parse("10000000-0000-0000-0000-000000000002").value;
    const auto missing_id = foundation::AssetId::parse("10000000-0000-0000-0000-000000000003").value;
    std::vector<tooling::AssetPackageEntry> entries;
    for (const auto& [id, kind] : {std::pair{texture_id, foundation::AssetKind::kTexture},
                                   std::pair{mesh_id, foundation::AssetKind::kMesh}}) {
        const auto bytes = asset::encode_cooked_asset({id, kind, 1, {}, {std::byte{0x01}}});
        if (!bytes) return false;
        entries.push_back({id, kind, 1, bytes.value});
    }
    if (!tooling::AssetPackageWriter{}.write(root / "assets.aegbin", entries)) return false;
    auto loaded = asset::AssetPackageReader::open(root / "assets.aegbin");
    if (!loaded) return false;
    auto reader = std::move(loaded.value);
    const feature::AssetKindResolver resolver = [&reader](foundation::AssetId id) -> foundation::ResultValue<foundation::AssetKind> {
        const auto cooked = reader.read_cooked(id);
        return cooked ? foundation::ResultValue<foundation::AssetKind>{foundation::Result::success(), cooked.value.kind}
                      : foundation::ResultValue<foundation::AssetKind>{cooked.result, {}};
    };
    int calls{};
    auto executable = definition("Logic.PackagedTexture", texture_id.to_string(), calls);
    feature::FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    if (!aeg::feature::test::register_clock_test_definition(registry, executable)) return false;
    feature::FeatureRuntime runtime{registry, resolver};
    auto graph = graph_for(executable);
    auto compiled = feature::FeatureGraphCompiler{}.compile(graph, registry);
    if (!require(compiled && runtime.publish_graph(graph, compiled.value), "valid packaged default failed graph publication")) return false;
    const auto instance = compiled.value.provenance.front().instance_id;
    if (!require(runtime.dispatch_call(instance) && calls == 1, "valid packaged default did not reach the callback")) return false;
    const auto generation = runtime.generation();
    for (const auto id : {mesh_id, missing_id}) {
        const auto payload = feature::serialize_parameter_value(foundation::AssetReference{id.to_string()}, feature::ParameterType::kAssetReference);
        if (!payload) return false;
        const auto blob = feature::encode_feature_parameters(
            std::vector<feature::FeatureParameterRecord>{{feature::ParameterId{1}, 1, payload.value}}, executable.contract.parameters);
        if (!blob) return false;
        auto invalid = graph;
        invalid.nodes.front().parameter_blob = blob.value;
        auto candidate = feature::FeatureGraphCompiler{}.compile(invalid, registry);
        if (!require(candidate && !runtime.publish_graph(invalid, candidate.value), "wrong-kind or missing packaged override published") ||
            !require(runtime.generation() == generation && runtime.find_instance(instance) != nullptr && calls == 1,
                     "rejected asset override changed the live cohort")) return false;
    }
    auto replacement = executable;
    replacement.version = {2, 0, 0}; replacement.contract.version = replacement.version;
    std::get<feature::AssetConstraint>(replacement.contract.parameters.front().metadata->constraint).kind = foundation::AssetKind::kMesh;
    const feature::StateMigration migration{executable.version, replacement.version, {1, 0, 0}, {1, 0, 0}};
    if (!require(!runtime.replace_definition(replacement, migration) && runtime.generation() == generation &&
                 registry.find_definition(executable.id)->version == executable.version,
                 "replacement accepted an existing parameter with the wrong packaged kind")) return false;

    // An Editor resolver may change while a cohort is active. Recheck before
    // invoking, without issuing work or retiring the preceding commit.
    const auto wrong_bytes = asset::encode_cooked_asset({texture_id, foundation::AssetKind::kMesh, 1, {}, {std::byte{0x02}}});
    if (!wrong_bytes || !tooling::AssetPackageWriter{}.write(root / "changed.aegbin",
            {{texture_id, foundation::AssetKind::kMesh, 1, wrong_bytes.value}})) return false;
    auto changed = asset::AssetPackageReader::open(root / "changed.aegbin");
    if (!changed) return false;
    reader = std::move(changed.value);
    const auto before = runtime.inspect_invocations(instance);
    if (!require(before && !runtime.dispatch_call(instance) && calls == 1 && runtime.generation() == generation,
                 "callback consumed a parameter whose resolved kind changed after publication")) return false;
    const auto after = runtime.inspect_invocations(instance);
    if (!require(after && after.value.open_attempts.empty() && after.value.last_issued && before.value.last_issued &&
                 after.value.last_issued->token.invocation_id == before.value.last_issued->token.invocation_id,
                 "asset rejection issued or leaked an invocation")) return false;
    return true;
}

bool validates_direct_and_composite_defaults(const std::filesystem::path& root) {
    auto loaded = asset::AssetPackageReader::open(root / "assets.aegbin");
    if (!loaded) return false;
    const feature::AssetKindResolver resolver = [&reader = loaded.value](foundation::AssetId id) -> foundation::ResultValue<foundation::AssetKind> {
        const auto cooked = reader.read_cooked(id);
        return cooked ? foundation::ResultValue<foundation::AssetKind>{foundation::Result::success(), cooked.value.kind}
                      : foundation::ResultValue<foundation::AssetKind>{cooked.result, {}};
    };
    int calls{};
    auto child = definition("Logic.AssetChild", "10000000-0000-0000-0000-000000000002", calls);
    feature::FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    if (!aeg::feature::test::register_clock_test_definition(registry, child)) return false;
    feature::FeatureRuntime runtime{registry, resolver};
    const auto expanded = registry.expand(child.id, {});
    if (!expanded) return false;
    const auto direct = runtime.activate(expanded.value, child.contract.scope.id);
    if (!require(direct && !runtime.dispatch_call(direct.value) && calls == 0,
                 "direct dispatch accepted a wrong-kind default")) return false;
    auto parent = child;
    parent.id = feature::FeatureDefinitionId{"Logic.AssetComposite"};
    parent.contract.definition_id = parent.id;
    parent.contract.parameters.clear();
    parent.contract.structure = feature::FeatureStructure::kComposite; parent.contract.clock.work_budget_ns = 0;
    parent.contract.children = {child.id};
    if (!aeg::feature::test::register_clock_test_definition(registry, parent)) return false;
    auto graph = graph_for(parent); graph.contract_digest = registry.contract_digest();
    auto compiled = feature::FeatureGraphCompiler{}.compile(graph, registry);
    const auto generation = runtime.generation();
    if (!require(compiled && !runtime.publish_graph(graph, compiled.value) && runtime.generation() == generation,
                 "Composite child default bypassed packaged kind validation")) return false;
    auto nullable = definition("Logic.NullableAsset", "", calls);
    std::get<feature::AssetConstraint>(nullable.contract.parameters.front().metadata->constraint).optional = true;
    feature::FeatureRegistry null_registry{aeg::feature::test::clock_test_host()};
    if (!aeg::feature::test::register_clock_test_definition(null_registry, nullable)) return false;
    feature::FeatureRuntime null_runtime{null_registry};
    auto null_graph = graph_for(nullable);
    auto null_compiled = feature::FeatureGraphCompiler{}.compile(null_graph, null_registry);
    return require(null_compiled && null_runtime.publish_graph(null_graph, null_compiled.value) &&
                   null_runtime.dispatch_call(null_compiled.value.provenance.front().instance_id) && calls == 1,
                   "optional null required a resolver or failed callback dispatch");
}

bool validates_inline_retry(const std::filesystem::path& root) {
    auto valid = asset::AssetPackageReader::open(root / "assets.aegbin");
    auto wrong = asset::AssetPackageReader::open(root / "changed.aegbin");
    if (!valid || !wrong) return false;
    auto reader = valid.value;
    const feature::AssetKindResolver resolver = [&reader](foundation::AssetId id) -> foundation::ResultValue<foundation::AssetKind> {
        const auto cooked = reader.read_cooked(id);
        return cooked ? foundation::ResultValue<foundation::AssetKind>{foundation::Result::success(), cooked.value.kind}
                      : foundation::ResultValue<foundation::AssetKind>{cooked.result, {}};
    };
    int calls{};
    auto executable = definition("Logic.RetryTexture", "10000000-0000-0000-0000-000000000001", calls);
    executable.contract.failure = feature::FailurePolicy::kRetry;
    executable.contract.retry = {2, 0};
    executable.execute = [&](const feature::InvocationToken& token, const feature::StateSnapshot&,
                              std::span<const uint8_t>, std::span<const uint8_t>) -> foundation::ResultValue<feature::StateDelta> {
        ++calls;
        if (calls == 1) {
            reader = wrong.value;
            return {foundation::invalid_state("retry after asset refresh"), {}};
        }
        return {foundation::Result::success(), {token.state_version, 0}};
    };
    feature::FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    if (!aeg::feature::test::register_clock_test_definition(registry, executable)) return false;
    feature::FeatureRuntime runtime{registry, resolver};
    auto graph = graph_for(executable);
    auto compiled = feature::FeatureGraphCompiler{}.compile(graph, registry);
    if (!compiled || !runtime.publish_graph(graph, compiled.value)) return false;
    const auto instance = compiled.value.provenance.front().instance_id;
    if (!require(!runtime.dispatch_call(instance) && calls == 1,
                 "zero-backoff retry consumed an asset whose kind changed in the failed callback")) return false;
    const auto snapshot = runtime.inspect_invocations(instance);
    if (!require(snapshot && snapshot.value.retry && snapshot.value.open_attempts.empty(),
                 "asset rejection discarded or issued the pending inline retry")) return false;
    reader = std::move(valid.value);
    return require(runtime.tick(executable.contract.clock.id) && calls == 2 &&
                   !runtime.inspect_invocations(instance).value.retry,
                   "preserved retry did not recover when its packaged asset was restored");
}

bool validates_any_asset_existence(const std::filesystem::path& root) {
    const auto reader = asset::AssetPackageReader::open(root / "assets.aegbin");
    if (!reader) return false;
    const feature::AssetKindResolver resolver = [&reader](foundation::AssetId id) -> foundation::ResultValue<foundation::AssetKind> {
        const auto cooked = reader.value.read_cooked(id);
        return cooked ? foundation::ResultValue<foundation::AssetKind>{foundation::Result::success(), cooked.value.kind}
                      : foundation::ResultValue<foundation::AssetKind>{cooked.result, {}};
    };
    int calls{};
    auto executable = definition("Logic.AnyAsset", "10000000-0000-0000-0000-000000000002", calls);
    executable.contract.parameters.front().metadata->constraint = feature::AssetConstraint{false};
    feature::FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    if (!aeg::feature::test::register_clock_test_definition(registry, executable)) return false;
    feature::FeatureRuntime runtime{registry, resolver};
    auto graph = graph_for(executable);
    auto compiled = feature::FeatureGraphCompiler{}.compile(graph, registry);
    if (!require(compiled && runtime.publish_graph(graph, compiled.value) &&
                 runtime.dispatch_call(compiled.value.provenance.front().instance_id) && calls == 1,
                 "Any asset rejected an existing packaged Mesh")) return false;
    const auto generation = runtime.generation();
    const auto payload = feature::serialize_parameter_value(
        foundation::AssetReference{"10000000-0000-0000-0000-000000000003"}, feature::ParameterType::kAssetReference);
    if (!payload) return false;
    const auto blob = feature::encode_feature_parameters(
        std::vector<feature::FeatureParameterRecord>{{feature::ParameterId{1}, 1, payload.value}}, executable.contract.parameters);
    if (!blob) return false;
    graph.nodes.front().parameter_blob = blob.value;
    auto missing = feature::FeatureGraphCompiler{}.compile(graph, registry);
    return require(missing && !runtime.publish_graph(graph, missing.value) &&
                   runtime.generation() == generation && calls == 1,
                   "Any asset reference published a missing packaged UUID");
}
}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "aeg_feature_asset_parameters_smoke";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    if (!rejects_missing_resolver()) return 1;
    if (!validates_packaged_assets(root)) return 2;
    if (!validates_direct_and_composite_defaults(root)) return 3;
    if (!validates_inline_retry(root)) return 4;
    if (!validates_any_asset_existence(root)) return 5;
    std::filesystem::remove_all(root, error);
    std::cout << "Feature asset parameters validate real packaged kinds before publication and callback\n";
    return 0;
}
