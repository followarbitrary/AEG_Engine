#include <iostream>
#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <map>
#include <span>
#include <vector>

#include "project_cooker.hpp"
#include "project_context.hpp"
#include "module_dynamic_feature_fixture.hpp"
#include "module_bundle_test_fixture.hpp"
#include "asset_metadata.hpp"
#include "asset_package_reader.hpp"
#include "cooked_artifact_store.hpp"
#include "cooked_asset.hpp"
#include "feature_graph_asset.hpp"
#include "feature_graph_io.hpp"
#include "feature_world_transform.hpp"
#include "game_build_manifest.hpp"
#include "reflection.hpp"
#include "scene_asset.hpp"
#include "scene_payload.hpp"
#include "serialization.hpp"
#include "world_command_routes.hpp"

namespace {
using namespace aeg;

bool check(bool value, const std::string& message) {
    if (!value) std::cerr << message << '\n';
    return value;
}

std::string file_bytes(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>{input}, {}};
}

struct ParameterCookFixture {
    std::filesystem::path root;
    application::project::ProjectConfig config{};
    tooling::AssetMeta graph_meta{};
    feature::FeatureGraphDocument graph{};
    module::ModuleBundle bundle{};
    foundation::AssetId reference = foundation::AssetId::parse("00000000-0000-4000-8000-000000000023").value;
    tooling::SourceId reference_source = tooling::SourceId::parse("00000000-0000-4000-8000-000000000024").value;
    feature::FeatureDefinition leaf{};

    ParameterCookFixture(const std::filesystem::path& binary, const std::filesystem::path& directory,
                         bool defaults, bool composite = false, bool optional = false) : root(directory) {
        using namespace feature;
        std::filesystem::create_directories(root / "Assets/Scenes");
        std::filesystem::create_directories(root / "Assets/Features");
        leaf.id = FeatureDefinitionId("Cook.AssetParameter");
        leaf.version = {1, 0, 0};
        leaf.owner_module = ModuleIdentity("com.aeg.dynamic.fixture");
        auto& contract = leaf.contract;
        contract.definition_id = leaf.id;
        contract.version = leaf.version;
        contract.domains = to_mask(FeatureDomain::kLogic);
        contract.scope.id = ScopeId("World");
        contract.clock = aeg::feature::make_periodic_clock_contract(ClockId("Cook.Clock"), 60, 4, LateDataPolicy::kReject,
            aeg::feature::DeadlinePolicy::kObserve, contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
        contract.permissions.version = 1;
        ParameterMetadata metadata{};
        metadata.type = ParameterType::kAssetReference;
        metadata.constraint = AssetConstraint{optional, foundation::AssetKind::kTexture};
        if (defaults) metadata.default_value = foundation::AssetReference{optional ? "" : reference.to_string()};
        contract.parameters = {{ParameterId("Texture"), 1, 16, true, metadata}};
        module::FeatureModuleDefinition module_definition{};
        module_definition.descriptor = {module::ModuleId("com.aeg.dynamic.fixture"), "Cook Parameters", {1, 0, 0}, {}};
        module_bundle_test::require(static_cast<bool>(module_definition.feature_catalog.export_definition(leaf)),
                                    "parameter fixture leaf contract failed");
        auto root_definition = leaf;
        if (composite) {
            auto inner = leaf;
            inner.id = FeatureDefinitionId("Cook.InnerComposite");
            inner.contract.definition_id = inner.id;
            inner.contract.structure = FeatureStructure::kComposite; inner.contract.clock.work_budget_ns = 0;
            inner.contract.parameters.clear();
            inner.contract.children = {leaf.id};
            module_bundle_test::require(static_cast<bool>(module_definition.feature_catalog.export_definition(inner)),
                                        "parameter fixture inner Composite failed");
            root_definition = inner;
            root_definition.id = FeatureDefinitionId("Cook.RootComposite");
            root_definition.contract.definition_id = root_definition.id;
            root_definition.contract.children = {inner.id};
            module_bundle_test::require(static_cast<bool>(module_definition.feature_catalog.export_definition(root_definition)),
                                        "parameter fixture root Composite failed");
        }
        bundle = module_bundle_test::write_bundle(binary, root / "Module",
            module_bundle_test::metadata_for(module_definition));
        config.name = "Parameter Cook";
        config.project_uuid = "00000000-0000-4000-8000-000000000009";
        config.module.id = "com.aeg.dynamic.fixture";
        config.target = "Fixture";
        config.startup_feature_graphs = {"Assets/Features/Startup.aegfeature"};
        module_bundle_test::require(static_cast<bool>(application::project::ProjectFileCodec{}.save(root / "Project.aegproj", config)),
                                    "parameter fixture project failed");
        const scene::SceneAsset authored({0x4000, 0x8000000000000010ull}, "Parameters", 1);
        serialization::BinaryWriter archive;
        module_bundle_test::require(static_cast<bool>(scene::SceneSerializer::serialize(authored, archive)),
                                    "parameter fixture scene failed");
        {
            std::ofstream output(root / config.startup_scene, std::ios::binary);
            output.write(reinterpret_cast<const char*>(archive.data().data()), static_cast<std::streamsize>(archive.size()));
            module_bundle_test::require(static_cast<bool>(output), "parameter fixture scene write failed");
        }
        tooling::AssetMeta scene_meta{};
        scene_meta.source = tooling::SourceId::parse("00000000-0000-4000-8000-000000000011").value;
        scene_meta.importer = "aeg.scene";
        scene_meta.products = {{foundation::AssetId::parse("00000000-0000-4000-8000-000000000010").value,
                                foundation::AssetKind::kScene, "Main"}};
        module_bundle_test::require(static_cast<bool>(tooling::AssetMetaCodec{}.save(
            (root / config.startup_scene).string() + ".meta", scene_meta)), "parameter fixture scene metadata failed");
        graph.asset_uuid = "00000000-0000-4000-8000-000000000021";
        graph.schema_version = 1;
        graph.graph_version = 1;
        graph.contract_digest = module_definition.feature_catalog.contract_digest();
        graph.nodes = {{FeatureNodeId("ParameterNode"), root_definition.id, root_definition.version, {}, {}}};
        if (!defaults && !composite) {
            const auto payload = serialize_parameter_value(foundation::AssetReference{reference.to_string()}, metadata.type);
            const std::array values{FeatureParameterRecord{contract.parameters.front().id, 1, payload.value}};
            const auto encoded = encode_feature_parameters(values, contract.parameters);
            module_bundle_test::require(static_cast<bool>(encoded), "parameter fixture override failed");
            graph.nodes.front().parameter_blob = encoded.value;
        }
        graph_meta.source = tooling::SourceId::parse("00000000-0000-4000-8000-000000000022").value;
        graph_meta.importer = "aeg.feature_graph";
        graph_meta.products = {{foundation::AssetId::parse(graph.asset_uuid).value, foundation::AssetKind::kFeatureGraph, "Startup"}};
        module_bundle_test::require(static_cast<bool>(application::authoring::save_feature_graph_document(graph_path(), graph)),
                                    "parameter fixture graph failed");
        module_bundle_test::require(static_cast<bool>(tooling::AssetMetaCodec{}.save(graph_path().string() + ".meta", graph_meta)),
                                    "parameter fixture graph metadata failed");
    }

    std::filesystem::path graph_path() const { return root / config.startup_feature_graphs.front(); }
    foundation::ResultValue<tooling::ProjectCookResult> cook() const {
        return tooling::cook_project({root / "Project.aegproj", bundle.manifest_path, root / "Output", "Debug"});
    }
    bool publish_reference(foundation::AssetKind actual_kind) const {
        // The source declaration deliberately claims Texture even when the actual
        // immutable envelope is Shader. The Cook resolver must inspect the latter.
        tooling::AssetMeta claimed{};
        claimed.source = reference_source;
        claimed.importer = "fixture.texture";
        claimed.products = {{reference, foundation::AssetKind::kTexture, "Texture"}};
        if (!tooling::AssetMetaCodec{}.save(root / "Assets/Reference.png.meta", claimed)) return false;
        const std::vector<std::byte> texture{
            std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0},
            std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0},
            std::byte{4}, std::byte{0}, std::byte{0}, std::byte{0},
            std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}};
        const auto bytes = asset::encode_cooked_asset({reference, actual_kind, 1, {}, texture});
        return bytes && tooling::CookedArtifactStore(root / "Cache").publish(
            reference_source, {{reference, actual_kind, 1, bytes.value}});
    }
    bool publish_graph() const {
        const auto payload = asset::encode_feature_graph_asset(graph);
        if (!payload) return false;
        const auto id = graph_meta.products.front().id;
        const auto bytes = asset::encode_cooked_asset({id, foundation::AssetKind::kFeatureGraph, 1, {}, payload.value});
        return bytes && tooling::CookedArtifactStore(root / "Cache").publish(
            graph_meta.source, {{id, foundation::AssetKind::kFeatureGraph, 1, bytes.value}});
    }
};

bool test_parameter_reference_rejections(const std::filesystem::path& binary, const std::filesystem::path& root) {
    const auto wrong = foundation::AssetKind::kShader;
    for (const auto defaults : {false, true}) {
        ParameterCookFixture fixture(binary, root / (defaults ? "DefaultWrong" : "OverrideWrong"), defaults);
        if (!fixture.publish_reference(wrong)) return false;
        const auto cooked = fixture.cook();
        if (!check(!cooked && cooked.result.message.find("kind") != std::string::npos,
                   "Cook accepted a parameter UUID whose actual cooked kind differs from its contract")) return false;
        ParameterCookFixture missing(binary, root / (defaults ? "DefaultMissing" : "OverrideMissing"), defaults);
        const auto absent = missing.cook();
        if (!check(!absent && absent.result.code == foundation::ResultCode::kNotFound &&
                   !std::filesystem::exists(missing.root / "Output/current.toml"),
                   "Cook published a parameter reference to a missing cooked asset")) return false;
    }
    ParameterCookFixture child(binary, root / "ChildWrong", true, true);
    if (!child.publish_reference(wrong)) return false;
    const auto cooked_child = child.cook();
    return check(!cooked_child && cooked_child.result.message.find("kind") != std::string::npos,
                 "Cook accepted a nested Composite child's wrong-kind default");
}

bool test_parameter_dependency_closure_and_cache(const std::filesystem::path& binary, const std::filesystem::path& root) {
    for (const auto mode : {0, 1, 2}) {
        ParameterCookFixture fixture(binary, root / std::to_string(mode), mode != 0, mode == 2);
        if (!fixture.publish_reference(foundation::AssetKind::kTexture)) return false;
        const auto first = fixture.cook();
        if (!check(static_cast<bool>(first), "valid parameter asset Cook failed: " + first.result.message)) return false;
        const auto package = asset::AssetPackageReader::open(first.value.package);
        const auto graph = package ? package.value.read_cooked(fixture.graph_meta.products.front().id)
                                  : foundation::ResultValue<asset::CookedAsset>{package.result, {}};
        if (!check(package && package.value.read_cooked(fixture.reference) && graph &&
                   graph.value.runtime_dependencies == std::vector<foundation::AssetId>{fixture.reference},
                   "parameter override/default/Composite child did not enter packaged runtime dependency closure")) return false;
        const auto selector = file_bytes(fixture.root / "Output/current.toml");
        const auto manifest = file_bytes(first.value.game_manifest);
        const auto package_bytes = file_bytes(first.value.package);
        if (!fixture.publish_reference(foundation::AssetKind::kShader)) return false;
        const auto cached = fixture.cook();
        if (!check(!cached && cached.result.message.find("kind") != std::string::npos &&
                   file_bytes(fixture.root / "Output/current.toml") == selector &&
                   file_bytes(first.value.game_manifest) == manifest && file_bytes(first.value.package) == package_bytes,
                   "warm-cache Cook bypassed actual asset validation or changed the last good publication")) return false;
    }
    ParameterCookFixture optional(binary, root / "Optional", true, false, true);
    const auto cooked = optional.cook();
    return check(static_cast<bool>(cooked), "optional null asset parameter should Cook without a dependency");
}

bool test_cached_graph_parameter_contracts(const std::filesystem::path& binary, const std::filesystem::path& root) {
    constexpr std::array expected_messages{"schema version", "trailing bytes", "missing or stale",
                                            "contract digest", "contract digest", "undeclared"};
    for (unsigned bad = 0; bad != 6; ++bad) {
        ParameterCookFixture fixture(binary, root / std::to_string(bad), false);
        if (!fixture.publish_reference(foundation::AssetKind::kTexture)) return false;
        // Establish the source fingerprint first. A legacy unstamped cache is
        // reimported from source; a matching warm cache remains subject to the
        // final-package contract checks below.
        const auto baseline = fixture.cook();
        if (!check(static_cast<bool>(baseline), "cached contract baseline Cook failed: " + baseline.result.message)) return false;
        auto& node = fixture.graph.nodes.front();
        if (bad == 0) node.parameter_blob.at(12) = 2;  // Stale parameter schema version.
        if (bad == 1) node.parameter_blob.push_back(0); // Trailing bytes.
        if (bad == 2) node.definition_version = {2, 0, 0};
        if (bad == 3) fixture.graph.contract_digest ^= 1;
        if (bad == 4) fixture.graph.contract_digest = 0;
        if (bad == 5) node.parameter_blob.at(4) ^= 1;  // Undeclared parameter ID.
        if (!fixture.publish_graph()) return false;
        const auto cooked = fixture.cook();
        if (!check(!cooked && cooked.result.code == foundation::ResultCode::kInvalidArgument &&
                   cooked.result.message.find(expected_messages[bad]) != std::string::npos,
                   "Cook accepted a cached FeatureGraph with stale or malformed parameter contracts")) return false;
    }
    return true;
}

using FileSnapshot = std::map<std::filesystem::path, std::string>;

FileSnapshot snapshot_files(const std::filesystem::path& directory) {
    FileSnapshot result;
    for (const auto& file : std::filesystem::recursive_directory_iterator(directory)) {
        if (file.is_regular_file()) result.emplace(file.path().lexically_relative(directory), file_bytes(file.path()));
    }
    return result;
}

bool write_bytes(const std::filesystem::path& file, std::span<const std::byte> bytes) {
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

bool test_startup_source_edits_refresh_warm_cache(const std::filesystem::path& binary,
                                                  const std::filesystem::path& root) {
    ParameterCookFixture fixture(binary, root, true, false, true);
    const auto source_a = fixture.graph;
    // Older Cookers could leave an unbound graph in the cache after rejecting
    // package publication. First use of source fingerprints must repair it.
    fixture.graph.contract_digest = 0;
    if (!fixture.publish_graph()) return false;
    fixture.graph = source_a;
    const auto first = fixture.cook();
    if (!check(static_cast<bool>(first), "Cook did not repair an unstamped legacy graph cache: " + first.result.message)) return false;
    const auto first_package = file_bytes(first.value.package);
    const auto first_manifest = file_bytes(first.value.game_manifest);
    const auto read_graph = [&](const tooling::ProjectCookResult& result) {
        const auto package = asset::AssetPackageReader::open(result.package);
        if (!package) return foundation::ResultValue<feature::FeatureGraphDocument>{package.result, {}};
        const auto cooked = package.value.read_cooked(fixture.graph_meta.products.front().id);
        if (!cooked) return foundation::ResultValue<feature::FeatureGraphDocument>{cooked.result, {}};
        return asset::decode_feature_graph_asset(cooked.value.payload);
    };
    const auto graph_a = read_graph(first.value);
    if (!check(graph_a && graph_a.value.contract_digest == source_a.contract_digest,
               "Cook published the legacy zero-digest artifact instead of the source")) return false;

    auto source_b = source_a;
    ++source_b.graph_version; ++source_b.draft_revision;
    source_b.nodes.front().node_id = feature::FeatureNodeId("Edited.Startup.Node");
    if (!application::authoring::save_feature_graph_document(fixture.graph_path(), source_b)) return false;
    auto second = fixture.cook();
    if (!check(static_cast<bool>(second), "Cook of an edited startup source failed: " + second.result.message)) return false;
    const auto graph_b = read_graph(second.value);
    if (!check(graph_b && graph_b.value.graph_version == source_b.graph_version &&
               graph_b.value.nodes.front().node_id == source_b.nodes.front().node_id &&
               file_bytes(first.value.package) == first_package && file_bytes(first.value.game_manifest) == first_manifest,
               "Save -> Cook retained the old graph at the same AssetId or changed the old publication")) return false;

    const auto before_meta = tooling::CookedArtifactStore(root / "Cache").load_current(fixture.graph_meta.source);
    fixture.graph_meta.products.front().stable_key = "Edited startup product";
    if (!tooling::AssetMetaCodec{}.save(fixture.graph_path().string() + ".meta", fixture.graph_meta)) return false;
    const auto meta_updated = fixture.cook();
    const auto after_meta = tooling::CookedArtifactStore(root / "Cache").load_current(fixture.graph_meta.source);
    if (!check(meta_updated && before_meta && after_meta && before_meta.value.manifest != after_meta.value.manifest,
               "Cook ignored a startup .meta edit with an unchanged product AssetId")) return false;
    second = meta_updated;

    const auto second_selector = file_bytes(root / "Output/current.toml");
    const auto second_package = file_bytes(second.value.package);
    const auto second_manifest = file_bytes(second.value.game_manifest);
    const auto cache_before_invalid = snapshot_files(root / "Cache");
    { std::ofstream malformed(fixture.graph_path(), std::ios::binary | std::ios::trunc); malformed << "nodes = [broken"; }
    const auto rejected_import = fixture.cook();
    if (!check(!rejected_import && file_bytes(root / "Output/current.toml") == second_selector &&
               snapshot_files(root / "Cache") == cache_before_invalid &&
               file_bytes(second.value.package) == second_package && file_bytes(second.value.game_manifest) == second_manifest,
               "Invalid updated source reused warm data or changed the last-good cache/publication")) return false;

    auto invalid_contract = source_b;
    invalid_contract.contract_digest = 0;
    if (!application::authoring::save_feature_graph_document(fixture.graph_path(), invalid_contract)) return false;
    const auto rejected_contract = fixture.cook();
    if (!check(!rejected_contract && rejected_contract.result.message.find("contract digest") != std::string::npos &&
               file_bytes(root / "Output/current.toml") == second_selector &&
               file_bytes(second.value.package) == second_package && file_bytes(second.value.game_manifest) == second_manifest,
               "Changed source bypassed strict contract validation or replaced last-good delivery")) return false;
    if (!application::authoring::save_feature_graph_document(fixture.graph_path(), source_b)) return false;
    const auto restored = fixture.cook();
    const auto restored_graph = restored ? read_graph(restored.value)
        : foundation::ResultValue<feature::FeatureGraphDocument>{restored.result, {}};
    return check(restored_graph && restored_graph.value.contract_digest == source_b.contract_digest &&
        restored_graph.value.nodes.front().node_id == source_b.nodes.front().node_id,
        "Cook did not recover from a rejected update after restoring the valid source");
}

foundation::ResultValue<asset::GameBuildManifest> read_game_manifest(const std::filesystem::path& file) {
    const auto bytes = file_bytes(file);
    return asset::decode_game_build_manifest(
        {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()});
}

struct WorldRouteCookFixture {
    inline static constexpr scene::SceneAssetId kAuthoredScene{0x0123456789abcdefull, 0x8fedcba987654321ull};
    inline static constexpr scene::SceneEntityId kFirstEntity{0xabcdef0102030405ull, 0x8a0b0c0d0e0f1001ull};
    inline static constexpr scene::SceneEntityId kSecondEntity{0xabcdef0102030405ull, 0x8a0b0c0d0e0f1002ull};
    inline static constexpr scene::SceneEntityId kNoTransformEntity{0xabcdef0102030405ull, 0x8a0b0c0d0e0f1003ull};
    inline static constexpr feature::FeatureDefinitionId kSelectedLeaf{"Cook.World.Selected"};
    inline static constexpr feature::FeatureDefinitionId kDecoyLeaf{"Cook.World.Decoy"};
    inline static constexpr feature::FeatureDefinitionId kMiddle{"Cook.World.Middle"};
    inline static constexpr feature::FeatureDefinitionId kComposite{"Cook.World.Composite"};
    inline static constexpr feature::FeatureNodeId kCompositeNode{"World.Composite.Node"};
    inline static constexpr feature::FeatureNodeId kAtomicNode{"World.Atomic.Node"};
    inline static constexpr feature::PortId kObservation{"World.Observation"};

    // Tooling only needs an honestly reflected presence marker. Runtime Transform
    // storage is deliberately absent from this Cook-only fixture and its links.
    struct TransformMarker { float position{5.f}; };

    std::filesystem::path root;
    application::project::ProjectConfig config{};
    tooling::AssetMeta scene_meta{};
    tooling::AssetMeta graph_meta{};
    feature::FeatureGraphDocument graph{};
    module::ModuleBundle bundle{};
    std::vector<asset::WorldCommandRoute> routes{};

    WorldRouteCookFixture(const std::filesystem::path& binary, const std::filesystem::path& directory,
                         bool wrapped_scene = false) : root(directory) {
        using namespace feature;
        std::filesystem::create_directories(root / "Assets/Scenes");
        std::filesystem::create_directories(root / "Assets/Features");
        config.name = "World Route Cook";
        config.project_uuid = "ABCD0000-0000-4000-8000-000000000001";
        config.module.id = "com.aeg.dynamic.fixture";
        config.target = "Fixture";
        config.startup_feature_graphs = {"Assets/Features/Startup.aegfeature"};
        module_bundle_test::require(static_cast<bool>(application::project::ProjectFileCodec{}.save(
            root / "Project.aegproj", config)), "world route fixture project failed");

        module::FeatureModuleDefinition module_definition{};
        module_definition.descriptor = {module::ModuleId(config.module.id), "Cook World Routes", {1, 0, 0}, {}};
        const auto definition = [&](FeatureDefinitionId id, bool command) {
            FeatureDefinition result{};
            result.id = id; result.version = {1, 0, 0};
            result.owner_module = ModuleIdentity(config.module.id);
            auto& contract = result.contract;
            contract.definition_id = id; contract.version = result.version;
            contract.domains = to_mask(FeatureDomain::kLogic);
            contract.scope.id = ScopeId("World");
            contract.clock = aeg::feature::make_periodic_clock_contract(ClockId("Cook.World.Clock"), 60, 4, LateDataPolicy::kReject,
                aeg::feature::DeadlinePolicy::kObserve, contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
            contract.permissions.version = 1;
            if (command) {
                contract.side_effects = {{kWorldTransformCommandEffect, kWorldTransformCommandVersion,
                    SideEffectKind::kCommand, CommitPolicy::kOutbox},
                    {SideEffectId("Other.Command"), 1, SideEffectKind::kCommand, CommitPolicy::kOutbox}};
                contract.ports = {{kObservation, PortDirection::kInput, kWorldTransformInputSchema, kWorldTransformInputVersion},
                    {PortId("Output.Observation"), PortDirection::kOutput, kWorldTransformInputSchema, kWorldTransformInputVersion},
                    {PortId("Wrong.Schema"), PortDirection::kInput, ValueSchemaId("Other.Schema"), 1},
                    {PortId("Wrong.Version"), PortDirection::kInput, kWorldTransformInputSchema, 2}};
            }
            return result;
        };
        auto middle = definition(kMiddle, false);
        middle.contract.structure = FeatureStructure::kComposite; middle.contract.clock.work_budget_ns = 0;
        middle.contract.children = {kSelectedLeaf};
        auto composite = definition(kComposite, false);
        composite.contract.structure = FeatureStructure::kComposite; composite.contract.clock.work_budget_ns = 0;
        composite.contract.children = {kDecoyLeaf, kMiddle};
        for (const auto& item : {definition(kSelectedLeaf, true), definition(kDecoyLeaf, false), middle, composite}) {
            module_bundle_test::require(static_cast<bool>(module_definition.feature_catalog.export_definition(item)),
                                        "world route fixture Feature catalog failed");
        }
        bundle = module_bundle_test::write_bundle(binary, root / "Module",
            module_bundle_test::metadata_for(module_definition));

        scene_meta.source = tooling::SourceId::parse("00000000-0000-4000-8000-000000000031").value;
        scene_meta.importer = "aeg.scene";
        scene_meta.products = {{foundation::AssetId::parse("ABCDEF00-0000-4000-8000-000000000032").value,
                                foundation::AssetKind::kScene, "Main"}};
        module_bundle_test::require(scene_meta.products.front().id.value != kAuthoredScene.uuid(),
                                    "world route fixture must relocate the authored Scene identity");
        module_bundle_test::require(static_cast<bool>(tooling::AssetMetaCodec{}.save(
            scene_path().string() + ".meta", scene_meta)), "world route fixture Scene metadata failed");
        const auto authored = make_scene(kAuthoredScene);
        serialization::BinaryWriter archive;
        module_bundle_test::require(static_cast<bool>(scene::SceneSerializer::serialize(authored, archive)),
                                    "world route fixture Scene serialization failed");
        const auto archive_bytes = std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(archive.data().data()), archive.size()};
        if (wrapped_scene) {
            asset::ScenePayload payload{};
            payload.serialized_scene.assign(archive_bytes.begin(), archive_bytes.end());
            const auto encoded = asset::encode_scene_payload(payload);
            module_bundle_test::require(encoded && write_bytes(scene_path(), encoded.value),
                                        "world route fixture wrapped Scene write failed");
        } else {
            module_bundle_test::require(write_bytes(scene_path(), archive_bytes), "world route fixture Scene write failed");
        }

        graph.asset_uuid = "ABCDEF01-2345-4ABC-8DEF-ABCDEF000033";
        graph.schema_version = 1; graph.graph_version = 1;
        graph.contract_digest = module_definition.feature_catalog.contract_digest();
        graph.nodes = {{kCompositeNode, kComposite, {1, 0, 0},
                        OwnerPath{{FeatureNodeId("Author.Owner"), FeatureNodeId("Author.Scope")}}, {}},
                       {kAtomicNode, kSelectedLeaf, {1, 0, 0}, {}, {}}};
        graph_meta.source = tooling::SourceId::parse("00000000-0000-4000-8000-000000000034").value;
        graph_meta.importer = "aeg.feature_graph";
        graph_meta.products = {{foundation::AssetId::parse(graph.asset_uuid).value,
                                foundation::AssetKind::kFeatureGraph, "Startup"}};
        module_bundle_test::require(static_cast<bool>(application::authoring::save_feature_graph_document(graph_path(), graph)),
                                    "world route fixture graph failed");
        module_bundle_test::require(static_cast<bool>(tooling::AssetMetaCodec{}.save(
            graph_path().string() + ".meta", graph_meta)), "world route fixture graph metadata failed");

        asset::WorldCommandRoute nested{};
        nested.graph_asset_uuid = graph.asset_uuid;
        nested.node_id = kCompositeNode;
        nested.owner_path = graph.nodes.front().owner_path;
        nested.owner_path.nodes.insert(nested.owner_path.nodes.end(),
            {kCompositeNode, FeatureNodeId(kMiddle.value()), FeatureNodeId(kSelectedLeaf.value())});
        nested.scene_asset = {kAuthoredScene.uuid()};
        nested.scene_entity = kFirstEntity.uuid();
        nested.effect = kWorldTransformCommandEffect;
        nested.version = kWorldTransformCommandVersion;
        nested.observation_port = kObservation;
        auto atomic = nested;
        atomic.node_id = kAtomicNode; atomic.owner_path.nodes.clear();
        atomic.scene_entity = kSecondEntity.uuid();
        routes = {nested, atomic};
        module_bundle_test::require(save_routes(routes), "world route fixture sidecar failed");
    }

    std::filesystem::path scene_path() const { return root / config.startup_scene; }
    std::filesystem::path graph_path() const { return root / config.startup_feature_graphs.front(); }
    std::filesystem::path sidecar_path() const { return asset::world_command_routes_sidecar_path(scene_path()); }
    foundation::ResultValue<tooling::ProjectCookResult> cook() const {
        return tooling::cook_project({root / "Project.aegproj", bundle.manifest_path, root / "Output", "Debug"});
    }
    bool save_routes(std::span<const asset::WorldCommandRoute> value) const {
        const auto encoded = asset::encode_world_command_routes(value);
        return encoded && write_bytes(sidecar_path(), encoded.value);
    }
    scene::SceneAsset make_scene(scene::SceneAssetId id, bool include_first = true,
                                 bool first_transform = true) const {
        const auto component_name = feature::kWorldTransformComponentName;
        const auto schema = reflection::TypeId(component_name);
        reflection::TypeRegistry registry;
        reflection::TypeBuilder builder(reflection::TypeDescriptor{schema, std::string(component_name), 1,
            reflection::TypeKind::kComponent, sizeof(TransformMarker), alignof(TransformMarker)});
        module_bundle_test::require(static_cast<bool>(builder.add_field("position", reflection::type_id<float>(),
            reflection::make_member_accessor(&TransformMarker::position))), "world route fixture Transform field failed");
        auto descriptor = std::move(builder).finish();
        module_bundle_test::require(descriptor && registry.register_type(std::move(descriptor.value)),
                                    "world route fixture Transform schema failed");
        scene::SceneAsset result(id, "World Routes", 1);
        for (const auto entity : {kFirstEntity, kSecondEntity, kNoTransformEntity}) {
            if (entity == kFirstEntity && !include_first) continue;
            module_bundle_test::require(static_cast<bool>(result.add_entity(
                {entity, {}, "Route target", scene::SceneEntityFlags::kNone, {}})), "world route fixture entity failed");
            if (entity == kNoTransformEntity || (entity == kFirstEntity && !first_transform)) continue;
            const TransformMarker marker{entity == kFirstEntity ? 5.f : 23.f};
            module_bundle_test::require(static_cast<bool>(result.add_reflected_component(entity,
                ecs::ComponentTypeId(component_name), registry, schema, &marker)),
                "world route fixture reflected Transform failed");
        }
        return result;
    }
    bool publish_scene(const scene::SceneAsset& document, uint32_t payload_schema = 1) const {
        serialization::BinaryWriter archive;
        if (!scene::SceneSerializer::serialize(document, archive)) return false;
        asset::ScenePayload payload{};
        payload.schema_version = payload_schema;
        payload.serialized_scene.assign(reinterpret_cast<const std::byte*>(archive.data().data()),
            reinterpret_cast<const std::byte*>(archive.data().data() + archive.size()));
        const auto encoded = asset::encode_scene_payload(payload);
        if (!encoded) return false;
        const auto id = scene_meta.products.front().id;
        const auto envelope = asset::encode_cooked_asset({id, foundation::AssetKind::kScene, payload_schema, {}, encoded.value});
        return envelope && tooling::CookedArtifactStore(root / "Cache").publish(
            scene_meta.source, {{id, foundation::AssetKind::kScene, payload_schema, envelope.value}});
    }
    bool publish_graph(const feature::FeatureGraphDocument& document) const {
        const auto payload = asset::encode_feature_graph_asset(document);
        if (!payload) return false;
        const auto id = graph_meta.products.front().id;
        const auto envelope = asset::encode_cooked_asset({id, foundation::AssetKind::kFeatureGraph, 1, {}, payload.value});
        return envelope && tooling::CookedArtifactStore(root / "Cache").publish(
            graph_meta.source, {{id, foundation::AssetKind::kFeatureGraph, 1, envelope.value}});
    }
};

struct CookPublicationSnapshot {
    std::string selector;
    std::string manifest;
    std::string package;
    FileSnapshot cache;

    CookPublicationSnapshot(const WorldRouteCookFixture& fixture, const tooling::ProjectCookResult& good)
        : selector(file_bytes(fixture.root / "Output/current.toml")), manifest(file_bytes(good.game_manifest)),
          package(file_bytes(good.package)), cache(snapshot_files(fixture.root / "Cache")) {}

    bool unchanged(const WorldRouteCookFixture& fixture, const tooling::ProjectCookResult& good) const {
        return file_bytes(fixture.root / "Output/current.toml") == selector &&
               file_bytes(good.game_manifest) == manifest && file_bytes(good.package) == package &&
               snapshot_files(fixture.root / "Cache") == cache;
    }
};

bool test_startup_scene_source_edits_refresh_warm_cache(const std::filesystem::path& binary,
                                                       const std::filesystem::path& root) {
    WorldRouteCookFixture fixture(binary, root);
    const auto first = fixture.cook();
    if (!check(static_cast<bool>(first), "Scene source-refresh baseline Cook failed: " + first.result.message)) return false;
    const auto first_package = file_bytes(first.value.package);
    auto edited = fixture.make_scene(WorldRouteCookFixture::kAuthoredScene);
    if (!edited.set_entity_name(WorldRouteCookFixture::kFirstEntity, "Edited saved scene")) return false;
    serialization::BinaryWriter archive;
    if (!scene::SceneSerializer::serialize(edited, archive) || !write_bytes(fixture.scene_path(),
        {reinterpret_cast<const std::byte*>(archive.data().data()), archive.size()})) return false;
    const auto second = fixture.cook();
    if (!check(static_cast<bool>(second), "Cook of an edited startup Scene failed: " + second.result.message)) return false;
    const auto package = asset::AssetPackageReader::open(second.value.package);
    const auto cooked = package ? package.value.read_cooked(fixture.scene_meta.products.front().id)
        : foundation::ResultValue<asset::CookedAsset>{package.result, {}};
    const auto payload = cooked ? asset::decode_scene_payload(cooked.value.payload)
        : foundation::ResultValue<asset::ScenePayload>{cooked.result, {}};
    if (!check(static_cast<bool>(payload), "Edited Scene is absent from the final package")) return false;
    serialization::BinaryReader reader({reinterpret_cast<const uint8_t*>(payload.value.serialized_scene.data()),
                                        payload.value.serialized_scene.size()});
    const auto actual = scene::SceneSerializer::deserialize(reader);
    if (!check(actual && actual.value.entities().front().name == "Edited saved scene" &&
               file_bytes(first.value.package) == first_package,
               "Cook reused the old Scene at the same AssetId or mutated the previous package")) return false;
    const CookPublicationSnapshot previous(fixture, second.value);
    { std::ofstream malformed(fixture.scene_path(), std::ios::binary | std::ios::trunc); malformed << "broken scene"; }
    const auto invalid = fixture.cook();
    return check(!invalid && previous.unchanged(fixture, second.value),
        "Invalid updated Scene was hidden by a warm cache or changed the last-good cache/publication");
}

bool test_world_routes_relocate_and_reload_warm_sidecar(const std::filesystem::path& binary,
                                                       const std::filesystem::path& root, bool wrapped_scene) {
    WorldRouteCookFixture fixture(binary, root, wrapped_scene);
    const auto first = fixture.cook();
    if (!check(static_cast<bool>(first), "World route initial Cook failed: " + first.result.message)) return false;
    const auto manifest = read_game_manifest(first.value.game_manifest);
    if (!check(manifest && manifest.value.schema_version == 3 && manifest.value.world_command_routes.size() == 2,
               "Cook did not publish both authored World routes in its versioned manifest")) return false;
    const auto scene_product = fixture.scene_meta.products.front().id;
    const auto& nested = manifest.value.world_command_routes[0];
    const auto& atomic = manifest.value.world_command_routes[1];
    if (!check(nested.scene_asset == scene_product && atomic.scene_asset == scene_product &&
               nested.scene_entity == WorldRouteCookFixture::kFirstEntity.uuid() &&
               atomic.scene_entity == WorldRouteCookFixture::kSecondEntity.uuid() &&
               nested.graph_asset_uuid == "ABCDEF01-2345-4ABC-8DEF-ABCDEF000033" &&
               atomic.graph_asset_uuid == "ABCDEF01-2345-4ABC-8DEF-ABCDEF000033" &&
               nested.node_id == WorldRouteCookFixture::kCompositeNode &&
               nested.owner_path.nodes == fixture.routes[0].owner_path.nodes &&
               atomic.node_id == WorldRouteCookFixture::kAtomicNode && atomic.owner_path.empty() &&
               nested.effect == feature::kWorldTransformCommandEffect && nested.version == 1 &&
               nested.observation_port == WorldRouteCookFixture::kObservation,
               "Cook lost exact graph/source/entity identity or did not relocate only the Scene product")) return false;
    const auto package = asset::AssetPackageReader::open(first.value.package);
    if (!check(static_cast<bool>(package), "World route package could not be reopened")) return false;
    const auto cooked_scene = package.value.read_cooked(scene_product);
    if (!check(static_cast<bool>(cooked_scene), "World route Scene product absent from package")) return false;
    const auto scene_payload = asset::decode_scene_payload(cooked_scene.value.payload);
    if (!check(static_cast<bool>(scene_payload), "World route Scene payload invalid")) return false;
    serialization::BinaryReader reader({reinterpret_cast<const uint8_t*>(scene_payload.value.serialized_scene.data()),
                                         scene_payload.value.serialized_scene.size()});
    const auto relocated = scene::SceneSerializer::deserialize(reader);
    if (!check(relocated && relocated.value.id().uuid() == scene_product.value &&
               relocated.value.entities().size() == 3 && relocated.value.components().size() == 2,
               "Cook did not retain both Transform targets and the unrelated no-Transform entity")) return false;

    const CookPublicationSnapshot previous(fixture, first.value);
    const auto source_scene = file_bytes(fixture.scene_path());
    const auto source_graph = file_bytes(fixture.graph_path());
    fixture.routes[0].scene_entity = WorldRouteCookFixture::kSecondEntity.uuid();
    fixture.routes[0].observation_port = {};
    fixture.routes[1].scene_entity = WorldRouteCookFixture::kFirstEntity.uuid();
    if (!fixture.save_routes(fixture.routes)) return false;
    const auto warm = fixture.cook();
    if (!check(static_cast<bool>(warm), "sidecar-only warm Cook failed: " + warm.result.message)) return false;
    const auto updated = read_game_manifest(warm.value.game_manifest);
    return check(updated && updated.value.world_command_routes.size() == 2 &&
        updated.value.world_command_routes[0].scene_asset == scene_product &&
        updated.value.world_command_routes[0].scene_entity == WorldRouteCookFixture::kSecondEntity.uuid() &&
        !updated.value.world_command_routes[0].observation_port.is_valid() &&
        updated.value.world_command_routes[1].scene_entity == WorldRouteCookFixture::kFirstEntity.uuid() &&
        updated.value.world_command_routes[0].graph_asset_uuid == "ABCDEF01-2345-4ABC-8DEF-ABCDEF000033" &&
        file_bytes(fixture.root / "Output/current.toml") != previous.selector &&
        file_bytes(first.value.game_manifest) == previous.manifest && file_bytes(first.value.package) == previous.package &&
        snapshot_files(fixture.root / "Cache") == previous.cache &&
        file_bytes(warm.value.package) == previous.package &&
        file_bytes(fixture.scene_path()) == source_scene && file_bytes(fixture.graph_path()) == source_graph,
        "warm Cook ignored sidecar-only route edits, required observation, or mutated cache/the last good package");
}

bool test_world_route_rejections_preserve_publication(const std::filesystem::path& binary,
                                                      const std::filesystem::path& root) {
    WorldRouteCookFixture fixture(binary, root);
    const auto good = fixture.cook();
    if (!check(static_cast<bool>(good), "World route rejection control Cook failed: " + good.result.message)) return false;
    const CookPublicationSnapshot previous(fixture, good.value);
    const auto encoded = asset::encode_world_command_routes(fixture.routes);
    if (!encoded) return false;
    std::vector<std::pair<std::string, std::vector<std::byte>>> failures;
    auto bytes = encoded.value;
    bytes[0] = std::byte{0}; failures.emplace_back("malformed magic", bytes);
    bytes = encoded.value; bytes.pop_back(); failures.emplace_back("truncated record", bytes);
    bytes = encoded.value; bytes.push_back(std::byte{0}); failures.emplace_back("trailing bytes", bytes);
    bytes = encoded.value; bytes[8] = std::byte{1}; failures.emplace_back("old sidecar schema", bytes);
    bytes = encoded.value; bytes.resize(asset::kMaxWorldCommandRoutesBytes + 1);
    failures.emplace_back("sidecar over the shared byte budget", std::move(bytes));
    const std::array single{fixture.routes.front()};
    const auto one = asset::encode_world_command_routes(single);
    if (!one) return false;
    bytes = one.value;
    bytes[12] = std::byte{2};
    bytes.insert(bytes.end(), one.value.begin() + 16, one.value.end());
    failures.emplace_back("duplicate source route", std::move(bytes));
    const auto add = [&](const std::string& name, const auto& mutation) {
        auto invalid = fixture.routes;
        mutation(invalid.front());
        const auto wire = asset::encode_world_command_routes(invalid);
        module_bundle_test::require(static_cast<bool>(wire), "semantic route rejection fixture failed to encode");
        failures.emplace_back(name, wire.value);
    };
    add("product Scene UUID used as authored source", [&](auto& route) { route.scene_asset = fixture.scene_meta.products.front().id; });
    add("wrong source Scene", [](auto& route) { route.scene_asset.value.bytes.back() ^= 0x40; });
    add("wrong graph", [](auto& route) { route.graph_asset_uuid = "ABCDEF01-2345-4ABC-8DEF-ABCDEF000099"; });
    add("normalized graph spelling", [](auto& route) { route.graph_asset_uuid = "abcdef01-2345-4abc-8def-abcdef000033"; });
    add("wrong node", [](auto& route) { route.node_id = feature::FeatureNodeId("Missing.Node"); });
    add("missing owner path", [](auto& route) { route.owner_path.nodes.clear(); });
    add("wrong authored owner", [](auto& route) { route.owner_path.nodes.front() = feature::FeatureNodeId("Other.Owner"); });
    add("missing authored node in expanded owner", [](auto& route) { route.owner_path.nodes.erase(route.owner_path.nodes.begin() + 2); });
    add("incomplete Composite suffix", [](auto& route) { route.owner_path.nodes.erase(route.owner_path.nodes.begin() + 3); });
    add("Composite root instead of selected leaf", [](auto& route) { route.owner_path.nodes.resize(3); });
    add("leaf without World command contract", [](auto& route) {
        route.owner_path.nodes.resize(3);
        route.owner_path.nodes.emplace_back(feature::FeatureNodeId(WorldRouteCookFixture::kDecoyLeaf.value()));
    });
    add("missing target entity", [](auto& route) { route.scene_entity.bytes.back() ^= 0x40; });
    add("target without Transform", [](auto& route) { route.scene_entity = WorldRouteCookFixture::kNoTransformEntity.uuid(); });
    add("other declared effect", [](auto& route) { route.effect = feature::SideEffectId("Other.Command"); });
    add("wrong command version", [](auto& route) { route.version = 2; });
    add("undeclared observation", [](auto& route) { route.observation_port = feature::PortId("Missing.Observation"); });
    add("output observation", [](auto& route) { route.observation_port = feature::PortId("Output.Observation"); });
    add("wrong observation schema", [](auto& route) { route.observation_port = feature::PortId("Wrong.Schema"); });
    add("wrong observation version", [](auto& route) { route.observation_port = feature::PortId("Wrong.Version"); });
    for (const auto& [name, wire] : failures) {
        if (!write_bytes(fixture.sidecar_path(), wire)) return false;
        const auto rejected = fixture.cook();
        if (!check(!rejected && !rejected.value.diagnostics.empty() && previous.unchanged(fixture, good.value),
                   "Cook accepted " + name + " or changed cache/last-good publication")) return false;
    }
    if (!fixture.save_routes(fixture.routes)) return false;
    const auto recovered = fixture.cook();
    return check(static_cast<bool>(recovered), "Cook did not recover after restoring valid World route sidecar");
}

bool test_world_routes_validate_final_cached_package(const std::filesystem::path& binary,
                                                     const std::filesystem::path& root) {
    constexpr std::array cases{"cached Scene missing target", "cached Scene missing Transform", "cached graph missing owner",
        "cached ScenePayload unsupported schema", "cached Transform unsupported schema", "cached Transform truncated payload"};
    bool all_rejected = true;
    for (unsigned variant = 0; variant < cases.size(); ++variant) {
        WorldRouteCookFixture fixture(binary, root / std::to_string(variant));
        const auto good = fixture.cook();
        if (!check(static_cast<bool>(good), "final-package route control Cook failed: " + good.result.message)) return false;
        const auto source_scene = file_bytes(fixture.scene_path());
        const auto source_graph = file_bytes(fixture.graph_path());
        if (variant < 2) {
            const auto product_identity = scene::SceneAssetId::from_uuid(fixture.scene_meta.products.front().id.value);
            if (!fixture.publish_scene(fixture.make_scene(product_identity, variant != 0, false))) return false;
        } else if (variant == 2) {
            auto cached = fixture.graph;
            cached.nodes.front().owner_path.nodes.front() = feature::FeatureNodeId("Cached.Other.Owner");
            if (!fixture.publish_graph(cached)) return false;
        } else {
            const auto product_identity = scene::SceneAssetId::from_uuid(fixture.scene_meta.products.front().id.value);
            auto cached = fixture.make_scene(product_identity);
            if (variant == 3) {
                // Both envelope and payload carry the same formally encoded
                // version: only supported ScenePayload schema validation rejects it.
                if (!fixture.publish_scene(cached, 2)) return false;
            } else {
                const auto component_type = ecs::ComponentTypeId(feature::kWorldTransformComponentName);
                const auto found = std::find_if(cached.components().begin(), cached.components().end(),
                    [&](const auto& component) {
                        return component.entity == WorldRouteCookFixture::kFirstEntity &&
                               component.component_type == component_type;
                    });
                if (!check(found != cached.components().end(), "final-package Transform mutation target is absent")) return false;
                auto component = *found;
                if (variant == 4) component.schema_version = 2;
                else component.payload.pop_back();
                if (!cached.remove_component(component.entity, component.component_type) ||
                    !cached.add_component(std::move(component)) || !fixture.publish_scene(cached)) return false;
            }
        }
        // The replacement is a valid officially published cooked artifact. The
        // sidecar still selects a real target and owner in the unchanged source.
        const CookPublicationSnapshot previous(fixture, good.value);
        const auto rejected = fixture.cook();
        if (!check(!rejected && !rejected.value.diagnostics.empty() && previous.unchanged(fixture, good.value) &&
                   file_bytes(fixture.scene_path()) == source_scene && file_bytes(fixture.graph_path()) == source_graph,
                   std::string("Cook validated source instead of final package: ") + cases[variant])) all_rejected = false;
    }
    return all_rejected;
}

bool test_cook_rejects_valid_bundle_from_a_different_project(const std::filesystem::path& binary) {
    using namespace aeg;
    const auto root = std::filesystem::temp_directory_path() / "aeg_project_cooker_module_agreement";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    const auto bundle = module_bundle_test::write_bundle(binary, root / "Bundle",
        module_bundle_test::metadata_for(dynamic_feature_test::definition()));
    application::project::ProjectConfig config{};
    config.name = "Module Agreement";
    config.project_uuid = "00000000-0000-0000-0000-000000000001";
    config.module.id = "com.aeg.dynamic.fixture";
    config.module.target = "IndependentCmakeTarget";
    config.target = "Fixture";
    const auto project_file = root / "Project.aegproj";
    const auto output = root / "Output";
    const application::project::ProjectFileCodec codec;
    struct Mismatch { const char* module; const char* platform; const char* configuration; };
    constexpr std::array cases{
        Mismatch{"com.aeg.other", "Fixture", "Debug"},
        Mismatch{"com.aeg.dynamic.fixture", "windows-x64-vulkan", "Debug"},
        Mismatch{"com.aeg.dynamic.fixture", "Fixture", "Release"},
        Mismatch{"", "Fixture", "Debug"}};
    for (const auto& mismatch : cases) {
        config.module.id = mismatch.module;
        config.target = mismatch.platform;
        if (!codec.save(project_file, config) || !module::validate_module_bundle(bundle.manifest_path)) return false;
        const auto result = tooling::cook_project({project_file, bundle.manifest_path, output, mismatch.configuration});
        if (result || result.value.diagnostics.empty() || result.value.diagnostics.front().code != "cook.module_project" ||
            std::filesystem::exists(output)) {
            std::cerr << "valid bundle project mismatch was not rejected before cook: "
                      << (result.value.diagnostics.empty() ? "no diagnostic" : result.value.diagnostics.front().code) << '\n';
            return false;
        }
    }
    config.module.id = "com.aeg.dynamic.fixture";
    config.target = "Fixture";
    if (!codec.save(project_file, config)) return false;
    const auto matching = tooling::cook_project({project_file, bundle.manifest_path, output, "Debug"});
    // The matching bundle gets past agreement validation to the intentionally
    // absent source scene. This control does not claim a complete Cook package.
    const bool passed = !matching && !matching.value.diagnostics.empty() &&
        matching.value.diagnostics.front().code == "cook.startup_scene" && !std::filesystem::exists(output);
    std::filesystem::remove_all(root, error);
    return passed;
}
}

int main(int argc, char** argv) {
    aeg::tooling::ProjectCookRequest request{};
    const auto result = aeg::tooling::cook_project(request);
    if (result || result.value.diagnostics.empty() || result.value.diagnostics.front().code != "cook.request")
        return 1;
    if (argc != 2 || !test_cook_rejects_valid_bundle_from_a_different_project(std::filesystem::absolute(argv[1]))) return 2;
    const auto root = std::filesystem::temp_directory_path() / "aeg_project_parameter_cooker_smoke";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    const auto binary = std::filesystem::absolute(argv[1]);
    if (!test_parameter_reference_rejections(binary, root / "Reject")) return 3;
    if (!test_parameter_dependency_closure_and_cache(binary, root / "Closure")) return 4;
    if (!test_cached_graph_parameter_contracts(binary, root / "Contracts")) return 5;
    if (!test_startup_source_edits_refresh_warm_cache(binary, root / "SourceRefresh")) return 9;
    if (!test_world_routes_relocate_and_reload_warm_sidecar(binary, root / "WorldRoutes/PublishRaw", false) ||
        !test_world_routes_relocate_and_reload_warm_sidecar(binary, root / "WorldRoutes/PublishWrapped", true)) return 6;
    if (!test_world_route_rejections_preserve_publication(binary, root / "WorldRoutes/Reject")) return 7;
    if (!test_world_routes_validate_final_cached_package(binary, root / "WorldRoutes/FinalPackage")) return 8;
    if (!test_startup_scene_source_edits_refresh_warm_cache(binary, root / "SceneSourceRefresh")) return 10;
    std::filesystem::remove_all(root, error);
    std::cout << "Project cooker smoke passed\n";
    return 0;
}
