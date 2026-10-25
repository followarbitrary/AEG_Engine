#include "editor_feature_definition_fixture.hpp"
#include "editor_node_contract_storage.hpp"
#include "editor_node_definition_service.hpp"
#include "feature_graph_io.hpp"
#include "project_context.hpp"
#include <array>
#include <iostream>
#include <limits>

namespace {
bool parameter_type_edits_do_not_reinterpret_old_bytes(const std::filesystem::path& root) {
    using namespace aeg;
    auto original = tests::feature_definition_draft();
    auto& parameter = original.entry.contract.parameters.front();
    parameter.metadata->constraint = {};
    const auto payload = feature::serialize_parameter_value(1.0, feature::ParameterType::kDouble);
    if (!payload) return false;
    const auto envelope = feature::encode_feature_parameters(
        std::vector<feature::FeatureParameterRecord>{{parameter.id, parameter.version, payload.value}},
        original.entry.contract.parameters);
    if (!envelope) return false;
    auto graph = tests::feature_graph(original.entry);
    graph.nodes.front().parameter_blob = envelope.value;
    application::EditorFeatureGraphDocument document{graph, tests::feature_catalog(original.entry)};
    application::EditorNodeDefinitionService service;
    const auto original_saved = service.save(document, {original, true, false}, root);
    if (!original_saved) {
        std::cerr << "Initial parameter migration definition save failed: " << original_saved.result.message << '\n';
        return false;
    }
    const auto owner = document.snapshot_owner();
    const auto revision = document.revision();
    const auto digest = document.catalog().contract_digest();
    const auto contract_path = root / "Source/FlowGraph/CustomFeature.flowgraph.contract.json";
    const auto old_contract = tests::read_text(contract_path);
    auto changed = original;
    changed.entry.contract.parameters.front().metadata->type = feature::ParameterType::kInt64;
    changed.entry.contract.parameters.front().metadata->default_value = int64_t{7};
    changed.entry.contract.parameters.front().metadata->constraint = {};
    const auto saved = service.save(document, {changed, true, true}, root);
    if (saved) {
        const auto records = feature::decode_feature_parameters(document.snapshot().nodes.front().parameter_blob,
                                                                 changed.entry.contract.parameters);
        if (records && !records.value.empty()) {
            const auto value = feature::deserialize_parameter_value(records.value.front().payload, feature::ParameterType::kInt64);
            if (value) std::cerr << "TYPE_REINTERPRET accepted=1 double=1 int64=" << std::get<int64_t>(value.value) << '\n';
        }
    }
    if (saved || document.snapshot_owner() != owner || document.revision() != revision ||
        document.catalog().contract_digest() != digest || tests::read_text(contract_path) != old_contract) {
        std::cerr << "Type edit without versions and explicit migration accepted old bytes or changed prior state\n";
        return false;
    }
    return true;
}

bool explicit_parameter_migrations_preserve_values_and_history(const std::filesystem::path& root) {
    using namespace aeg;
    using Action = application::NodeParameterMigrationAction;
    auto original = tests::feature_definition_draft();
    original.entry.contract.parameters.front().metadata->constraint = {};
    const auto id = original.entry.contract.parameters.front().id;
    const auto payload = feature::serialize_parameter_value(1.0, feature::ParameterType::kDouble);
    const auto envelope = feature::encode_feature_parameters(
        std::vector<feature::FeatureParameterRecord>{{id, 1, payload.value}}, original.entry.contract.parameters);
    auto graph = tests::feature_graph(original.entry); graph.nodes.front().parameter_blob = envelope.value;
    application::EditorFeatureGraphDocument first{graph, tests::feature_catalog(original.entry)};
    graph.asset_uuid = "second-parameter-migration";
    application::EditorFeatureGraphDocument second{graph, tests::feature_catalog(original.entry)};
    application::EditorNodeDefinitionService service;
    service.set_project_documents({&first, &second});
    if (!service.save(first, {original, true, false}, root)) return false;
    const auto before_blob = first.snapshot().nodes.front().parameter_blob;
    const auto before_digest = first.catalog().contract_digest();
    const auto path = root / "Source/FlowGraph/CustomFeature.flowgraph.contract.json";
    auto changed = original;
    changed.entry.version = {2, 0, 0}; changed.entry.contract.version = changed.entry.version;
    auto& next_parameter = changed.entry.contract.parameters.front();
    next_parameter.version = 2; next_parameter.metadata->type = feature::ParameterType::kInt64;
    next_parameter.metadata->default_value = int64_t{7};
    application::NodeDefinitionSaveRequest request{changed, true, true};
    const auto rejected_unchanged = [&](const application::NodeDefinitionSaveRequest& invalid) {
        const auto first_owner = first.snapshot_owner(), second_owner = second.snapshot_owner();
        const auto first_revision = first.revision(), second_revision = second.revision();
        const auto old_file = tests::read_text(path);
        const auto result = service.save(first, invalid, root);
        if (result || first.snapshot_owner() != first_owner || second.snapshot_owner() != second_owner ||
            first.revision() != first_revision || second.revision() != second_revision || tests::read_text(path) != old_file) {
            std::cerr << "Rejected parameter migration changed a document or generated contract\n"; return false;
        }
        return true;
    };
    // Increasing versions alone never authorizes reinterpretation or reset.
    if (!rejected_unchanged(request)) return false;
    request.parameter_migrations = {{id, Action::kConvertValue}};
    auto wrong_version = request;
    wrong_version.draft.entry.contract.parameters.front().version = 1;
    if (!rejected_unchanged(wrong_version)) return false;
    wrong_version = request; wrong_version.draft.entry.version = {1, 1, 0};
    wrong_version.draft.entry.contract.version = wrong_version.draft.entry.version;
    if (!rejected_unchanged(wrong_version)) return false;
    auto duplicate = request; duplicate.parameter_migrations.push_back(duplicate.parameter_migrations.front());
    if (!rejected_unchanged(duplicate)) return false;
    auto unknown = request; unknown.parameter_migrations.front().id = feature::ParameterId{999};
    if (!rejected_unchanged(unknown)) return false;
    if (!service.save(first, request, root)) { std::cerr << "Explicit exact Double to Int64 conversion failed\n"; return false; }
    const auto check_value = [&](const application::EditorFeatureGraphDocument& document, int64_t expected) {
        for (const auto& node : document.snapshot().nodes) {
            if (node.definition_version != feature::Version{2, 0, 0}) return false;
            const auto records = feature::decode_feature_parameters(node.parameter_blob, changed.entry.contract.parameters);
            if (!records || records.value.size() != 1 || records.value.front().version != 2) return false;
            const auto value = feature::deserialize_parameter_value(records.value.front().payload, feature::ParameterType::kInt64);
            if (!value || std::get<int64_t>(value.value) != expected) return false;
        }
        return document.snapshot().edges.size() == 1;
    };
    if (!check_value(first, 1) || !check_value(second, 1)) { std::cerr << "Converted value or version was not preserved across documents\n"; return false; }
    const auto converted_blob = first.snapshot().nodes.front().parameter_blob;
    if (!first.undo() || !second.undo() || first.snapshot().nodes.front().parameter_blob != before_blob ||
        first.catalog().contract_digest() != before_digest || second.catalog().contract_digest() != before_digest ||
        first.snapshot().nodes.front().definition_version != feature::Version{1, 0, 0} ||
        !first.redo() || !second.redo() || first.snapshot().nodes.front().parameter_blob != converted_blob ||
        !check_value(first, 1) || !check_value(second, 1)) return false;
    if (!first.undo() || !second.undo()) return false;
    request.parameter_migrations = {{id, Action::kResetValue}};
    if (!service.save(first, request, root) || !check_value(first, 7) || !check_value(second, 7)) {
        std::cerr << "Explicit reset did not use the new default\n"; return false;
    }
    if (!first.undo() || !second.undo()) return false;
    auto missing_required = request;
    missing_required.draft.entry.contract.parameters.front().required = true;
    missing_required.draft.entry.contract.parameters.front().metadata->default_value.reset();
    if (!rejected_unchanged(missing_required)) return false;
    auto removal = request;
    removal.draft.entry.contract.parameters.clear();
    if (!service.save(first, removal, root) || !first.snapshot().nodes.front().parameter_blob.empty() ||
        !second.snapshot().nodes.front().parameter_blob.empty()) {
        std::cerr << "Explicit parameter removal failed\n"; return false;
    }
    return true;
}

bool lossy_parameter_conversion_is_rejected(const std::filesystem::path& root) {
    using namespace aeg;
    using Action = application::NodeParameterMigrationAction;
    const double old_values[] = {1.25, 9223372036854775808.0, -9223372036854777856.0};
    for (std::size_t index = 0; index < std::size(old_values); ++index) {
        auto original = tests::feature_definition_draft();
        auto& parameter = original.entry.contract.parameters.front(); parameter.metadata->constraint = {};
        parameter.metadata->default_value = old_values[index];
        auto graph = tests::feature_graph(original.entry);
        application::EditorFeatureGraphDocument document{graph, tests::feature_catalog(original.entry)};
        application::EditorNodeDefinitionService service;
        const auto case_root = root / std::to_string(index);
        if (!service.save(document, {original, true, false}, case_root)) return false;
        const auto owner = document.snapshot_owner();
        const auto path = case_root / "Source/FlowGraph/CustomFeature.flowgraph.contract.json";
        const auto file = tests::read_text(path);
        auto changed = original; changed.entry.version = {2, 0, 0}; changed.entry.contract.version = changed.entry.version;
        auto& next = changed.entry.contract.parameters.front(); next.version = 2;
        next.metadata->type = feature::ParameterType::kInt64; next.metadata->default_value = int64_t{7};
        application::NodeDefinitionSaveRequest request{changed, true, true};
        request.parameter_migrations = {{parameter.id, Action::kConvertValue}};
        if (service.save(document, request, case_root) || document.snapshot_owner() != owner || tests::read_text(path) != file) {
            std::cerr << "Lossy or overflowing conversion was accepted: " << old_values[index] << '\n'; return false;
        }
    }
    return true;
}

bool numeric_migration_boundaries(const std::filesystem::path& root) {
    using namespace aeg;
    using Type = feature::ParameterType;
    using Value = foundation::ParameterValue;
    struct Case { Type before; Value value; Type after; bool accepted; Value expected; };
    const Case cases[] = {
        {Type::kDouble, 1.5, Type::kFloat, true, 1.5},
        {Type::kDouble, 0.1, Type::kFloat, false, 0.0},
        {Type::kInt64, (std::numeric_limits<int64_t>::max)(), Type::kDouble, false, 0.0},
        {Type::kUInt64, (std::numeric_limits<uint64_t>::max)(), Type::kDouble, false, 0.0},
        {Type::kUInt64, uint64_t{1} << 63, Type::kDouble, true, 9223372036854775808.0},
        {Type::kInt64, int64_t{-1}, Type::kUInt64, false, uint64_t{0}},
        {Type::kDouble, -1.0, Type::kUInt64, false, uint64_t{0}},
        {Type::kDouble, 18446744073709551616.0, Type::kUInt64, false, uint64_t{0}},
        {Type::kUInt64, (std::numeric_limits<uint64_t>::max)(), Type::kInt64, false, int64_t{0}},
        {Type::kDouble, 2147483648.0, Type::kInt32, false, int64_t{0}},
        {Type::kInt64, int64_t{16777217}, Type::kFloat, false, 0.0},
        {Type::kInt64, int64_t{16777216}, Type::kFloat, true, 16777216.0},
    };
    for (std::size_t index = 0; index < std::size(cases); ++index) {
        const auto& test = cases[index];
        auto original = tests::feature_definition_draft();
        auto& parameter = original.entry.contract.parameters.front(); parameter.max_size = 32;
        parameter.metadata->type = test.before; parameter.metadata->default_value = test.value; parameter.metadata->constraint = {};
        application::EditorFeatureGraphDocument document{tests::feature_graph(original.entry), tests::feature_catalog(original.entry)};
        application::EditorNodeDefinitionService service;
        const auto case_root = root / std::to_string(index);
        if (!service.save(document, {original, true, false}, case_root)) return false;
        const auto owner = document.snapshot_owner();
        const auto path = case_root / "Source/FlowGraph/CustomFeature.flowgraph.contract.json";
        const auto old_file = tests::read_text(path);
        auto changed = original; changed.entry.version = {2, 0, 0}; changed.entry.contract.version = changed.entry.version;
        auto& next = changed.entry.contract.parameters.front(); next.version = 2; next.metadata->type = test.after;
        next.metadata->default_value = test.expected;
        application::NodeDefinitionSaveRequest request{changed, true, true};
        request.parameter_migrations = {{parameter.id, application::NodeParameterMigrationAction::kConvertValue}};
        const auto migrated = service.save(document, request, case_root);
        if (static_cast<bool>(migrated) != test.accepted) {
            std::cerr << "Numeric migration boundary " << index << " accepted=" << static_cast<bool>(migrated)
                      << " error=" << migrated.result.message << '\n'; return false;
        }
        if (!migrated) {
            if (document.snapshot_owner() != owner || tests::read_text(path) != old_file) return false;
            continue;
        }
        const auto records = feature::decode_feature_parameters(document.snapshot().nodes.front().parameter_blob, changed.entry.contract.parameters);
        if (!records || records.value.size() != 1 || records.value.front().version != 2) return false;
        const auto value = feature::deserialize_parameter_value(records.value.front().payload, test.after);
        if (!value || value.value != test.expected) { std::cerr << "Numeric migration lost value at case " << index << '\n'; return false; }
    }
    return true;
}

bool parameter_migration_rejects_unknown_and_stale_schemas(const std::filesystem::path& root) {
    using namespace aeg;
    auto original = tests::feature_definition_draft();
    auto graph = tests::feature_graph(original.entry);
    application::EditorFeatureGraphDocument active{graph, tests::feature_catalog(original.entry)};
    application::EditorNodeDefinitionService service;
    if (!service.save(active, {original, true, false}, root)) return false;
    auto changed = original; changed.entry.version = {2, 0, 0}; changed.entry.contract.version = changed.entry.version;
    changed.entry.contract.parameters.front().version = 2;
    changed.entry.contract.parameters.front().metadata->type = feature::ParameterType::kInt64;
    changed.entry.contract.parameters.front().metadata->default_value = int64_t{7};
    changed.entry.contract.parameters.front().metadata->constraint = {};
    application::NodeDefinitionSaveRequest request{changed, true, true};
    request.parameter_migrations = {{original.entry.contract.parameters.front().id, application::NodeParameterMigrationAction::kConvertValue}};
    const auto path = root / "Source/FlowGraph/CustomFeature.flowgraph.contract.json";
    const auto file = tests::read_text(path); const auto active_owner = active.snapshot_owner();
    auto different = original; different.entry.contract.parameters.front().metadata->default_value = 2.0;
    graph.asset_uuid = "different-old-schema";
    application::EditorFeatureGraphDocument other{graph, tests::feature_catalog(different.entry)};
    service.set_project_documents({&active, &other});
    const auto other_owner = other.snapshot_owner();
    if (service.save(active, request, root) || active.snapshot_owner() != active_owner || other.snapshot_owner() != other_owner || tests::read_text(path) != file) {
        std::cerr << "Mixed document catalog baselines were accepted\n"; return false;
    }
    graph.nodes.front().definition_version = {9, 0, 0};
    application::EditorFeatureGraphDocument stale{graph, tests::feature_catalog(original.entry)};
    service.set_project_documents({&active, &stale});
    if (service.save(active, request, root) || active.snapshot_owner() != active_owner || tests::read_text(path) != file) {
        std::cerr << "Stale node definition version was migrated against another schema\n"; return false;
    }
    feature::FeatureCatalog missing_catalog;
    application::EditorFeatureGraphDocument missing{graph, missing_catalog};
    service.set_project_documents({&active, &missing});
    if (service.save(active, request, root) || active.snapshot_owner() != active_owner || tests::read_text(path) != file) return false;
    return true;
}

bool opaque_parameters_require_reset_on_schema_changes(const std::filesystem::path& root) {
    using namespace aeg;
    for (const bool old_opaque : {false, true}) {
        auto original = tests::feature_definition_draft();
        auto& parameter = original.entry.contract.parameters.front();
        if (old_opaque) parameter.metadata.reset();
        auto graph = tests::feature_graph(original.entry);
        if (old_opaque) {
            const auto blob = feature::encode_feature_parameters(
                std::vector<feature::FeatureParameterRecord>{{parameter.id, 1, {0, 0, 0, 0, 0, 0, 240, 63}}},
                original.entry.contract.parameters);
            if (!blob) return false;
            graph.nodes.front().parameter_blob = blob.value;
        }
        application::EditorFeatureGraphDocument document{graph, tests::feature_catalog(original.entry)};
        application::EditorNodeDefinitionService service;
        const auto case_root = root / (old_opaque ? "opaque-to-typed" : "typed-to-opaque");
        if (!service.save(document, {original, true, false}, case_root)) return false;
        const auto owner = document.snapshot_owner();
        auto changed = original; changed.entry.version = {2, 0, 0}; changed.entry.contract.version = changed.entry.version;
        auto& next = changed.entry.contract.parameters.front(); next.version = 2;
        if (old_opaque) {
            feature::ParameterMetadata metadata{}; metadata.type = feature::ParameterType::kInt64; metadata.default_value = int64_t{7};
            next.metadata = metadata;
        } else next.metadata.reset();
        application::NodeDefinitionSaveRequest request{changed, true, true};
        request.parameter_migrations = {{parameter.id, application::NodeParameterMigrationAction::kConvertValue}};
        if (service.save(document, request, case_root) || document.snapshot_owner() != owner) {
            std::cerr << "Opaque bytes were treated as a typed conversion\n"; return false;
        }
        request.parameter_migrations.front().action = application::NodeParameterMigrationAction::kResetValue;
        if (!service.save(document, request, case_root)) { std::cerr << "Explicit opaque reset failed\n"; return false; }
        const auto records = feature::decode_feature_parameters(document.snapshot().nodes.front().parameter_blob, changed.entry.contract.parameters);
        if (!records || (!old_opaque && !records.value.empty())) return false;
        if (old_opaque) {
            if (records.value.size() != 1) return false;
            const auto value = feature::deserialize_parameter_value(records.value.front().payload, feature::ParameterType::kInt64);
            if (!value || std::get<int64_t>(value.value) != 7) return false;
            if (!document.undo()) return false;
            auto opaque_version = original; opaque_version.entry.version = {2, 0, 0};
            opaque_version.entry.contract.version = opaque_version.entry.version;
            opaque_version.entry.contract.parameters.front().version = 2;
            application::NodeDefinitionSaveRequest opaque_request{opaque_version, true, true};
            if (service.save(document, opaque_request, case_root)) { std::cerr << "Opaque version changed without reset\n"; return false; }
            opaque_request.parameter_migrations = {{parameter.id, application::NodeParameterMigrationAction::kResetValue}};
            if (!service.save(document, opaque_request, case_root) || !document.snapshot().nodes.front().parameter_blob.empty()) return false;
        }
    }
    return true;
}

bool missing_or_partial_author_schema_cannot_replace_old_definitions(const std::filesystem::path& root) {
    using namespace aeg;
    bool passed = true;
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        const auto case_root = root / std::to_string(scenario);
        auto original = tests::feature_definition_draft();
        original.entry.contract.parameters.front().metadata->constraint = {};
        const auto catalog = tests::feature_catalog(original.entry);
        auto graph = tests::feature_graph(original.entry);
        application::EditorNodeDefinitionService service;
        if (scenario == 0) {
            application::EditorFeatureGraphDocument generated{graph, catalog};
            if (!service.save(generated, {original, true, false}, case_root)) return false;
        }
        const auto file = case_root / "Source/FlowGraph/CustomFeature.flowgraph.contract.json";
        const auto before_file = tests::read_text(file);
        std::filesystem::create_directories(case_root / "Assets/Features");
        auto saved_graph = graph; saved_graph.asset_uuid = "unopened-migration-graph";
        const auto saved_path = case_root / "Assets/Features" / (scenario == 3 ? "Old.AEGFEATURE" : "Old.aegfeature");
        if (scenario != 2 && !application::authoring::save_feature_graph_document(saved_path, saved_graph)) return false;
        const auto saved_bytes = tests::read_text(saved_path);
        feature::FeatureCatalog active_catalog;
        if (scenario == 2) {
            auto summary = catalog.entries().front(); summary.contract = {};
            if (!active_catalog.add_entry(summary)) return false;
        } else if (scenario == 3) active_catalog = catalog;
        else { graph.nodes.clear(); graph.edges.clear(); }
        graph.asset_uuid = "current-migration-graph"; graph.contract_digest = active_catalog.contract_digest();
        application::EditorFeatureGraphDocument document{graph, active_catalog};
        const auto owner = document.snapshot_owner();
        auto changed = original;
        changed.entry.contract.parameters.front().metadata->type = feature::ParameterType::kInt64;
        changed.entry.contract.parameters.front().metadata->default_value = int64_t{7};
        if (scenario == 3) {
            changed.entry.version = {2, 0, 0}; changed.entry.contract.version = changed.entry.version;
            changed.entry.contract.parameters.front().version = 2;
        }
        application::NodeDefinitionSaveRequest request{changed, true, true};
        if (scenario == 3) request.parameter_migrations = {{original.entry.contract.parameters.front().id,
            application::NodeParameterMigrationAction::kConvertValue}};
        const auto result = service.save(document, request, case_root);
        if (result || document.snapshot_owner() != owner || tests::read_text(file) != before_file ||
            tests::read_text(saved_path) != saved_bytes) {
            std::cerr << "AUTHOR_SCHEMA_BYPASS scenario=" << scenario << " accepted=" << static_cast<bool>(result)
                      << " (generated-without-catalog, saved-usage-without-schema, summary-only, uppercase-usage)\n";
            passed = false;
        }
    }
    return passed;
}

bool breaking_edits_check_unopened_graphs_in_configured_assets(const std::filesystem::path& root) {
    using namespace aeg;
    std::filesystem::create_directories(root / "Content/Features");
    application::project::ProjectConfig config{};
    config.name = "Custom content usage";
    config.paths.assets = "Content";
    if (!application::project::ProjectFileCodec{}.save(root / "Project.aegproj", config)) return false;
    const auto original = tests::feature_definition_draft();
    const auto catalog = tests::feature_catalog(original.entry);
    application::EditorFeatureGraphDocument loaded{tests::feature_graph(original.entry), catalog};
    application::EditorNodeDefinitionService service;
    if (!service.save(loaded, {original, true, false}, root)) return false;
    auto unopened = tests::feature_graph(original.entry);
    unopened.asset_uuid = "custom-content-unopened-feature-graph";
    const auto graph_path = root / "Content/Features/Unopened.aegfeature";
    if (!application::authoring::save_feature_graph_document(graph_path, unopened)) return false;
    const auto graph_bytes = tests::read_text(graph_path);
    const auto owner = loaded.snapshot_owner();
    const auto revision = loaded.revision();
    const auto digest = loaded.catalog().contract_digest();
    const std::array extensions{".flowgraph.hpp", ".flowgraph.cpp", ".flowgraph.registration.cpp", ".flowgraph.contract.json"};
    std::vector<std::string> generated_bytes;
    for (const auto* extension : extensions)
        generated_bytes.push_back(tests::read_text(root / "Source/FlowGraph" / (std::string{"CustomFeature"} + extension)));
    auto breaking = original;
    breaking.entry.contract.ports.erase(breaking.entry.contract.ports.begin());
    breaking.port_labels.erase(breaking.port_labels.begin());
    // The loaded graph is known, but the distinct graph under configured
    // Content is deliberately absent from the service's open documents.
    service.set_project_documents({&loaded});
    const auto result = service.save(loaded, {breaking, true, true}, root);
    if (result || result.result.message.find("Open the affected graph") == std::string::npos ||
        loaded.snapshot_owner() != owner || loaded.revision() != revision ||
        loaded.catalog().contract_digest() != digest || tests::read_text(graph_path) != graph_bytes) {
        std::cerr << "Breaking definition edit missed an unopened graph in configured Content or changed prior state: "
                  << result.result.message << '\n';
        return false;
    }
    for (std::size_t index = 0; index < extensions.size(); ++index)
        if (tests::read_text(root / "Source/FlowGraph" / (std::string{"CustomFeature"} + extensions[index])) != generated_bytes[index]) {
            std::cerr << "Rejected custom Content usage edit changed generated source files\n";
            return false;
        }
    return true;
}

bool asset_parameter_saves_validate_real_project_metadata(const std::filesystem::path& root) {
    using namespace aeg;
    const std::string mesh = "10000000-0000-0000-0000-000000000001";
    const std::string texture = "10000000-0000-0000-0000-000000000002";
    const std::string missing = "10000000-0000-0000-0000-000000000003";
    std::filesystem::create_directories(root / "Assets");
    { std::ofstream source(root / "Assets/asset.bin"); source << "source"; }
    {
        std::ofstream meta(root / "Assets/asset.bin.meta");
        meta << "version = 1\nsource_uuid = \"20000000-0000-0000-0000-000000000001\"\n"
                "importer = \"fixture\"\n[[products]]\nuuid = \"" << mesh <<
                "\"\nkind = \"mesh\"\nkey = \"mesh\"\n[[products]]\nuuid = \"" << texture <<
                "\"\nkind = \"texture\"\nkey = \"texture\"\n";
    }
    auto draft = tests::feature_definition_draft();
    feature::ParameterMetadata metadata{};
    metadata.type = feature::ParameterType::kAssetReference;
    metadata.default_value = foundation::AssetReference{mesh};
    metadata.constraint = feature::AssetConstraint{true, foundation::AssetKind::kMesh};
    draft.entry.contract.parameters = {{feature::ParameterId{7}, 1, 64, false, metadata}};
    const auto catalog = tests::feature_catalog(draft.entry);
    application::EditorFeatureGraphDocument document{tests::feature_graph(draft.entry), catalog};
    application::EditorNodeDefinitionService service;
    if (!service.save(document, {draft, true, false}, root)) return false;
    const auto path = root / "Source/FlowGraph/CustomFeature.flowgraph.contract.json";
    const auto bytes = tests::read_text(path);
    const auto owner = document.snapshot_owner();
    const auto digest = document.catalog().contract_digest();
    const auto revision = document.revision();
    for (const auto& uuid : {texture, missing}) {
        auto invalid = draft;
        invalid.entry.contract.parameters.front().metadata->default_value = foundation::AssetReference{uuid};
        for (const bool generate : {false, true}) {
            if (service.save(document, {invalid, generate, false}, root) ||
                document.snapshot_owner() != owner || document.revision() != revision ||
                document.catalog().contract_digest() != digest || tests::read_text(path) != bytes ||
                std::filesystem::exists(root / "Source/FlowGraph/CustomFeature.flowgraph.draft.json")) {
                std::cerr << "Asset default save accepted a wrong-kind or absent product, or changed prior state\n";
                return false;
            }
        }
    }
    // A changed kind also checks existing node values before generated files install.
    const auto payload = feature::serialize_parameter_value(foundation::AssetReference{mesh}, feature::ParameterType::kAssetReference);
    if (!payload) return false;
    const auto encoded = feature::encode_feature_parameters(
        std::vector<feature::FeatureParameterRecord>{{feature::ParameterId{7}, 1, payload.value}},
        draft.entry.contract.parameters);
    if (!encoded) return false;
    auto graph = tests::feature_graph(draft.entry);
    graph.nodes.front().parameter_blob = encoded.value;
    application::EditorFeatureGraphDocument used{graph, catalog};
    auto changed_kind = draft;
    changed_kind.entry.contract.parameters.front().metadata->default_value = foundation::AssetReference{};
    changed_kind.entry.contract.parameters.front().metadata->constraint = feature::AssetConstraint{true, foundation::AssetKind::kTexture};
    if (service.save(used, {changed_kind, true, false}, root) || used.revision() != 0 ||
        used.dirty() || tests::read_text(path) != bytes) {
        std::cerr << "Changing an asset kind installed an invalid existing node value\n";
        return false;
    }
    auto null_default = draft;
    null_default.entry.contract.parameters.front().metadata->default_value = foundation::AssetReference{};
    if (!service.save(document, {null_default, false, false}, root)) {
        std::cerr << "A null asset default must remain valid\n";
        return false;
    }
    application::project::ProjectConfig project_config;
    project_config.name = "Selected asset project";
    const auto selected_project = root / "Project.aegproj";
    if (!application::project::ProjectFileCodec{}.save(selected_project, project_config) ||
        !application::project::ProjectFileCodec{}.save(root / "Other.aegproj", project_config)) return false;
    if (service.save(document, {draft, false, false}, root)) {
        std::cerr << "A root-only asset lookup guessed between project files\n";
        return false;
    }
    if (!service.save(document, {draft, false, false,
            application::make_editor_asset_kind_resolver(selected_project)}, root)) {
        std::cerr << "The selected project resolver was lost while saving a definition default\n";
        return false;
    }
    return true;
}
}  // namespace

int main() {
    using namespace aeg;
    const auto root=std::filesystem::temp_directory_path() / "aeg_editor_node_definition_service_smoke";
    std::error_code error; std::filesystem::remove_all(root,error); std::filesystem::create_directories(root);
    if (!parameter_type_edits_do_not_reinterpret_old_bytes(root / "parameter-type-migration")) return 25;
    if (!explicit_parameter_migrations_preserve_values_and_history(root / "explicit-parameter-migration")) return 26;
    if (!lossy_parameter_conversion_is_rejected(root / "lossy-parameter-migration")) return 27;
    if (!numeric_migration_boundaries(root / "numeric-migration")) return 28;
    if (!parameter_migration_rejects_unknown_and_stale_schemas(root / "invalid-old-schemas")) return 29;
    if (!opaque_parameters_require_reset_on_schema_changes(root / "opaque-migration")) return 30;
    if (!missing_or_partial_author_schema_cannot_replace_old_definitions(root / "author-schema-baseline")) return 31;
    if (!asset_parameter_saves_validate_real_project_metadata(root / "asset-parameters")) return 23;
    if (!breaking_edits_check_unopened_graphs_in_configured_assets(root / "custom-content-unopened")) return 24;
    auto original=tests::feature_definition_draft(); auto catalog=tests::feature_catalog(original.entry);
    application::EditorFeatureGraphDocument editor{tests::feature_graph(original.entry),catalog};
    application::EditorNodeDefinitionService service;
    auto draft=original; draft.display_name="Draft Feature";
    const auto before_digest=editor.catalog().contract_digest(); const auto before_revision=editor.revision();
    const auto saved=service.save(editor,{draft,false,false},root);
    if (!saved || !saved.value.draft_saved || editor.catalog().contract_digest()!=before_digest || editor.revision()!=before_revision || editor.dirty()) return 1;
    const auto file=root / "Source/FlowGraph/CustomFeature.flowgraph.contract.json";
    const auto reopened=application::read_editor_node_contract(file);
    if (!reopened || reopened.value.draft.generated) return 2;
    if (const auto generated=service.save(editor,{draft,true,false},root); !generated) { std::cerr << generated.result.message << "\n"; return 3; }
    if (!service.save(editor,{draft,false,false},root) || !std::filesystem::exists(root / "Source/FlowGraph/CustomFeature.flowgraph.draft.json")) return 4;
    if (!service.save(editor,{draft,true,false},root) || std::filesystem::exists(root / "Source/FlowGraph/CustomFeature.flowgraph.draft.json")) return 5;
    auto removed=draft; removed.entry.contract.ports.erase(removed.entry.contract.ports.begin()); removed.port_labels.erase(removed.port_labels.begin());
    const auto revision=editor.revision(); const auto bytes=tests::read_text(file);
    if (service.save(editor,{removed,true,false},root) || editor.revision()!=revision || tests::read_text(file)!=bytes) return 6;
    if (!service.save(editor,{removed,true,true},root) || !editor.snapshot().edges.empty()) return 7;
    const auto new_digest=editor.catalog().contract_digest();
    if (!editor.undo() || editor.snapshot().edges.size()!=1 || editor.catalog().contract_digest()!=before_digest) return 8;
    if (!editor.redo() || !editor.snapshot().edges.empty() || editor.catalog().contract_digest()!=new_digest) return 9;
    auto readonly=draft; readonly.source=application::NodeContractSource::kCompiled;
    if (service.save(editor,{readonly,false,false},root)) return 10;
    auto bad=draft; bad.entry.contract.ports[0].schema={}; if (application::validate_node_definition(bad)) return 11;
    if (!service.save(editor,{removed,false,false},root)) return 12;
    auto partial=draft; partial.entry.contract.structure=feature::FeatureStructure::kComposite; partial.entry.contract.clock.work_budget_ns = 0;
    const auto unchanged=editor.revision(); const auto unchanged_digest=editor.catalog().contract_digest(); const auto unchanged_bytes=tests::read_text(file);
    if (!service.save(editor,{partial,false,false},root) || service.save(editor,{partial,true,true},root) ||
        editor.revision()!=unchanged || editor.catalog().contract_digest()!=unchanged_digest || tests::read_text(file)!=unchanged_bytes) return 13;
    // The same PortId on a different definition must remain connected.
    auto other=original; other.entry.id=feature::FeatureDefinitionId{"Game.Other"}; other.entry.contract.definition_id=other.entry.id;
    auto precise_catalog=catalog; if (!precise_catalog.export_definition(other.entry)) return 14;
    auto graph=tests::feature_graph(original.entry);
    graph.nodes.push_back({feature::FeatureNodeId{201},other.entry.id,other.entry.version,{},{}});
    graph.nodes.push_back({feature::FeatureNodeId{202},other.entry.id,other.entry.version,{},{}});
    graph.edges.push_back({feature::FeatureNodeId{201},feature::PortId{12},feature::FeatureNodeId{202},feature::PortId{11},feature::EdgeKind::kData,other.entry.contract.clock.id,other.entry.contract.clock.id,1});
    application::EditorFeatureGraphDocument precise{graph,precise_catalog};
    if (!service.save(precise,{removed,true,true},root) || precise.snapshot().edges.size()!=1 || precise.snapshot().edges[0].source_node!=feature::FeatureNodeId{201}) return 15;
    application::EditorFeatureGraphDocument failed_editor{tests::feature_graph(original.entry),catalog};
    const auto blocked=root / "blocked-root"; { std::ofstream out(blocked); out << "file"; }
    if (service.save(failed_editor,{draft,true,false},blocked) || failed_editor.revision()!=0 || failed_editor.dirty() || failed_editor.catalog().contract_digest()!=before_digest) return 16;
    // A failed sidecar retirement must preserve the whole generated file batch.
    const auto sidecar=root / "Source/FlowGraph/CustomFeature.flowgraph.draft.json";
    std::filesystem::remove(sidecar,error); std::filesystem::create_directories(sidecar);
    { std::ofstream out(sidecar / "keep"); out << "keep"; }
    const std::array extensions{".flowgraph.hpp",".flowgraph.cpp",".flowgraph.registration.cpp",".flowgraph.contract.json"};
    std::vector<std::string> original_files;
    for (const auto* extension : extensions) original_files.push_back(tests::read_text(root / "Source/FlowGraph" / (std::string("CustomFeature")+extension)));
    auto cleanup=draft; cleanup.display_name="Cleanup Failure";
    const auto cleanup_revision=failed_editor.revision();
    const auto cleanup_save=service.save(failed_editor,{cleanup,true,false},root);
    if (cleanup_save || failed_editor.revision()!=cleanup_revision) return 17;
    for (std::size_t index=0;index<extensions.size();++index)
        if (tests::read_text(root / "Source/FlowGraph" / (std::string("CustomFeature")+extensions[index]))!=original_files[index]) {
            std::cerr << "failed draft retirement changed generated files\n"; return 18;
        }
    // Project usage must be complete before a breaking edit, and exact
    // edge indices belong to each open document rather than a global ID set.
    std::filesystem::remove_all(sidecar, error);
    auto second_graph = graph; second_graph.asset_uuid = "second-feature-graph";
    application::EditorFeatureGraphDocument first{graph, precise_catalog}, second{second_graph, precise_catalog};
    std::filesystem::create_directories(root / "Assets/Features");
    if (!application::authoring::save_feature_graph_document(root / "Assets/Features/Second.aegfeature", second_graph)) return 19;
    const auto first_revision = first.revision(); const auto file_before = tests::read_text(file);
    service.set_project_documents({&first});
    if (service.save(first, {removed, true, true}, root) || first.revision() != first_revision || tests::read_text(file) != file_before) return 20;
    service.set_project_documents({&first, &second});
    if (!service.save(first, {removed, true, true}, root) || first.snapshot().edges.size() != 1 || second.snapshot().edges.size() != 1 ||
        first.snapshot().edges.front().source_node != feature::FeatureNodeId{201} ||
        second.snapshot().edges.front().source_node != feature::FeatureNodeId{201} || first.catalog().contract_digest() != second.catalog().contract_digest()) return 21;
    if (!first.undo() || !second.undo() || first.snapshot().edges.size() != 2 || second.snapshot().edges.size() != 2 ||
        first.catalog().contract_digest() != precise_catalog.contract_digest() || second.catalog().contract_digest() != precise_catalog.contract_digest()) return 22;
    std::filesystem::remove_all(root,error); std::cout << "Feature definition service passed\n";
}
