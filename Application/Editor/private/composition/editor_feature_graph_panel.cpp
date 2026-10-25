#include "editor_feature_graph_panel.hpp"
#include "editor_feature_graph_model.hpp"
#include "editor_render_graph_inspection.hpp"
#include "editor_parameter_widget.hpp"
#include "editor_project_feature_authoring.hpp"
#include "editor_feature_presentation.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <string_view>

namespace aeg::application {
namespace {

const char* domain_name(feature::FeatureDomain domain) noexcept {
    switch (domain) {
    case feature::FeatureDomain::kLogic: return "Logic";
    case feature::FeatureDomain::kPhysics: return "Physics";
    case feature::FeatureDomain::kRender: return "Render";
    case feature::FeatureDomain::kAudio: return "Audio";
    case feature::FeatureDomain::kStream: return "Stream";
    }
    return "Unknown";
}

std::string domain_mask_name(feature::FeatureDomainMask mask) {
    std::string result;
    for (unsigned index = 0; index < 5; ++index) {
        if ((mask & feature::to_mask(static_cast<feature::FeatureDomain>(index))) == 0)
            continue;
        if (!result.empty())
            result += ", ";
        result += domain_name(static_cast<feature::FeatureDomain>(index));
    }
    return result.empty() ? "-" : result;
}

const char* edge_name(feature::EdgeKind kind) noexcept {
    switch (kind) {
    case feature::EdgeKind::kData: return "Data";
    case feature::EdgeKind::kSnapshot: return "Snapshot";
    case feature::EdgeKind::kQueue: return "Queue";
    case feature::EdgeKind::kEvent: return "Event";
    case feature::EdgeKind::kCommand: return "Command";
    case feature::EdgeKind::kReceipt: return "Receipt";
    }
    return "Unknown";
}

const char* execution_name(feature::ExecutionKind kind) noexcept {
    switch (kind) {
    case feature::ExecutionKind::kCall: return "Call";
    case feature::ExecutionKind::kProcess: return "Process";
    case feature::ExecutionKind::kContinuous: return "Continuous";
    }
    return "Unknown";
}

const char* state_name(feature::StatePolicy state) noexcept {
    switch (state) {
    case feature::StatePolicy::kStateless: return "Stateless";
    case feature::StatePolicy::kInstanceState: return "Instance state";
    case feature::StatePolicy::kProcessState: return "Process state";
    case feature::StatePolicy::kExternalSnapshot: return "External snapshot";
    }
    return "Unknown";
}

std::string owner_path_text(const feature::OwnerPath& path) {
    std::string text;
    for (const auto id : path.nodes) {
        if (!text.empty()) text += '/';
        text += std::to_string(id.value());
    }
    return text.empty() ? "(root)" : text;
}

bool catalog_matches(const feature::FeatureCatalogEntry& entry, const char* filter, int domain_filter,
                     int execution_filter, std::optional<feature::ClockId> clock_filter,
                     int structure_filter, std::optional<feature::ScopeId> scope_filter) {
    if (filter != nullptr && *filter != '\0') {
        const std::string id = std::to_string(entry.definition_id.value());
        if (id.find(filter) == std::string::npos)
            return false;
    }
    FeaturePaletteFilter palette_filter;
    if (domain_filter > 0) palette_filter.domain = static_cast<feature::FeatureDomain>(domain_filter - 1);
    if (execution_filter > 0) palette_filter.execution = static_cast<feature::ExecutionKind>(execution_filter - 1);
    palette_filter.clock = clock_filter;
    if (structure_filter > 0) palette_filter.structure = static_cast<feature::FeatureStructure>(structure_filter - 1);
    palette_filter.scope = scope_filter;
    return feature_palette_matches({entry.definition_id, entry.domains, entry.execution, entry.clock,
                                    entry.contract.structure, entry.contract.scope.id}, palette_filter);
}

feature::FeatureNodeId next_editor_node_id(const feature::FeatureGraphDocument& graph,
                                           const feature::FeatureCatalogEntry& entry) {
    for (uint64_t ordinal = 1;; ++ordinal) {
        const feature::FeatureNodeId candidate(
            "Editor.Feature." + std::to_string(entry.definition_id.value()) + "." + std::to_string(ordinal));
        const auto exists = std::find_if(graph.nodes.begin(), graph.nodes.end(), [candidate](const auto& node) {
            return node.node_id == candidate;
        });
        if (exists == graph.nodes.end())
            return candidate;
    }
}

template <typename Id>
bool parse_numeric_id(std::string_view text, Id& result) {
    uint64_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0)
        return false;
    result = Id(value);
    return true;
}

const feature::FeatureNode* find_node(const feature::FeatureGraphDocument& graph, feature::FeatureNodeId id) {
    const auto it = std::find_if(graph.nodes.begin(), graph.nodes.end(), [id](const auto& node) {
        return node.node_id == id;
    });
    return it == graph.nodes.end() ? nullptr : &*it;
}

const feature::FeatureCatalogEntry* find_entry(const feature::FeatureCatalog& catalog,
                                               feature::FeatureDefinitionId id) {
    const auto it = std::find_if(catalog.entries().begin(), catalog.entries().end(), [id](const auto& entry) {
        return entry.definition_id == id;
    });
    return it == catalog.entries().end() ? nullptr : &*it;
}

const feature::FeatureCatalogEntry* find_entry(const feature::FeatureCatalog& catalog,
                                               feature::FeatureDefinitionId id,
                                               const feature::Version& version) {
    const auto it = std::find_if(catalog.entries().begin(), catalog.entries().end(),
                                 [id, &version](const auto& entry) {
                                     return entry.definition_id == id && entry.version == version;
                                 });
    return it == catalog.entries().end() ? nullptr : &*it;
}

std::string edge_selection_key(const feature::FeatureEdge& edge) {
    return std::to_string(edge.source_node.value()) + ":" + std::to_string(edge.source_port.value()) +
           ">" + std::to_string(edge.target_node.value()) + ":" + std::to_string(edge.target_port.value());
}

const char* late_data_name(feature::LateDataPolicy policy) noexcept {
    switch (policy) {
    case feature::LateDataPolicy::kReject: return "Reject";
    case feature::LateDataPolicy::kUseLatest: return "UseLatest";
    case feature::LateDataPolicy::kQueue: return "Queue";
    case feature::LateDataPolicy::kDrop: return "Drop";
    }
    return "Unknown";
}

void draw_edge_endpoint_contract(const feature::FeatureGraphDocument& graph,
    const feature::FeatureCatalog* catalog, feature::FeatureNodeId node_id, feature::PortId port_id,
    feature::PortDirection direction, const char* endpoint) {
    if (catalog == nullptr) {
        ImGui::TextDisabled("%s contract unavailable: no catalog", endpoint);
        return;
    }
    const auto* node = find_node(graph, node_id);
    if (node == nullptr) {
        ImGui::TextDisabled("%s contract unavailable: node missing", endpoint);
        return;
    }
    const auto* entry = find_entry(*catalog, node->definition_id);
    if (entry == nullptr) {
        ImGui::TextDisabled("%s contract unavailable: definition missing", endpoint);
        return;
    }
    if (node->definition_version != entry->version) {
        ImGui::TextDisabled("%s contract unavailable: stale definition version", endpoint);
        return;
    }
    const auto port = std::find_if(entry->ports.begin(), entry->ports.end(), [port_id, direction](const auto& value) {
        return value.id == port_id && value.direction == direction;
    });
    if (port == entry->ports.end()) {
        ImGui::TextDisabled("%s contract unavailable: port missing or direction changed", endpoint);
        return;
    }
    ImGui::Text("%s schema: %llu", endpoint, static_cast<unsigned long long>(port->schema.value()));
    ImGui::Text("%s port version: %u", endpoint, port->version);
    if (entry->contract.definition_id != entry->definition_id || entry->contract.version != entry->version) {
        ImGui::TextDisabled("%s Clock contract unavailable: canonical metadata missing", endpoint);
        return;
    }
    const auto& clock = entry->contract.clock;
    ImGui::Text("%s Clock id: %llu", endpoint, static_cast<unsigned long long>(clock.id.value()));
    ImGui::Text("%s Clock frequency (Hz): %u", endpoint, clock.frequency_hz);
    ImGui::Text("%s Clock max backlog: %u", endpoint, clock.max_backlog);
    ImGui::Text("%s Clock late data: %s", endpoint, late_data_name(clock.late_data_policy));
    ImGui::Text("%s Clock capability version: %u.%u.%u", endpoint,
        clock.capability_version.major, clock.capability_version.minor, clock.capability_version.patch);
    ImGui::Text("%s Clock work budget (ns): %llu", endpoint,
        static_cast<unsigned long long>(clock.work_budget_ns));
    if (clock.timing) {
        ImGui::Text("%s Clock phase (ns): %llu", endpoint,
            static_cast<unsigned long long>(clock.timing->phase_ns));
        ImGui::Text("%s Clock relative deadline (ns): %llu", endpoint,
            static_cast<unsigned long long>(clock.timing->relative_deadline_ns));
        const char* policy = "Unspecified";
        if (clock.timing->deadline_policy == feature::DeadlinePolicy::kObserve) policy = "Observe";
        else if (clock.timing->deadline_policy == feature::DeadlinePolicy::kRejectCommit) policy = "Reject commit";
        ImGui::Text("%s Clock deadline policy: %s", endpoint, policy);
    } else ImGui::TextDisabled("%s Clock timing unavailable", endpoint);
}

void draw_saved_edge_details(const feature::FeatureGraphDocument& graph, const feature::FeatureEdge& edge,
                             const feature::FeatureCatalog* catalog) {
    ImGui::Text("Saved edge %s", edge_selection_key(edge).c_str());
    ImGui::Text("Kind: %s", edge_name(edge.kind));
    ImGui::Text("Version: %u", edge.version);
    ImGui::Text("Capacity: %u", edge.capacity);
    ImGui::Text("Deadline ticks: %u", edge.deadline_ticks);
    ImGui::TextDisabled("Deadline unit: source Clock periods");
    ImGui::Text("Source clock: %llu", static_cast<unsigned long long>(edge.source_clock.value()));
    ImGui::Text("Target clock: %llu", static_cast<unsigned long long>(edge.target_clock.value()));
    ImGui::TextDisabled("Current port and Clock contracts");
    draw_edge_endpoint_contract(graph, catalog, edge.source_node, edge.source_port,
                                feature::PortDirection::kOutput, "Source");
    draw_edge_endpoint_contract(graph, catalog, edge.target_node, edge.target_port,
                                feature::PortDirection::kInput, "Target");
}

}  // namespace

void EditorFeatureGraphPanel::draw(EditorFeatureGraphDocument& document,
                                   const std::filesystem::path& source_path, const Actions& requested_actions) {
    Actions actions = requested_actions;
    // A successful Save and Generate updates the editable catalog immediately.
    // Its labels must be available before the next host asset refresh too.
    std::vector<NodeDefinitionDraft> presentations;
    const auto overlay = definition_panel_.presentation_overlay(document,
        actions.project_file.empty() ? actions.project_root : actions.project_file);
    if (!overlay.empty()) {
        presentations.assign(overlay.begin(), overlay.end());
        presentations.insert(presentations.end(), actions.feature_presentations.begin(), actions.feature_presentations.end());
        actions.feature_presentations = presentations;
    }
    if (!document.catalog().entries().empty()) actions.feature_catalog = &document.catalog();
    if (actions.feature_catalog == nullptr) {
        catalog_scope_filter_.reset();
        actions.feature_catalog = &document.catalog();
    }
    if (preview_document_identity_ != document.identity_owner() || last_good_preview_path_ != source_path ||
        preview_asset_uuid_ != document.snapshot().asset_uuid) {
        last_good_preview_.reset(); last_good_preview_snapshot_.reset();
        current_projection_.reset(); projection_snapshot_.reset();
        preview_diagnostics_.clear(); preview_action_status_.clear(); last_action_.clear();
        preview_document_identity_ = document.identity_owner();
        last_good_preview_path_ = source_path; preview_asset_uuid_ = document.snapshot().asset_uuid;
        selected_node_.clear(); selected_edge_.clear(); process_owner_node_.clear();
        show_definitions_ = false;
    }
    const ImGuiWindowFlags flags = actions.native_window
                                       ? ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                             ImGuiWindowFlags_NoScrollWithMouse
                                       : ImGuiWindowFlags_None;
    if (actions.native_window)
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0f, 0.0f});
    ImGui::Begin("Flow Graph###FlowGraph", nullptr, flags);
    if (actions.report_focus)
        actions.report_focus(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows));
    if (actions.native_window) {
        ImGui::PopStyleVar();
        aeg::editor::draw_editor_window_chrome("FeatureGraph", source_path.filename().string().c_str(),
                                               actions.window_actions);
    }
    if (actions.draw_graph_selector) actions.draw_graph_selector();
    if (actions.native_window)
        ImGui::BeginChild("FeatureGraphContent", {0, 0});

    bool focus_definitions = false;
    ImGui::TextUnformatted("FeatureGraph - compose behavior with Feature nodes");
    ImGui::TextWrapped("1. Add nodes from Feature Catalog.  2. Drag an output port to an input port.  "
                       "3. Validate draft, then Compile preview.  4. Save; build and load your module before Apply.");
    if (ImGui::Button("Node catalog")) authoring_jump_ = 2;
    ImGui::SameLine(); if (ImGui::Button("Check draft")) authoring_jump_ = 3;
    ImGui::SameLine(); if (ImGui::Button("Process editor")) authoring_jump_ = 4;
    ImGui::SameLine(); if (ImGui::Button("Feature definitions")) {
        show_definitions_ = true;
        focus_definitions = true;
    }
    if (document.snapshot().nodes.empty())
        ImGui::TextWrapped("This graph has no nodes yet. Use Node catalog > Add node. "
                           "If the catalog is empty, open Feature Definitions > New Feature, then Save and Generate.");
    ImGui::SeparatorText("Run / publish - requires a built game module");
    if (!actions.apply_graph)
        ImGui::TextWrapped("No executable module loaded. You can edit and preview now. "
                           "Use Build > Build / Cook, load the resulting module manifest here, then Apply.");

    bool has_diagnostics = false;
    for (const auto& authored : actions.authored_feature_documents) {
        if (authored.load_result)
            continue;
        if (!has_diagnostics) {
            ImGui::TextUnformatted("FeatureGraph diagnostics");
            has_diagnostics = true;
        }
        ImGui::TextColored(ImVec4{1.0f, 0.45f, 0.30f, 1.0f}, "%s",
                           authored.path.generic_string().c_str());
        ImGui::TextWrapped("%s", authored.load_result.message.c_str());
    }

    draw_parameter_text("Module manifest", module_manifest_);
    draw_parameter_text("Configuration", module_configuration_);
    const bool review_capable = actions.active_module_catalog != nullptr && static_cast<bool>(actions.inspect_module_catalog);
    if (reviewed_manifest_ != module_manifest_ || reviewed_configuration_ != module_configuration_) {
        has_contract_review_ = false;
        contract_review_error_.clear();
    }
    const auto current_review = [&] {
        return has_contract_review_ && reviewed_manifest_ == module_manifest_ &&
               reviewed_configuration_ == module_configuration_ && actions.active_module_catalog != nullptr &&
               reviewed_before_digest_ == actions.active_module_catalog->contract_digest();
    };
    bool review_load_allowed = !review_capable;
    if (review_capable) {
        if (ImGui::Button("Review Contract Changes")) {
            const auto candidate = actions.inspect_module_catalog(module_manifest_);
            if (!candidate) {
                contract_review_error_ = candidate.result.message;
                has_contract_review_ = false;
            } else {
                contract_review_ = diff_feature_catalogs(*actions.active_module_catalog, candidate.value,
                                                          actions.authored_feature_documents);
                reviewed_manifest_ = module_manifest_;
                reviewed_configuration_ = module_configuration_;
                reviewed_before_digest_ = contract_review_.before_digest;
                reviewed_after_digest_ = contract_review_.after_digest;
                contract_review_error_.clear();
                has_contract_review_ = true;
            }
        }
        review_load_allowed = current_review();
        if (review_load_allowed) {
            review_load_allowed = contract_review_.before_digest == actions.active_module_catalog->contract_digest();
            ImGui::Text("Module contract review: %zu definition changes, %zu affected graphs",
                        contract_review_.changes.size(), contract_review_.impacts.size());
            for (const auto& change : contract_review_.changes) {
                const char* kind = change.kind == FeatureCatalogChangeKind::kAdded ? "Added" :
                                   change.kind == FeatureCatalogChangeKind::kRemoved ? "Removed" : "Changed";
                ImGui::BulletText("%s definition %llu (v%u.%u.%u -> v%u.%u.%u)", kind,
                    static_cast<unsigned long long>(change.definition_id.value()), change.before_version.major,
                    change.before_version.minor, change.before_version.patch, change.after_version.major,
                    change.after_version.minor, change.after_version.patch);
                if (!change.changed_fields.empty()) {
                    std::string fields;
                    for (const auto& field : change.changed_fields) {
                        if (!fields.empty()) fields += ", ";
                        fields += field;
                    }
                    ImGui::TextDisabled("  Contract fields: %s", fields.c_str());
                }
                const auto show_ids = [](const char* label, const auto& ids) {
                    if (ids.empty()) return;
                    ImGui::TextDisabled("  %s:", label);
                    for (const auto id : ids)
                        ImGui::BulletText("%llu", static_cast<unsigned long long>(id.value()));
                };
                show_ids("Ports added", change.added_ports);
                show_ids("Ports removed", change.removed_ports);
                show_ids("Ports changed", change.changed_ports);
                show_ids("Parameters added", change.added_parameters);
                show_ids("Parameters removed", change.removed_parameters);
                show_ids("Parameters changed", change.changed_parameters);
            }
            for (const auto& impact : contract_review_.impacts) {
                ImGui::BulletText("%s: %zu affected nodes, %zu incident edges", impact.path.generic_string().c_str(),
                                  impact.node_ids.size(), impact.edges.size());
                ImGui::TextDisabled("  Asset %s", impact.asset_uuid.c_str());
                for (const auto node : impact.node_ids)
                    ImGui::TextDisabled("  Node %llu", static_cast<unsigned long long>(node.value()));
            }
        }
        if (!contract_review_error_.empty())
            ImGui::TextColored(ImVec4{1.0f, 0.45f, 0.30f, 1.0f}, "Contract review failed: %s",
                               contract_review_error_.c_str());
    }
    const bool changed_contract = review_capable && current_review() && !contract_review_.empty();
    const auto can_load = [&](const auto& action) {
        return static_cast<bool>(action) && !actions.publication_pending &&
               (!review_capable || review_load_allowed);
    };
    const auto accept_review_before_load = [&]() -> foundation::Result {
        if (!review_capable) return foundation::Result::success();
        const auto candidate = actions.inspect_module_catalog(module_manifest_);
        if (!candidate) return candidate.result;
        const auto fresh = diff_feature_catalogs(*actions.active_module_catalog, candidate.value,
                                                 actions.authored_feature_documents);
        if (fresh.before_digest != reviewed_before_digest_ || fresh.after_digest != reviewed_after_digest_) {
            has_contract_review_ = false;
            contract_review_error_ = "Module catalog changed after review. Review the contract again before loading.";
            return foundation::invalid_state(contract_review_error_);
        }
        return foundation::Result::success();
    };
    ImGui::BeginDisabled(!can_load(actions.select_module));
    const char* module_label = changed_contract ? "Accept & Load Module" : "Load Module";
    if (ImGui::Button(module_label)) {
        const auto reviewed = accept_review_before_load();
        const auto result = reviewed ? actions.select_module(module_manifest_, module_configuration_) : reviewed;
        last_action_ = result ? "Executable module queued for the next Scene frame" : result.message;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_load(actions.select_saved_module));
    const char* saved_label = changed_contract ? "Accept & Load Saved Project" : "Load Saved Project";
    if (ImGui::Button(saved_label)) {
        const auto reviewed = accept_review_before_load();
        const auto result = reviewed ? actions.select_saved_module(module_manifest_, module_configuration_) : reviewed;
        last_action_ = result ? "Saved startup FeatureGraphs queued for the next Scene frame" : result.message;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Load all startup FeatureGraphs from the saved project file. The open author draft is retained.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!actions.apply_graph || actions.publication_pending);
    if (ImGui::Button("Apply")) {
        const auto result = actions.apply_graph(document.snapshot());
        last_action_ = result ? "Complete FeatureGraph batch queued for the next Scene frame" : result.message;
    }
    ImGui::EndDisabled();
    if (actions.publication_pending) {
        ImGui::SameLine(); ImGui::TextDisabled("Publication pending");
    }
    auto graph_owner = document.snapshot_owner();
    const auto& toolbar_graph = *graph_owner;
    ImGui::Text("FeatureGraph  %s", source_path.filename().string().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("schema %u  graph %llu  draft %llu", toolbar_graph.schema_version,
                        static_cast<unsigned long long>(toolbar_graph.graph_version),
                        static_cast<unsigned long long>(toolbar_graph.draft_revision));
    if (ImGui::Button("Save") && actions.save_draft) {
        const auto result = actions.save_draft();
        last_action_ = result ? "FeatureGraph saved" : result.message;
    }
    ImGui::SameLine();
    if (ImGui::Button("Undo")) {
        const auto result = document.undo();
        last_action_ = result ? "FeatureGraph edit undone" : result.message;
    }
    ImGui::SameLine();
    if (ImGui::Button("Redo")) {
        const auto result = document.redo();
        last_action_ = result ? "FeatureGraph edit redone" : result.message;
    }
    if (!last_action_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", last_action_.c_str());
    }
    ImGui::Separator();

    if (!document.catalog().entries().empty()) actions.feature_catalog = &document.catalog();
    feature_canvas_.draw(document, actions.feature_catalog, selected_node_, last_action_, actions.feature_presentations);
    graph_owner = document.snapshot_owner();
    const auto& graph = *graph_owner;
    if (actions.feature_catalog) draw_orchestration(document, graph, *actions.feature_catalog, source_path, actions.feature_presentations);
    ImGui::Text("Nodes (%zu)", graph.nodes.size());
    if (ImGui::BeginTable("FeatureGraphNodes", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Node");
        ImGui::TableSetupColumn("Definition");
        ImGui::TableSetupColumn("Version");
        ImGui::TableSetupColumn("Action");
        ImGui::TableHeadersRow();
        for (const auto& node : graph.nodes) {
            const std::string node_id = std::to_string(node.node_id.value());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const bool selected = selected_node_ == node_id;
            if (ImGui::Selectable(node_id.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns))
                selected_node_ = node_id;
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%llu", static_cast<unsigned long long>(node.definition_id.value()));
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%u.%u.%u", node.definition_version.major, node.definition_version.minor,
                        node.definition_version.patch);
            ImGui::TableSetColumnIndex(3);
            ImGui::PushID(node_id.c_str());
            if (ImGui::SmallButton("Delete")) {
                const auto id = node.node_id;
                const auto result = document.erase_node(id);
                last_action_ = result ? "Feature node deleted" : result.message;
                if (result)
                    selected_node_.clear();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Checkbox("Show ExecutionPlan", &show_execution_plan_);
    if (show_execution_plan_) draw_execution_inspect(graph, actions);
    ImGui::Checkbox("Show RenderGraph", &show_render_graph_);
    if (show_render_graph_) draw_render_graph_inspect(graph, actions);
    draw_runtime_inspect(graph, actions);
    draw_world_binding_editor(graph, actions);
    ImGui::Separator();
    if (actions.feature_catalog != nullptr) {
        if (authoring_jump_ == 2) { ImGui::SetScrollHereY(0.0f); authoring_jump_ = 0; }
        ImGui::Text("Feature Catalog");
        ImGui::TextWrapped("Choose a contract and Add node. Logic: gameplay; Physics: simulation; Render: presentation; "
                           "Audio: sound; Stream: asynchronous data. Select a node to inspect its contract.");
        ImGui::InputText("Search name or definition id###Search definition id", catalog_search_.data(), catalog_search_.size());
        const char* domain_choices[] = {"All domains", "Logic", "Physics", "Render", "Audio", "Stream"};
        ImGui::Combo("Domain", &catalog_domain_filter_, domain_choices, IM_ARRAYSIZE(domain_choices));
        const char* executions[] = {"All execution", "Call", "Process", "Continuous"};
        ImGui::Combo("Execution", &catalog_execution_filter_, executions, IM_ARRAYSIZE(executions));
        const char* structures[] = {"All structures", "Atomic", "Composite"};
        ImGui::Combo("Structure", &catalog_structure_filter_, structures, IM_ARRAYSIZE(structures));
        std::vector<feature::ClockId> clocks;
        std::vector<feature::ScopeId> scopes;
        for (const auto& entry : actions.feature_catalog->entries()) {
            clocks.push_back(entry.clock);
            scopes.push_back(entry.contract.scope.id);
        }
        std::sort(clocks.begin(), clocks.end(), [](auto left, auto right) { return left.value() < right.value(); });
        clocks.erase(std::unique(clocks.begin(), clocks.end()), clocks.end());
        if (catalog_clock_filter_ && std::find(clocks.begin(), clocks.end(), *catalog_clock_filter_) == clocks.end())
            catalog_clock_filter_.reset();
        const auto clock_label = [](feature::ClockId clock) { return "Clock " + std::to_string(clock.value()); };
        const auto clock_preview = catalog_clock_filter_ ? clock_label(*catalog_clock_filter_) : "All clocks";
        if (ImGui::BeginCombo("Clock", clock_preview.c_str())) {
            if (ImGui::Selectable("All clocks", !catalog_clock_filter_)) catalog_clock_filter_.reset();
            for (auto clock : clocks) {
                const auto label = clock_label(clock);
                const bool selected = catalog_clock_filter_ == clock;
                if (ImGui::Selectable(label.c_str(), selected)) catalog_clock_filter_ = clock;
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        std::sort(scopes.begin(), scopes.end(), [](auto left, auto right) { return left.value() < right.value(); });
        scopes.erase(std::unique(scopes.begin(), scopes.end()), scopes.end());
        if (catalog_scope_filter_ && std::find(scopes.begin(), scopes.end(), *catalog_scope_filter_) == scopes.end())
            catalog_scope_filter_.reset();
        const auto scope_label = [](feature::ScopeId scope) { return "Scope " + std::to_string(scope.value()); };
        const auto scope_preview = catalog_scope_filter_ ? scope_label(*catalog_scope_filter_) : "All scopes";
        if (ImGui::BeginCombo("Scope", scope_preview.c_str())) {
            if (ImGui::Selectable("All scopes", !catalog_scope_filter_)) catalog_scope_filter_.reset();
            for (auto scope : scopes) {
                const auto label = scope_label(scope);
                const bool selected = catalog_scope_filter_ == scope;
                if (ImGui::Selectable(label.c_str(), selected)) catalog_scope_filter_ = scope;
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::BeginTable("FeatureGraphCatalog", 6,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Definition");
            ImGui::TableSetupColumn("Domain mask");
            ImGui::TableSetupColumn("Execution");
            ImGui::TableSetupColumn("Clock");
            ImGui::TableSetupColumn("Ports");
            ImGui::TableSetupColumn("Action");
            ImGui::TableHeadersRow();
            for (const auto& entry : actions.feature_catalog->entries()) {
                if (!feature_presentation_matches(entry, actions.feature_presentations, catalog_search_.data()) ||
                    !catalog_matches(entry, "", catalog_domain_filter_,
                                     catalog_execution_filter_, catalog_clock_filter_,
                                     catalog_structure_filter_, catalog_scope_filter_))
                    continue;
                const std::string entry_id = std::to_string(entry.definition_id.value());
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(feature_presentation_name(entry, actions.feature_presentations).c_str());
                ImGui::TextDisabled("ID %s", entry_id.c_str());
                ImGui::TableSetColumnIndex(1);
                const auto domains = domain_mask_name(entry.domains);
                ImGui::TextUnformatted(domains.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(execution_name(entry.execution));
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%llu", static_cast<unsigned long long>(entry.clock.value()));
                ImGui::TableSetColumnIndex(4);
            if (entry.ports.empty()) {
                ImGui::TextUnformatted("-");
            } else {
                for (const auto& port : entry.ports) {
                    ImGui::Text("%c %llu/%llu v%u",
                                port.direction == feature::PortDirection::kInput ? 'I' : 'O',
                                static_cast<unsigned long long>(port.id.value()),
                                static_cast<unsigned long long>(port.schema.value()), port.version);
                }
            }
            ImGui::TableSetColumnIndex(5);
                ImGui::PushID(entry_id.c_str());
                if (ImGui::SmallButton("Add node")) {
                    const auto node_id = next_editor_node_id(graph, entry);
                    const auto result = document.add_node(
                        {node_id, entry.definition_id, entry.version, {}, {}},
                        actions.feature_catalog->contract_digest());
                    last_action_ = result ? "Feature node added" : result.message;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::Separator();
    }
    if (actions.feature_catalog) draw_parameters(document, *actions.feature_catalog, actions.asset_kind_resolver);
    ImGui::Text("Connect ports");
    ImGui::TextDisabled("Select source/target nodes, then enter numeric port ids from the catalog.");
    if (ImGui::Button("Use selected as source") && !selected_node_.empty())
        edge_source_node_ = selected_node_;
    ImGui::SameLine();
    if (ImGui::Button("Use selected as target") && !selected_node_.empty())
        edge_target_node_ = selected_node_;
    char source_node_buffer[64]{};
    char target_node_buffer[64]{};
    std::copy(edge_source_node_.begin(), edge_source_node_.end(), source_node_buffer);
    std::copy(edge_target_node_.begin(), edge_target_node_.end(), target_node_buffer);
    if (ImGui::InputText("Source node", source_node_buffer, sizeof(source_node_buffer)))
        edge_source_node_ = source_node_buffer;
    if (ImGui::InputText("Target node", target_node_buffer, sizeof(target_node_buffer)))
        edge_target_node_ = target_node_buffer;
    ImGui::InputText("Source output port", edge_source_port_.data(), edge_source_port_.size());
    ImGui::InputText("Target input port", edge_target_port_.data(), edge_target_port_.size());
    const char* edge_kinds[] = {"Data", "Snapshot", "Queue", "Event", "Command", "Receipt"};
    ImGui::Combo("Kind", &edge_kind_, edge_kinds, IM_ARRAYSIZE(edge_kinds));
    if (edge_kind_ == static_cast<int>(feature::EdgeKind::kQueue))
        ImGui::InputInt("Capacity", &edge_capacity_);
    if (edge_kind_ == static_cast<int>(feature::EdgeKind::kSnapshot)) {
        ImGui::InputInt("Deadline ticks", &edge_deadline_);
        ImGui::TextDisabled("Deadline unit: source Clock periods");
    }
    if (ImGui::Button("Add edge") && actions.feature_catalog != nullptr) {
        feature::FeatureNodeId source_node;
        feature::FeatureNodeId target_node;
        feature::PortId source_port;
        feature::PortId target_port;
        const bool source_ok = parse_numeric_id(edge_source_node_, source_node);
        const bool target_ok = parse_numeric_id(edge_target_node_, target_node);
        const bool source_port_ok = parse_numeric_id(edge_source_port_.data(), source_port);
        const bool target_port_ok = parse_numeric_id(edge_target_port_.data(), target_port);
        const auto* source = source_ok ? find_node(graph, source_node) : nullptr;
        const auto* target = target_ok ? find_node(graph, target_node) : nullptr;
        const auto* source_entry = source == nullptr ? nullptr : find_entry(*actions.feature_catalog, source->definition_id);
        const auto* target_entry = target == nullptr ? nullptr : find_entry(*actions.feature_catalog, target->definition_id);
        uint32_t edge_version = 0;
        if (source_entry != nullptr && source_port_ok) {
            const auto source_contract_port = std::find_if(
                source_entry->ports.begin(), source_entry->ports.end(), [&](const auto& port) {
                    return port.id == source_port && port.direction == feature::PortDirection::kOutput;
                });
            if (source_contract_port != source_entry->ports.end())
                edge_version = source_contract_port->version;
        }
        const auto edge = feature::FeatureEdge{source_node, source_port, target_node, target_port,
                                               static_cast<feature::EdgeKind>(edge_kind_),
                                               source_entry == nullptr ? feature::ClockId{} : source_entry->clock,
                                               target_entry == nullptr ? feature::ClockId{} : target_entry->clock,
                                               edge_version,
                                               edge_capacity_ > 0 ? static_cast<uint32_t>(edge_capacity_) : 0u,
                                               edge_deadline_ > 0 ? static_cast<uint32_t>(edge_deadline_) : 0u};
        const auto result = (source_ok && target_ok && source_port_ok && target_port_ok)
                                ? document.add_edge(edge, *actions.feature_catalog)
                                : foundation::invalid_argument("FeatureGraph edge ids must be numeric and nonzero");
        last_action_ = result ? "Feature edge added" : result.message;
    }

    ImGui::Separator();
    ImGui::Text("Edges (%zu)", graph.edges.size());
    if (ImGui::BeginTable("FeatureGraphEdges", 5,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Source");
        ImGui::TableSetupColumn("Target");
        ImGui::TableSetupColumn("Kind");
        ImGui::TableSetupColumn("Capacity");
        ImGui::TableSetupColumn("Action");
        ImGui::TableHeadersRow();
        for (const auto& edge : graph.edges) {
            const std::string edge_id = edge_selection_key(edge);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable(edge_id.c_str(), selected_edge_ == edge_id)) selected_edge_ = edge_id;
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%llu:%llu", static_cast<unsigned long long>(edge.target_node.value()),
                        static_cast<unsigned long long>(edge.target_port.value()));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(edge_name(edge.kind));
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%u", edge.capacity);
            ImGui::TableSetColumnIndex(4);
            ImGui::PushID(edge_id.c_str());
            if (ImGui::SmallButton("Delete")) {
                const auto source = edge.source_node;
                const auto source_port = edge.source_port;
                const auto target = edge.target_node;
                const auto target_port = edge.target_port;
                const auto result = document.erase_edge(source, source_port, target, target_port);
                last_action_ = result ? "Feature edge deleted" : result.message;
                if (result && selected_edge_ == edge_id) selected_edge_.clear();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    // Resolve against this draw's current document; selection never retains an edge/port pointer.
    const auto& current_graph = document.snapshot();
    const auto selected_edge = std::find_if(current_graph.edges.begin(), current_graph.edges.end(), [this](const auto& edge) {
        return edge_selection_key(edge) == selected_edge_;
    });
    if (selected_edge != current_graph.edges.end())
        draw_saved_edge_details(current_graph, *selected_edge, actions.feature_catalog);
    else
        selected_edge_.clear();

    ImGui::Separator();
    if (authoring_jump_ == 4) {
        process_owner_node_ = selected_node_;
        ImGui::SetScrollHereY(0.0f); authoring_jump_ = 0;
    }
    ImGui::Text("Process subgraphs (%zu)", graph.process_subgraphs.size());
    ImGui::TextWrapped("A Process is a state machine owned by one Feature node. Select a Process node, "
                       "use it as the owner, then edit states and transitions below.");
    if (ImGui::Button("Use selected as Process owner") && !selected_node_.empty())
        process_owner_node_ = selected_node_;
    char process_owner_buffer[64]{};
    std::copy(process_owner_node_.begin(), process_owner_node_.end(), process_owner_buffer);
    if (ImGui::InputText("Process owner node", process_owner_buffer, sizeof(process_owner_buffer)))
        process_owner_node_ = process_owner_buffer;
    feature::FeatureNodeId canvas_owner{};
    if (parse_numeric_id(process_owner_node_, canvas_owner))
        process_canvas_.draw(document, canvas_owner, last_action_);
    bool add_process_state = false;
    bool add_process_transition = false;
    feature::FeatureNodeId add_process_owner{};
    std::string add_process_state_name;
    std::string add_process_transition_from;
    std::string add_process_transition_to;
    ImGui::InputText("New state", process_state_.data(), process_state_.size());
    if (ImGui::Button("Add state")) {
        feature::FeatureNodeId owner;
        if (!parse_numeric_id(process_owner_node_, owner)) {
            last_action_ = "Process owner node id must be numeric and nonzero";
        } else {
            add_process_state = true;
            add_process_owner = owner;
            add_process_state_name = process_state_.data();
        }
    }
    ImGui::InputText("Transition from", process_from_state_.data(), process_from_state_.size());
    ImGui::InputText("Transition to", process_to_state_.data(), process_to_state_.size());
    if (ImGui::Button("Add transition")) {
        feature::FeatureNodeId owner;
        if (!parse_numeric_id(process_owner_node_, owner)) {
            last_action_ = "Process owner node id must be numeric and nonzero";
        } else {
            add_process_transition = true;
            add_process_owner = owner;
            add_process_transition_from = process_from_state_.data();
            add_process_transition_to = process_to_state_.data();
        }
    }
    bool delete_process_state = false;
    bool delete_process_transition = false;
    feature::FeatureNodeId delete_process_owner{};
    std::string delete_process_state_name;
    std::string delete_process_transition_from;
    std::string delete_process_transition_to;
    bool set_process_initial = false;
    bool set_process_terminal = false;
    bool terminal_state_value = false;
    feature::FeatureNodeId set_process_owner{};
    std::string set_process_state_name;
    for (const auto& process : graph.process_subgraphs) {
        const auto process_id = std::to_string(process.owner_node.value());
        ImGui::PushID(process_id.c_str());
        ImGui::Text("Owner %llu  schema %u", static_cast<unsigned long long>(process.owner_node.value()),
                    process.state_schema_version);
        ImGui::TextDisabled("Initial: %s", process.initial_state.empty() ? "(implicit)" : process.initial_state.c_str());
        for (std::size_t state_index = 0; state_index < process.states.size(); ++state_index) {
            const auto& state = process.states[state_index];
            ImGui::BulletText("State %s", state.c_str());
            ImGui::SameLine();
            ImGui::PushID(static_cast<int>(state_index));
            if (process.initial_state != state) {
                if (ImGui::SmallButton("Set initial")) {
                    set_process_initial = true;
                    set_process_owner = process.owner_node;
                    set_process_state_name = state;
                }
                ImGui::SameLine();
            }
            bool terminal = std::find(process.terminal_states.begin(), process.terminal_states.end(), state) !=
                            process.terminal_states.end();
            if (ImGui::Checkbox("Terminal", &terminal)) {
                set_process_terminal = true;
                set_process_owner = process.owner_node;
                set_process_state_name = state;
                terminal_state_value = terminal;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete state")) {
                delete_process_state = true;
                delete_process_owner = process.owner_node;
                delete_process_state_name = state;
            }
            ImGui::PopID();
        }
        for (std::size_t transition_index = 0; transition_index < process.transitions.size(); ++transition_index) {
            const auto& transition = process.transitions[transition_index];
            ImGui::BulletText("Transition %s", transition.c_str());
            ImGui::SameLine();
            ImGui::PushID(static_cast<int>(transition_index));
            if (ImGui::SmallButton("Delete transition")) {
                const auto separator = transition.find("->");
                if (separator != std::string::npos) {
                    delete_process_transition = true;
                    delete_process_owner = process.owner_node;
                    delete_process_transition_from = transition.substr(0, separator);
                    delete_process_transition_to = transition.substr(separator + 2);
                }
            }
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    if (add_process_state) {
        const auto result = document.add_process_state(add_process_owner, std::move(add_process_state_name));
        last_action_ = result ? "Process state added" : result.message;
    } else if (add_process_transition) {
        const auto result = document.add_process_transition(add_process_owner,
                                                            std::move(add_process_transition_from),
                                                            std::move(add_process_transition_to));
        last_action_ = result ? "Process transition added" : result.message;
    } else if (delete_process_state) {
        const auto result = document.erase_process_state(delete_process_owner, delete_process_state_name);
        last_action_ = result ? "Process state deleted" : result.message;
    } else if (delete_process_transition) {
        const auto result = document.erase_process_transition(delete_process_owner,
                                                              delete_process_transition_from,
                                                              delete_process_transition_to);
        last_action_ = result ? "Process transition deleted" : result.message;
    } else if (set_process_initial) {
        const auto result = document.set_process_initial_state(set_process_owner, std::move(set_process_state_name));
        last_action_ = result ? "Process initial state updated" : result.message;
    } else if (set_process_terminal) {
        const auto result = document.set_process_terminal_state(set_process_owner, std::move(set_process_state_name),
                                                                terminal_state_value);
        last_action_ = result ? "Process terminal state updated" : result.message;
    }
    if (actions.native_window) ImGui::EndChild();
    ImGui::End();
    if (show_definitions_) {
        ImGui::SetNextWindowSize({920, 720}, ImGuiCond_FirstUseEver);
        if (focus_definitions) { ImGui::SetNextWindowCollapsed(false); ImGui::SetNextWindowFocus(); }
        definition_panel_.draw(document, actions.project_root, actions.compiled_features,
                               actions.asset_kind_resolver, actions.project_file, {}, actions.feature_presentations,
                               &show_definitions_);
    }
}

void EditorFeatureGraphPanel::draw_orchestration(EditorFeatureGraphDocument& document,
                                                 const feature::FeatureGraphDocument& graph,
                                                 const feature::FeatureCatalog& catalog,
                                                 const std::filesystem::path& source_path,
                                                 std::span<const NodeDefinitionDraft> presentations) {
    const auto snapshot = document.snapshot_owner();
    const auto digest = catalog.contract_digest();
    // The live projection belongs to the editable snapshot, never to last-good.
    // Repeated UI frames reuse immutable results instead of revalidating every edge.
    if (projection_snapshot_ != snapshot || projection_catalog_digest_ != digest) {
        auto compiled = EditorFeatureGraphPreviewCompiler{}.compile(graph, catalog);
        projection_result_ = compiled.result;
        current_projection_.reset();
        if (compiled) current_projection_ = std::move(compiled.value);
        projection_snapshot_ = snapshot;
        projection_catalog_digest_ = digest;
        projection_semantic_digest_ = feature::graph_semantic_digest(graph);
        preview_action_status_.clear();
        preview_diagnostics_ = current_projection_ ? current_projection_->diagnostics :
            std::vector<EditorFeatureGraphPreviewDiagnostic>{{projection_result_.message}};
    }
    const auto cache_preview = [&](const EditorFeatureGraphPreview& value) {
        last_good_preview_ = value;
        last_good_preview_path_ = source_path;
        preview_asset_uuid_ = graph.asset_uuid;
        last_good_preview_revision_ = graph.draft_revision;
        last_good_preview_catalog_digest_ = digest;
        last_good_preview_semantic_digest_ = projection_semantic_digest_;
        last_good_preview_snapshot_ = snapshot;
    };
    bool stale = last_good_preview_ &&
                       (last_good_preview_path_ != source_path || preview_asset_uuid_ != graph.asset_uuid ||
                        last_good_preview_revision_ != graph.draft_revision ||
                        last_good_preview_catalog_digest_ != digest ||
                        last_good_preview_semantic_digest_ != projection_semantic_digest_ ||
                        !projection_result_ || (current_projection_ && current_projection_->has_errors()));
    ImGui::Separator();
    if (authoring_jump_ == 3) { ImGui::SetScrollHereY(0.0f); authoring_jump_ = 0; }
    ImGui::TextUnformatted("Five-domain orchestration");
    ImGui::SameLine();
    if (ImGui::Button("Validate draft")) {
        preview_action_status_ = preview_diagnostics_.empty() ? "Draft validation passed" :
                                                               "Draft validation found errors";
    }
    ImGui::SameLine();
    if (ImGui::Button("Compile preview")) {
        if (!projection_result_ || !current_projection_ || current_projection_->has_errors()) {
            preview_action_status_ = last_good_preview_ ? "Compile preview blocked; last-good preview retained" :
                                                         "Compile preview blocked; no successful preview yet";
        } else {
            cache_preview(*current_projection_);
            stale = false;
            preview_action_status_ = "Authoring preview compiled";
        }
    }
    if (!preview_action_status_.empty()) ImGui::TextWrapped("%s", preview_action_status_.c_str());
    ImGui::TextDisabled("Authoring preview — not published. No executable module is required; Apply remains module-gated.");
    if (last_good_preview_) {
        ImGui::Text("Preview: %s", stale ? "last-good (stale)" : "last-good");
    } else {
        ImGui::TextDisabled("Preview unavailable until Compile preview succeeds.");
    }
    if (!preview_diagnostics_.empty()) {
        ImGui::Text("Preview diagnostics (%zu)", preview_diagnostics_.size());
        for (const auto& diagnostic : preview_diagnostics_) {
            std::string location;
            if (diagnostic.node_id) location += "Node " + std::to_string(diagnostic.node_id->value());
            if (diagnostic.port_id) {
                if (!location.empty()) location += ", ";
                location += "Port " + std::to_string(diagnostic.port_id->value());
            }
            if (diagnostic.edge_index) {
                if (!location.empty()) location += ", ";
                location += "Edge " + std::to_string(*diagnostic.edge_index);
            }
            if (!diagnostic.owner_path.empty()) {
                if (!location.empty()) location += ", ";
                location += "OwnerPath " + owner_path_text(diagnostic.owner_path);
            }
            if (location.empty()) ImGui::TextWrapped("%s", diagnostic.message.c_str());
            else ImGui::TextWrapped("[%s] %s", location.c_str(), diagnostic.message.c_str());
        }
    }
    const auto* preview = current_projection_ ? &*current_projection_ : nullptr;
    if (preview == nullptr) {
        ImGui::TextDisabled("Current draft projection is unavailable.");
        return;
    }
    ImGui::TextUnformatted("Current draft - select a node in any lane to inspect it");
    if (ImGui::BeginTable("FiveDomainLanes", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        for (const auto& lane : preview->lanes) {
            ImGui::PushID(static_cast<int>(lane.domain));
            ImGui::TableNextColumn();
            ImGui::Text("%s (%zu)", domain_name(lane.domain), lane.node_ids.size());
            for (const auto id : lane.node_ids) {
                const auto identity = std::to_string(id.value());
                const auto* authored = find_node(graph, id);
                const auto* contract = authored ? find_entry(catalog, authored->definition_id, authored->definition_version) : nullptr;
                const auto title = contract ? feature_presentation_name(*contract, presentations) : "Node " + identity;
                const auto label = title + "###" + identity;
                if (ImGui::Selectable(label.c_str(), selected_node_ == identity)) selected_node_ = identity;
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nNode %s", title.c_str(), identity.c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextUnformatted("Contract Inspector (read-only)");
    feature::FeatureNodeId selected{};
    const auto* node = parse_numeric_id(selected_node_, selected) ? find_node(graph, selected) : nullptr;
    const auto* entry = node ? find_entry(catalog, node->definition_id, node->definition_version) : nullptr;
    if (entry) {
        const auto& c = entry->contract;
        ImGui::TextUnformatted(feature_presentation_name(*entry, presentations).c_str());
        ImGui::Text("Definition: %llu  version %u.%u.%u", static_cast<unsigned long long>(entry->definition_id.value()), entry->version.major, entry->version.minor, entry->version.patch);
        ImGui::Text("Domain: %s", domain_mask_name(c.domains).c_str());
        ImGui::Text("Structure: %s  Execution: %s  StatePolicy: %s",
                    c.structure == feature::FeatureStructure::kComposite ? "Composite" : "Atomic",
                    execution_name(c.execution), state_name(c.state));
        ImGui::TextWrapped("%s", c.execution == feature::ExecutionKind::kProcess ?
            "Process: advances through its authored states and transitions." :
            c.execution == feature::ExecutionKind::kContinuous ? "Continuous: produces work on its declared Clock." :
            "Call: performs one invocation on its declared Clock.");
        if (ImGui::Button("Edit selected definition")) {
            show_definitions_ = true;
            definition_panel_.request_open(node->definition_id);
        }
        ImGui::Text("Scope: %llu  Clock: %llu", static_cast<unsigned long long>(c.scope.id.value()), static_cast<unsigned long long>(c.clock.id.value()));
        std::string owner_path;
        for (const auto owner : node->owner_path.nodes) {
            if (!owner_path.empty()) owner_path += '/';
            owner_path += std::to_string(owner.value());
        }
        ImGui::Text("owner: %llu  owner path: %s", static_cast<unsigned long long>(entry->owner_module.value()),
                    owner_path.empty() ? "(root)" : owner_path.c_str());
        ImGui::Text("ports: %zu  children: %zu", c.ports.size(), c.children.size());
        for (const auto& port : c.ports)
            ImGui::BulletText("%s port %llu - schema %llu v%u",
                port.direction == feature::PortDirection::kInput ? "Input" : "Output",
                static_cast<unsigned long long>(port.id.value()), static_cast<unsigned long long>(port.schema.value()), port.version);
        if (!c.children.empty()) {
            std::string children;
            for (const auto child : c.children) {
                if (!children.empty()) children += ", ";
                children += std::to_string(child.value());
            }
            ImGui::Text("Composite children: %s", children.c_str());
        }
        const auto process = std::find_if(graph.process_subgraphs.begin(), graph.process_subgraphs.end(),
                                          [selected](const auto& value) { return value.owner_node == selected; });
        if (process != graph.process_subgraphs.end()) {
            ImGui::Text("Process: %zu states, %zu transitions, initial %s, terminal %zu",
                        process->states.size(), process->transitions.size(),
                        process->initial_state.empty() ? "(derived)" : process->initial_state.c_str(),
                        process->terminal_states.size());
        } else {
            ImGui::Text("Process: %s", c.allows_child_process ? "allowed (no authored subgraph)" : "none");
        }
        ImGui::Text("opaque parameter/state payload: %zu / %u bytes", node->parameter_blob.size(), c.state_payload.max_size);
    } else if (node) {
        ImGui::Text("Definition: %llu  version %u.%u.%u", static_cast<unsigned long long>(node->definition_id.value()),
                    node->definition_version.major, node->definition_version.minor, node->definition_version.patch);
        ImGui::TextColored({1.0f, 0.55f, 0.25f, 1.0f}, "Stale or unavailable catalog contract for this Definition version");
    } else ImGui::TextDisabled("Select a node on the canvas or in a domain lane.");
    ImGui::TextUnformatted("Cross-domain edges / transport diagnostics");
    if (ImGui::BeginTable("CrossDomainEdges", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (const auto* h : {"Relation", "Transport", "Source Clock", "Target Clock", "Source Domain", "Target Domain", "Capacity", "Deadline", "Diagnostic"}) ImGui::TableSetupColumn(h);
        ImGui::TableHeadersRow();
        for (const auto& edge : preview->edges) {
            ImGui::TableNextRow();
            const bool cross_clock = edge.source_clock != edge.target_clock;
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(edge.cross_domain ? "Cross-domain" : (cross_clock ? "Cross-clock" : "Local"));
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(edge_name(edge.transport));
            ImGui::TableSetColumnIndex(2); ImGui::Text("%llu", static_cast<unsigned long long>(edge.source_clock.value()));
            ImGui::TableSetColumnIndex(3); ImGui::Text("%llu", static_cast<unsigned long long>(edge.target_clock.value()));
            ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(domain_mask_name(edge.source_domains).c_str());
            ImGui::TableSetColumnIndex(5); ImGui::TextUnformatted(domain_mask_name(edge.target_domains).c_str());
            ImGui::TableSetColumnIndex(6); ImGui::Text("%u", edge.capacity);
            ImGui::TableSetColumnIndex(7); ImGui::Text("%u", edge.deadline_ticks);
            ImGui::TableSetColumnIndex(8); if (!edge.validation) ImGui::TextUnformatted(edge.validation.message.c_str()); else ImGui::TextUnformatted("OK");
        }
        ImGui::EndTable();
    }
    if (last_good_preview_) {
        const auto& good = *last_good_preview_;
        ImGui::SeparatorText("Compiled authoring preview - last successful result");
        ImGui::Text("Compiled draft revision %llu%s", static_cast<unsigned long long>(good.graph_revision),
                    stale ? " (stale; current draft differs)" : "");
        ImGui::Text("Clock groups (%zu)", good.clock_groups.size());
        for (const auto& group : good.clock_groups)
            ImGui::Text("Clock %llu: %zu node(s)", static_cast<unsigned long long>(group.clock.value()), group.node_ids.size());
        ImGui::Text("Provenance (%zu)", good.provenance.size());
        for (const auto& origin : good.provenance)
            ImGui::Text("Node %llu -> Definition %llu owner %llu path %s",
                static_cast<unsigned long long>(origin.node_id.value()), static_cast<unsigned long long>(origin.definition_id.value()),
                static_cast<unsigned long long>(origin.owner_module.value()), owner_path_text(origin.owner_path).c_str());
        for (const auto& edge : good.edges)
            ImGui::Text("Compiled edge %zu: %s", edge.edge_index, edge_name(edge.transport));
    }
}


void EditorFeatureGraphPanel::draw_execution_inspect(const feature::FeatureGraphDocument& graph, const Actions& actions) {
    ImGui::Separator(); ImGui::TextUnformatted("Published ExecutionPlan");
    if (!actions.inspect_execution) { ImGui::TextDisabled("No executable Feature session is available."); return; }
    const auto observed = actions.inspect_execution(graph.asset_uuid);
    if (!observed) { ImGui::TextWrapped("%s", observed.result.message.c_str()); return; }
    const auto& snapshot = observed.value;
    if (snapshot.asset_uuid != graph.asset_uuid) { ImGui::TextDisabled("Published view belongs to another FeatureGraph."); return; }
    ImGui::Text("Generation %llu  graph %llu  draft %llu", static_cast<unsigned long long>(snapshot.generation.value()),
                static_cast<unsigned long long>(snapshot.graph_version), static_cast<unsigned long long>(snapshot.draft_revision));
    ImGui::Text("Asset %s", snapshot.asset_uuid.c_str());
    if (!snapshot.source_path.empty()) ImGui::TextDisabled("Source %s", snapshot.source_path.generic_string().c_str());
    if (graph.graph_version != snapshot.graph_version || graph.draft_revision != snapshot.draft_revision || graph.contract_digest != snapshot.contract_digest)
        ImGui::TextDisabled("Author draft differs from this published generation.");
    if (actions.publication_pending) ImGui::TextDisabled("Publication pending; this view retains the active generation.");
    if (ImGui::BeginTable("FeatureGraphExecution", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (const auto* label : {"Clock", "Order", "Definition", "Source node", "Instance", "Process", "Invocation", "Domains", "Module"})
            ImGui::TableSetupColumn(label);
        ImGui::TableHeadersRow();
        for (const auto& plan : snapshot.plans) {
            for (std::size_t index = 0; index < plan.instances.size(); ++index) {
                const auto row = std::find_if(snapshot.provenance.begin(), snapshot.provenance.end(),
                    [&](const auto& item) { return item.runtime.instance_id == plan.instances[index]; });
                if (row == snapshot.provenance.end()) continue;
                const auto& origin = row->runtime;
                ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                ImGui::Text("%llu", static_cast<unsigned long long>(plan.clock.value()));
                ImGui::TableSetColumnIndex(1); ImGui::Text("%zu", index + 1);
                ImGui::TableSetColumnIndex(2); ImGui::Text("%llu", static_cast<unsigned long long>(origin.definition_id.value()));
                ImGui::TableSetColumnIndex(3);
                const auto instance = std::to_string(origin.instance_id.value()); ImGui::PushID(instance.c_str());
                const auto node = std::to_string(row->authored_node_id.value());
                ImGui::BeginDisabled(find_node(graph, row->authored_node_id) == nullptr);
                if (ImGui::Selectable(("Node " + node).c_str(), selected_node_ == node)) selected_node_ = node;
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    const auto path = [](const char* label, const feature::OwnerPath& owner) {
                        std::string value;
                        for (auto id : owner.nodes) { if (!value.empty()) value += '/'; value += std::to_string(id.value()); }
                        ImGui::Text("%s %s", label, value.empty() ? "(root)" : value.c_str());
                    };
                    path("Authored owner:", row->authored_owner_path); path("Runtime owner:", origin.owner_path);
                    ImGui::Text("Runtime node: %llu", static_cast<unsigned long long>(origin.node_id.value()));
                    ImGui::EndTooltip();
                }
                ImGui::PopID();
                ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(instance.c_str());
                ImGui::TableSetColumnIndex(5); ImGui::Text("%llu", static_cast<unsigned long long>(origin.process_id.value()));
                ImGui::TableSetColumnIndex(6); ImGui::Text("%llu", static_cast<unsigned long long>(origin.invocation_id.value()));
                ImGui::TableSetColumnIndex(7); const auto domains = domain_mask_name(origin.domains); ImGui::TextUnformatted(domains.c_str());
                ImGui::TableSetColumnIndex(8); ImGui::Text("%llu", static_cast<unsigned long long>(origin.owner_module.value()));
            }
        }
        ImGui::EndTable();
    }
}

void EditorFeatureGraphPanel::draw_render_graph_inspect(const feature::FeatureGraphDocument& graph, const Actions& actions) {
    ImGui::Separator(); ImGui::TextUnformatted("Captured RenderGraph");
    if (!actions.inspect_render_graph) { ImGui::TextDisabled("No Scene frame inspection is available."); return; }
    const auto observed = actions.inspect_render_graph();
    if (!observed || !observed.value) { ImGui::TextWrapped("%s", observed.result.message.c_str()); return; }
    const auto& snapshot = *observed.value;
    ImGui::Text("Successful frame %llu  revision %llu", static_cast<unsigned long long>(snapshot.frame_index),
                static_cast<unsigned long long>(snapshot.plan.revision));
    ImGui::Text("Resources %u  passes %u  culled %u  batches %u  barriers %u", snapshot.compilation.resource_count,
                snapshot.compilation.pass_count, snapshot.compilation.culled_pass_count, snapshot.compilation.batch_count, snapshot.compilation.barrier_count);
    const auto active = actions.inspect_execution ? actions.inspect_execution(graph.asset_uuid)
                                                 : foundation::ResultValue<runtime::FeatureRuntimeExecutionSnapshot>{foundation::not_ready("No published root"), {}};
    if (ImGui::BeginTable("FeatureGraphRenderGraph", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (const auto* label : {"Order / pass", "Batch", "Barriers", "Feature", "Source", "Invocation", "Operation"}) ImGui::TableSetupColumn(label);
        ImGui::TableHeadersRow();
        for (auto pass_index : snapshot.plan.topological_order) {
            if (pass_index >= snapshot.plan.pass_plans.size()) continue;
            const auto& pass = snapshot.plan.pass_plans[pass_index];
            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::Text("%u / %u", pass.topological_index, pass.pass.index);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%u", pass.batch_index);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%zu", pass.barriers.size());
            ImGui::TableSetColumnIndex(3);
            if (!pass.feature_work) { ImGui::TextDisabled("Frame infrastructure"); continue; }
            const auto& work = *pass.feature_work;
            ImGui::Text("%llu / module %llu", static_cast<unsigned long long>(work.feature_definition_id.value()), static_cast<unsigned long long>(work.owner_module.value()));
            const runtime::FeatureRuntimeExecutionSnapshot* source = nullptr;
            const runtime::FeatureRuntimeExecutionProvenance* attribution = nullptr;
            for (const auto& candidate : snapshot.sources) {
                if (candidate.generation != work.invocation.generation) continue;
                const auto row = std::find_if(candidate.provenance.begin(), candidate.provenance.end(), [&](const auto& item) {
                    return item.runtime.instance_id == work.instance_id && item.runtime.node_id == work.node_id &&
                           item.runtime.definition_id == work.feature_definition_id && item.runtime.owner_module == work.owner_module;
                });
                if (row != candidate.provenance.end()) { source = &candidate; attribution = &*row; break; }
            }
            ImGui::TableSetColumnIndex(4);
            if (source && attribution) {
                ImGui::PushID(static_cast<int>(pass_index));
                const auto node = std::to_string(attribution->authored_node_id.value());
                const auto label = "Node " + node;
                const bool current = active && active.value.generation == source->generation;
                ImGui::BeginDisabled(!current || source->asset_uuid != graph.asset_uuid || find_node(graph, attribution->authored_node_id) == nullptr);
                if (ImGui::Selectable(label.c_str())) selected_node_ = node;
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip(); ImGui::TextUnformatted(source->asset_uuid.c_str());
                    ImGui::TextUnformatted(source->source_path.generic_string().c_str()); ImGui::EndTooltip();
                }
                ImGui::PopID();
            } else ImGui::TextDisabled("Unattributed infrastructure Feature");
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%llu / attempt %llu", static_cast<unsigned long long>(work.invocation.invocation_id.value()), static_cast<unsigned long long>(work.invocation.attempt_id.value()));
            ImGui::TableSetColumnIndex(6); ImGui::Text("%u / %s", work.operation_index, work.kind == render_graph::RenderWorkItemKind::kExecute ? "execute" : "prepare");
        }
        ImGui::EndTable();
    }
    if (ImGui::TreeNode("Queue dependencies")) {
        for (const auto& dependency : snapshot.plan.queue_dependencies)
            ImGui::BulletText("Batch %u -> %u  queues %u -> %u  hazards %u", dependency.source_batch, dependency.destination_batch,
                static_cast<unsigned>(dependency.source_queue), static_cast<unsigned>(dependency.destination_queue), dependency.resource_hazard_count);
        ImGui::TreePop();
    }
}

void EditorFeatureGraphPanel::draw_runtime_inspect(const feature::FeatureGraphDocument& graph,
                                                        const Actions& actions) {
    feature::FeatureNodeId node{};
    if (!actions.inspect_node || !parse_numeric_id(selected_node_, node)) return;
    // This value owns its state and invocation observations; no worker/DLL/GPU pointers are cached by the panel.
    const auto observed = actions.inspect_node(graph.asset_uuid, node);
    ImGui::Separator();
    ImGui::TextUnformatted("Runtime Inspect");
    if (!observed) {
        ImGui::TextDisabled("%s", observed.result.message.c_str());
        return;
    }
    const auto& snapshot = observed.value;
    ImGui::Text("Generation %llu  asset %s  node %llu",
                static_cast<unsigned long long>(snapshot.generation.value()), snapshot.asset_uuid.c_str(),
                static_cast<unsigned long long>(snapshot.node_id.value()));
    if (!snapshot.source_path.empty())
        ImGui::TextDisabled("Source: %s", snapshot.source_path.generic_string().c_str());
    for (const auto& owner : snapshot.owners) {
        const auto owner_id = std::to_string(owner.owner.instance_id.value());
        ImGui::PushID(owner_id.c_str());
        ImGui::Text("Feature %llu  instance %s  state version %llu  payload %zu bytes",
                    static_cast<unsigned long long>(owner.definition_id.value()), owner_id.c_str(),
                    static_cast<unsigned long long>(owner.state.version.value()), owner.state.payload.size());
        const auto domains = domain_mask_name(owner.contract.domains);
        ImGui::TextDisabled("Module %llu  contract clock %llu  declared domains %s",
            static_cast<unsigned long long>(owner.owner_module.value()), static_cast<unsigned long long>(owner.contract.clock.id.value()), domains.c_str());
        std::string owner_path;
        for (auto id : owner.owner_path.nodes) { if (!owner_path.empty()) owner_path += '/'; owner_path += std::to_string(id.value()); }
        ImGui::TextDisabled("Runtime OwnerPath %s", owner_path.empty() ? "(root)" : owner_path.c_str());
        for (const auto& open : owner.invocations.open_attempts) {
            ImGui::Text("Invocation %llu  attempt %llu  %s",
                static_cast<unsigned long long>(open.attempt.token.invocation_id.value()),
                static_cast<unsigned long long>(open.attempt.token.attempt_id.value()),
                open.committable ? "committable" : "retired");
        }
        if (owner.invocations.last_committed) {
            const auto& commit = *owner.invocations.last_committed;
            ImGui::Text("Committed invocation %llu  state %llu",
                static_cast<unsigned long long>(commit.attempt.token.invocation_id.value()),
                static_cast<unsigned long long>(commit.state_version.value()));
        }
        if (owner.invocations.last_failed) {
            const auto& failure = *owner.invocations.last_failed;
            ImGui::Text("Failed invocation %llu  stage %u: %s",
                static_cast<unsigned long long>(failure.attempt.token.invocation_id.value()),
                static_cast<unsigned>(failure.stage), failure.result.message.c_str());
        }
        if (owner.invocations.retry) {
            ImGui::Text("Retry attempt %u  due tick %llu", owner.invocations.retry->next_attempt,
                static_cast<unsigned long long>(owner.invocations.retry->due_tick));
        }
        for (const auto& process : owner.processes) {
            const auto process_id = std::to_string(process.id.value());
            ImGui::PushID(process_id.c_str());
            ImGui::Text("Process %s  revision %llu  status %u  state %s", process_id.c_str(),
                static_cast<unsigned long long>(process.revision.value()),
                static_cast<unsigned>(process.status), process.current_state.c_str());
            ImGui::BeginDisabled(!actions.control_process || actions.publication_pending);
            const auto control = [&](const char* label, runtime::RuntimeProcessAction action) {
                if (ImGui::SmallButton(label)) {
                    const runtime::RuntimeProcessCommand command{snapshot.generation, snapshot.asset_uuid,
                        snapshot.node_id, owner.owner, process.id, process.revision, action};
                    const auto result = actions.control_process(command);
                    last_action_ = result ? std::string(label) + " succeeded" : result.message;
                }
            };
            control("Suspend", runtime::RuntimeProcessAction::kSuspend); ImGui::SameLine();
            control("Resume", runtime::RuntimeProcessAction::kResume); ImGui::SameLine();
            control("Cancel", runtime::RuntimeProcessAction::kCancel);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::PopID();
    }
}

void EditorFeatureGraphPanel::draw_world_binding_editor(const feature::FeatureGraphDocument& graph,
                                                        const Actions& actions) {
    if (!actions.selected_world_target_label && !actions.bind_world_command && !actions.world_binding_status)
        return;
    ImGui::Separator();
    ImGui::TextUnformatted("Scene World Route");
    const std::string target = actions.selected_world_target_label ? actions.selected_world_target_label() : std::string{};
    if (target.empty())
        ImGui::TextDisabled("Select a Scene entity in Hierarchy.");
    else
        ImGui::Text("Hierarchy target: %s", target.c_str());

    feature::FeatureNodeId selected{};
    const bool valid_selection = parse_numeric_id(selected_node_, selected) && find_node(graph, selected) != nullptr;
    if (!valid_selection) {
        ImGui::TextDisabled("Select a published Feature node to inspect its World route.");
        return;
    }
    const std::string status = actions.world_binding_status
        ? actions.world_binding_status(graph.asset_uuid, selected) : std::string{};
    if (!status.empty()) {
        ImGui::TextWrapped("%s", status.c_str());
        ImGui::BeginDisabled(!actions.world_binding_editable || !actions.clear_world_command);
        if (ImGui::Button("Remove selected node World route")) {
            const auto result = actions.clear_world_command(graph.asset_uuid, selected);
            last_action_ = result ? "Scene World route saved" : result.message;
        }
        ImGui::EndDisabled();
    } else {
        ImGui::BeginDisabled(!actions.world_binding_editable || target.empty() || !actions.bind_world_command);
        if (ImGui::Button("Bind selected node to selected Scene entity")) {
            const auto result = actions.bind_world_command(graph.asset_uuid, selected);
            last_action_ = result ? "Scene World route saved" : result.message;
        }
        ImGui::EndDisabled();
    }
    if (!last_action_.empty()) ImGui::TextDisabled("%s", last_action_.c_str());
}

void EditorFeatureGraphPanel::draw_parameters(EditorFeatureGraphDocument& document,
                                               const feature::FeatureCatalog& catalog,
                                               const feature::AssetKindResolver& resolver) {
    feature::FeatureNodeId selected{};
    if (!parse_numeric_id(selected_node_, selected)) return;
    const auto snapshot = document.snapshot_owner();
    const auto* node = find_node(*snapshot, selected);
    if (!node) return;
    const auto* entry = find_entry(catalog, node->definition_id);
    if (!entry || entry->version != node->definition_version || !entry->contract.definition_id.is_valid()) {
        ImGui::TextDisabled("Parameter editing requires the matching module contract."); return;
    }
    if (entry->contract.parameters.empty()) return;
    ImGui::TextUnformatted("Feature parameters");
    if (parameter_snapshot_ != snapshot || parameter_node_ != selected ||
        parameter_revision_ != document.revision() ||
        parameter_catalog_digest_ != catalog.contract_digest()) {
        auto draft_schema = entry->contract.parameters;
        for (auto& parameter : draft_schema) parameter.required = false; // A draft may still need required values.
        const auto records = feature::decode_feature_parameters(node->parameter_blob, draft_schema);
        if (!records) { ImGui::TextWrapped("%s", records.result.message.c_str()); return; }
        parameter_draft_.clear(); opaque_parameters_.clear();
        for (const auto& parameter : entry->contract.parameters) {
            const auto record = std::find_if(records.value.begin(), records.value.end(), [&](const auto& item) { return item.id == parameter.id; });
            if (!parameter.metadata) {
                if (record != records.value.end()) opaque_parameters_.push_back(*record);
                continue;
            }
            ParameterDraft draft{parameter.id, {}};
            if (record != records.value.end()) {
                const auto decoded = feature::deserialize_parameter_value(record->payload, parameter.metadata->type);
                if (!decoded) { ImGui::TextWrapped("%s", decoded.result.message.c_str()); return; }
                draft.value = decoded.value;
            }
            parameter_draft_.push_back(std::move(draft));
        }
        parameter_snapshot_ = snapshot;
        parameter_node_ = selected; parameter_revision_ = document.revision();
        parameter_catalog_digest_ = catalog.contract_digest();
    }
    ImGui::Checkbox("Show expert parameters", &show_expert_parameters_);
    std::vector<const feature::ParameterContract*> ordered;
    for (const auto& parameter : entry->contract.parameters) ordered.push_back(&parameter);
    std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
        if (a->metadata.has_value() != b->metadata.has_value()) return a->metadata.has_value();
        if (!a->metadata) return a->id < b->id;
        if (a->metadata->ui_group != b->metadata->ui_group) return a->metadata->ui_group < b->metadata->ui_group;
        if (a->metadata->ui_order != b->metadata->ui_order) return a->metadata->ui_order < b->metadata->ui_order;
        return a->id < b->id;
    });
    std::string group;
    for (const auto* parameter : ordered) {
        if (!parameter->metadata) { ImGui::TextDisabled("Parameter %llu uses an opaque Feature payload.", static_cast<unsigned long long>(parameter->id.value())); continue; }
        const auto& metadata = *parameter->metadata;
        if (metadata.expert_only && !show_expert_parameters_) continue;
        if (metadata.ui_group != group) { group = metadata.ui_group; if (!group.empty()) ImGui::TextDisabled("%s", group.c_str()); }
        auto draft = std::find_if(parameter_draft_.begin(), parameter_draft_.end(), [&](const auto& item) { return item.id == parameter->id; });
        if (draft == parameter_draft_.end()) continue;
        const auto id = std::to_string(parameter->id.value());
        ImGui::PushID(id.c_str());
        const auto label = metadata.display_name.empty() ? id : metadata.display_name;
        ImGui::TextUnformatted(label.c_str());
        if (!metadata.description.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", metadata.description.c_str());
        if (parameter->required) { ImGui::SameLine(); ImGui::TextDisabled("Required"); }
        if (const auto* asset = std::get_if<feature::AssetConstraint>(&metadata.constraint); asset && asset->kind)
            ImGui::TextDisabled("Asset kind: %s", parameter_asset_kind_name(*asset->kind));
        if (draft->value) {
            draw_parameter_value("##value", *draft->value, metadata);
            if (const auto* range = std::get_if<feature::NumericRange>(&metadata.constraint))
                ImGui::TextDisabled("Range %.9g to %.9g; step %.9g", range->min, range->max, range->step);
            const auto valid = feature::validate_parameter_value(*draft->value, metadata);
            if (!valid) ImGui::TextWrapped("%s", valid.message.c_str());
        } else {
            if (ImGui::SmallButton("Set value")) {
                const auto initial = try_initial_parameter_value(metadata);
                if (initial) draft->value = initial.value;
                else last_action_ = initial.result.message;
            }
            if (!draft->value) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Enter manually")) draft->value = initial_parameter_value(metadata);
            }
        }
        if (metadata.default_value) {
            if (ImGui::SmallButton("Use default")) draft->value = *metadata.default_value;
        } else if (!parameter->required && draft->value) {
            if (ImGui::SmallButton("Unset")) draft->value.reset();
        }
        ImGui::PopID();
    }
    if (ImGui::Button("Apply parameters")) {
        auto records = opaque_parameters_;
        auto result = foundation::Result::success();
        for (const auto& draft : parameter_draft_) {
            if (!draft.value) continue;
            const auto schema = std::find_if(entry->contract.parameters.begin(), entry->contract.parameters.end(), [&](const auto& parameter) { return parameter.id == draft.id; });
            const auto payload = feature::serialize_parameter_value(*draft.value, schema->metadata->type);
            if (!payload) { result = payload.result; break; }
            records.push_back({draft.id, schema->version, payload.value});
        }
        if (result) result = document.set_parameters(selected, records, catalog, resolver);
        last_action_ = result ? "Feature parameters updated" : result.message;
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard parameter edits")) parameter_revision_ = UINT64_MAX;
    ImGui::Separator();
}

}  // namespace aeg::application
