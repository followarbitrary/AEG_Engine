/**
 * @file editor_feature_graph_smoke.cpp
 * @brief Feature-only editor palette, Process view and provenance inspect.
 */
#include "feature_clock_test_fixture.hpp"
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <imgui_internal.h>

#include "editor_feature_graph_model.hpp"
#include "editor_feature_graph_document.hpp"
#include "editor_feature_graph_panel.hpp"
#include "feature_registry.hpp"
#include "feature_parameters.hpp"
#include "editor_parameter_widget.hpp"
#include "project_context.hpp"

namespace {
using namespace aeg::feature;
using namespace aeg::application;

FeatureDefinition definition(std::string_view id, FeatureDomain domain, ExecutionKind execution) {
    FeatureDefinition result{};
    result.id = FeatureDefinitionId(id);
    result.version = {1, 0, 0};
    result.contract.definition_id = result.id;
    result.contract.version = result.version;
    result.contract.domains = to_mask(domain);
    result.contract.execution = execution;
    result.contract.state = execution == ExecutionKind::kProcess ? StatePolicy::kProcessState
                                                                  : StatePolicy::kStateless;
    result.contract.scope = ScopeContract{ScopeId("World")};
    result.contract.clock = aeg::feature::make_periodic_clock_contract(ClockId("LogicTick"), 60, 4, LateDataPolicy::kReject,
        aeg::feature::DeadlinePolicy::kObserve, result.contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    result.contract.permissions.version = 1;
    result.contract.has_continuation = execution == ExecutionKind::kProcess;
    result.contract.ports = {{PortId("In"), PortDirection::kInput, ValueSchemaId("Int.v1"), 1}};
    return result;
}

class FeatureGraphPanelHarness final {
public:
    FeatureGraphPanelHarness() {
        context_ = ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2{1200, 2400};
        io.DeltaTime = 1.0f / 60.0f;
        io.ConfigErrorRecoveryEnableAssert = false;
        io.ConfigErrorRecoveryEnableDebugLog = false;
        io.ConfigErrorRecoveryEnableTooltip = false;
        unsigned char* pixels{};
        int width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        context_->ErrorCallbackUserData = &errors_;
        context_->ErrorCallback = [](ImGuiContext*, void* data, const char* message) {
            static_cast<std::vector<std::string>*>(data)->emplace_back(message);
        };
    }

    ~FeatureGraphPanelHarness() {
        ImGui::DestroyContext(context_);
        for (const auto& error : errors_) std::cerr << error << '\n';
        if (!errors_.empty()) std::exit(1);
    }

    void draw(EditorFeatureGraphDocument& document, const std::filesystem::path& path,
              const char* button = nullptr, bool select_node = false,
              const EditorFeatureGraphPanel::Actions& actions = {}) {
        ImGui::NewFrame();
        if (button || select_node) {
            auto* window = ImGui::FindWindowByName("Flow Graph###FlowGraph");
            assert(window != nullptr);
            const auto id = select_node
                ? ImHashStr("1", 0, window->GetID("FeatureGraphNodes"))
                : std::string_view{button} == "Use default"
                    ? ImHashStr(button, 0, ImHashStr("7", 0, window->ID))
                    : window->GetID(button);
            context_->NavWindow = window;
            context_->NavId = id;
            context_->NavInputSource = ImGuiInputSource_Keyboard;
            context_->NavActivateId = context_->NavActivateDownId = id;
        }
        ImGui::SetNextWindowPos(ImVec2{0, 0});
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        panel_.draw(document, path, actions);
        ImGui::Render();
    }

private:
    ImGuiContext* context_{};
    EditorFeatureGraphPanel panel_{};
    std::vector<std::string> errors_{};
};

struct PanelParameterFixture final {
    FeatureDefinition executable{definition("Logic.PanelParameters", FeatureDomain::kLogic, ExecutionKind::kCall)};
    FeatureCatalog catalog{};

    PanelParameterFixture() {
        ParameterMetadata metadata{};
        metadata.type = ParameterType::kFloat;
        metadata.default_value = 2.0;
        executable.contract.parameters = {{ParameterId{7}, 1, 4, true, metadata}};
        assert(catalog.export_definition(executable));
    }

    FeatureGraphDocument graph(std::string asset, double value) const {
        const auto payload = serialize_parameter_value(value, ParameterType::kFloat);
        assert(payload);
        const std::vector<FeatureParameterRecord> records{{ParameterId{7}, 1, payload.value}};
        const auto blob = encode_feature_parameters(records, executable.contract.parameters);
        assert(blob);
        FeatureGraphDocument graph{};
        graph.asset_uuid = std::move(asset);
        graph.schema_version = 1;
        graph.graph_version = 1;
        graph.nodes = {{FeatureNodeId{1}, executable.id, executable.version, {}, blob.value}};
        return graph;
    }

    void expect_value(const EditorFeatureGraphDocument& document, double expected,
                      const char* message) const {
        const auto records = decode_feature_parameters(document.snapshot().nodes.front().parameter_blob,
                                                       executable.contract.parameters);
        assert(records && records.value.size() == 1);
        const auto value = deserialize_parameter_value(records.value.front().payload, ParameterType::kFloat);
        assert(value);
        if (std::get<double>(value.value) != expected) {
            std::cerr << message << ": expected " << expected << ", got "
                      << std::get<double>(value.value) << '\n';
            std::exit(1);
        }
    }
};

bool test_asset_parameters_resolve_project_product_uuid_and_keep_failed_edits_atomic() {
    using aeg::foundation::AssetId;
    using aeg::foundation::AssetKind;
    using aeg::foundation::AssetReference;
    const auto root = std::filesystem::temp_directory_path() / "aeg_editor_asset_parameter_graph_smoke";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "Assets");
    const std::string mesh = "10000000-0000-0000-0000-000000000001";
    const std::string texture = "10000000-0000-0000-0000-000000000002";
    const std::string missing = "10000000-0000-0000-0000-000000000003";
    const auto write_meta = [&](const std::filesystem::path& path, const char* texture_kind,
                                const char* mesh_kind = "mesh") {
        auto source_path = path; source_path.replace_extension();
        { std::ofstream source(source_path); source << "source"; }
        std::ofstream meta(path);
        meta << "version = 1\nsource_uuid = \"20000000-0000-0000-0000-000000000001\"\n"
                "importer = \"fixture\"\n[[products]]\nuuid = \"" << mesh <<
                "\"\nkind = \"" << mesh_kind << "\"\nkey = \"mesh\"\n[[products]]\nuuid = \"" << texture <<
                "\"\nkind = \"" << texture_kind << "\"\nkey = \"texture\"\n";
    };
    write_meta(root / "Assets/source.bin.meta", "texture");
    const auto resolver = make_editor_asset_kind_resolver(root);
    const auto snapshot_resolver = snapshot_editor_asset_kind_resolver(root);
    const auto mesh_kind = resolver(AssetId::parse(mesh).value);
    assert(mesh_kind && mesh_kind.value == AssetKind::kMesh);
    const auto texture_kind = resolver(AssetId::parse(texture).value);
    assert(texture_kind && texture_kind.value == AssetKind::kTexture);
    assert(!resolver(AssetId::parse(missing).value));
    assert(!resolver(AssetId::parse("20000000-0000-0000-0000-000000000001").value));
    assert(!resolver({}));

    auto executable = definition("Logic.MeshParameter", FeatureDomain::kLogic, ExecutionKind::kCall);
    ParameterMetadata metadata{};
    metadata.type = ParameterType::kAssetReference;
    metadata.default_value = AssetReference{mesh};
    metadata.constraint = AssetConstraint{true, AssetKind::kMesh};
    executable.contract.parameters = {{ParameterId{7}, 1, 64, false, metadata}};
    FeatureCatalog catalog;
    assert(catalog.export_definition(executable));
    FeatureGraphDocument graph{};
    graph.asset_uuid = "asset-parameter-save-fixture";
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.nodes = {{FeatureNodeId{1}, executable.id, executable.version, {}, {}}};
    EditorFeatureGraphDocument document{graph, catalog};
    const auto records = [&](const std::string& uuid) {
        const auto payload = serialize_parameter_value(AssetReference{uuid}, ParameterType::kAssetReference);
        assert(payload);
        return std::vector<FeatureParameterRecord>{{ParameterId{7}, 1, payload.value}};
    };
    const auto owner = document.snapshot_owner();
    assert(!document.set_parameters(FeatureNodeId{1}, records(mesh), catalog));
    for (const auto& uuid : {texture, missing}) {
        assert(!document.set_parameters(FeatureNodeId{1}, records(uuid), catalog, resolver));
        assert(document.snapshot_owner() == owner && document.revision() == 0 && !document.dirty());
    }
    assert(document.set_parameters(FeatureNodeId{1}, {}, catalog, resolver)); // Resolve the effective default.
    const auto values = decode_feature_parameters(document.snapshot().nodes.front().parameter_blob,
                                                   executable.contract.parameters);
    assert(values && values.value.size() == 1);
    const auto value = deserialize_parameter_value(values.value.front().payload, ParameterType::kAssetReference);
    assert(value && std::get<AssetReference>(value.value).uuid == mesh);
    assert(document.set_parameters(FeatureNodeId{1}, records(""), catalog));
    assert(document.undo());
    assert(document.undo());
    assert(document.snapshot_owner() == owner && !document.dirty());

    const auto saved_path = root / "Assets/Typed.aegfeature";
    const auto file_bytes = [&] {
        std::ifstream input(saved_path, std::ios::binary);
        return std::string{std::istreambuf_iterator<char>{input}, {}};
    };
    assert(document.save(saved_path, resolver));
    assert(document.can_redo() && !document.can_undo() && !document.dirty());
    const auto saved_bytes = file_bytes();
    const auto saved_owner = document.snapshot_owner();
    const auto saved_revision = document.revision();
    const auto saved_undo = document.can_undo();
    const auto saved_redo = document.can_redo();
    write_meta(root / "Assets/source.bin.meta", "texture", "texture");
    const auto fresh_kind = resolver(AssetId::parse(mesh).value);
    const auto retained_kind = snapshot_resolver(AssetId::parse(mesh).value);
    assert(fresh_kind && fresh_kind.value == AssetKind::kTexture);
    assert(retained_kind && retained_kind.value == AssetKind::kMesh);
    assert(!document.save(saved_path, resolver));
    assert(document.snapshot_owner() == saved_owner && document.revision() == saved_revision &&
           !document.dirty() && document.can_undo() == saved_undo && document.can_redo() == saved_redo &&
           file_bytes() == saved_bytes);
    write_meta(root / "Assets/source.bin.meta", "texture");
    const auto blocked = root / "blocked";
    { std::ofstream file(blocked); file << "block"; }
    assert(!document.save(blocked / "Typed.aegfeature", resolver));
    assert(document.snapshot_owner() == saved_owner && document.revision() == saved_revision &&
           document.can_redo() == saved_redo && file_bytes() == saved_bytes);

    auto unfinished_definition = executable;
    unfinished_definition.contract.parameters.front().required = true;
    unfinished_definition.contract.parameters.front().metadata->default_value.reset();
    FeatureCatalog unfinished_catalog;
    assert(unfinished_catalog.export_definition(unfinished_definition));
    EditorFeatureGraphDocument unfinished{graph, unfinished_catalog};
    assert(!unfinished.set_parameters(FeatureNodeId{1}, {}, unfinished_catalog, resolver));
    assert(unfinished.save(root / "Assets/Unfinished.aegfeature", resolver));
    assert(unfinished.snapshot().nodes.front().parameter_blob.empty() && !unfinished.dirty());
    EditorFeatureGraphDocument opaque_draft{graph};
    assert(opaque_draft.save(root / "Assets/Opaque.aegfeature", resolver));

    auto child = executable;
    child.id = FeatureDefinitionId{"Logic.AssetChild"};
    child.contract.definition_id = child.id;
    auto middle = definition("Logic.AssetMiddle", FeatureDomain::kLogic, ExecutionKind::kCall);
    middle.contract.structure = FeatureStructure::kComposite; middle.contract.clock.work_budget_ns = 0;
    middle.contract.ports.clear();
    middle.contract.children = {child.id};
    auto composite = definition("Logic.AssetComposite", FeatureDomain::kLogic, ExecutionKind::kCall);
    composite.contract.structure = FeatureStructure::kComposite; composite.contract.clock.work_budget_ns = 0;
    composite.contract.ports.clear();
    composite.contract.children = {middle.id};
    const auto composite_catalog = [&](const FeatureDefinition& leaf) {
        FeatureCatalog result;
        assert(result.export_definition(leaf));
        assert(result.export_definition(middle));
        assert(result.export_definition(composite));
        return result;
    };
    auto composite_graph = graph;
    composite_graph.asset_uuid = "composite-asset-save-fixture";
    composite_graph.nodes = {{FeatureNodeId{1}, composite.id, composite.version, {}, {}}};
    const auto composite_path = root / "Assets/Composite.aegfeature";
    EditorFeatureGraphDocument valid_composite{composite_graph, composite_catalog(child)};
    assert(valid_composite.save(composite_path, resolver));
    std::ifstream composite_input(composite_path, std::ios::binary);
    const std::string composite_bytes{std::istreambuf_iterator<char>{composite_input}, {}};
    composite_input.close();
    auto wrong_child = child;
    wrong_child.contract.parameters.front().metadata->default_value = AssetReference{texture};
    EditorFeatureGraphDocument wrong_composite{composite_graph, composite_catalog(wrong_child)};
    const auto composite_owner = wrong_composite.snapshot_owner();
    if (wrong_composite.save(composite_path, resolver)) {
        std::cerr << "Composite save accepted a wrong-kind child default\n";
        return false;
    }
    std::ifstream unchanged_composite(composite_path, std::ios::binary);
    assert(std::string(std::istreambuf_iterator<char>{unchanged_composite}, {}) == composite_bytes);
    unchanged_composite.close();
    assert(wrong_composite.snapshot_owner() == composite_owner && wrong_composite.revision() == 0 &&
           !wrong_composite.dirty() && !wrong_composite.can_undo());
    auto unfinished_child = child;
    unfinished_child.contract.parameters.front().required = true;
    unfinished_child.contract.parameters.front().metadata->default_value.reset();
    EditorFeatureGraphDocument unfinished_composite{composite_graph, composite_catalog(unfinished_child)};
    assert(unfinished_composite.save(composite_path, resolver));
    FeatureCatalog partial_catalog;
    assert(partial_catalog.export_definition(composite));
    EditorFeatureGraphDocument partial_composite{composite_graph, partial_catalog};
    assert(partial_composite.save(composite_path, resolver));

    // The UI must pass its resolver and observe new metadata without reopening the document.
    auto wrong_graph = graph;
    const auto wrong_blob = encode_feature_parameters(records(texture), executable.contract.parameters);
    assert(wrong_blob);
    wrong_graph.nodes.front().parameter_blob = wrong_blob.value;
    EditorFeatureGraphDocument panel_document{wrong_graph, catalog};
    EditorFeatureGraphPanel::Actions actions;
    actions.asset_kind_resolver = resolver;
    FeatureGraphPanelHarness ui;
    ui.draw(panel_document, "Assets.aegfeature", nullptr, false, actions);
    ui.draw(panel_document, "Assets.aegfeature", nullptr, true, actions);
    ui.draw(panel_document, "Assets.aegfeature", "Apply parameters", false, actions);
    assert(panel_document.revision() == 0 && !panel_document.dirty());
    write_meta(root / "Assets/source.bin.meta", "mesh");
    ui.draw(panel_document, "Assets.aegfeature", "Apply parameters", false, actions);
    assert(panel_document.revision() == 1 && panel_document.dirty());

    write_meta(root / "Assets/duplicate.bin.meta", "mesh");
    assert(!resolver(AssetId::parse(mesh).value));
    std::filesystem::remove(root / "Assets/duplicate.bin.meta", error);
    { std::ofstream malformed(root / "Assets/bad.meta"); malformed << "invalid metadata"; }
    assert(!resolver(AssetId::parse(mesh).value));
    std::filesystem::remove(root / "Assets/bad.meta", error);
    std::filesystem::remove(root / "Assets/source.bin", error);
    assert(!resolver(AssetId::parse(mesh).value));
    const auto failed_snapshot = snapshot_editor_asset_kind_resolver(root);
    assert(!failed_snapshot(AssetId::parse(mesh).value));
    const auto other_root = make_editor_asset_kind_resolver(root / "other-project");
    assert(!other_root(AssetId::parse(mesh).value));

    // The configured assets directory is authoritative, including refreshes of that configuration.
    std::filesystem::create_directories(root / "Content/Models");
    write_meta(root / "Content/Models/mesh.bin.meta", "texture");
    aeg::application::project::ProjectConfig project_config;
    project_config.name = "Asset parameters";
    project_config.paths.assets = "Content";
    const auto project_file = root / "Project.aegproj";
    assert(aeg::application::project::ProjectFileCodec{}.save(project_file, project_config));
    const auto configured = resolver(AssetId::parse(mesh).value);
    assert(configured && configured.value == AssetKind::kMesh);
    assert(!failed_snapshot(AssetId::parse(mesh).value)); // A later fixed project belongs to a new snapshot.
    const auto unchanged_snapshot = snapshot_resolver(AssetId::parse(mesh).value);
    assert(unchanged_snapshot && unchanged_snapshot.value == AssetKind::kMesh);
    const auto selected_project = make_editor_asset_kind_resolver(project_file);
    assert(selected_project(AssetId::parse(mesh).value));
    assert(aeg::application::project::ProjectFileCodec{}.save(root / "Other.aegproj", project_config));
    assert(!resolver(AssetId::parse(mesh).value));
    assert(selected_project(AssetId::parse(mesh).value));
    { std::ofstream invalid(project_file); invalid << "format_version = 2\n[project]\nname = \"Unsafe\"\n[paths]\nassets = \"../outside\"\n"; }
    assert(!selected_project(AssetId::parse(mesh).value));
    std::filesystem::remove_all(root, error);
    return true;
}

void test_panel_parameter_drafts_are_isolated_between_feature_graph_roots() {
    PanelParameterFixture fixture;
    EditorFeatureGraphDocument first{fixture.graph("asset://first", 1.0), fixture.catalog};
    EditorFeatureGraphDocument second{fixture.graph("asset://second", 3.0), fixture.catalog};
    FeatureGraphPanelHarness ui;
    ui.draw(first, "First.aegfeature");
    ui.draw(first, "First.aegfeature", nullptr, true);
    ui.draw(second, "Second.aegfeature");
    ui.draw(second, "Second.aegfeature", "Apply parameters");
    fixture.expect_value(second, 3.0, "parameter draft leaked from first root into second root");
    fixture.expect_value(first, 1.0, "switching roots changed the previous document");
    // Return with the same revision to catch A/B/A cache collisions as well.
    first = EditorFeatureGraphDocument{fixture.graph("asset://first", 1.0), fixture.catalog};
    second = EditorFeatureGraphDocument{fixture.graph("asset://second", 3.0), fixture.catalog};
    ui.draw(second, "Second.aegfeature");
    ui.draw(first, "First.aegfeature");
    ui.draw(first, "First.aegfeature", "Apply parameters");
    fixture.expect_value(first, 1.0, "parameter draft leaked when returning to first root");
}

void test_panel_reloads_parameters_when_the_same_document_object_is_reopened() {
    PanelParameterFixture fixture;
    EditorFeatureGraphDocument document{fixture.graph("asset://reopened", 1.0), fixture.catalog};
    FeatureGraphPanelHarness ui;
    ui.draw(document, "Reopened.aegfeature");
    ui.draw(document, "Reopened.aegfeature", nullptr, true);
    ui.draw(document, "Reopened.aegfeature", "Use default");
    // A failed root switch leaves the current document/source in place. Merely
    // drawing it again must retain the useful, unapplied parameter draft.
    ui.draw(document, "Reopened.aegfeature");
    ui.draw(document, "Reopened.aegfeature", "Apply parameters");
    fixture.expect_value(document, 2.0, "unchanged document lost its unapplied parameter draft");
    assert(document.undo());
    ui.draw(document, "Reopened.aegfeature");
    ui.draw(document, "Reopened.aegfeature", "Apply parameters");
    fixture.expect_value(document, 1.0, "Undo did not reload the restored parameter values");

    document = EditorFeatureGraphDocument{fixture.graph("asset://reopened", 1.0), fixture.catalog};
    ui.draw(document, "Reopened.aegfeature");
    ui.draw(document, "Reopened.aegfeature", "Use default");
    // Reopening can reuse the object, root, revision and source bytes; an old
    // UI draft still belongs to the previous immutable document snapshot.
    document = EditorFeatureGraphDocument{fixture.graph("asset://reopened", 1.0), fixture.catalog};
    ui.draw(document, "Reopened.aegfeature");
    ui.draw(document, "Reopened.aegfeature", "Apply parameters");
    fixture.expect_value(document, 1.0, "reopened document reused an old session's parameter draft");

    document = EditorFeatureGraphDocument{fixture.graph("asset://reopened", 3.0), fixture.catalog};
    ui.draw(document, "Reopened.aegfeature");
    ui.draw(document, "Reopened.aegfeature", "Apply parameters");
    fixture.expect_value(document, 3.0, "same-object replacement did not reload parameter values");
}

void test_typed_parameters_share_contract_and_keep_node_values_independent() {
    auto executable = definition("Logic.Typed", FeatureDomain::kLogic, ExecutionKind::kCall);
    ParameterContract speed{ParameterId("Speed"), 1, 4, true};
    speed.metadata = ParameterMetadata{};
    speed.metadata->type = ParameterType::kFloat;
    speed.metadata->default_value = 1.0;
    speed.metadata->constraint = NumericRange{0, 4, 0, false};
    executable.contract.parameters = {speed};
    FeatureCatalog catalog;
    assert(catalog.export_definition(executable));
    FeatureGraphDocument graph{};
    graph.nodes = {{FeatureNodeId("A"), executable.id, executable.version, {}, {}},
                   {FeatureNodeId("B"), executable.id, executable.version, {}, {}}};
    EditorFeatureGraphDocument document(graph);
    const auto explicit_value = serialize_parameter_value(2.0, ParameterType::kFloat);
    assert(explicit_value);
    assert(document.set_parameters(FeatureNodeId("A"), std::vector<FeatureParameterRecord>{{speed.id, 1, explicit_value.value}}, catalog));
    assert(document.set_parameters(FeatureNodeId("B"), {}, catalog));
    const auto first = decode_feature_parameters(document.snapshot().nodes[0].parameter_blob, executable.contract.parameters);
    const auto second = decode_feature_parameters(document.snapshot().nodes[1].parameter_blob, executable.contract.parameters);
    assert(first && first.value[0].payload == std::vector<uint8_t>({0,0,0,0x40}));
    assert(second && second.value[0].payload == std::vector<uint8_t>({0,0,0x80,0x3f}));
    const auto revision = document.revision();
    const auto invalid_value = serialize_parameter_value(8.0, ParameterType::kFloat);
    assert(!document.set_parameters(FeatureNodeId("A"), std::vector<FeatureParameterRecord>{{speed.id,1,invalid_value.value}}, catalog));
    assert(document.revision() == revision);
    assert(document.undo());
    assert(document.snapshot().nodes[1].parameter_blob.empty());
    assert(document.undo());
    assert(document.snapshot().nodes[0].parameter_blob.empty());
    assert(document.redo());
    assert(document.snapshot().nodes[0].parameter_blob == encode_feature_parameters(std::vector<FeatureParameterRecord>{{speed.id,1,explicit_value.value}}, executable.contract.parameters).value);
}

void test_editor_feature_palette_and_process_view() {
    FeatureCatalog catalog;
    const auto logic = definition("Logic.Skill", FeatureDomain::kLogic, ExecutionKind::kProcess);
    const auto render = definition("Render.Icon", FeatureDomain::kRender, ExecutionKind::kContinuous);
    assert(catalog.export_definition(logic));
    assert(catalog.export_definition(render));
    FeatureRegistry registry{aeg::feature::test::clock_test_host()};
    assert(aeg::feature::test::register_clock_test_definition(registry, logic));
    assert(aeg::feature::test::register_clock_test_definition(registry, render));
    assert(catalog.contract_digest() == registry.contract_digest());
    EditorFeatureGraphModel model;
    assert(model.replace_catalog(catalog));
    FeaturePaletteFilter filter{};
    filter.domain = FeatureDomain::kLogic;
    const auto entries = model.palette(filter);
    assert(entries.size() == 1);
    assert(entries.front().definition_id == logic.id);

    FeatureGraphDocument graph{};
    graph.asset_uuid = "asset://editor";
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.nodes = {{FeatureNodeId("Skill"), logic.id, logic.version, {}, {}}};
    graph.process_subgraphs = {{FeatureNodeId("Skill"), {"Idle", "Done"}, {"Idle->Done"}, 1}};
    assert(model.open_document(graph));
    const auto process_view = model.open_process(FeatureNodeId("Skill"));
    assert(process_view && process_view.value->states.size() == 2);
}

void test_runtime_inspect_preserves_source_chain() {
    FeatureNodeProvenance provenance{};
    provenance.definition_id = FeatureDefinitionId("Logic.Skill");
    provenance.node_id = FeatureNodeId("Skill");
    provenance.instance_id = FeatureInstanceId("Skill.Instance");
    provenance.process_id = ProcessId("Skill.Process");
    provenance.invocation_id = InvocationId("Skill.Invocation");
    provenance.owner_path.nodes = {FeatureNodeId("Root")};
    const auto path = EditorFeatureGraphModel::inspect(provenance);
    assert(path.definition_id == provenance.definition_id);
    assert(path.node_id == provenance.node_id);
    assert(path.instance_id == provenance.instance_id);
    assert(path.process_id == provenance.process_id);
    assert(path.invocation_id == provenance.invocation_id);
}

void test_feature_graph_document_history() {
    FeatureGraphDocument graph{};
    graph.asset_uuid = "asset://history";
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.nodes = {{FeatureNodeId("A"), FeatureDefinitionId("Logic.A"), {1, 0, 0}, {}, {}}};
    aeg::application::EditorFeatureGraphDocument document(std::move(graph));
    assert(!document.dirty());
    assert(document.execute("remove node", [](auto& next) {
        next.nodes.clear();
        ++next.graph_version;
        ++next.draft_revision;
        return aeg::foundation::Result::success();
    }));
    assert(document.dirty() && document.snapshot().nodes.empty() && document.can_undo());
    assert(document.undo());
    assert(document.snapshot().nodes.size() == 1);
    assert(document.redo());
    assert(document.snapshot().nodes.empty());
    document.mark_saved();
    assert(!document.dirty());
}

void test_feature_parameters_edit_is_validated_and_undoable() {
    auto parameterized = definition("Logic.Parameters", FeatureDomain::kLogic, ExecutionKind::kCall);
    parameterized.contract.parameters = {{ParameterId(7), 3, 2, true}};
    FeatureCatalog catalog;
    assert(catalog.export_definition(parameterized));
    const std::vector<FeatureParameterRecord> original{{ParameterId(7), 3, {1}}};
    const auto encoded = encode_feature_parameters(original, parameterized.contract.parameters);
    assert(encoded);
    FeatureGraphDocument graph{};
    graph.nodes = {{FeatureNodeId("Node"), parameterized.id, parameterized.version, {}, encoded.value}};
    EditorFeatureGraphDocument document(std::move(graph));
    const std::vector<FeatureParameterRecord> replacement{{ParameterId(7), 3, {2, 3}}};
    assert(document.set_parameters(FeatureNodeId("Node"), replacement, catalog));
    assert(document.dirty() && document.snapshot().graph_version == 1 && document.snapshot().draft_revision == 1);
    assert(document.snapshot().contract_digest == catalog.contract_digest());
    const auto edited = decode_feature_parameters(document.snapshot().nodes.front().parameter_blob,
                                                   parameterized.contract.parameters);
    assert(edited && edited.value == replacement);
    const auto revision = document.revision();
    assert(!document.set_parameters(FeatureNodeId("Node"), {}, catalog));
    assert(!document.set_parameters(FeatureNodeId("Missing"), replacement, catalog));
    assert(document.revision() == revision);
    assert(document.undo());
    assert(!document.dirty() && document.snapshot().nodes.front().parameter_blob == encoded.value);
    assert(document.redo());
    assert(document.dirty());

    auto wrong_version = parameterized;
    wrong_version.version = {2, 0, 0};
    wrong_version.contract.version = wrong_version.version;
    FeatureCatalog newer_catalog;
    assert(newer_catalog.export_definition(wrong_version));
    assert(!document.set_parameters(FeatureNodeId("Node"), replacement, newer_catalog));
    assert(document.revision() == revision + 2);
}

void test_feature_graph_document_add_edge_validates_catalog_and_history() {
    auto source = definition("Logic.Source", FeatureDomain::kLogic, ExecutionKind::kCall);
    auto target = definition("Logic.Target", FeatureDomain::kLogic, ExecutionKind::kCall);
    source.contract.ports = {{PortId("Out"), PortDirection::kOutput, ValueSchemaId("Int.v1"), 7}};
    target.contract.ports = {{PortId("In"), PortDirection::kInput, ValueSchemaId("Int.v1"), 7}};
    target.contract.clock = aeg::feature::make_periodic_clock_contract(ClockId("OtherTick"), 60, 4, LateDataPolicy::kReject,
        aeg::feature::DeadlinePolicy::kObserve, target.contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    FeatureCatalog catalog;
    assert(catalog.export_definition(source));
    assert(catalog.export_definition(target));
    FeatureGraphDocument graph{};
    graph.asset_uuid = "asset://edge-authoring";
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.nodes = {{FeatureNodeId("Source"), source.id, source.version, {}, {}},
                   {FeatureNodeId("Target"), target.id, target.version, {}, {}}};
    aeg::application::EditorFeatureGraphDocument document(std::move(graph));
    const FeatureEdge edge{FeatureNodeId("Source"), PortId("Out"), FeatureNodeId("Target"), PortId("In"),
                           EdgeKind::kData, ClockId("LogicTick"), ClockId("OtherTick"), 1, 0, 0};
    assert(document.add_edge(edge, catalog).code == aeg::foundation::ResultCode::kInvalidArgument);
    auto transported_edge = edge;
    transported_edge.kind = EdgeKind::kSnapshot;
    transported_edge.deadline_ticks = 1;
    assert(document.add_edge(transported_edge, catalog));
    assert(document.snapshot().edges.size() == 1);
    assert(document.snapshot().edges.front().version == 7);
    assert(document.can_undo());
    assert(document.add_edge(transported_edge, catalog).code == aeg::foundation::ResultCode::kInvalidState);
    assert(document.snapshot().edges.size() == 1);
    const auto revision_before_invalid = document.revision();
    auto bad_edge = transported_edge;
    bad_edge.target_port = PortId("Unknown");
    assert(document.add_edge(bad_edge, catalog).code == aeg::foundation::ResultCode::kInvalidArgument);
    assert(document.revision() == revision_before_invalid);
    bad_edge = transported_edge;
    bad_edge.source_clock = ClockId("OtherTick");
    assert(document.add_edge(bad_edge, catalog).code == aeg::foundation::ResultCode::kInvalidArgument);
    assert(document.revision() == revision_before_invalid);
    assert(document.undo());
    assert(document.snapshot().edges.empty());
}

void test_editor_rejects_cross_domain_data_through_composite_ports() {
    auto logic_child = definition("Logic.CompositeSource", FeatureDomain::kLogic, ExecutionKind::kCall);
    logic_child.contract.ports = {{PortId("Out"), PortDirection::kOutput, ValueSchemaId("Int.v1"), 1}};
    auto physics_child = definition("Physics.CompositeTarget", FeatureDomain::kPhysics, ExecutionKind::kCall);
    physics_child.contract.ports = {{PortId("In"), PortDirection::kInput, ValueSchemaId("Int.v1"), 1}};

    auto source = definition("Composite.Source", FeatureDomain::kLogic, ExecutionKind::kCall);
    source.contract.structure = FeatureStructure::kComposite; source.contract.clock.work_budget_ns = 0;
    source.contract.domains = to_mask(FeatureDomain::kLogic);
    source.contract.children = {logic_child.id};
    source.contract.ports = {{PortId("Out"), PortDirection::kOutput, ValueSchemaId("Int.v1"), 1}};
    source.contract.child_port_mappings = {
        {PortId("Out"), logic_child.id, PortId("Out"), PortDirection::kOutput}};

    auto target = definition("Composite.Target", FeatureDomain::kPhysics, ExecutionKind::kCall);
    target.contract.structure = FeatureStructure::kComposite; target.contract.clock.work_budget_ns = 0;
    target.contract.domains = to_mask(FeatureDomain::kPhysics);
    target.contract.children = {physics_child.id};
    target.contract.ports = {{PortId("In"), PortDirection::kInput, ValueSchemaId("Int.v1"), 1}};
    target.contract.child_port_mappings = {
        {PortId("In"), physics_child.id, PortId("In"), PortDirection::kInput}};

    FeatureCatalog catalog;
    assert(catalog.export_definition(logic_child));
    assert(catalog.export_definition(physics_child));
    assert(catalog.export_definition(source));
    assert(catalog.export_definition(target));
    FeatureGraphDocument graph{};
    graph.asset_uuid = "asset://editor-composite-domains";
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.nodes = {{FeatureNodeId("Source"), source.id, source.version, {}, {}},
                   {FeatureNodeId("Target"), target.id, target.version, {}, {}}};
    aeg::application::EditorFeatureGraphDocument document(std::move(graph));
    const FeatureEdge edge{FeatureNodeId("Source"), PortId("Out"), FeatureNodeId("Target"), PortId("In"),
                           EdgeKind::kData, ClockId("LogicTick"), ClockId("LogicTick"), 1, 0, 0};
    assert(document.add_edge(edge, catalog).code == aeg::foundation::ResultCode::kInvalidArgument);
    auto transported_edge = edge;
    transported_edge.kind = EdgeKind::kQueue;
    transported_edge.capacity = 1;
    assert(document.add_edge(transported_edge, catalog));
}

void test_feature_graph_authoring_records_digest_and_process_history() {
    const auto process_definition = definition("Logic.Process", FeatureDomain::kLogic, ExecutionKind::kProcess);
    FeatureCatalog catalog;
    assert(catalog.export_definition(process_definition));
    FeatureGraphDocument graph{};
    graph.asset_uuid = "asset://authoring";
    graph.schema_version = 1;
    graph.graph_version = 1;
    graph.nodes.push_back({FeatureNodeId("Call"), FeatureDefinitionId("Logic.Call"), {1, 0, 0}, {}, {}});
    auto call_definition = definition("Logic.Call", FeatureDomain::kLogic, ExecutionKind::kCall);
    assert(catalog.export_definition(call_definition));
    aeg::application::EditorFeatureGraphDocument document(std::move(graph), catalog);
    assert(document.add_process_state(FeatureNodeId("Call"), "Illegal")
               .code == aeg::foundation::ResultCode::kInvalidArgument);
    const FeatureNode node{FeatureNodeId("Process"), process_definition.id, process_definition.version, {}, {}};
    assert(document.add_node(node, catalog.contract_digest()));
    assert(document.snapshot().nodes.size() == 2);
    assert(document.snapshot().contract_digest == catalog.contract_digest());
    assert(document.add_process_state(node.node_id, "Idle"));
    assert(document.add_process_state(node.node_id, "Done"));
    assert(document.add_process_transition(node.node_id, "Idle", "Done"));
    assert(document.snapshot().process_subgraphs.size() == 1);
    assert(document.snapshot().process_subgraphs.front().states.size() == 2);
    assert(document.snapshot().process_subgraphs.front().transitions.size() == 1);
    assert(document.set_process_initial_state(node.node_id, "Done"));
    assert(document.snapshot().process_subgraphs.front().initial_state == "Done");
    assert(document.set_process_terminal_state(node.node_id, "Done", true));
    assert(document.snapshot().process_subgraphs.front().terminal_states == std::vector<std::string>{"Done"});
    assert(document.set_process_terminal_state(node.node_id, "Done", false));
    assert(document.snapshot().process_subgraphs.front().terminal_states.empty());
    assert(document.undo());
    assert(document.snapshot().process_subgraphs.front().terminal_states == std::vector<std::string>{"Done"});
    assert(document.undo());
    assert(document.snapshot().process_subgraphs.front().terminal_states.empty());
    assert(document.undo());
    assert(document.snapshot().process_subgraphs.front().initial_state == "Idle");
    assert(document.undo());
    assert(document.snapshot().process_subgraphs.front().transitions.empty());
    assert(document.redo());
    assert(document.erase_process_state(node.node_id, "Idle"));
    assert(document.snapshot().process_subgraphs.front().states.size() == 1);
    assert(document.snapshot().process_subgraphs.front().transitions.empty());
}

void test_feature_graph_node_delete_cascades_owner_path() {
    FeatureGraphDocument graph{};
    graph.asset_uuid = "asset://owner-path";
    graph.schema_version = 1;
    graph.nodes = {
        {FeatureNodeId("Root"), FeatureDefinitionId("Logic.Root"), {1, 0, 0}, {}, {}},
        {FeatureNodeId("Child"), FeatureDefinitionId("Logic.Child"), {1, 0, 0}, {{FeatureNodeId("Root")}}, {}},
        {FeatureNodeId("Grandchild"), FeatureDefinitionId("Logic.Grandchild"), {1, 0, 0},
         {{FeatureNodeId("Root"), FeatureNodeId("Child")}}, {}},
        {FeatureNodeId("Other"), FeatureDefinitionId("Logic.Other"), {1, 0, 0}, {}, {}},
    };
    graph.edges = {{FeatureNodeId("Root"), PortId("Out"), FeatureNodeId("Other"), PortId("In"), EdgeKind::kData}};
    graph.process_subgraphs = {{FeatureNodeId("Root"), {"Idle"}, {}, 1},
                               {FeatureNodeId("Child"), {"Idle"}, {}, 1}};
    aeg::application::EditorFeatureGraphDocument document(std::move(graph));
    assert(document.erase_node(FeatureNodeId("Missing")).code == aeg::foundation::ResultCode::kNotFound);
    assert(document.revision() == 0);
    assert(document.erase_node(FeatureNodeId("Root")));
    assert(document.snapshot().nodes.size() == 1);
    assert(document.snapshot().nodes.front().node_id == FeatureNodeId("Other"));
    assert(document.snapshot().edges.empty());
    assert(document.snapshot().process_subgraphs.empty());
    assert(document.undo());
    assert(document.snapshot().nodes.size() == 4);
    assert(document.snapshot().nodes[2].owner_path.nodes.size() == 2);
    assert(document.redo());
    assert(document.snapshot().nodes.size() == 1);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view{argv[1]} == "--panel-reopen") {
        test_panel_reloads_parameters_when_the_same_document_object_is_reopened();
        std::cout << "editor parameter panel reopen smoke passed\n";
        return 0;
    }
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(640, 480);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* font_pixels{};
    int font_width{}, font_height{};
    io.Fonts->GetTexDataAsRGBA32(&font_pixels, &font_width, &font_height);
    ImGui::NewFrame();
    ImGui::Begin("Parameter widget regression");
    aeg::foundation::ParameterValue empty_vector = std::vector<double>{};
    ParameterMetadata vector_metadata{};
    vector_metadata.type = ParameterType::kVector3;
    draw_parameter_value("Empty vector", empty_vector, vector_metadata);
    assert(std::get<std::vector<double>>(empty_vector).size() == 3);
    aeg::foundation::ParameterValue existing_vector = std::vector<double>{1,2,3,4};
    draw_parameter_value("Existing vector", existing_vector, vector_metadata);
    assert(std::get<std::vector<double>>(existing_vector) == std::vector<double>({1,2,3,4}));
    ImGui::End();
    ImGui::Render();
    ImGui::DestroyContext();
    test_panel_parameter_drafts_are_isolated_between_feature_graph_roots();
    test_panel_reloads_parameters_when_the_same_document_object_is_reopened();
    test_typed_parameters_share_contract_and_keep_node_values_independent();
    if (!test_asset_parameters_resolve_project_product_uuid_and_keep_failed_edits_atomic()) return 1;
    test_feature_parameters_edit_is_validated_and_undoable();
    test_editor_feature_palette_and_process_view();
    test_runtime_inspect_preserves_source_chain();
    test_feature_graph_document_history();
    test_feature_graph_node_delete_cascades_owner_path();
    test_feature_graph_document_add_edge_validates_catalog_and_history();
    test_editor_rejects_cross_domain_data_through_composite_ports();
    test_feature_graph_authoring_records_digest_and_process_history();
    std::cout << "editor feature graph smoke passed\n";
    return 0;
}
