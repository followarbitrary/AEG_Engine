#include "editor_node_definition_service.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <type_traits>

#include "editor_node_contract_storage.hpp"
#include "feature_graph_io.hpp"

namespace aeg::application {
namespace {

const feature::FeatureCatalogEntry* definition_entry(const EditorFeatureGraphDocument& document,
                                                     feature::FeatureDefinitionId id) {
    const auto& entries = document.catalog().entries();
    const auto found = std::find_if(entries.begin(), entries.end(),
                                   [id](const auto& entry) { return entry.definition_id == id; });
    return found == entries.end() ? nullptr : &*found;
}

const feature::ParameterContract* parameter_schema(std::span<const feature::ParameterContract> parameters,
                                                  feature::ParameterId id) {
    const auto found = std::find_if(parameters.begin(), parameters.end(),
                                   [id](const auto& parameter) { return parameter.id == id; });
    return found == parameters.end() ? nullptr : &*found;
}

const NodeParameterMigration* parameter_migration(std::span<const NodeParameterMigration> migrations,
                                                 feature::ParameterId id) {
    const auto found = std::find_if(migrations.begin(), migrations.end(),
                                   [id](const auto& migration) { return migration.id == id; });
    return found == migrations.end() ? nullptr : &*found;
}

foundation::ResultValue<const feature::FeatureCatalogEntry*> definition_baseline(
    std::span<EditorFeatureGraphDocument* const> documents, feature::FeatureDefinitionId id) {
    const feature::FeatureCatalogEntry* baseline = nullptr;
    for (const auto* document : documents) {
        const auto* before = definition_entry(*document, id);
        if (before && (before->contract.definition_id != id || before->contract.version != before->version))
            return {foundation::invalid_state("Affected Feature catalog has only a summary; refresh its full old definition schema"), {}};
        for (const auto& node : document->snapshot().nodes) {
            if (node.definition_id != id) continue;
            if (!before)
                return {foundation::invalid_state("Affected Feature graph has no old definition schema"), {}};
            if (node.definition_version != before->version)
                return {foundation::invalid_state("Affected Feature node version does not match its old definition schema"), {}};
        }
        // Documents with neither a catalog entry nor an instance of this
        // definition have no old payload/schema to migrate.
        if (!before) continue;
        if (baseline && (before->version != baseline->version || before->contract_digest != baseline->contract_digest))
            return {foundation::invalid_state("Affected Feature graphs have different old definition schemas; synchronize them before migration"), {}};
        baseline = before;
    }
    return {foundation::Result::success(), baseline};
}

foundation::Result validate_parameter_migrations(const feature::FeatureCatalogEntry* before,
                                                const NodeDefinitionSaveRequest& request) {
    const auto old_parameters = before ? std::span<const feature::ParameterContract>{before->contract.parameters}
                                       : std::span<const feature::ParameterContract>{};
    const auto& next = request.draft.entry;
    for (std::size_t index = 0; index < request.parameter_migrations.size(); ++index) {
        const auto& migration = request.parameter_migrations[index];
        const auto* old_schema = parameter_schema(old_parameters, migration.id);
        if (!old_schema)
            return foundation::invalid_argument("Parameter migration references an unknown old parameter id");
        for (std::size_t previous = 0; previous < index; ++previous)
            if (request.parameter_migrations[previous].id == migration.id)
                return foundation::invalid_argument("Parameter migration contains a duplicate parameter id");
        const auto* new_schema = parameter_schema(next.contract.parameters, migration.id);
        if (migration.action != NodeParameterMigrationAction::kConvertValue &&
            migration.action != NodeParameterMigrationAction::kResetValue)
            return foundation::invalid_argument("Parameter migration action is invalid");
        if (migration.action == NodeParameterMigrationAction::kConvertValue &&
            (!new_schema || !old_schema->metadata || !new_schema->metadata))
            return foundation::invalid_argument("Only retained typed parameters support value conversion; reset opaque or removed parameters");
    }
    if (!before) return foundation::Result::success();
    if (next.version < before->version)
        return foundation::invalid_argument("Feature definition version cannot move backwards");
    for (const auto& old_schema : old_parameters) {
        const auto* new_schema = parameter_schema(next.contract.parameters, old_schema.id);
        const auto* migration = parameter_migration(request.parameter_migrations, old_schema.id);
        if (!new_schema) {
            if (next.version.major <= before->version.major)
                return foundation::invalid_argument("Removing a parameter requires a newer Feature definition major version");
            if (!migration || migration->action != NodeParameterMigrationAction::kResetValue)
                return foundation::invalid_state("Removing a parameter requires an explicit reset migration");
            continue;
        }
        if (new_schema->version < old_schema.version)
            return foundation::invalid_argument("Parameter schema version cannot move backwards");
        const bool changed_type = old_schema.metadata.has_value() != new_schema->metadata.has_value() ||
            (old_schema.metadata && new_schema->metadata && old_schema.metadata->type != new_schema->metadata->type);
        const bool changed_opaque_version = !old_schema.metadata && !new_schema->metadata &&
                                             old_schema.version != new_schema->version;
        if (!changed_type && !changed_opaque_version) continue;
        if (new_schema->version <= old_schema.version)
            return foundation::invalid_argument("Changing a parameter type requires a newer parameter schema version");
        if (next.version.major <= before->version.major)
            return foundation::invalid_argument("Changing a parameter type or opaque schema requires a newer Feature definition major version");
        if (!migration)
            return foundation::invalid_state("Changing a parameter type or opaque schema requires an explicit value migration");
        if (changed_opaque_version && migration->action != NodeParameterMigrationAction::kResetValue)
            return foundation::invalid_argument("An opaque parameter schema change requires a reset migration");
    }
    return foundation::Result::success();
}

foundation::ResultValue<foundation::ParameterValue> convert_parameter_value(
    const foundation::ParameterValue& value, feature::ParameterType type) {
    using feature::ParameterType;
    constexpr double signed_limit = 9223372036854775808.0;
    constexpr double unsigned_limit = 18446744073709551616.0;
    const auto invalid = []() -> foundation::ResultValue<foundation::ParameterValue> {
        return {foundation::invalid_argument("Parameter conversion must preserve the exact numeric value and fit the new type"), {}};
    };
    if (type == ParameterType::kInt32 || type == ParameterType::kInt64) {
        if (const auto* number = std::get_if<int64_t>(&value))
            return {foundation::Result::success(), *number};
        if (const auto* number = std::get_if<uint64_t>(&value)) {
            if (*number > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)())) return invalid();
            return {foundation::Result::success(), static_cast<int64_t>(*number)};
        }
        if (const auto* number = std::get_if<double>(&value)) {
            if (!std::isfinite(*number) || std::trunc(*number) != *number ||
                *number < -signed_limit || *number >= signed_limit) return invalid();
            return {foundation::Result::success(), static_cast<int64_t>(*number)};
        }
        return invalid();
    }
    if (type == ParameterType::kUInt32 || type == ParameterType::kUInt64) {
        if (const auto* number = std::get_if<uint64_t>(&value))
            return {foundation::Result::success(), *number};
        if (const auto* number = std::get_if<int64_t>(&value)) {
            if (*number < 0) return invalid();
            return {foundation::Result::success(), static_cast<uint64_t>(*number)};
        }
        if (const auto* number = std::get_if<double>(&value)) {
            if (!std::isfinite(*number) || std::trunc(*number) != *number ||
                *number < 0 || *number >= unsigned_limit) return invalid();
            return {foundation::Result::success(), static_cast<uint64_t>(*number)};
        }
        return invalid();
    }
    if (type == ParameterType::kFloat || type == ParameterType::kDouble) {
        double converted = 0;
        if (const auto* signed_number = std::get_if<int64_t>(&value)) {
            converted = static_cast<double>(*signed_number);
            if (converted < -signed_limit || converted >= signed_limit ||
                static_cast<int64_t>(converted) != *signed_number) return invalid();
        } else if (const auto* unsigned_number = std::get_if<uint64_t>(&value)) {
            converted = static_cast<double>(*unsigned_number);
            if (converted >= unsigned_limit || static_cast<uint64_t>(converted) != *unsigned_number) return invalid();
        } else if (const auto* floating_number = std::get_if<double>(&value)) {
            converted = *floating_number;
        } else return invalid();
        if (!std::isfinite(converted)) return invalid();
        if (type == ParameterType::kFloat &&
            (std::abs(converted) > (std::numeric_limits<float>::max)() ||
             static_cast<double>(static_cast<float>(converted)) != converted)) return invalid();
        return {foundation::Result::success(), converted};
    }
    // Non-numeric values retain their variant and dimensions. The new typed
    // validator accepts compatible String/Enum or Vector4/Color values only.
    return {foundation::Result::success(), value};
}

foundation::ResultValue<std::vector<uint8_t>> migrate_parameter_blob(
    std::span<const uint8_t> blob, std::span<const feature::ParameterContract> before,
    std::span<const feature::ParameterContract> after, std::span<const NodeParameterMigration> migrations) {
    // This intentionally preserves effective old values, including old defaults
    // materialized by decode_feature_parameters; it never guesses override intent.
    const auto decoded = feature::decode_feature_parameters(blob, before);
    if (!decoded) return {decoded.result, {}};
    std::vector<feature::FeatureParameterRecord> records;
    records.reserve(decoded.value.size());
    for (const auto& record : decoded.value) {
        const auto* new_schema = parameter_schema(after, record.id);
        const auto* migration = parameter_migration(migrations, record.id);
        if (!new_schema || (migration && migration->action == NodeParameterMigrationAction::kResetValue)) continue;
        feature::FeatureParameterRecord next{record.id, new_schema->version, record.payload};
        if (migration && migration->action == NodeParameterMigrationAction::kConvertValue) {
            const auto* old_schema = parameter_schema(before, record.id);
            const auto old_value = feature::deserialize_parameter_value(record.payload, old_schema->metadata->type);
            if (!old_value) return {old_value.result, {}};
            const auto converted = convert_parameter_value(old_value.value, new_schema->metadata->type);
            if (!converted) return {converted.result, {}};
            if (const auto valid = feature::validate_parameter_value(converted.value, *new_schema->metadata); !valid)
                return {valid, {}};
            auto payload = feature::serialize_parameter_value(converted.value, new_schema->metadata->type);
            if (!payload) return {payload.result, {}};
            next.payload = std::move(payload.value);
        }
        records.push_back(std::move(next));
    }
    // The new codec fills reset/new defaults and rejects required values without
    // a replacement, stale versions, size changes, and incompatible constraints.
    return feature::encode_feature_parameters(records, after);
}


std::string file_stem(std::string value) {
    const auto dot = value.find_last_of('.');
    if (dot != std::string::npos)
        value.erase(0, dot + 1);
    for (char& character : value) {
        if (std::isalnum(static_cast<unsigned char>(character)) == 0 && character != '_')
            character = '_';
    }
    if (value.empty() || std::isdigit(static_cast<unsigned char>(value.front())) != 0)
        value.insert(value.begin(), '_');
    return value;
}

std::string unique_file_stem(const std::filesystem::path& source_dir, const NodeDefinitionDraft& draft) {
    const std::string base = file_stem(draft.name);
    std::string stem = base;
    constexpr std::array<std::string_view, 5> extensions{
        ".flowgraph.contract.json", ".flowgraph.draft.json", ".flowgraph.hpp", ".flowgraph.cpp",
        ".flowgraph.registration.cpp"};
    for (;;) {
        bool collision = false;
        bool matching_contract = false;
        for (const auto extension : {std::string_view{".flowgraph.contract.json"},
                                     std::string_view{".flowgraph.draft.json"}}) {
            const auto path = source_dir / (stem + std::string{extension});
            std::error_code error;
            if (!std::filesystem::exists(path, error))
                continue;
            const auto existing = read_editor_node_contract(path);
            if (existing && existing.value.draft.entry.id == draft.entry.id) {
                matching_contract = true;
                break;
            }
        }
        if (matching_contract)
            return stem;
        for (const auto extension : extensions) {
            const auto path = source_dir / (stem + std::string{extension});
            std::error_code error;
            if (!std::filesystem::exists(path, error))
                continue;
            collision = true;
            break;
        }
        if (!collision)
            return stem;
        stem = base + "_" + std::to_string(draft.entry.id.value());
    }
}

}  // namespace
void EditorNodeDefinitionService::set_project_documents(std::vector<EditorFeatureGraphDocument*> documents) {
    project_documents_.clear();
    for (auto* document : documents)
        if (document && std::find(project_documents_.begin(), project_documents_.end(), document) == project_documents_.end())
            project_documents_.push_back(document);
}
std::vector<NodeContractUsage> EditorNodeDefinitionService::usages_for(
    const EditorFeatureGraphDocument& active, const feature::FeatureDefinitionId& id) const {
    std::vector<NodeContractUsage> usages;
    const auto append = [&](const EditorFeatureGraphDocument& editor) {
        const auto& graph = editor.snapshot();
        for (const auto& node : graph.nodes) {
            if (node.definition_id != id) continue;
            NodeContractUsage usage{}; usage.document_identity = &editor; usage.node_id = node.node_id;
            for (std::size_t index = 0; index < graph.edges.size(); ++index) {
                const auto& edge = graph.edges[index];
                if (edge.source_node == node.node_id)
                    usage.edges.push_back({std::to_string(index), edge.source_port.value(), index});
                if (edge.target_node == node.node_id)
                    usage.edges.push_back({std::to_string(index), edge.target_port.value(), index});
            }
            usages.push_back(std::move(usage));
        }
    };
    append(active);
    for (const auto* editor : project_documents_) if (editor != &active) append(*editor);
    return usages;
}
foundation::ResultValue<NodeContractDiff> EditorNodeDefinitionService::preview(
    const EditorFeatureGraphDocument& document, const NodeDefinitionDraft& draft) const {
    if (const auto valid = validate_node_definition(draft); !valid) return {valid, {}};
    const auto* before = definition_entry(document, draft.entry.id);
    if (!before)
        for (const auto* candidate : project_documents_)
            if ((before = definition_entry(*candidate, draft.entry.id))) break;
    if (!before) return {foundation::Result::success(), {}};
    return {foundation::Result::success(), diff_node_definition(node_definition_from_catalog(*before), draft, usages_for(document, draft.entry.id))};
}

foundation::ResultValue<NodeDefinitionSaveResult> EditorNodeDefinitionService::save(
    EditorFeatureGraphDocument& document, const NodeDefinitionSaveRequest& request,
    const std::filesystem::path& project_root) {
    if (project_root.empty())
        return {foundation::invalid_argument("node definition save requires a project root"), {}};
    if (request.draft.source != NodeContractSource::kEditorDraft)
        return {foundation::unsupported("compiled or conflicted contracts are read-only; edit their source in the IDE"), {}};
    const auto preview_result = preview(document, request.draft);
    if (!preview_result)
        return {preview_result.result, {}};
    const auto& asset_project = request.selected_project_file.empty() ? project_root : request.selected_project_file;
    const auto asset_resolver = request.asset_kind_resolver ? request.asset_kind_resolver
                                                          : make_editor_asset_kind_resolver(asset_project);
    for (const auto& parameter : request.draft.entry.contract.parameters) {
        if (!parameter.metadata || !parameter.metadata->default_value) continue;
        if (const auto valid = feature::validate_parameter_value(*parameter.metadata->default_value,
                *parameter.metadata, asset_resolver); !valid)
            return {valid, {}};
    }
    const auto diff = preview_result.value;
    if (request.generate_stub && diff.requires_confirmation && !request.disconnect_affected_edges)
        return {foundation::invalid_state("node definition changes affect connected edges; confirm disconnect"), {}};

    // The root-only service API predates selectable project files and writes
    // to Source even before the root exists. An opaque asset resolver cannot
    // also select a source tree; that authority is the explicit project file.
    const auto source = request.selected_project_file.empty()
        ? foundation::ResultValue<std::filesystem::path>{foundation::Result::success(), project_root / "Source"}
        : resolve_editor_source_directory(request.selected_project_file);
    if (!source) return {source.result, {}};
    const auto contract_directory = source.value / "FlowGraph";
    const std::string type_name = unique_file_stem(contract_directory, request.draft);
    const auto contract_path = contract_directory /
                               (type_name + ".flowgraph.contract.json");
    NodeDefinitionDraft draft = request.draft;
    draft.generated = request.generate_stub;
    NodeContractFile contract{contract_path, draft, {}, NodeContractSource::kEditorDraft};
    if (!request.generate_stub) {
        // Preserve the last generated contract while a newer editor draft is
        // being worked on. Drafts are not an authority for runtime loading.
        if (const auto previous = read_editor_node_contract(contract_path); previous && previous.value.draft.generated)
            contract.path = contract_directory / (type_name + ".flowgraph.draft.json");
        if (const auto result = write_editor_node_contract_atomic(contract, project_root); !result)
            return {result, {}};
        NodeDefinitionSaveResult result;
        result.diff = diff;
        result.draft_saved = true;
        return {foundation::Result::success(), std::move(result)};
    }

    CustomNodeScaffoldRequest scaffold{};
    scaffold.project_root = project_root;
    std::error_code error;
    scaffold.source_subdirectory = std::filesystem::relative(contract_directory, project_root, error);
    if (error) return {foundation::invalid_argument("Cannot resolve project Feature source directory"), {}};
    scaffold.type_name = type_name;
    scaffold.owner_module = draft.entry.owner_module;
    scaffold.mode = CustomNodeGenerationMode::kCreate;
    scaffold.contract_digest = draft.draft_revision_hash;
    const auto source_dir = project_root / scaffold.source_subdirectory;
    const auto generated_header = source_dir / (type_name + ".flowgraph.hpp");
    const auto generated_impl = source_dir / (type_name + ".flowgraph.cpp");
    const auto generated_registration = source_dir / (type_name + ".flowgraph.registration.cpp");
    if (std::filesystem::exists(generated_header, error) || std::filesystem::exists(generated_impl, error) ||
        std::filesystem::exists(generated_registration, error) || std::filesystem::exists(contract_path, error))
        scaffold.mode = CustomNodeGenerationMode::kUpdateGeneratedRegions;
    std::vector<EditorFeatureGraphDocument*> documents{&document};
    for (auto* candidate : project_documents_)
        if (candidate && std::find(documents.begin(), documents.end(), candidate) == documents.end())
            documents.push_back(candidate);
    const auto baseline = definition_baseline(documents, draft.entry.id);
    if (!baseline) return {baseline.result, {}};
    if (!baseline.value) {
        const auto installed = read_editor_node_contract(contract_path);
        if (installed && installed.value.draft.generated)
            return {foundation::invalid_state("Refresh the Feature catalog before replacing an existing generated definition"), {}};
    }
    if (const auto valid = validate_parameter_migrations(baseline.value, request); !valid)
        return {valid, {}};
    // A destructive change cannot proceed with an incomplete project usage
    // index. Unopened authoring documents must be reviewed in their editor.
    if (!baseline.value || diff.version_level == NodeContractVersionLevel::kMajor) {
        const auto assets = resolve_editor_asset_directory(asset_project);
        if (!assets) return {assets.result, {}};
        if (std::filesystem::exists(assets.value, error)) {
            for (std::filesystem::recursive_directory_iterator it{assets.value, error};
                 !error && it != std::filesystem::recursive_directory_iterator{}; it.increment(error)) {
                auto extension = it->path().extension().string();
                std::transform(extension.begin(), extension.end(), extension.begin(),
                               [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
                if (extension != ".aegfeature")
                    continue;
                const auto saved = authoring::load_feature_graph_document(it->path());
                if (!saved)
                    return {foundation::invalid_state("Cannot inspect project usage: " + it->path().string()), {}};
                const bool referenced = std::any_of(saved.value.nodes.begin(), saved.value.nodes.end(),
                                                   [&](const feature::FeatureNode& node) {
                                                       return node.definition_id.value() == draft.entry.id.value();
                                                   });
                if (referenced) {
                    if (!baseline.value)
                        return {foundation::invalid_state("Refresh the old Feature definition schema before changing saved graph usage: " +
                                                           it->path().string()), {}};
                    const bool loaded = std::any_of(documents.begin(), documents.end(), [&](const auto* editor) {
                        return !saved.value.asset_uuid.empty() && editor->snapshot().asset_uuid == saved.value.asset_uuid;
                    });
                    if (!loaded)
                        return {foundation::invalid_state("Open the affected graph before a breaking edit: " +
                                                           it->path().string()), {}};
                }
            }
        }
        if (error)
            return {foundation::invalid_state("Cannot scan project graph usages: " + error.message()), {}};
    }
    std::vector<std::pair<EditorFeatureGraphDocument*, EditorFeatureGraphDocument>> prepared;
    for (auto* candidate : documents) {
        const auto* old_definition = definition_entry(*candidate, draft.entry.id);
        feature::FeatureCatalog next_catalog;
        for (const auto& entry : candidate->catalog().entries()) {
            if (entry.definition_id == draft.entry.id) continue;
            if (const auto added = next_catalog.add_entry(entry); !added) return {added, {}};
        }
        if (const auto added = next_catalog.export_definition(draft.entry); !added) return {added, {}};
        EditorFeatureGraphDocument next = *candidate;
        const auto transaction = next.apply_contract_transaction("Save Feature definition", next_catalog,
            [&](feature::FeatureGraphDocument& graph) {
                if (request.disconnect_affected_edges) {
                    std::unordered_set<std::size_t> removed;
                    for (const auto& impact : diff.impacts)
                        if (impact.document_identity == candidate) removed.insert(impact.edge_index);
                    for (std::size_t index = graph.edges.size(); index > 0; --index)
                        if (removed.contains(index - 1)) graph.edges.erase(graph.edges.begin() + static_cast<std::ptrdiff_t>(index - 1));
                }
                for (auto& node : graph.nodes) {
                    if (node.definition_id != draft.entry.id) continue;
                    auto parameters = migrate_parameter_blob(node.parameter_blob, old_definition->contract.parameters,
                                                             draft.entry.contract.parameters, request.parameter_migrations);
                    if (!parameters) return parameters.result;
                    if (const auto valid = feature::validate_feature_parameter_assets(parameters.value,
                            draft.entry.contract.parameters, asset_resolver); !valid)
                        return valid;
                    node.parameter_blob = std::move(parameters.value);
                    node.definition_version = draft.entry.version;
                }
                auto validation_graph = graph; validation_graph.edges.clear();
                for (const auto& edge : graph.edges) {
                    if (const auto valid = feature::validate_feature_edge(validation_graph, edge, next_catalog); !valid) return valid;
                    validation_graph.edges.push_back(edge);
                }
                graph.contract_digest = next_catalog.contract_digest(); ++graph.graph_version; ++graph.draft_revision;
                return foundation::Result::success();
            });
        if (!transaction) return {transaction, {}};
        prepared.emplace_back(candidate, std::move(next));
    }
    // Every allocating/validating in-memory operation has succeeded before
    // the file batch is installed. Publishing prepared states cannot fail.
    auto generated = scaffold_custom_node(scaffold, draft);
    if (!generated)
        return {generated.result, {}};
    static_assert(std::is_nothrow_move_assignable_v<EditorFeatureGraphDocument>);
    for (auto& [candidate, next] : prepared)
        *candidate = std::move(next);
    NodeDefinitionSaveResult result;
    result.diff = diff;
    result.generated = std::move(generated.value);
    result.document_changed = true;
    result.generated_stub = true;
    return {foundation::Result::success(), std::move(result)};
}

}  // namespace aeg::application
