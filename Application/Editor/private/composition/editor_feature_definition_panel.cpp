#include "editor_feature_definition_panel.hpp"
#include "editor_feature_presentation.hpp"
#include "editor_node_contract_storage.hpp"
#include "editor_feature_implementation_resolver.hpp"
#include "editor_parameter_widget.hpp"
#include "project_context.hpp"
#include <algorithm>
#include <exception>
#include <variant>

namespace aeg::application {
namespace {
foundation::ResultValue<feature::ModuleIdentity> project_definition_owner(
    const std::filesystem::path& project_root, const std::filesystem::path& selected_project_file) {
    const auto file = resolve_editor_project_file(selected_project_file.empty() ? project_root : selected_project_file);
    if (!file) return {file.result, {}};
    if (file.value.empty())
        return {foundation::invalid_state("Select a project file before creating a Feature"), {}};
    std::error_code error;
    if (!std::filesystem::equivalent(project_root, file.value.parent_path(), error) || error)
        return {foundation::invalid_argument("Selected project file does not belong to the current project root"), {}};
    const auto config = project::ProjectFileCodec{}.load(file.value);
    if (!config) return {config.result, {}};
    if (config.value.config.module.id.empty())
        return {foundation::invalid_state("Project module.id is required before authoring Features"), {}};
    const feature::ModuleIdentity owner{config.value.config.module.id};
    if (!owner.is_valid()) return {foundation::invalid_argument("Project module.id is invalid"), {}};
    return {foundation::Result::success(), owner};
}

template<class Id> void edit_id(const char* label, Id& id) {
    uint64_t value = id.value();
    if (ImGui::InputScalar(label, ImGuiDataType_U64, &value)) id = Id{value};
}
template<class Enum> void edit_enum(const char* label, Enum& value, const char* const* names, int count) {
    int index = static_cast<int>(value);
    if (ImGui::Combo(label, &index, names, count)) value = static_cast<Enum>(index);
}
void edit_version(const char* label, feature::Version& version) {
    ImGui::PushID(label);
    ImGui::InputScalar("Major", ImGuiDataType_U16, &version.major);
    ImGui::InputScalar("Minor", ImGuiDataType_U16, &version.minor);
    ImGui::InputScalar("Patch", ImGuiDataType_U16, &version.patch);
    ImGui::PopID();
}
void draw_numeric_range(feature::NumericRange& range) {
    ImGui::InputDouble("Minimum", &range.min); ImGui::InputDouble("Maximum", &range.max);
    ImGui::InputDouble("Step", &range.step); ImGui::Checkbox("Normalized range", &range.normalized);
}
void draw_parameter_constraint(feature::ParameterMetadata& metadata) {
    if (metadata.type == feature::ParameterType::kBool) return;
    bool constrained = !std::holds_alternative<std::monostate>(metadata.constraint);
    if (ImGui::Checkbox("Constraint", &constrained)) {
        if (!constrained) metadata.constraint = {};
        else if (metadata.type <= feature::ParameterType::kDouble) metadata.constraint = feature::NumericRange{};
        else if (metadata.type == feature::ParameterType::kEnum) metadata.constraint = feature::EnumChoices{};
        else if (metadata.type == feature::ParameterType::kString) metadata.constraint = feature::StringPattern{};
        else if (metadata.type == feature::ParameterType::kAssetReference) metadata.constraint = feature::AssetConstraint{};
        else metadata.constraint = feature::VectorConstraint{};
    }
    if (auto* range = std::get_if<feature::NumericRange>(&metadata.constraint)) draw_numeric_range(*range);
    if (auto* choices = std::get_if<feature::EnumChoices>(&metadata.constraint)) {
        if (ImGui::Button("Add choice")) { choices->labels.emplace_back(); choices->descriptions.resize(choices->labels.size()); }
        for (std::size_t index = 0; index < choices->labels.size();) {
            ImGui::PushID(static_cast<int>(index)); draw_parameter_text("Choice", choices->labels[index]);
            if (choices->descriptions.size() < choices->labels.size()) choices->descriptions.resize(choices->labels.size());
            draw_parameter_text("Description", choices->descriptions[index]);
            if (ImGui::Button("Remove choice")) {
                choices->labels.erase(choices->labels.begin() + static_cast<std::ptrdiff_t>(index));
                choices->descriptions.erase(choices->descriptions.begin() + static_cast<std::ptrdiff_t>(index));
            } else ++index;
            ImGui::PopID();
        }
    }
    if (auto* pattern = std::get_if<feature::StringPattern>(&metadata.constraint)) {
        draw_parameter_text("Pattern", pattern->regex);
        ImGui::InputScalar("Minimum bytes", ImGuiDataType_U32, &pattern->min_length);
        ImGui::InputScalar("Maximum bytes", ImGuiDataType_U32, &pattern->max_length);
        ImGui::Checkbox("File path", &pattern->is_file_path); ImGui::Checkbox("Color string", &pattern->is_color);
    }
    if (auto* asset = std::get_if<feature::AssetConstraint>(&metadata.constraint)) {
        ImGui::Checkbox("Null asset allowed", &asset->optional);
        const char* kinds[] = {"Any", "Mesh", "Texture", "Material", "Scene", "Font", "FeatureGraph", "Shader"};
        int selected = asset->kind ? static_cast<int>(*asset->kind) + 1 : 0;
        if (ImGui::Combo("Asset kind", &selected, kinds, 8)) {
            if (selected == 0) asset->kind.reset();
            else asset->kind = static_cast<foundation::AssetKind>(selected - 1);
        }
    }
    if (auto* vector = std::get_if<feature::VectorConstraint>(&metadata.constraint)) {
        ImGui::Checkbox("Unit vector", &vector->normalized);
        bool range = vector->component_range.has_value();
        if (ImGui::Checkbox("Component range", &range)) {
            if (range) vector->component_range.emplace(); else vector->component_range.reset();
        }
        if (vector->component_range) draw_numeric_range(*vector->component_range);
    }
}

}

void EditorFeatureDefinitionPanel::draw(EditorFeatureGraphDocument& document,
    const std::filesystem::path& project_root, std::span<const feature::FeatureDefinitionId> compiled_features,
    const feature::AssetKindResolver& resolver, const std::filesystem::path& selected_project_file,
    const EditorIdeLauncher& launcher, std::span<const NodeDefinitionDraft> presentations, bool* workspace_open) {
    if (project_root.empty()) return;
    const auto project_identity = selected_project_file.empty() ? project_root : selected_project_file;
    if (project_identity_ != project_identity) {
        project_identity_ = project_identity;
        open_ = false; selected_ = {}; implementation_.reset(); implementation_status_.clear();
        parameter_migrations_.clear(); status_.clear(); saved_drafts_.clear(); diagnostics_.clear();
    }
    if (pending_open_) {
        ImGui::SetNextWindowCollapsed(false);
        ImGui::SetNextWindowFocus();
    }
    if (!ImGui::Begin("Feature Definitions", workspace_open)) { ImGui::End(); return; }
    const auto refresh_drafts = [&] {
        diagnostics_.clear(); saved_drafts_.clear();
        presentation_document_identity_ = document.identity_owner();
        const auto source = resolve_editor_source_directory(project_identity);
        if (!source) { diagnostics_.push_back(source.result.message); return; }
        const auto files = scan_editor_node_contracts(project_root, source.value, &diagnostics_);
        if (!files) { diagnostics_.push_back(files.result.message); return; }
        for (const auto& file : files.value) saved_drafts_.push_back(file.draft);
    };
    const auto locate_implementation = [&]() -> foundation::ResultValue<CustomNodeDefinitionTarget> {
        if (!selected_project_file.empty()) {
            const auto file = resolve_editor_project_file(selected_project_file);
            if (!file) return {file.result, {}};
            std::error_code error;
            if (file.value.empty() || !std::filesystem::equivalent(project_root, file.value.parent_path(), error) || error)
                return {foundation::invalid_argument("Implementation project file does not belong to the current project root"), {}};
        }
        return locate_feature_implementation(project_identity, draft_.entry.id);
    };
    const auto refresh_implementation = [&] {
        implementation_.reset();
        const auto target = locate_implementation();
        implementation_status_ = target ? std::string{} : target.result.message;
        if (target) implementation_ = target.value;
    };
    const auto open_definition = [&](feature::FeatureDefinitionId id) {
        selected_ = id;
        refresh_drafts();
        std::optional<NodeDefinitionDraft> candidate;
        const auto entry = std::find_if(document.catalog().entries().begin(), document.catalog().entries().end(),
            [&](const auto& value) { return value.definition_id == id; });
        if (entry != document.catalog().entries().end()) candidate = node_definition_from_catalog(*entry);
        const bool compiled = std::find(compiled_features.begin(), compiled_features.end(), id) != compiled_features.end();
        for (const auto& saved : saved_drafts_) {
            if (saved.entry.id != id) continue;
            if (compiled && entry != document.catalog().entries().end()) {
                // A newer unbuilt editor draft cannot replace the contract
                // displayed as compiled. Matching source labels are harmless.
                if (feature_presentation(*entry, std::span<const NodeDefinitionDraft>{&saved, 1})) {
                    candidate->name = saved.name; candidate->display_name = saved.display_name;
                    candidate->port_labels = saved.port_labels;
                }
                continue;
            }
            candidate = saved;
            if (!saved.generated) break;
        }
        if (!candidate) {
            open_ = false; implementation_.reset();
            status_ = "Feature Definition was not found in the current catalog or project drafts";
            return;
        }
        draft_ = std::move(*candidate);
        if (compiled) draft_.source = NodeContractSource::kCompiled;
        open_ = true; disconnect_ = false; parameter_migrations_.clear(); status_.clear();
        refresh_implementation();
    };
    if (pending_open_) {
        const auto id = *pending_open_;
        pending_open_.reset();
        open_definition(id);
    }
    if (ImGui::Button("New Feature")) {
        refresh_drafts();
        draft_ = {}; draft_.name = "Game.NewFeature"; draft_.display_name = "New Feature";
        static uint64_t sequence{};
        do { draft_.entry.id = feature::FeatureDefinitionId{draft_.name + std::to_string(++sequence)}; }
        while (std::any_of(document.catalog().entries().begin(), document.catalog().entries().end(),
            [&](const auto& entry) { return entry.definition_id == draft_.entry.id; }) ||
            std::any_of(saved_drafts_.begin(), saved_drafts_.end(),
                [&](const auto& draft) { return draft.entry.id == draft_.entry.id; }));
        draft_.entry.version = {1, 0, 0};
        const auto owner = project_definition_owner(project_root, selected_project_file);
        if (owner) draft_.entry.owner_module = owner.value;
        auto& c = draft_.entry.contract; c.definition_id = draft_.entry.id; c.version = draft_.entry.version;
        c.domains = feature::to_mask(feature::FeatureDomain::kLogic); c.scope.id = feature::ScopeId{"World"};
        c.clock = feature::make_periodic_clock_contract(feature::ClockId{"Simulation"}, 60, 8,
            feature::LateDataPolicy::kQueue, feature::DeadlinePolicy::kObserve, 250000, 0);
        c.permissions.version = 1;
        open_ = true; disconnect_ = false; parameter_migrations_.clear(); status_.clear();
        implementation_.reset(); implementation_status_ = "No project implementation has been generated for this Feature";
        if (!owner) status_ = owner.result.message;
    }
    if (ImGui::BeginCombo("Definition", selected_.is_valid() ? std::to_string(selected_.value()).c_str() : "Select")) {
        if (ImGui::IsWindowAppearing()) refresh_drafts();
        for (const auto& entry : document.catalog().entries()) {
            const auto label = feature_presentation_name(entry, presentations) + "###" + std::to_string(entry.definition_id.value());
            if (ImGui::Selectable(label.c_str(), selected_ == entry.definition_id)) selected_ = entry.definition_id;
        }
        for (const auto& draft : saved_drafts_) {
            if (std::any_of(document.catalog().entries().begin(), document.catalog().entries().end(),
                [&](const auto& entry) { return entry.definition_id == draft.entry.id; })) continue;
            ImGui::PushID(std::to_string(draft.entry.id.value()).c_str());
            if (ImGui::Selectable((draft.display_name + " (draft)").c_str(), selected_ == draft.entry.id)) selected_ = draft.entry.id;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Open") && selected_.is_valid()) {
        open_definition(selected_);
    }
    for (const auto& diagnostic : diagnostics_) ImGui::TextWrapped("%s", diagnostic.c_str());
    if (!open_) { ImGui::TextWrapped("%s", status_.c_str()); ImGui::End(); return; }
    const bool readonly = draft_.source != NodeContractSource::kEditorDraft;
    ImGui::BeginDisabled(readonly);
    draw_parameter_text("Name", draft_.name); draw_parameter_text("Display name", draft_.display_name);
    ImGui::Text("Definition %llu", static_cast<unsigned long long>(draft_.entry.id.value()));
    edit_id("Owner module", draft_.entry.owner_module);
    edit_version("Definition version", draft_.entry.version);
    auto& c = draft_.entry.contract; c.definition_id = draft_.entry.id; c.version = draft_.entry.version;
    const auto previous_structure = c.structure;
    const char* structures[] = {"Atomic", "Composite"}; edit_enum("Structure", c.structure, structures, 2);
    if (c.structure != previous_structure)
        c.clock.work_budget_ns = c.structure == feature::FeatureStructure::kComposite ? 0 : 250000;
    const char* domains[] = {"Logic", "Physics", "Render", "Audio", "Stream"};
    for (unsigned index = 0; index < 5; ++index) {
        const auto mask = feature::to_mask(static_cast<feature::FeatureDomain>(index)); bool selected = (c.domains & mask) != 0;
        if (ImGui::Checkbox(domains[index], &selected)) c.domains = static_cast<feature::FeatureDomainMask>(selected ? c.domains | mask : c.domains & ~mask);
        if (index < 4) ImGui::SameLine();
    }
    const char* executions[] = {"Call", "Process", "Continuous"}; edit_enum("Execution", c.execution, executions, 3);
    const char* states[] = {"Stateless", "Instance state", "Process state", "External snapshot"}; edit_enum("State", c.state, states, 4);
    if (feature::owns_state_payload(c.state)) {
        edit_version("State schema", c.state_payload.schema_version);
        ImGui::InputScalar("State byte limit", ImGuiDataType_U32, &c.state_payload.max_size);
    } else c.state_payload = {};
    ImGui::Checkbox("Has continuation", &c.has_continuation); ImGui::Checkbox("Allows child process", &c.allows_child_process);
    edit_id("Scope", c.scope.id); edit_id("Clock", c.clock.id);
    ImGui::InputScalar("Frequency Hz", ImGuiDataType_U32, &c.clock.frequency_hz);
    ImGui::InputScalar("Backlog", ImGuiDataType_U32, &c.clock.max_backlog);
    const char* late[] = {"Reject", "Use latest", "Queue", "Drop"}; edit_enum("Late data", c.clock.late_data_policy, late, 4);
    ImGui::TextUnformatted("Clock capability version");
    edit_version("Clock capability version", c.clock.capability_version);
    bool has_clock_timing = c.clock.timing.has_value();
    if (ImGui::Checkbox("Clock timing", &has_clock_timing)) {
        if (has_clock_timing) c.clock.timing.emplace();
        else c.clock.timing.reset();
    }
    if (c.clock.timing) {
        ImGui::InputScalar("Clock phase (ns)", ImGuiDataType_U64, &c.clock.timing->phase_ns);
        ImGui::InputScalar("Clock relative deadline (ns)", ImGuiDataType_U64, &c.clock.timing->relative_deadline_ns);
        const char* deadlines[] = {"Unspecified", "Observe", "Reject commit"};
        edit_enum("Clock deadline policy", c.clock.timing->deadline_policy, deadlines, 3);
    }
    ImGui::InputScalar("Clock work budget (ns)", ImGuiDataType_U64, &c.clock.work_budget_ns);
    ImGui::InputScalar("Capabilities", ImGuiDataType_U64, &c.permissions.capability_mask);
    ImGui::InputScalar("Permission version", ImGuiDataType_U32, &c.permissions.version);
    const char* failures[] = {"Stop", "Retry", "Compensate"}; edit_enum("Failure", c.failure, failures, 3);
    ImGui::InputScalar("Retry attempts", ImGuiDataType_U32, &c.retry.max_attempts);
    ImGui::InputScalar("Backoff ticks", ImGuiDataType_U32, &c.retry.backoff_ticks);
    if (ImGui::CollapsingHeader("Ports")) {
        ImGui::PushID("Ports");
        if (ImGui::Button("Add port")) {
            uint64_t id = 1;
            while (std::find(draft_.retired_port_ids.begin(), draft_.retired_port_ids.end(), id) != draft_.retired_port_ids.end() ||
                std::any_of(c.ports.begin(), c.ports.end(), [id](const auto& port) { return port.id.value() == id; })) ++id;
            c.ports.push_back({feature::PortId{id}, feature::PortDirection::kInput, feature::ValueSchemaId{"Game.Value"}, 1});
        }
        for (std::size_t index = 0; index < c.ports.size();) {
            ImGui::PushID(static_cast<int>(index)); auto& port = c.ports[index];
            ImGui::Text("Port %llu", static_cast<unsigned long long>(port.id.value())); edit_id("Schema", port.schema);
            auto label = std::find_if(draft_.port_labels.begin(), draft_.port_labels.end(), [&](const auto& value) { return value.id == port.id; });
            if (label == draft_.port_labels.end()) label = draft_.port_labels.insert(draft_.port_labels.end(), {port.id, {}});
            draw_parameter_text("Label", label->name);
            ImGui::InputScalar("Version", ImGuiDataType_U32, &port.version);
            const char* directions[] = {"Input", "Output"}; edit_enum("Direction", port.direction, directions, 2);
            if (ImGui::Button("Remove port")) {
                draft_.retired_port_ids.push_back(port.id.value());
                std::erase_if(draft_.port_labels, [&](const auto& label) { return label.id == port.id; });
                c.ports.erase(c.ports.begin() + static_cast<std::ptrdiff_t>(index));
            } else ++index;
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    if (ImGui::CollapsingHeader("Parameters")) {
        ImGui::PushID("Parameters");
        if (ImGui::Button("Add parameter")) {
            uint64_t id = 1;
            while (std::find(draft_.retired_parameter_ids.begin(), draft_.retired_parameter_ids.end(), id) != draft_.retired_parameter_ids.end() ||
                std::any_of(c.parameters.begin(), c.parameters.end(), [id](const auto& parameter) { return parameter.id.value() == id; })) ++id;
            feature::ParameterMetadata metadata{}; metadata.type = feature::ParameterType::kDouble; metadata.default_value = 0.0;
            c.parameters.push_back({feature::ParameterId{id}, 1, 8, false, metadata});
        }
        for (std::size_t index = 0; index < c.parameters.size();) {
            ImGui::PushID(static_cast<int>(index)); auto& parameter = c.parameters[index];
            ImGui::Text("Parameter %llu", static_cast<unsigned long long>(parameter.id.value()));
            ImGui::InputScalar("Version", ImGuiDataType_U32, &parameter.version);
            ImGui::InputScalar("Byte limit", ImGuiDataType_U32, &parameter.max_size); ImGui::Checkbox("Required", &parameter.required);
            if (parameter.metadata) {
                auto& metadata = *parameter.metadata;
                draw_parameter_text("Label", metadata.display_name);
                draw_parameter_text("Description", metadata.description); draw_parameter_text("Group", metadata.ui_group);
                ImGui::InputScalar("Order", ImGuiDataType_U32, &metadata.ui_order); ImGui::Checkbox("Expert only", &metadata.expert_only);
                const char* types[] = {"Bool","Int32","Int64","UInt32","UInt64","Float","Double","String","Enum","Asset","Vector2","Vector3","Vector4","Color"};
                const auto before = metadata.type; edit_enum("Type", metadata.type, types, 14);
                if (before != metadata.type) { metadata.constraint = {}; metadata.default_value.reset(); }
                draw_parameter_constraint(metadata);
                bool has_default = metadata.default_value.has_value();
                if (ImGui::Checkbox("Default", &has_default)) {
                    if (has_default) {
                        const auto initial = try_initial_parameter_value(metadata);
                        if (initial) metadata.default_value = initial.value;
                        else status_ = initial.result.message;
                    } else metadata.default_value.reset();
                }
                if (!metadata.default_value && ImGui::SmallButton("Enter default manually"))
                    metadata.default_value = initial_parameter_value(metadata);
                if (metadata.default_value) draw_parameter_value("Default value", *metadata.default_value, metadata);
            }
            if (ImGui::Button("Remove parameter")) {
                draft_.retired_parameter_ids.push_back(parameter.id.value()); c.parameters.erase(c.parameters.begin() + static_cast<std::ptrdiff_t>(index));
            } else ++index;
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    if (ImGui::CollapsingHeader("Children")) {
        ImGui::PushID("Children");
        if (ImGui::Button("Add child")) c.children.push_back({});
        for (std::size_t index = 0; index < c.children.size();) {
            ImGui::PushID(static_cast<int>(index)); edit_id("Child definition", c.children[index]);
            if (ImGui::Button("Remove child")) c.children.erase(c.children.begin() + static_cast<std::ptrdiff_t>(index)); else ++index;
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    if (ImGui::CollapsingHeader("Side effects")) {
        ImGui::PushID("SideEffects");
        if (ImGui::Button("Add side effect")) c.side_effects.push_back({});
        for (std::size_t index = 0; index < c.side_effects.size();) {
            ImGui::PushID(static_cast<int>(index)); auto& effect = c.side_effects[index];
            edit_id("Effect ID", effect.id); ImGui::InputScalar("Version", ImGuiDataType_U32, &effect.version);
            const char* kinds[] = {"State delta", "Intent", "Event", "Command", "Receipt", "External IO"};
            const char* commits[] = {"Owner boundary", "Outbox", "External owner"};
            edit_enum("Kind", effect.kind, kinds, 6); edit_enum("Commit", effect.commit, commits, 3);
            if (ImGui::Button("Remove side effect")) c.side_effects.erase(c.side_effects.begin() + static_cast<std::ptrdiff_t>(index)); else ++index;
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    if (ImGui::CollapsingHeader("Composite port mappings")) {
        ImGui::PushID("Mappings");
        if (ImGui::Button("Add mapping")) c.child_port_mappings.push_back({});
        for (std::size_t index = 0; index < c.child_port_mappings.size();) {
            ImGui::PushID(static_cast<int>(index)); auto& mapping = c.child_port_mappings[index];
            edit_id("Parent port", mapping.parent_port); edit_id("Child definition", mapping.child_definition); edit_id("Child port", mapping.child_port);
            const char* directions[] = {"Input", "Output"}; edit_enum("Direction", mapping.direction, directions, 2);
            if (ImGui::Button("Remove mapping")) c.child_port_mappings.erase(c.child_port_mappings.begin() + static_cast<std::ptrdiff_t>(index)); else ++index;
            ImGui::PopID();
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    if (readonly) ImGui::TextWrapped("Compiled contracts are read-only. Edit the Feature source in your IDE.");
    if (implementation_) {
        const auto source_text = implementation_->path.generic_u8string();
        const std::string source{source_text.begin(), source_text.end()};
        ImGui::TextWrapped("Implementation: %s:%u (%s)", source.c_str(), implementation_->line,
                           implementation_->symbol.c_str());
        const char* ide_names[] = {"Auto detect", "Visual Studio Code", "CLion", "Visual Studio"};
        edit_enum("IDE", ide_kind_, ide_names, 4);
        if (ImGui::Button("Open Implementation in IDE")) {
            // Re-resolve on the explicit action: a cached target is not
            // authority to launch a deleted, replaced or escaped source.
            const auto target = locate_implementation();
            if (!target) {
                implementation_.reset(); implementation_status_ = target.result.message; status_ = target.result.message;
            } else {
                implementation_ = target.value;
                const EditorIdeLaunchRequest request{project_root, target.value.path, target.value.line, 1, ide_kind_};
                try {
                    const auto result = launcher ? launcher(request) : launch_editor_ide(request);
                    status_ = result ? "IDE launch requested" : result.message;
                } catch (const std::exception& error) {
                    status_ = std::string{"IDE launch failed: "} + error.what();
                } catch (...) {
                    status_ = "IDE launch failed with an unknown exception";
                }
            }
        }
    } else {
        ImGui::TextWrapped("No project implementation available: %s", implementation_status_.c_str());
    }
    service_.set_project_documents({&document});
    const auto preview = service_.preview(document, draft_);
    if (!preview) ImGui::TextWrapped("%s", preview.result.message.c_str());
    else if (preview.value.requires_confirmation) ImGui::Checkbox("Confirm affected connections", &disconnect_);
    const auto previous = std::find_if(document.catalog().entries().begin(), document.catalog().entries().end(),
        [&](const auto& entry) { return entry.definition_id == draft_.entry.id; });
    std::erase_if(parameter_migrations_, [&](const auto& migration) {
        if (previous == document.catalog().entries().end()) return true;
        const auto old = std::find_if(previous->contract.parameters.begin(), previous->contract.parameters.end(),
            [&](const auto& parameter) { return parameter.id == migration.id; });
        const auto next = std::find_if(c.parameters.begin(), c.parameters.end(),
            [&](const auto& parameter) { return parameter.id == migration.id; });
        return old == previous->contract.parameters.end() || (next != c.parameters.end() && *next == *old);
    });
    if (previous != document.catalog().entries().end()) {
        bool heading = false;
        ImGui::BeginDisabled(readonly);
        ImGui::PushID("ParameterMigrations");
        for (const auto& old_parameter : previous->contract.parameters) {
            const auto next = std::find_if(c.parameters.begin(), c.parameters.end(),
                [&](const auto& parameter) { return parameter.id == old_parameter.id; });
            if (next != c.parameters.end() && *next == old_parameter) continue;
            if (!heading) {
                ImGui::TextUnformatted("Parameter value migration");
                ImGui::TextWrapped("Keep preserves current effective values, including previous defaults. "
                    "Convert requires an exact value conversion. Reset discards the old value and uses the new default.");
                heading = true;
            }
            ImGui::PushID(std::to_string(old_parameter.id.value()).c_str());
            ImGui::Text("Parameter %llu: version %u -> %u", static_cast<unsigned long long>(old_parameter.id.value()),
                        old_parameter.version, next == c.parameters.end() ? 0u : next->version);
            const auto selected = std::find_if(parameter_migrations_.begin(), parameter_migrations_.end(),
                [&](const auto& migration) { return migration.id == old_parameter.id; });
            int action = selected == parameter_migrations_.end() ? 0 :
                selected->action == NodeParameterMigrationAction::kConvertValue ? 1 : 2;
            const char* actions[] = {"Keep compatible values", "Convert value",
                next == c.parameters.end() ? "Discard value" : "Reset to new default"};
            if (ImGui::Combo("Action", &action, actions, 3)) {
                std::erase_if(parameter_migrations_, [&](const auto& migration) { return migration.id == old_parameter.id; });
                if (action) parameter_migrations_.push_back({old_parameter.id,
                    action == 1 ? NodeParameterMigrationAction::kConvertValue : NodeParameterMigrationAction::kResetValue});
            }
            const bool incompatible = next == c.parameters.end() ||
                old_parameter.metadata.has_value() != next->metadata.has_value() ||
                (old_parameter.metadata && next->metadata && old_parameter.metadata->type != next->metadata->type) ||
                (!old_parameter.metadata && next->version != old_parameter.version);
            if (incompatible)
                ImGui::TextWrapped("Changing this schema requires an explicit migration, a newer parameter version "
                    "for a retained parameter, and a newer Definition major version.");
            ImGui::PopID();
        }
        ImGui::PopID();
        ImGui::EndDisabled();
    }
    const auto save_definition = [&](bool generate) -> foundation::ResultValue<NodeDefinitionSaveResult> {
        const auto owner = project_definition_owner(project_root, selected_project_file);
        if (!owner) return {owner.result, {}};
        if (draft_.entry.owner_module != owner.value)
            return {foundation::invalid_argument("Feature owner does not match the selected project module.id"), {}};
        const auto project_file = resolve_editor_project_file(project_identity);
        if (!project_file) return {project_file.result, {}};
        return service_.save(document, {draft_, generate, generate && disconnect_, resolver,
                                       project_file.value, parameter_migrations_}, project_root);
    };
    ImGui::BeginDisabled(readonly || !preview);
    if (ImGui::Button("Save Draft")) {
        const auto result = save_definition(false);
        status_ = result ? "Feature draft saved" : result.result.message;
        if (result) refresh_drafts();
    }
    ImGui::SameLine();
    if (ImGui::Button("Save and Generate")) {
        const auto result = save_definition(true);
        status_ = result ? "Feature contract and source generated" : result.result.message;
        if (result) { draft_.generated = true; parameter_migrations_.clear(); disconnect_ = false; refresh_drafts(); refresh_implementation(); }
    }
    ImGui::EndDisabled();
    ImGui::TextWrapped("%s", status_.c_str());
    if (ImGui::Button("Close definition")) open_ = false;
    ImGui::End();
}
}  // namespace aeg::application
