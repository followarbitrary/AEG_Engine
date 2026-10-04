#include "editor_node_definition_service.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_set>
#include <type_traits>

#include "editor_node_contract_storage.hpp"
#include "pipeline_document_io.hpp"

namespace aeg::application {
namespace {

using pipeline::AuthoringTypeKey;
using pipeline::AuthoringTypeKind;

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

std::string unique_file_stem(const std::filesystem::path& project_root, const NodeDefinitionDraft& draft) {
    const auto source_dir = project_root / "Source" / "FlowGraph";
    const std::string base = file_stem(draft.entry.name);
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
            if (existing && existing.value.draft.entry.key == draft.entry.key) {
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
        stem = base + "_" + std::to_string(draft.entry.key.id);
    }
}

bool node_matches(const pipeline::PipelineDocument& document, const AuthoringTypeKey& key,
                  std::string_view node_id) {
    if (key.kind == AuthoringTypeKind::kPipeline)
        return std::any_of(document.pipeline_nodes.begin(), document.pipeline_nodes.end(),
                           [&](const auto& node) { return node.node_id == node_id && node.type.value() == key.id; });
    if (key.kind == AuthoringTypeKind::kFeature)
        return std::any_of(document.feature_nodes.begin(), document.feature_nodes.end(),
                           [&](const auto& node) { return node.node_id == node_id && node.type.value() == key.id; });
    if (key.kind == AuthoringTypeKind::kRenderPass)
        return std::any_of(document.pass_nodes.begin(), document.pass_nodes.end(),
                           [&](const auto& node) { return node.node_id == node_id && node.type.value() == key.id; });
    return false;
}

void append_usage_for_node(std::vector<NodeContractUsage>& usages,
                           const EditorCompositionDocument& editor, const AuthoringTypeKey& key,
                           std::string node_id) {
    const auto document = editor.snapshot();
    if (!document || !node_matches(*document, key, node_id))
        return;
    NodeContractUsage usage{};
    usage.document_identity = &editor;
    usage.node_id = std::move(node_id);
    for (const auto& edge : document->edges) {
        uint64_t port_id = 0;
        if (edge.source.node_id == usage.node_id)
            port_id = edge.source.port_id;
        else if (edge.destination.node_id == usage.node_id)
            port_id = edge.destination.port_id;
        else
            continue;
        usage.edge_ids.push_back(edge.edge_id);
        usage.edges.push_back({edge.edge_id, port_id, edge.binding_id});
    }
    for (const auto& binding : document->input_bindings) {
        if (binding.node_id != usage.node_id)
            continue;
        usage.binding_ids.push_back(binding.port_id ? binding.port_id : binding.default_id);
        usage.bindings.push_back({binding.port_id, binding.port_id});
    }
    usages.push_back(std::move(usage));
}

void remove_impacted_edges(pipeline::PipelineDocument& document, const NodeContractDiff& diff,
                           const void* document_identity) {
    std::unordered_set<std::string> edges;
    const bool scoped = std::any_of(diff.impacts.begin(), diff.impacts.end(),
                                    [document_identity](const auto& impact) {
                                        return impact.document_identity != nullptr;
                                    });
    for (const auto& impact : diff.impacts)
        if ((!scoped || impact.document_identity == document_identity) && !impact.edge_id.empty())
            edges.insert(impact.edge_id);
    if (!scoped)
        for (const auto& edge_id : diff.affected_edge_ids)
            edges.insert(edge_id);
    if (edges.empty())
        return;
    document.edges.erase(std::remove_if(document.edges.begin(), document.edges.end(),
                                        [&](const auto& edge) { return edges.contains(edge.edge_id); }),
                         document.edges.end());
    for (auto& binding : document.input_bindings)
        binding.edge_ids.erase(std::remove_if(binding.edge_ids.begin(), binding.edge_ids.end(),
                                              [&](const auto& edge_id) { return edges.contains(edge_id); }),
                               binding.edge_ids.end());
}

void remove_impacted_bindings(pipeline::PipelineDocument& document, const NodeContractDiff& diff,
                              const void* document_identity) {
    const bool scoped = std::any_of(diff.impacts.begin(), diff.impacts.end(),
                                    [document_identity](const auto& impact) {
                                        return impact.document_identity != nullptr;
                                    });
    document.input_bindings.erase(
        std::remove_if(document.input_bindings.begin(), document.input_bindings.end(),
                       [&](const auto& binding) {
                           return std::any_of(diff.impacts.begin(), diff.impacts.end(), [&](const auto& impact) {
                               return (!scoped || impact.document_identity == document_identity) &&
                                      impact.node_id == binding.node_id && impact.port_id == binding.port_id;
                           });
                       }),
        document.input_bindings.end());
}

}  // namespace

void EditorNodeDefinitionService::set_project_documents(std::vector<EditorCompositionDocument*> documents) {
    project_documents_.clear();
    for (auto* document : documents) {
        if (document && std::find(project_documents_.begin(), project_documents_.end(), document) ==
                            project_documents_.end())
            project_documents_.push_back(document);
    }
}

std::vector<NodeContractUsage> EditorNodeDefinitionService::usages_for(
    const EditorCompositionDocument& active, const AuthoringTypeKey& key) const {
    std::vector<NodeContractUsage> usages;
    auto append = [&](const EditorCompositionDocument& editor) {
        const auto document = editor.snapshot();
        if (!document)
            return;
        if (key.kind == AuthoringTypeKind::kPipeline) {
            for (const auto& node : document->pipeline_nodes)
                if (node.type.value() == key.id)
                    append_usage_for_node(usages, editor, key, node.node_id);
        } else if (key.kind == AuthoringTypeKind::kFeature) {
            for (const auto& node : document->feature_nodes)
                if (node.type.value() == key.id)
                    append_usage_for_node(usages, editor, key, node.node_id);
        } else if (key.kind == AuthoringTypeKind::kRenderPass) {
            for (const auto& node : document->pass_nodes)
                if (node.type.value() == key.id)
                    append_usage_for_node(usages, editor, key, node.node_id);
        }
    };
    append(active);
    for (const auto* document : project_documents_)
        if (document != &active)
            append(*document);
    return usages;
}

foundation::ResultValue<NodeContractDiff> EditorNodeDefinitionService::preview(
    const EditorCompositionDocument& document, const NodeDefinitionDraft& draft) const {
    if (const auto valid = validate_node_definition(draft); !valid)
        return {valid, {}};
    const auto* before = document.catalog().find(draft.entry.key);
    if (!before)
        return {foundation::not_found("node definition type is not registered in the active catalog"), {}};
    return {foundation::Result::success(), diff_node_definition(*before, draft, usages_for(document, draft.entry.key))};
}

foundation::ResultValue<NodeDefinitionSaveResult> EditorNodeDefinitionService::save(
    EditorCompositionDocument& document, const NodeDefinitionSaveRequest& request,
    const std::filesystem::path& project_root) {
    if (project_root.empty())
        return {foundation::invalid_argument("node definition save requires a project root"), {}};
    if (request.draft.source != NodeContractSource::kEditorDraft)
        return {foundation::unsupported("compiled or conflicted contracts are read-only; edit their source in the IDE"), {}};
    const auto preview_result = preview(document, request.draft);
    if (!preview_result)
        return {preview_result.result, {}};
    const auto diff = preview_result.value;
    if (request.generate_stub && diff.requires_confirmation && !request.disconnect_affected_edges)
        return {foundation::invalid_state("node definition changes affect connected edges; confirm disconnect"), {}};

    const std::string type_name = unique_file_stem(project_root, request.draft);
    const auto contract_path = project_root / "Source" / "FlowGraph" /
                               (type_name + ".flowgraph.contract.json");
    NodeDefinitionDraft draft = request.draft;
    draft.generated = request.generate_stub;
    NodeContractFile contract{contract_path, draft, {}, NodeContractSource::kEditorDraft};
    if (!request.generate_stub) {
        // Preserve the last generated contract while a newer editor draft is
        // being worked on. Drafts are not an authority for runtime loading.
        if (const auto previous = read_editor_node_contract(contract_path); previous && previous.value.draft.generated)
            contract.path = project_root / "Source" / "FlowGraph" / (type_name + ".flowgraph.draft.json");
        if (const auto result = write_editor_node_contract_atomic(contract, project_root); !result)
            return {result, {}};
        NodeDefinitionSaveResult result;
        result.diff = diff;
        result.draft_saved = true;
        return {foundation::Result::success(), std::move(result)};
    }

    CustomNodeScaffoldRequest scaffold{};
    scaffold.project_root = project_root;
    scaffold.type_name = type_name;
    scaffold.owner_module = draft.entry.owner_module;
    scaffold.mode = CustomNodeGenerationMode::kCreate;
    scaffold.contract_digest = draft.draft_revision_hash;
    const auto source_dir = project_root / scaffold.source_subdirectory;
    const auto generated_header = source_dir / (type_name + ".flowgraph.hpp");
    const auto generated_impl = source_dir / (type_name + ".flowgraph.cpp");
    const auto generated_registration = source_dir / (type_name + ".flowgraph.registration.cpp");
    std::error_code error;
    if (std::filesystem::exists(generated_header, error) || std::filesystem::exists(generated_impl, error) ||
        std::filesystem::exists(generated_registration, error) || std::filesystem::exists(contract_path, error))
        scaffold.mode = CustomNodeGenerationMode::kUpdateGeneratedRegions;
    std::vector<EditorCompositionDocument*> documents{&document};
    for (auto* candidate : project_documents_)
        if (candidate && std::find(documents.begin(), documents.end(), candidate) == documents.end())
            documents.push_back(candidate);
    // A destructive change cannot proceed with an incomplete project usage
    // index. Unopened authoring documents must be reviewed in their editor.
    if (diff.version_level == NodeContractVersionLevel::kMajor) {
        const auto assets = project_root / "Assets";
        if (std::filesystem::exists(assets, error)) {
            for (std::filesystem::recursive_directory_iterator it{assets, error};
                 !error && it != std::filesystem::recursive_directory_iterator{}; it.increment(error)) {
                if (it->path().extension() != ".aegpipe")
                    continue;
                const auto saved = project::load_pipeline_document(it->path());
                if (!saved)
                    return {foundation::invalid_state("Cannot inspect project usage: " + it->path().string()), {}};
                const auto referenced = [&](const auto& nodes) {
                    return std::any_of(nodes.begin(), nodes.end(), [&](const auto& node) {
                        return node.type.value() == draft.entry.key.id;
                    });
                };
                if (referenced(saved.value.pipeline_nodes) || referenced(saved.value.feature_nodes) ||
                    referenced(saved.value.pass_nodes)) {
                    const bool loaded = std::any_of(documents.begin(), documents.end(), [&](const auto* editor) {
                        return !saved.value.asset_uuid.empty() && editor->snapshot()->asset_uuid == saved.value.asset_uuid;
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
    std::vector<std::pair<EditorCompositionDocument*, EditorCompositionDocument>> prepared;
    for (auto* candidate : documents) {
        if (!candidate->catalog().find(draft.entry.key))
            continue;
        pipeline::TypeCatalog next_catalog;
        for (const auto& entry : candidate->catalog().entries()) {
            if (entry.key == draft.entry.key)
                static_cast<void>(next_catalog.add(draft.entry));
            else
                static_cast<void>(next_catalog.add(entry));
        }
        EditorCompositionDocument next = *candidate;
        const auto transaction = next.apply_contract_transaction(
            "Save node definition", std::move(next_catalog), [&](pipeline::PipelineDocument& next) {
                if (request.disconnect_affected_edges) {
                    remove_impacted_edges(next, diff, candidate);
                    remove_impacted_bindings(next, diff, candidate);
                }
                return foundation::Result::success();
            });
        if (!transaction)
            return {transaction, {}};
        prepared.emplace_back(candidate, std::move(next));
    }
    // Every allocating/validating in-memory operation has succeeded before
    // the file batch is installed. Publishing prepared states cannot fail.
    auto generated = scaffold_custom_node(scaffold, draft);
    if (!generated)
        return {generated.result, {}};
    // A generated contract supersedes an earlier draft sidecar. Leaving that
    // sidecar in place would make the loader report a false conflict on the
    // next editor restart.
    const auto stale_draft_path = project_root / "Source" / "FlowGraph" /
                                  (type_name + ".flowgraph.draft.json");
    std::error_code stale_draft_error;
    if (std::filesystem::exists(stale_draft_path, stale_draft_error) && !stale_draft_error) {
        std::filesystem::remove(stale_draft_path, stale_draft_error);
        if (stale_draft_error)
            return {foundation::Result::failure(foundation::ResultCode::kUnknown,
                                                "cannot remove superseded editor draft: " +
                                                    stale_draft_path.string()),
                    {}};
    }
    static_assert(std::is_nothrow_move_assignable_v<EditorCompositionDocument>);
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
