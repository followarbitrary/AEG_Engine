#include "editor_feature_graph_document.hpp"
#include "feature_process_editor.hpp"
#include "asset_metadata.hpp"
#include "feature_graph_io.hpp"
#include "editor_feature_graph_layout.hpp"
#include "project_context.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <exception>
#include <iterator>
#include <limits>
#include <map>
#include <new>
#include <unordered_set>
#include <utility>
#include <type_traits>

namespace aeg::application {
namespace {
foundation::Result advance_layout_revision(feature::FeatureGraphDocument& graph) {
    if (graph.layout_revision == std::numeric_limits<uint64_t>::max())
        return foundation::invalid_state("FeatureGraph layout revision is exhausted");
    ++graph.layout_revision;
    return foundation::Result::success();
}

foundation::Result restore_process_layout(feature::FeatureGraphDocument& graph,
    feature::FeatureNodeId owner, const ProcessEditorLayout& layout,
    const ProcessEditorLayout& previous_layout,
    const std::vector<std::string>& previous_extensions) {
    if (layout == previous_layout) {
        // Valid imported extensions may use different JSON formatting or store
        // explicit defaults. Retain their bytes when the layout did not change.
        if (graph.extensions != previous_extensions) graph.extensions = previous_extensions;
        return foundation::Result::success();
    }
    const auto extension_count = graph.extensions.size();
    if (const auto result = write_process_editor_layout(graph, owner, layout); !result) return result;
    // Semantic edits remove the old record before changing its schema. Restore
    // the replacement to that same slot so other owners and vendor records keep
    // their order, and unchanged layout content does not advance its revision.
    if (previous_extensions.size() == extension_count + 1 && graph.extensions.size() == extension_count + 1) {
        std::size_t index = 0;
        while (index < extension_count && graph.extensions[index] == previous_extensions[index]) ++index;
        std::rotate(graph.extensions.begin() + static_cast<std::ptrdiff_t>(index),
                    graph.extensions.end() - 1, graph.extensions.end());
    }
    if (previous_extensions != graph.extensions) return advance_layout_revision(graph);
    return foundation::Result::success();
}
}  // namespace

foundation::ResultValue<std::filesystem::path> resolve_editor_project_file(
    const std::filesystem::path& project_root_or_file) {
    if (project_root_or_file.empty())
        return {foundation::invalid_argument("Authoring requires a project"), {}};
    try {
        std::error_code error;
        std::filesystem::path project_file;
        const auto is_project_file = [](const std::filesystem::path& path) {
            auto extension = path.extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
            return extension == ".aegproj";
        };
        if (is_project_file(project_root_or_file) || std::filesystem::is_regular_file(project_root_or_file, error)) {
            project_file = project_root_or_file;
        } else {
            if (!std::filesystem::is_directory(project_root_or_file, error) || error)
                return {foundation::not_found("Authoring project root does not exist"), {}};
            for (std::filesystem::directory_iterator it{project_root_or_file, error};
                 !error && it != std::filesystem::directory_iterator{}; it.increment(error)) {
                if (!is_project_file(it->path())) continue;
                if (!project_file.empty())
                    return {foundation::invalid_argument("Authoring requires an explicit project file when the root contains multiple projects"), {}};
                project_file = it->path();
            }
            if (error) return {foundation::invalid_state("Cannot inspect authoring project: " + error.message()), {}};
        }
        if (!project_file.empty()) {
            project_file = std::filesystem::weakly_canonical(project_file, error);
            if (error) return {foundation::invalid_state("Cannot resolve authoring project file: " + error.message()), {}};
        }
        return {foundation::Result::success(), std::move(project_file)};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("Project file resolution allocation failed"), {}};
    } catch (const std::exception& exception) {
        return {foundation::unknown_exception(exception), {}};
    }
}

foundation::ResultValue<std::filesystem::path> resolve_editor_asset_directory(
    const std::filesystem::path& project_root_or_file) {
    try {
        const auto project_file = resolve_editor_project_file(project_root_or_file);
        if (!project_file) return {project_file.result, {}};
        auto assets = project_root_or_file / "Assets";
        if (!project_file.value.empty()) {
            const auto project_config = project::ProjectFileCodec{}.load(project_file.value);
            if (!project_config) return {project_config.result, {}};
            const auto paths = project::ProjectPaths::resolve(project_file.value, project_config.value.config.paths);
            if (!paths) return {paths.result, {}};
            assets = paths.value.directory(project::ProjectDirectory::kAssets);
        }
        return {foundation::Result::success(), std::move(assets)};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("Project asset directory allocation failed"), {}};
    } catch (const std::exception& exception) {
        return {foundation::unknown_exception(exception), {}};
    }
}

foundation::ResultValue<std::filesystem::path> resolve_editor_source_directory(
    const std::filesystem::path& project_root_or_file) {
    try {
        const auto project_file = resolve_editor_project_file(project_root_or_file);
        if (!project_file) return {project_file.result, {}};
        auto source = project_root_or_file / "Source";
        if (!project_file.value.empty()) {
            const auto project_config = project::ProjectFileCodec{}.load(project_file.value);
            if (!project_config) return {project_config.result, {}};
            const auto paths = project::ProjectPaths::resolve(project_file.value, project_config.value.config.paths);
            if (!paths) return {paths.result, {}};
            source = paths.value.directory(project::ProjectDirectory::kSource);
        }
        return {foundation::Result::success(), std::move(source)};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("Project source directory allocation failed"), {}};
    } catch (const std::exception& exception) {
        return {foundation::unknown_exception(exception), {}};
    }
}

namespace {
using EditorAssetKindIndex = std::map<foundation::AssetId, foundation::AssetKind>;

foundation::ResultValue<EditorAssetKindIndex> scan_editor_asset_kinds(const std::filesystem::path& project_root_or_file) {
    try {
        const auto assets = resolve_editor_asset_directory(project_root_or_file);
        if (!assets) return {assets.result, {}};
        std::error_code error;
        if (!std::filesystem::is_directory(assets.value, error) || error)
            return {foundation::not_found("Project assets directory does not exist"), {}};
        EditorAssetKindIndex products;
        for (std::filesystem::recursive_directory_iterator it{assets.value, error};
             !error && it != std::filesystem::recursive_directory_iterator{}; it.increment(error)) {
            if (it->path().extension() != ".meta") continue;
            if (!it->is_regular_file(error)) {
                if (error) break;
                continue;
            }
            const auto metadata = tooling::AssetMetaCodec{}.load(it->path());
            if (!metadata) return {metadata.result, {}};
            auto source = it->path(); source.replace_extension();
            if (!std::filesystem::is_regular_file(source, error) || error)
                return {foundation::not_found("Asset metadata source file does not exist: " + source.string()), {}};
            if (metadata.value.source.value == foundation::Uuid{})
                return {foundation::invalid_argument("Asset metadata source UUID cannot be null"), {}};
            for (const auto& product : metadata.value.products) {
                if (product.id == foundation::AssetId{} || !products.emplace(product.id, product.kind).second)
                    return {foundation::invalid_argument("Project asset metadata contains null or duplicate product UUIDs"), {}};
            }
        }
        if (error) return {foundation::invalid_state("Cannot scan project asset metadata: " + error.message()), {}};
        return {foundation::Result::success(), std::move(products)};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("Project asset lookup allocation failed"), {}};
    } catch (const std::exception& exception) {
        return {foundation::unknown_exception(exception), {}};
    }
}

foundation::ResultValue<foundation::AssetKind> lookup_editor_asset_kind(
    const foundation::ResultValue<EditorAssetKindIndex>& index, foundation::AssetId id) {
    if (id == foundation::AssetId{})
        return {foundation::invalid_argument("Asset lookup requires a nonnull product UUID"), {}};
    if (!index) return {index.result, {}};
    const auto product = index.value.find(id);
    if (product == index.value.end()) return {foundation::not_found("Asset UUID has no declared project product"), {}};
    return {foundation::Result::success(), product->second};
}
}  // namespace

feature::AssetKindResolver make_editor_asset_kind_resolver(const std::filesystem::path& project_root_or_file) {
    return [project_root_or_file](foundation::AssetId id) {
        return lookup_editor_asset_kind(scan_editor_asset_kinds(project_root_or_file), id);
    };
}

feature::AssetKindResolver snapshot_editor_asset_kind_resolver(const std::filesystem::path& project_root_or_file) {
    auto index = std::make_shared<const foundation::ResultValue<EditorAssetKindIndex>>(
        scan_editor_asset_kinds(project_root_or_file));
    return [index = std::move(index)](foundation::AssetId id) {
        return lookup_editor_asset_kind(*index, id);
    };
}

EditorFeatureGraphDocument::EditorFeatureGraphDocument(feature::FeatureGraphDocument document, feature::FeatureCatalog catalog)
    : catalog_(std::make_shared<const feature::FeatureCatalog>(std::move(catalog))), current_(std::make_shared<const feature::FeatureGraphDocument>(std::move(document))) {
    saved_ = current_;
    identity_ = current_;
}

foundation::Result EditorFeatureGraphDocument::execute(
    std::string label, std::function<foundation::Result(feature::FeatureGraphDocument&)> mutation) {
    return execute_transaction(std::move(label), catalog_, std::move(mutation));
}
foundation::Result EditorFeatureGraphDocument::apply_contract_transaction(std::string label, feature::FeatureCatalog catalog,
    std::function<foundation::Result(feature::FeatureGraphDocument&)> mutation) {
    try {
        return execute_transaction(std::move(label), std::make_shared<const feature::FeatureCatalog>(std::move(catalog)), std::move(mutation));
    } catch (const std::bad_alloc&) { return foundation::out_of_memory("Feature catalog transaction allocation failed"); }
}
foundation::Result EditorFeatureGraphDocument::execute_transaction(std::string label,
    std::shared_ptr<const feature::FeatureCatalog> catalog,
    std::function<foundation::Result(feature::FeatureGraphDocument&)> mutation) {
    if (label.empty() || !mutation)
        return foundation::invalid_argument("FeatureGraph authoring transaction is incomplete");
    try {
        auto next = std::make_shared<feature::FeatureGraphDocument>(*current_);
        if (const foundation::Result result = mutation(*next); !result)
            return result;
        auto next_const = std::shared_ptr<const feature::FeatureGraphDocument>(std::move(next));
        std::vector<HistoryEntry> candidate = history_;
        candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(cursor_), candidate.end());
        candidate.push_back({std::move(label), current_, next_const, catalog_, catalog});
        history_.swap(candidate);
        ++cursor_;
        current_ = std::move(next_const);
        catalog_ = std::move(catalog);
        ++revision_;
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("FeatureGraph authoring transaction allocation failed");
    } catch (const std::exception& exception) {
        return foundation::unknown_exception(exception);
    }
}

foundation::Result EditorFeatureGraphDocument::move_history(bool forward) {
    if (forward) {
        if (!can_redo())
            return foundation::invalid_state("FeatureGraph has no redo state");
        current_ = history_[cursor_].after;
        catalog_ = history_[cursor_].after_catalog;
        ++cursor_;
    } else {
        if (!can_undo())
            return foundation::invalid_state("FeatureGraph has no undo state");
        --cursor_;
        current_ = history_[cursor_].before;
        catalog_ = history_[cursor_].before_catalog;
    }
    ++revision_;
    return foundation::Result::success();
}

foundation::Result EditorFeatureGraphDocument::undo() { return move_history(false); }
foundation::Result EditorFeatureGraphDocument::redo() { return move_history(true); }

foundation::Result EditorFeatureGraphDocument::set_process_layout(feature::FeatureNodeId owner_node,
                                                                   const ProcessEditorLayout& layout) {
    if (const auto owner = validate_process_owner(owner_node); !owner) return owner;
    const auto previous = read_process_editor_layout(*current_, owner_node);
    if (!previous) return previous.result;
    if (previous.value == layout) return foundation::Result::success();
    return execute("Set Process layout", [owner_node, layout](auto& next) {
        if (const auto result = write_process_editor_layout(next, owner_node, layout); !result) return result;
        return advance_layout_revision(next);
    });
}

foundation::Result EditorFeatureGraphDocument::set_feature_layout(const FeatureEditorLayout& layout) {
    const auto previous = read_feature_editor_layout(*current_);
    if (!previous) return previous.result;
    if (previous.value == layout) return foundation::Result::success();
    return execute("Set Feature layout", [layout](auto& next) {
        if (const auto result = write_feature_editor_layout(next, layout); !result) return result;
        return advance_layout_revision(next);
    });
}

void EditorFeatureGraphDocument::bind_source_file(const std::filesystem::path& file,
                                                authoring::FeatureGraphFileRevision revision) {
    source_file_ = std::filesystem::absolute(file).lexically_normal();
    source_revision_ = std::move(revision);
}

foundation::Result EditorFeatureGraphDocument::validate_process_owner(feature::FeatureNodeId owner_node) const {
    const auto node = std::find_if(current_->nodes.begin(), current_->nodes.end(), [owner_node](const auto& value) {
        return value.node_id == owner_node;
    });
    if (node == current_->nodes.end())
        return foundation::not_found("FeatureGraph Process owner node was not found");
    if (catalog_->entries().empty())
        return foundation::Result::success();
    const auto entry = std::find_if(catalog_->entries().begin(), catalog_->entries().end(),
                                    [node](const auto& value) {
                                        return value.definition_id == node->definition_id &&
                                               value.version == node->definition_version;
                                    });
    if (entry == catalog_->entries().end())
        return foundation::invalid_argument(
            "FeatureGraph Process owner requires a version-matched Feature catalog entry");
    if (entry->execution != feature::ExecutionKind::kProcess &&
        entry->contract.state != feature::StatePolicy::kProcessState)
        return foundation::invalid_argument(
            "FeatureGraph Process owner must be declared as a Process Feature");
    return foundation::Result::success();
}

foundation::Result EditorFeatureGraphDocument::set_parameters(feature::FeatureNodeId node_id,
    std::span<const feature::FeatureParameterRecord> parameters, const feature::FeatureCatalog& catalog,
    const feature::AssetKindResolver& resolver) {
    const auto node = std::find_if(current_->nodes.begin(), current_->nodes.end(), [node_id](const auto& value) {
        return value.node_id == node_id;
    });
    if (node == current_->nodes.end()) return foundation::not_found("FeatureGraph node was not found");
    const auto entry = std::find_if(catalog.entries().begin(), catalog.entries().end(), [&](const auto& value) {
        return value.definition_id == node->definition_id;
    });
    if (entry == catalog.entries().end() || entry->version != node->definition_version ||
        !entry->contract.definition_id.is_valid())
        return foundation::invalid_argument("FeatureGraph parameter edit requires the node's complete version-matched contract");
    const auto encoded = feature::encode_feature_parameters(parameters, entry->contract.parameters);
    if (!encoded) return encoded.result;
    if (const auto valid = feature::validate_feature_parameter_assets(encoded.value, entry->contract.parameters, resolver); !valid)
        return valid;
    return execute("Set Feature parameters", [node_id, blob = encoded.value,
                                               digest = catalog.contract_digest()](auto& next) {
        const auto target = std::find_if(next.nodes.begin(), next.nodes.end(), [node_id](const auto& value) {
            return value.node_id == node_id;
        });
        if (target == next.nodes.end()) return foundation::not_found("FeatureGraph node was not found");
        target->parameter_blob = blob;
        next.contract_digest = digest;
        ++next.graph_version;
        ++next.draft_revision;
        return foundation::Result::success();
    });
}

foundation::Result EditorFeatureGraphDocument::validate_parameter_assets(const feature::AssetKindResolver& resolver) const {
    std::unordered_set<feature::FeatureDefinitionId, feature::StableIdHash<feature::FeatureDefinitionIdTag>> child_defaults;
    const auto validate_entry = [&](const auto& self, const feature::FeatureCatalogEntry& entry,
                                    std::span<const uint8_t> blob) -> foundation::Result {
        auto draft_schema = entry.contract.parameters;
        for (auto& parameter : draft_schema) parameter.required = false;
        if (const auto valid = feature::validate_feature_parameter_assets(blob, draft_schema, resolver); !valid)
            return valid;
        if (entry.contract.structure != feature::FeatureStructure::kComposite)
            return foundation::Result::success();
        for (const auto child : entry.contract.children) {
            if (!child_defaults.insert(child).second) continue;
            const auto child_entry = std::find_if(catalog_->entries().begin(), catalog_->entries().end(), [&](const auto& candidate) {
                return candidate.definition_id == child && candidate.contract.definition_id.is_valid();
            });
            // A partial authoring catalog can await a child's compiled contract.
            if (child_entry == catalog_->entries().end()) continue;
            if (const auto valid = self(self, *child_entry, {}); !valid) return valid;
        }
        return foundation::Result::success();
    };
    for (const auto& node : current_->nodes) {
        const auto entry = std::find_if(catalog_->entries().begin(), catalog_->entries().end(), [&](const auto& candidate) {
            return candidate.definition_id == node.definition_id && candidate.version == node.definition_version &&
                   candidate.contract.definition_id.is_valid();
        });
        // Unknown or partial authoring contracts retain their opaque draft payloads.
        if (entry == catalog_->entries().end()) continue;
        if (const auto valid = validate_entry(validate_entry, *entry, node.parameter_blob); !valid)
            return valid;
    }
    return foundation::Result::success();
}

foundation::Result EditorFeatureGraphDocument::save(const std::filesystem::path& path,
                                                     const feature::AssetKindResolver& resolver) {
    try {
        if (const auto valid = validate_parameter_assets(resolver); !valid) return valid;
        auto candidate = *this;
        candidate.set_contract_digest(candidate.catalog().contract_digest());
        const auto absolute_path = std::filesystem::absolute(path).lexically_normal();
        const auto* expected = !source_file_.empty() && source_file_ == absolute_path ? &source_revision_ : nullptr;
        candidate.source_file_ = absolute_path; // Finish allocations before publishing bytes.
        auto written = authoring::save_feature_graph_source(path, candidate.snapshot(), expected);
        if (!written) return written.result;
        candidate.source_revision_ = std::move(written.value);
        candidate.mark_saved();
        static_assert(std::is_nothrow_move_assignable_v<EditorFeatureGraphDocument>);
        *this = std::move(candidate);
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("FeatureGraph save allocation failed");
    } catch (const std::exception& exception) {
        return foundation::unknown_exception(exception);
    }
}

foundation::Result EditorFeatureGraphDocument::erase_node(feature::FeatureNodeId node_id) {
    return erase_nodes(std::span<const feature::FeatureNodeId>{&node_id, 1});
}

foundation::Result EditorFeatureGraphDocument::erase_nodes(std::span<const feature::FeatureNodeId> node_ids) try {
    if (node_ids.empty()) return foundation::invalid_argument("FeatureGraph node selection is empty");
    for (const auto id : node_ids) {
        if (!id.is_valid()) return foundation::invalid_argument("FeatureGraph node identity is invalid");
        if (std::none_of(current_->nodes.begin(), current_->nodes.end(), [id](const auto& node) { return node.node_id == id; }))
            return foundation::not_found("FeatureGraph node was not found");
    }
    std::vector<feature::FeatureNodeId> selection(node_ids.begin(), node_ids.end());
    return execute("Delete Feature nodes", [selection = std::move(selection)](feature::FeatureGraphDocument& next) {
        std::unordered_set<feature::FeatureNodeId, feature::StableIdHash<feature::FeatureNodeIdTag>> removed;
        removed.insert(selection.begin(), selection.end());
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& node : next.nodes) {
                if (removed.contains(node.node_id))
                    continue;
                const bool owned_by_removed = std::any_of(
                    node.owner_path.nodes.begin(), node.owner_path.nodes.end(),
                    [&removed](feature::FeatureNodeId owner) { return removed.contains(owner); });
                if (owned_by_removed) {
                    removed.insert(node.node_id);
                    changed = true;
                }
            }
        }
        const auto extension_count = next.extensions.size();
        for (const auto owner : removed) {
            if (const auto result = erase_process_editor_layout(next, owner); !result) return result;
        }
        bool layout_changed = next.extensions.size() != extension_count;
        const auto layout = read_feature_editor_layout(next);
        if (!layout) return layout.result;
        auto cleaned = layout.value;
        for (const auto id : removed) cleaned.node_positions.erase(id.value());
        if (cleaned != layout.value) {
            if (const auto result = write_feature_editor_layout(next, cleaned); !result) return result;
            layout_changed = true;
        }
        if (layout_changed)
            if (const auto result = advance_layout_revision(next); !result) return result;
        next.nodes.erase(std::remove_if(next.nodes.begin(), next.nodes.end(), [&removed](const auto& node) {
                             return removed.contains(node.node_id);
                         }),
                         next.nodes.end());
        next.edges.erase(std::remove_if(next.edges.begin(), next.edges.end(), [&removed](const auto& edge) {
                             return removed.contains(edge.source_node) || removed.contains(edge.target_node);
                         }),
                         next.edges.end());
        next.process_subgraphs.erase(
            std::remove_if(next.process_subgraphs.begin(), next.process_subgraphs.end(),
                           [&removed](const auto& process) { return removed.contains(process.owner_node); }),
            next.process_subgraphs.end());
        ++next.graph_version;
        ++next.draft_revision;
        return foundation::Result::success();
    });
} catch (const std::bad_alloc&) {
    return foundation::out_of_memory("Feature node deletion allocation failed");
} catch (const std::exception& exception) {
    return foundation::unknown_exception(exception);
}

foundation::Result EditorFeatureGraphDocument::clone_nodes(std::span<const feature::FeatureNodeId> node_ids) try {
    if (node_ids.empty()) return foundation::invalid_argument("FeatureGraph node selection is empty");
    std::vector<feature::FeatureNodeId> selection(node_ids.begin(), node_ids.end());
    return execute("Copy Feature nodes", [selection = std::move(selection)](feature::FeatureGraphDocument& next) {
        std::unordered_set<uint64_t> selected, used;
        for (const auto& node : next.nodes) used.insert(node.node_id.value());
        for (const auto id : selection) {
            if (!id.is_valid() || !used.contains(id.value()))
                return foundation::invalid_argument("FeatureGraph copy requires declared selected nodes");
            selected.insert(id.value());
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& node : next.nodes) {
                if (selected.contains(node.node_id.value())) continue;
                if (std::any_of(node.owner_path.nodes.begin(), node.owner_path.nodes.end(), [&](auto id) {
                    return selected.contains(id.value());
                })) { selected.insert(node.node_id.value()); changed = true; }
            }
        }
        const auto previous_layout = read_feature_editor_layout(next);
        if (!previous_layout) return previous_layout.result;
        auto layout = previous_layout.value;
        std::map<uint64_t, feature::FeatureNodeId> remapped;
        const auto original_nodes = next.nodes;
        const auto original_edges = next.edges;
        const auto original_processes = next.process_subgraphs;
        std::map<uint64_t, ProcessEditorLayout> process_layouts;
        for (const auto& process : original_processes) {
            if (!selected.contains(process.owner_node.value())) continue;
            const auto value = read_process_editor_layout(next, process.owner_node);
            if (!value) return value.result;
            process_layouts.emplace(process.owner_node.value(), value.value);
        }
        for (const auto& node : original_nodes) {
            if (!selected.contains(node.node_id.value())) continue;
            uint64_t ordinal = 1;
            for (;;) {
                const feature::FeatureNodeId id{"Editor.Copy." + std::to_string(node.node_id.value()) + "." + std::to_string(ordinal)};
                if (id.is_valid() && used.insert(id.value()).second) { remapped.emplace(node.node_id.value(), id); break; }
                if (ordinal == std::numeric_limits<uint64_t>::max()) return foundation::invalid_state("Feature node identity space exhausted");
                ++ordinal;
            }
        }
        for (std::size_t index = 0; index < original_nodes.size(); ++index) {
            auto node = original_nodes[index];
            const auto replacement = remapped.find(node.node_id.value());
            if (replacement == remapped.end()) continue;
            const auto position = layout.node_positions.contains(node.node_id.value())
                ? layout.node_positions.at(node.node_id.value()) : default_feature_node_position(index);
            node.node_id = replacement->second;
            for (auto& owner : node.owner_path.nodes)
                if (const auto mapped = remapped.find(owner.value()); mapped != remapped.end()) owner = mapped->second;
            layout.node_positions[node.node_id.value()] = {position.first + 40.0f, position.second + 40.0f};
            next.nodes.push_back(std::move(node));
        }
        for (auto item : original_edges) {
            const auto source = remapped.find(item.source_node.value()), target = remapped.find(item.target_node.value());
            if (source == remapped.end() || target == remapped.end()) continue;
            item.source_node = source->second; item.target_node = target->second;
            next.edges.push_back(std::move(item));
        }
        for (auto process : original_processes) {
            const auto mapped = remapped.find(process.owner_node.value());
            if (mapped == remapped.end()) continue;
            process.owner_node = mapped->second; next.process_subgraphs.push_back(std::move(process));
        }
        for (const auto& [old_id, value] : process_layouts)
            if (const auto result = write_process_editor_layout(next, remapped.at(old_id), value); !result) return result;
        if (const auto result = write_feature_editor_layout(next, layout); !result) return result;
        if (const auto result = advance_layout_revision(next); !result) return result;
        ++next.graph_version; ++next.draft_revision;
        return foundation::Result::success();
    });
} catch (const std::bad_alloc&) {
    return foundation::out_of_memory("Feature node copy allocation failed");
} catch (const std::exception& exception) {
    return foundation::unknown_exception(exception);
}

foundation::Result EditorFeatureGraphDocument::erase_edge(feature::FeatureNodeId source_node,
                                                           feature::PortId source_port,
                                                           feature::FeatureNodeId target_node,
                                                           feature::PortId target_port) {
    if (!source_node.is_valid() || !target_node.is_valid() || !source_port.is_valid() || !target_port.is_valid())
        return foundation::invalid_argument("FeatureGraph edge identity is invalid");
    if (std::find_if(current_->edges.begin(), current_->edges.end(), [&](const auto& edge) {
            return edge.source_node == source_node && edge.source_port == source_port &&
                   edge.target_node == target_node && edge.target_port == target_port;
        }) == current_->edges.end())
        return foundation::not_found("FeatureGraph edge was not found");
    return execute("Delete Feature edge", [source_node, source_port, target_node, target_port](auto& next) {
        next.edges.erase(std::remove_if(next.edges.begin(), next.edges.end(), [&](const auto& edge) {
                             return edge.source_node == source_node && edge.source_port == source_port &&
                                    edge.target_node == target_node && edge.target_port == target_port;
                         }),
                         next.edges.end());
        ++next.graph_version;
        ++next.draft_revision;
        return foundation::Result::success();
    });
}

foundation::Result EditorFeatureGraphDocument::add_node(feature::FeatureNode node,
                                                        uint64_t contract_digest) {
    if (!node.node_id.is_valid() || !node.definition_id.is_valid() || node.definition_version.major == 0)
        return foundation::invalid_argument("FeatureGraph node identity or version is invalid");
    if (std::any_of(current_->nodes.begin(), current_->nodes.end(),
                    [&node](const auto& value) { return value.node_id == node.node_id; }))
        return foundation::invalid_state("FeatureGraph node already exists");
    return execute("Add Feature node", [node = std::move(node), contract_digest](auto& next) {
        next.nodes.push_back(node);
        if (contract_digest != 0)
            next.contract_digest = contract_digest;
        ++next.graph_version;
        ++next.draft_revision;
        return foundation::Result::success();
    });
}

foundation::Result EditorFeatureGraphDocument::add_edge(feature::FeatureEdge edge,
                                                         const feature::FeatureCatalog& catalog) {
    const auto source_node = std::find_if(current_->nodes.begin(), current_->nodes.end(),
                                          [&edge](const auto& node) { return node.node_id == edge.source_node; });
    if (source_node != current_->nodes.end()) {
        const auto source_entry = std::find_if(catalog.entries().begin(), catalog.entries().end(),
                                               [&source_node](const auto& entry) {
                                                   return entry.definition_id == source_node->definition_id;
                                               });
        if (source_entry != catalog.entries().end()) {
            const auto source_port = std::find_if(source_entry->ports.begin(), source_entry->ports.end(),
                                                  [&edge](const auto& port) {
                                                      return port.id == edge.source_port &&
                                                             port.direction == feature::PortDirection::kOutput;
                                                  });
            if (source_port != source_entry->ports.end())
                edge.version = source_port->version;
        }
    }
    if (const auto validation = feature::validate_feature_edge(*current_, edge, catalog); !validation)
        return validation;
    return execute("Add Feature edge", [edge](auto& next) {
        next.edges.push_back(edge);
        ++next.graph_version;
        ++next.draft_revision;
        return foundation::Result::success();
    });
}

foundation::Result EditorFeatureGraphDocument::add_process_state(feature::FeatureNodeId owner_node,
                                                                  std::string state,
                                                                  uint32_t state_schema_version) {
    if (!owner_node.is_valid() || state.empty() || state.find("->") != std::string::npos ||
        state_schema_version == 0)
        return foundation::invalid_argument("FeatureGraph Process state is invalid");
    if (const auto owner = validate_process_owner(owner_node); !owner)
        return owner;
    return execute("Add Process state", [owner_node, state = std::move(state), state_schema_version](auto& next) {
        auto process = std::find_if(next.process_subgraphs.begin(), next.process_subgraphs.end(),
                                    [owner_node](const auto& value) { return value.owner_node == owner_node; });
        if (process == next.process_subgraphs.end()) {
            // With no subgraph there can be no valid owner layout to remove;
            // this validates the reserved metadata before creating a schema.
            if (const auto result = erase_process_editor_layout(next, owner_node); !result) return result;
            next.process_subgraphs.push_back({owner_node, {}, {}, state_schema_version});
            process = std::prev(next.process_subgraphs.end());
        } else {
            if (const auto layout = read_process_editor_layout(next, owner_node); !layout) return layout.result;
        }
        if (process->state_schema_version != state_schema_version)
            return foundation::invalid_state("FeatureGraph Process state schema version does not match");
        if (std::find(process->states.begin(), process->states.end(), state) != process->states.end())
            return foundation::invalid_state("FeatureGraph Process state already exists");
        process->state_schema_version = state_schema_version;
        process->states.push_back(state);
        if (process->initial_state.empty()) process->initial_state = state;
        ++next.graph_version;
        ++next.draft_revision;
        return foundation::Result::success();
    });
}

foundation::Result EditorFeatureGraphDocument::add_process_transition(feature::FeatureNodeId owner_node,
                                                                       std::string from_state,
                                                                       std::string to_state) {
    if (!owner_node.is_valid() || from_state.empty() || to_state.empty() || from_state.find("->") != std::string::npos ||
        to_state.find("->") != std::string::npos)
        return foundation::invalid_argument("FeatureGraph Process transition is invalid");
    if (const auto owner = validate_process_owner(owner_node); !owner)
        return owner;
    return execute("Add Process transition", [owner_node, from_state = std::move(from_state),
                                                to_state = std::move(to_state)](auto& next) {
        const auto process = std::find_if(next.process_subgraphs.begin(), next.process_subgraphs.end(),
                                          [owner_node](const auto& value) { return value.owner_node == owner_node; });
        if (process == next.process_subgraphs.end())
            return foundation::not_found("FeatureGraph Process subgraph was not found");
        if (std::find(process->states.begin(), process->states.end(), from_state) == process->states.end() ||
            std::find(process->states.begin(), process->states.end(), to_state) == process->states.end())
            return foundation::not_found("FeatureGraph Process transition state was not found");
        const std::string transition = from_state + "->" + to_state;
        if (std::find(process->transitions.begin(), process->transitions.end(), transition) !=
            process->transitions.end())
            return foundation::invalid_state("FeatureGraph Process transition already exists");
        const auto previous_layout = read_process_editor_layout(next, owner_node);
        if (!previous_layout) return previous_layout.result;
        auto layout = previous_layout.value;
        const auto previous_extensions = next.extensions;
        if (const auto result = erase_process_editor_layout(next, owner_node); !result) return result;
        if (!layout.transition_control_points.empty()) layout.transition_control_points.emplace_back();
        process->transitions.push_back(transition);
        if (const auto result = restore_process_layout(next, owner_node, layout, previous_layout.value,
                                                       previous_extensions); !result)
            return result;
        ++next.graph_version;
        ++next.draft_revision;
        return foundation::Result::success();
    });
}

foundation::Result EditorFeatureGraphDocument::set_process_initial_state(feature::FeatureNodeId owner_node,
                                                                          std::string state) {
    if (!owner_node.is_valid() || state.empty())
        return foundation::invalid_argument("FeatureGraph Process initial state is invalid");
    if (const auto owner = validate_process_owner(owner_node); !owner)
        return owner;
    const auto process = std::find_if(current_->process_subgraphs.begin(), current_->process_subgraphs.end(),
                                      [owner_node](const auto& value) { return value.owner_node == owner_node; });
    if (process == current_->process_subgraphs.end())
        return foundation::not_found("FeatureGraph Process subgraph was not found");
    if (std::find(process->states.begin(), process->states.end(), state) == process->states.end())
        return foundation::not_found("FeatureGraph Process initial state was not found");
    if (const auto layout = read_process_editor_layout(*current_, owner_node); !layout) return layout.result;
    if (process->initial_state == state)
        return foundation::Result::success();
    return execute("Set Process initial state", [owner_node, state = std::move(state)](auto& next) {
        const auto target = std::find_if(next.process_subgraphs.begin(), next.process_subgraphs.end(),
                                         [owner_node](const auto& value) { return value.owner_node == owner_node; });
        if (target == next.process_subgraphs.end())
            return foundation::not_found("FeatureGraph Process subgraph was not found");
        target->initial_state = state;
        ++next.graph_version;
        ++next.draft_revision;
        return foundation::Result::success();
    });
}

foundation::Result EditorFeatureGraphDocument::set_process_terminal_state(feature::FeatureNodeId owner_node,
                                                                           std::string state, bool terminal) {
    if (!owner_node.is_valid() || state.empty())
        return foundation::invalid_argument("FeatureGraph Process terminal state is invalid");
    if (const auto owner = validate_process_owner(owner_node); !owner)
        return owner;
    const auto process = std::find_if(current_->process_subgraphs.begin(), current_->process_subgraphs.end(),
                                      [owner_node](const auto& value) { return value.owner_node == owner_node; });
    if (process == current_->process_subgraphs.end())
        return foundation::not_found("FeatureGraph Process subgraph was not found");
    if (std::find(process->states.begin(), process->states.end(), state) == process->states.end())
        return foundation::not_found("FeatureGraph Process terminal state was not found");
    if (const auto layout = read_process_editor_layout(*current_, owner_node); !layout) return layout.result;
    const bool already_terminal = std::find(process->terminal_states.begin(), process->terminal_states.end(), state) !=
                                  process->terminal_states.end();
    if (already_terminal == terminal)
        return foundation::Result::success();
    return execute("Set Process terminal state", [owner_node, state = std::move(state), terminal](auto& next) {
        const auto target = std::find_if(next.process_subgraphs.begin(), next.process_subgraphs.end(),
                                         [owner_node](const auto& value) { return value.owner_node == owner_node; });
        if (target == next.process_subgraphs.end())
            return foundation::not_found("FeatureGraph Process subgraph was not found");
        const auto existing = std::find(target->terminal_states.begin(), target->terminal_states.end(), state);
        if (terminal) {
            if (existing == target->terminal_states.end())
                target->terminal_states.push_back(state);
        } else if (existing != target->terminal_states.end()) {
            target->terminal_states.erase(existing);
        }
        ++next.graph_version;
        ++next.draft_revision;
        return foundation::Result::success();
    });
}

foundation::Result EditorFeatureGraphDocument::erase_process_state(feature::FeatureNodeId owner_node,
                                                                    std::string state) {
    if (!owner_node.is_valid() || state.empty())
        return foundation::invalid_argument("FeatureGraph Process state identity is invalid");
    return execute("Delete Process state", [owner_node, state = std::move(state)](auto& next) {
        const auto process = std::find_if(next.process_subgraphs.begin(), next.process_subgraphs.end(),
                                          [owner_node](const auto& value) { return value.owner_node == owner_node; });
        if (process == next.process_subgraphs.end())
            return foundation::not_found("FeatureGraph Process subgraph was not found");
        if (std::find(process->states.begin(), process->states.end(), state) == process->states.end())
            return foundation::not_found("FeatureGraph Process state was not found");
        const auto previous_layout = read_process_editor_layout(next, owner_node);
        if (!previous_layout) return previous_layout.result;
        auto layout = previous_layout.value;
        const auto previous_extensions = next.extensions;
        if (const auto result = erase_process_editor_layout(next, owner_node); !result) return result;
        layout.state_positions.erase(state);
        layout.state_colors.erase(state);
        process->states.erase(std::remove(process->states.begin(), process->states.end(), state),
                              process->states.end());
        if (process->initial_state == state)
            process->initial_state = process->states.empty() ? std::string{} : process->states.front();
        process->terminal_states.erase(
            std::remove(process->terminal_states.begin(), process->terminal_states.end(), state),
            process->terminal_states.end());
        const std::string prefix = state + "->";
        const std::string suffix = "->" + state;
        if (!layout.transition_control_points.empty()) {
            std::vector<std::vector<std::pair<float, float>>> retained;
            for (std::size_t index = 0; index < process->transitions.size(); ++index) {
                const auto& transition = process->transitions[index];
                if (!transition.starts_with(prefix) && !transition.ends_with(suffix))
                    retained.push_back(std::move(layout.transition_control_points[index]));
            }
            layout.transition_control_points = std::move(retained);
        }
        process->transitions.erase(std::remove_if(process->transitions.begin(), process->transitions.end(),
                                                  [&](const auto& transition) {
                                                      return transition.starts_with(prefix) ||
                                                             transition.ends_with(suffix);
                                                  }),
                                  process->transitions.end());
        if (process->states.empty()) {
            next.process_subgraphs.erase(process);
            if (previous_extensions != next.extensions) {
                if (const auto result = advance_layout_revision(next); !result) return result;
            }
        } else {
            if (const auto result = restore_process_layout(next, owner_node, layout, previous_layout.value,
                                                           previous_extensions); !result)
                return result;
        }
        ++next.graph_version;
        ++next.draft_revision;
        return foundation::Result::success();
    });
}

foundation::Result EditorFeatureGraphDocument::erase_process_transition(feature::FeatureNodeId owner_node,
                                                                         std::string from_state,
                                                                         std::string to_state) {
    if (!owner_node.is_valid() || from_state.empty() || to_state.empty())
        return foundation::invalid_argument("FeatureGraph Process transition identity is invalid");
    return execute("Delete Process transition", [owner_node, transition = from_state + "->" + to_state](auto& next) {
        const auto process = std::find_if(next.process_subgraphs.begin(), next.process_subgraphs.end(),
                                          [owner_node](const auto& value) { return value.owner_node == owner_node; });
        if (process == next.process_subgraphs.end())
            return foundation::not_found("FeatureGraph Process subgraph was not found");
        const auto it = std::find(process->transitions.begin(), process->transitions.end(), transition);
        if (it == process->transitions.end())
            return foundation::not_found("FeatureGraph Process transition was not found");
        const auto previous_layout = read_process_editor_layout(next, owner_node);
        if (!previous_layout) return previous_layout.result;
        auto layout = previous_layout.value;
        const auto previous_extensions = next.extensions;
        if (const auto result = erase_process_editor_layout(next, owner_node); !result) return result;
        if (!layout.transition_control_points.empty()) {
            const auto index = std::distance(process->transitions.begin(), it);
            layout.transition_control_points.erase(layout.transition_control_points.begin() + index);
        }
        process->transitions.erase(it);
        if (const auto result = restore_process_layout(next, owner_node, layout, previous_layout.value,
                                                       previous_extensions); !result)
            return result;
        ++next.graph_version;
        ++next.draft_revision;
        return foundation::Result::success();
    });
}

void EditorFeatureGraphDocument::set_contract_digest(uint64_t contract_digest) {
    if (current_->contract_digest == contract_digest)
        return;
    auto next = std::make_shared<feature::FeatureGraphDocument>(*current_);
    next->contract_digest = contract_digest;
    current_ = std::shared_ptr<const feature::FeatureGraphDocument>(std::move(next));
    ++revision_;
}

}  // namespace aeg::application
