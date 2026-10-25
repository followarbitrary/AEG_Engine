#include "editor_feature_definition_panel.hpp"
#include "editor_feature_definition_fixture.hpp"
#include "editor_node_contract_storage.hpp"
#include "editor_parameter_widget.hpp"
#include "project_context.hpp"
#include <imgui_internal.h>
#include <cmath>
#include <iostream>
#include <limits>

namespace {
bool write_ui_project(const std::filesystem::path& root, const std::filesystem::path& file,
                      const char* module_id = "com.aeg.editor.fixture") {
    std::error_code error;
    std::filesystem::create_directories(root, error);
    aeg::application::project::ProjectConfig config{};
    config.name = "Feature authoring";
    config.module.id = module_id;
    config.module.target = "DifferentCMakeTarget";
    return !error && aeg::application::project::ProjectFileCodec{}.save(root / file, config);
}

enum class OwnerProjectSetup {
    kSelected, kRootSingle, kRootAmbiguous, kNoProject, kMissingSelected, kInvalidSelected,
    kNoModule, kEmptyModule, kCrossRoot, kChangedAfterNew, kDeletedAfterNew
};

bool first_feature_uses_selected_project_owner(const std::filesystem::path& root, bool foreign_catalog,
                                              OwnerProjectSetup setup = OwnerProjectSetup::kSelected) {
    using namespace aeg;
    constexpr const char* module_id = "com.aeg.selected.authoring";
    if (!write_ui_project(root, "Project.aegproj", "com.aeg.unselected") ||
        !write_ui_project(root, "Selected.AEGPROJ", module_id)) return false;
    auto selected_project = root / "Selected.AEGPROJ";
    std::error_code error;
    const bool expected_success = setup == OwnerProjectSetup::kSelected || setup == OwnerProjectSetup::kRootSingle;
    if (setup == OwnerProjectSetup::kRootSingle || setup == OwnerProjectSetup::kNoProject)
        std::filesystem::remove(root / "Project.aegproj", error);
    if (setup == OwnerProjectSetup::kRootSingle || setup == OwnerProjectSetup::kRootAmbiguous)
        selected_project.clear();
    if (setup == OwnerProjectSetup::kNoProject || setup == OwnerProjectSetup::kMissingSelected)
        std::filesystem::remove(root / "Selected.AEGPROJ", error);
    if (setup == OwnerProjectSetup::kNoProject) selected_project.clear();
    if (setup == OwnerProjectSetup::kInvalidSelected) std::ofstream(selected_project) << "not valid TOML";
    if (setup == OwnerProjectSetup::kEmptyModule || setup == OwnerProjectSetup::kNoModule) {
        if (!write_ui_project(root, "Selected.AEGPROJ", "")) return false;
        if (setup == OwnerProjectSetup::kNoModule) {
            auto bytes = tests::read_text(selected_project);
            const auto begin = bytes.find("[module]");
            if (begin == std::string::npos) return false;
            const auto end = bytes.find("\n[", begin);
            bytes.erase(begin, end == std::string::npos ? bytes.size() - begin : end - begin + 1);
            std::ofstream(selected_project, std::ios::binary) << bytes;
        }
    }
    if (setup == OwnerProjectSetup::kCrossRoot) {
        if (!write_ui_project(root / "other-project", "Other.aegproj", module_id)) return false;
        selected_project = root / "other-project/Other.aegproj";
    }
    if (error) return false;
    feature::FeatureCatalog catalog;
    if (foreign_catalog) {
        auto foreign = tests::feature_definition();
        foreign.owner_module = feature::ModuleIdentity{"com.aeg.renderer"};
        if (!catalog.export_definition(foreign)) return false;
    }
    feature::FeatureGraphDocument graph{};
    graph.asset_uuid = "10000000-0000-4000-8000-000000000042";
    graph.contract_digest = catalog.contract_digest();
    application::EditorFeatureGraphDocument document{graph, catalog};
    const auto before = document.snapshot_owner();
    struct ContextOwner {
        ImGuiContext* value{ImGui::CreateContext()};
        ~ContextOwner() { ImGui::DestroyContext(value); }
    } context_owner;
    auto* context = context_owner.value;
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.DisplaySize = {1200, 3000}; io.DeltaTime = 1.0f / 60.0f;
    io.ConfigErrorRecoveryEnableAssert = false; io.ConfigErrorRecoveryEnableDebugLog = false;
    io.ConfigErrorRecoveryEnableTooltip = false;
    std::vector<std::string> ui_errors;
    context->ErrorCallbackUserData = &ui_errors;
    context->ErrorCallback = [](ImGuiContext*, void* data, const char* message) {
        static_cast<std::vector<std::string>*>(data)->emplace_back(message);
    };
    unsigned char* pixels{}; int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    application::EditorFeatureDefinitionPanel panel;
    const auto frame = [&](const char* activate = nullptr) {
        ImGui::NewFrame();
        if (activate) {
            auto* window = ImGui::FindWindowByName("Feature Definitions");
            context->NavWindow = window; context->NavId = window->GetID(activate);
            context->NavInputSource = ImGuiInputSource_Keyboard;
            context->NavActivateId = context->NavActivateDownId = context->NavId;
        }
        ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(io.DisplaySize);
        panel.draw(document, root, {}, {}, selected_project);
        ImGui::Render();
    };
    frame(); frame("New Feature");
    if (setup == OwnerProjectSetup::kChangedAfterNew &&
        !write_ui_project(root, "Selected.AEGPROJ", "com.aeg.changed.after.open")) return false;
    if (setup == OwnerProjectSetup::kDeletedAfterNew && !std::filesystem::remove(selected_project)) return false;
    frame("Save Draft");
    const auto drafts = application::scan_editor_node_contracts(root);
    const auto expected_owner = feature::ModuleIdentity{module_id};
    const bool draft_ok = drafts && drafts.value.size() == 1 &&
        drafts.value.front().draft.entry.owner_module == expected_owner &&
        document.snapshot_owner() == before && document.revision() == 0 && !document.dirty();
    frame("Save and Generate");
    const auto generated = application::scan_editor_node_contracts(root);
    if (!expected_success) {
        const bool unchanged = drafts && drafts.value.empty() && generated && generated.value.empty() &&
            document.snapshot_owner() == before && document.revision() == 0 && !document.dirty() &&
            !document.can_undo() && !document.can_redo() &&
            document.catalog().contract_digest() == catalog.contract_digest() && ui_errors.empty();
        if (!unchanged) std::cerr << "INVALID_PROJECT_OWNER setup=" << static_cast<int>(setup)
                                  << " changed documents or generated files\n";
        return unchanged;
    }
    const bool generated_ok = generated && generated.value.size() == 1 &&
        generated.value.front().draft.generated &&
        generated.value.front().draft.entry.owner_module == expected_owner &&
        document.catalog().entries().size() == catalog.entries().size() + 1 &&
        document.revision() == 1 && document.dirty();
    if (!draft_ok || !generated_ok) {
        const auto actual_owner = generated && !generated.value.empty()
            ? generated.value.front().draft.entry.owner_module.value() : 0;
        std::cerr << "FIRST_FEATURE_OWNER foreign_catalog=" << foreign_catalog
                  << " draft_ok=" << draft_ok << " generated_ok=" << generated_ok
                  << " actual=" << actual_owner << " expected=" << expected_owner.value() << '\n';
    }
    for (const auto& message : ui_errors) std::cerr << message << '\n';
    return draft_ok && generated_ok && ui_errors.empty();
}

bool parameter_initial_values_are_safe() {
    using namespace aeg;
    using feature::NumericRange;
    using feature::ParameterMetadata;
    using feature::ParameterType;
    using feature::VectorConstraint;
    bool passed = true;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { std::cerr << message << '\n'; passed = false; }
    };
    struct IntegerCase { ParameterType type; NumericRange range; };
    const IntegerCase integer_cases[] = {
        {ParameterType::kInt32, {7, 19, 3}},
        {ParameterType::kInt64, {-9223372036854775808.0, -9223372036854773760.0, 1024}},
        {ParameterType::kUInt32, {4294967291.0, 4294967295.0, 2}},
        {ParameterType::kUInt64, {18446744073709547520.0, 18446744073709549568.0, 2048}},
    };
    for (const auto& test : integer_cases) {
        ParameterMetadata metadata{}; metadata.type = test.type; metadata.constraint = test.range;
        check(static_cast<bool>(feature::validate_parameter_metadata(metadata)), "Integer initial-value fixture is invalid");
        check(static_cast<bool>(feature::validate_parameter_value(application::initial_parameter_value(metadata), metadata)),
              "Integer initial value violates a valid range or step");
        const auto checked = application::try_initial_parameter_value(metadata);
        check(checked && feature::validate_parameter_value(checked.value, metadata),
              "Checked integer initial value was not accepted by the contract");
    }
    const IntegerCase invalid_integer_cases[] = {
        {ParameterType::kInt32, {1e300, 1e300}},
        {ParameterType::kInt64, {1e300, 1e300}},
        {ParameterType::kInt64, {9223372036854775808.0, 9223372036854775808.0}},
        {ParameterType::kInt64, {(std::numeric_limits<double>::quiet_NaN)(), 1}},
        {ParameterType::kUInt32, {-1, 1}},
        {ParameterType::kUInt64, {-1, 1}},
        {ParameterType::kUInt64, {18446744073709551616.0, 18446744073709551616.0}},
        {ParameterType::kUInt64, {(std::numeric_limits<double>::infinity)(), (std::numeric_limits<double>::infinity)()}},
    };
    for (const auto& test : invalid_integer_cases) {
        ParameterMetadata metadata{}; metadata.type = test.type; metadata.constraint = test.range;
        const auto initial = application::initial_parameter_value(metadata);
        const bool signed_type = test.type == ParameterType::kInt32 || test.type == ParameterType::kInt64;
        check(signed_type ? std::get_if<int64_t>(&initial) && std::get<int64_t>(initial) == 0
                          : std::get_if<uint64_t>(&initial) && std::get<uint64_t>(initial) == 0,
              "Invalid integer metadata did not yield a safe typed zero initial value");
        const auto checked = application::try_initial_parameter_value(metadata);
        check(!checked && checked.result.code == foundation::ResultCode::kInvalidArgument,
              "Checked integer initial value accepted invalid metadata");
    }
    struct VectorCase { ParameterType type; VectorConstraint constraint; };
    const VectorCase vector_cases[] = {
        {ParameterType::kVector2, {NumericRange{2, 4, 1}, false}},
        {ParameterType::kVector3, {NumericRange{-4, -2, 1}, false}},
        {ParameterType::kVector2, {{}, true}},
        {ParameterType::kVector3, {NumericRange{0.25, 0.9}, true}},
        {ParameterType::kVector4, {NumericRange{-0.9, -0.25}, true}},
        {ParameterType::kVector2, {NumericRange{0, 0.8, 0.2}, true}}, // A 0.6/0.8 unit vector exists.
        {ParameterType::kColor, {NumericRange{0, 1, 0.5}, true}},
    };
    for (const auto& test : vector_cases) {
        ParameterMetadata metadata{}; metadata.type = test.type; metadata.constraint = test.constraint;
        check(static_cast<bool>(feature::validate_parameter_metadata(metadata)), "Vector initial-value fixture is invalid");
        check(static_cast<bool>(feature::validate_parameter_value(application::initial_parameter_value(metadata), metadata)),
              "Vector initial value violates a satisfiable component range or unit constraint");
        const auto checked = application::try_initial_parameter_value(metadata);
        check(checked && feature::validate_parameter_value(checked.value, metadata),
              "Checked vector initial value was not accepted by the contract");
    }
    ParameterMetadata impossible{}; impossible.type = ParameterType::kVector2;
    impossible.constraint = VectorConstraint{NumericRange{1, 2}, true};
    check(static_cast<bool>(feature::serialize_parameter_value(application::initial_parameter_value(impossible), impossible.type)),
          "Unsatisfiable vector metadata produced an unsafe initial value");
    check(!application::try_initial_parameter_value(impossible), "Checked initial value accepted an impossible unit constraint");
    ParameterMetadata manual_unit{}; manual_unit.type = ParameterType::kVector2;
    manual_unit.constraint = VectorConstraint{NumericRange{0.1, 0.9, 0.0005}, true};
    check(static_cast<bool>(feature::validate_parameter_value(std::vector<double>{0.6, 0.8}, manual_unit)),
          "Manual-entry unit-vector fixture lost its legal value");
    const auto bounded = application::try_initial_parameter_value(manual_unit);
    check(!bounded && bounded.result.code == foundation::ResultCode::kUnsupported,
          "Bounded unit-vector initial search did not report its manual-entry boundary");
    ParameterMetadata explicit_default{}; explicit_default.type = ParameterType::kInt64;
    explicit_default.default_value = int64_t{42}; explicit_default.constraint = NumericRange{1e300, 1e300};
    check(application::initial_parameter_value(explicit_default) == foundation::ParameterValue{int64_t{42}},
          "Initial value generation changed an explicit author default");
    return passed;
}

bool parameter_migration_controls_apply_explicit_action(const std::filesystem::path& root, bool reset) {
    using namespace aeg;
    if (!write_ui_project(root, "Project.aegproj")) return false;
    auto original = tests::feature_definition_draft();
    auto& old_parameter = original.entry.contract.parameters.front();
    old_parameter.metadata->constraint = {};
    const auto payload = feature::serialize_parameter_value(1.0, feature::ParameterType::kDouble);
    if (!payload) return false;
    const auto envelope = feature::encode_feature_parameters(
        std::vector<feature::FeatureParameterRecord>{{old_parameter.id, old_parameter.version, payload.value}},
        original.entry.contract.parameters);
    if (!envelope) return false;
    auto graph = tests::feature_graph(original.entry);
    graph.nodes.front().parameter_blob = envelope.value;
    application::EditorFeatureGraphDocument document{graph, tests::feature_catalog(original.entry)};
    auto changed = original;
    changed.entry.version = {2, 0, 0}; changed.entry.contract.version = changed.entry.version;
    auto& next_parameter = changed.entry.contract.parameters.front();
    next_parameter.version = 2;
    next_parameter.metadata->type = feature::ParameterType::kInt64;
    next_parameter.metadata->default_value = int64_t{7};
    application::NodeContractFile seed{root / "Source/FlowGraph/CustomFeature.flowgraph.contract.json", changed, {},
                                     application::NodeContractSource::kEditorDraft};
    if (!application::write_editor_node_contract_atomic(seed, root)) return false;
    const auto before_owner = document.snapshot_owner();
    const auto before_digest = document.catalog().contract_digest();
    const auto before_file = tests::read_text(seed.path);
    struct ContextOwner {
        ImGuiContext* value{ImGui::CreateContext()};
        ~ContextOwner() { ImGui::DestroyContext(value); }
    } context_owner;
    auto* context = context_owner.value;
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.DisplaySize = {1200, 3000}; io.DeltaTime = 1.0f / 60.0f;
    io.ConfigErrorRecoveryEnableAssert = false; io.ConfigErrorRecoveryEnableDebugLog = false;
    io.ConfigErrorRecoveryEnableTooltip = false;
    unsigned char* pixels{}; int width{}, height{}; io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    std::vector<std::string> errors;
    context->ErrorCallbackUserData = &errors;
    context->ErrorCallback = [](ImGuiContext*, void* data, const char* message) {
        static_cast<std::vector<std::string>*>(data)->emplace_back(message);
    };
    application::EditorFeatureDefinitionPanel panel;
    const auto frame = [&](const char* activate = nullptr, ImGuiWindow* activation_window = nullptr,
                           ImGuiID activation_id = 0) {
        ImGui::NewFrame();
        if (activate || activation_id) {
            auto* window = activation_window ? activation_window : ImGui::FindWindowByName("Feature Definitions");
            context->NavWindow = window; context->NavId = activation_id ? activation_id : window->GetID(activate);
            context->NavInputSource = ImGuiInputSource_Keyboard;
            context->NavActivateId = context->NavActivateDownId = context->NavId;
        }
        ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(io.DisplaySize);
        panel.draw(document, root, {}); ImGui::Render();
    };
    const auto select_action = [&] {
        auto* window = ImGui::FindWindowByName("Feature Definitions");
        const auto parameter_scope = ImHashStr(std::to_string(old_parameter.id.value()).c_str(), 0,
                                               window->GetID("ParameterMigrations"));
        frame(nullptr, window, ImHashStr("Action", 0, parameter_scope));
        auto* popup = ImGui::FindWindowByName("##Combo_00");
        if (!popup || context->OpenPopupStack.empty()) {
            std::cerr << "Parameter migration action chooser did not open\n"; return false;
        }
        const int action_index = reset ? 2 : 1;
        const auto item_scope = ImHashData(&action_index, sizeof(action_index), popup->ID);
        frame(nullptr, popup, ImHashStr(reset ? "Reset to new default" : "Convert value", 0, item_scope));
        frame();
        return true;
    };
    const auto rejected_unchanged = [&] {
        if (document.snapshot_owner() != before_owner || document.revision() != 0 || document.dirty() ||
            document.can_undo() || document.can_redo() || document.catalog().contract_digest() != before_digest ||
            tests::read_text(seed.path) != before_file) {
            std::cerr << "UI save without a parameter migration changed the document or file\n"; return false;
        }
        return true;
    };
    frame(); frame("Definition");
    auto* popup = ImGui::FindWindowByName("##Combo_00");
    if (!popup || context->OpenPopupStack.empty()) return false;
    frame(nullptr, popup, popup->GetID(std::to_string(original.entry.id.value()).c_str()));
    frame(); frame("Open"); frame(); frame("Confirm affected connections"); frame("Save and Generate");
    if (!rejected_unchanged() || !select_action()) return false;
    // A new draft must not inherit a previously selected parameter's migration.
    frame("New Feature"); frame("Save Draft");
    const auto files = application::scan_editor_node_contracts(root);
    if (!files || files.value.size() != 2 || !rejected_unchanged()) {
        std::cerr << "New Feature retained a stale parameter migration\n"; return false;
    }
    frame("Open"); frame();
    if (!select_action()) return false;
    // Reopening the saved draft must also require a fresh explicit decision.
    frame("Open"); frame(); frame("Confirm affected connections"); frame("Save and Generate");
    if (!rejected_unchanged() || !select_action()) return false;
    frame("Save and Generate"); frame();
    const auto expected = int64_t{reset ? 7 : 1};
    for (const auto& node : document.snapshot().nodes) {
        const auto records = feature::decode_feature_parameters(node.parameter_blob, changed.entry.contract.parameters);
        if (node.definition_version != feature::Version{2, 0, 0} || !records || records.value.size() != 1 ||
            records.value.front().version != 2) {
            std::cerr << "UI migration did not publish the new parameter and definition versions\n"; return false;
        }
        const auto value = feature::deserialize_parameter_value(records.value.front().payload, feature::ParameterType::kInt64);
        if (!value || std::get<int64_t>(value.value) != expected) {
            std::cerr << "UI migration did not apply its selected convert/reset action\n"; return false;
        }
    }
    if (document.revision() != 1 || !document.dirty() || !document.can_undo() || document.can_redo() ||
        tests::read_text(seed.path) == before_file || !document.undo() || document.snapshot_owner() != before_owner ||
        document.catalog().contract_digest() != before_digest || !document.redo()) {
        std::cerr << "UI migration did not preserve one undoable contract transaction\n"; return false;
    }
    for (const auto& message : errors) std::cerr << message << '\n';
    return errors.empty();
}
} // namespace

int main() {
    using namespace aeg;
    if (!parameter_initial_values_are_safe()) return 11;
    const auto root = std::filesystem::temp_directory_path() / "aeg_feature_definition_ui";
    std::error_code error; std::filesystem::remove_all(root, error);
    const bool empty_owner = first_feature_uses_selected_project_owner(root / "owner-empty", false);
    const bool foreign_owner = first_feature_uses_selected_project_owner(root / "owner-foreign", true);
    if (!empty_owner || !foreign_owner) return 14;
    constexpr OwnerProjectSetup owner_cases[] = {
        OwnerProjectSetup::kRootSingle, OwnerProjectSetup::kRootAmbiguous, OwnerProjectSetup::kNoProject,
        OwnerProjectSetup::kMissingSelected, OwnerProjectSetup::kInvalidSelected, OwnerProjectSetup::kNoModule,
        OwnerProjectSetup::kEmptyModule, OwnerProjectSetup::kCrossRoot, OwnerProjectSetup::kChangedAfterNew,
        OwnerProjectSetup::kDeletedAfterNew
    };
    for (const auto setup : owner_cases)
        if (!first_feature_uses_selected_project_owner(root / ("owner-case-" + std::to_string(static_cast<int>(setup))),
                                                     true, setup)) return 16;
    if (!write_ui_project(root, "Project.aegproj")) return 15;
    auto original = tests::feature_definition_draft();
    auto catalog = tests::feature_catalog(original.entry);
    application::EditorFeatureGraphDocument document{tests::feature_graph(original.entry), catalog};
    auto saved = original;
    saved.entry.id = feature::FeatureDefinitionId{"Game.NewFeature1"}; saved.entry.contract.definition_id = saved.entry.id;
    saved.name = "Game.Preserved";
    auto& asset_parameter = saved.entry.contract.parameters.front();
    asset_parameter.max_size = 16;
    asset_parameter.metadata->type = feature::ParameterType::kAssetReference;
    asset_parameter.metadata->default_value.reset();
    asset_parameter.metadata->constraint = feature::AssetConstraint{true};
    application::NodeContractFile seed{root / "Source/FlowGraph/Preserved.flowgraph.contract.json", saved, {}, application::NodeContractSource::kEditorDraft};
    if (!application::write_editor_node_contract_atomic(seed, root)) return 1;
    const auto original_bytes = tests::read_text(seed.path);
    application::EditorFeatureDefinitionPanel panel;
    auto* context = ImGui::CreateContext(); auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.DisplaySize = {1200, 3000}; io.DeltaTime = 1.0f / 60.0f;
    io.ConfigErrorRecoveryEnableAssert = false; io.ConfigErrorRecoveryEnableDebugLog = false; io.ConfigErrorRecoveryEnableTooltip = false;
    unsigned char* pixels{}; int width{}, height{}; io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    std::vector<std::string> errors;
    context->ErrorCallbackUserData = &errors;
    context->ErrorCallback = [](ImGuiContext*, void* data, const char* message) { static_cast<std::vector<std::string>*>(data)->emplace_back(message); };
    const auto frame = [&](const char* activate = nullptr, bool collapsed = false,
                           ImGuiWindow* activation_window = nullptr, ImGuiID activation_id = 0) {
        ImGui::NewFrame();
        if (activate || activation_id) {
            auto* window = activation_window ? activation_window : ImGui::FindWindowByName("Feature Definitions");
            context->NavWindow = window; context->NavId = activation_id ? activation_id : window->GetID(activate); context->NavInputSource = ImGuiInputSource_Keyboard;
            context->NavActivateId = context->NavActivateDownId = context->NavId;
        }
        ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(io.DisplaySize); ImGui::SetNextWindowCollapsed(collapsed);
        panel.draw(document, root, {}); ImGui::Render();
    };
    frame(); frame("New Feature"); frame("Save Draft");
    auto files = application::scan_editor_node_contracts(root);
    if (!files || files.value.size() != 2 || document.revision() != 0 || document.dirty() || document.catalog().entries().size() != 1) return 2;
    frame("New Feature"); frame("Save Draft");
    files = application::scan_editor_node_contracts(root);
    if (!files || files.value.size() != 3 || tests::read_text(seed.path) != original_bytes) return 3;
    frame("Save and Generate");
    if (document.catalog().entries().size() != 2 || !document.dirty() || document.revision() != 1) return 4;
    files = application::scan_editor_node_contracts(root);
    if (!files || std::count_if(files.value.begin(), files.value.end(), [](const auto& file) { return file.draft.generated; }) != 1) return 5;
    frame(nullptr, true); frame();
    frame("Definition");
    auto* popup = ImGui::FindWindowByName("##Combo_00");
    if (!popup || context->OpenPopupStack.empty()) { std::cerr << "Definition chooser did not open\n"; return 7; }
    const auto definition_scope = popup->GetID(std::to_string(saved.entry.id.value()).c_str());
    frame(nullptr, false, popup, ImHashStr((saved.display_name + " (draft)").c_str(), 0, definition_scope));
    frame(); frame("Open"); frame("Parameters"); frame();
    auto* window = ImGui::FindWindowByName("Feature Definitions");
    const int parameter_index = 0;
    const auto parameter_scope = ImHashData(&parameter_index, sizeof(parameter_index), window->GetID("Parameters"));
    frame(nullptr, false, window, ImHashStr("Asset kind", 0, parameter_scope));
    popup = ImGui::FindWindowByName("##Combo_00");
    if (!popup || context->OpenPopupStack.empty()) { std::cerr << "Asset kind chooser did not open\n"; return 8; }
    const int texture_index = 2;
    frame(nullptr, false, popup, ImHashStr("Texture", 0, ImHashData(&texture_index, sizeof(texture_index), popup->ID)));
    frame(); frame("Save Draft");
    files = application::scan_editor_node_contracts(root);
    if (!files) return 9;
    const auto typed = std::find_if(files.value.begin(), files.value.end(), [&](const auto& file) { return file.draft.entry.id == saved.entry.id; });
    if (typed == files.value.end() || !typed->draft.entry.contract.parameters.front().metadata ||
        std::get<feature::AssetConstraint>(typed->draft.entry.contract.parameters.front().metadata->constraint).kind != foundation::AssetKind::kTexture) {
        std::cerr << "Asset kind selected through the actual UI was not saved\n"; return 10;
    }
    ImGui::DestroyContext(context);
    for (const auto& message : errors) std::cerr << message << '\n';
    if (!errors.empty()) return 6;
    std::filesystem::remove_all(root, error);
    if (!parameter_migration_controls_apply_explicit_action(root / "convert", false)) return 12;
    if (!parameter_migration_controls_apply_explicit_action(root / "reset", true)) return 13;
    std::filesystem::remove_all(root, error);
    std::cout << "Feature authoring UI saves distinct drafts, typed asset constraints and publishes through the service\n";
}
