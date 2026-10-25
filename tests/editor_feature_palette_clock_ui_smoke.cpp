/** @brief Real ImGui clock/domain/execution palette selection and node creation. */
#include "editor_feature_graph_panel.hpp"
#include <imgui_internal.h>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace aeg;
constexpr feature::ClockId kLowClock{uint64_t{7}};
constexpr feature::ClockId kHighClock{uint64_t{0x8000000100000007ull}};
constexpr feature::ScopeId kLowScope{uint64_t{7}};
constexpr feature::ScopeId kHighScope{uint64_t{0x8000000100000007ull}};

void require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

feature::FeatureDefinition definition(uint64_t id, feature::FeatureDomain domain,
                                      feature::ExecutionKind execution, feature::ClockId clock) {
    feature::FeatureDefinition value;
    value.id = feature::FeatureDefinitionId{id}; value.version = {1, 0, 0};
    auto& contract = value.contract;
    contract.definition_id = value.id; contract.version = value.version;
    contract.domains = feature::to_mask(domain); contract.execution = execution;
    contract.state = execution == feature::ExecutionKind::kProcess ? feature::StatePolicy::kProcessState
                                                                : feature::StatePolicy::kStateless;
    contract.has_continuation = execution == feature::ExecutionKind::kProcess;
    contract.scope = {feature::ScopeId{"World"}};
    contract.clock = aeg::feature::make_periodic_clock_contract(clock, 60, 4, feature::LateDataPolicy::kReject,
        aeg::feature::DeadlinePolicy::kObserve, contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
    contract.permissions.version = 1;
    return value;
}

feature::FeatureCatalog catalog(bool reversed = false, bool only_low = false) {
    auto low = definition(1, feature::FeatureDomain::kLogic, feature::ExecutionKind::kCall, kLowClock);
    auto process = definition(2, feature::FeatureDomain::kLogic, feature::ExecutionKind::kProcess, kHighClock);
    auto continuous = definition(3, feature::FeatureDomain::kPhysics, feature::ExecutionKind::kContinuous, kHighClock);
    feature::FeatureCatalog result;
    if (reversed && !only_low) {
        require(static_cast<bool>(result.export_definition(continuous)), "Continuous contract failed");
        require(static_cast<bool>(result.export_definition(process)), "Process contract failed");
    }
    require(static_cast<bool>(result.export_definition(low)), "Call contract failed");
    if (!reversed && !only_low) {
        require(static_cast<bool>(result.export_definition(process)), "Process contract failed");
        require(static_cast<bool>(result.export_definition(continuous)), "Continuous contract failed");
    }
    return result;
}

feature::FeatureGraphDocument graph() {
    feature::FeatureGraphDocument value;
    value.asset_uuid = "asset://clock-palette"; value.schema_version = 1;
    value.graph_version = 2; value.draft_revision = 1;
    return value;
}

class Harness final {
public:
    Harness() {
        context = ImGui::CreateContext();
        auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
        io.DisplaySize = {1600, 3000}; io.DeltaTime = 1.0f / 60.0f;
        io.ConfigErrorRecoveryEnableAssert = io.ConfigErrorRecoveryEnableDebugLog = io.ConfigErrorRecoveryEnableTooltip = false;
        unsigned char* pixels{}; int width{}, height{}; io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        context->ErrorCallbackUserData = &errors;
        context->ErrorCallback = [](ImGuiContext*, void* data, const char* message) {
            static_cast<std::vector<std::string>*>(data)->emplace_back(message);
        };
    }
    ~Harness() {
        ImGui::DestroyContext(context);
        for (const auto& error : errors) std::cerr << error << '\n';
        require(errors.empty(), "ImGui palette error");
    }
    void draw(application::EditorFeatureGraphDocument& document, ImGuiWindow* nav_window = nullptr,
              ImGuiID id = 0, bool prefer_input = false) {
        ImGui::NewFrame();
        if (id != 0) {
            context->NavWindow = nav_window; context->NavId = id;
            context->NavInputSource = ImGuiInputSource_Keyboard;
            context->NavActivateId = context->NavActivateDownId = id;
            context->NavActivateFlags = prefer_input ? ImGuiActivateFlags_PreferInput : ImGuiActivateFlags_None;
        }
        ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        panel.draw(document, "Clock.aegfeature", {}); ImGui::Render();
    }
    void select(application::EditorFeatureGraphDocument& document, const char* combo, const std::string& item,
                int builtin_index = -1) {
        auto* root = ImGui::FindWindowByName("Flow Graph###FlowGraph"); require(root != nullptr, "Palette window missing");
        draw(document, root, root->GetID(combo));
        auto* popup = ImGui::FindWindowByName("##Combo_00");
        if (popup == nullptr || context->OpenPopupStack.empty()) {
            std::cerr << combo << " chooser did not open\n"; std::exit(1);
        }
        auto seed = popup->ID;
        if (builtin_index >= 0) seed = ImHashData(&builtin_index, sizeof(builtin_index), seed);
        draw(document, popup, ImHashStr(item.c_str(), 0, seed));
        draw(document);
    }
    void add(application::EditorFeatureGraphDocument& document, uint64_t definition_id) {
        auto* root = ImGui::FindWindowByName("Flow Graph###FlowGraph"); require(root != nullptr, "Palette window missing");
        const auto entry = std::to_string(definition_id);
        const auto row = ImHashStr(entry.c_str(), 0, root->GetID("FeatureGraphCatalog"));
        draw(document, root, ImHashStr("Add node", 0, row));
        draw(document);
    }
    void search(application::EditorFeatureGraphDocument& document, const char* text) {
        auto* root = ImGui::FindWindowByName("Flow Graph###FlowGraph");
        require(root != nullptr, "Palette window missing before search");
        const auto id = root->GetID("Search name or definition id###Search definition id");
        draw(document, root, id, true);
        require(context->ActiveId == id, "Definition search did not accept keyboard focus");
        auto& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_A, true); draw(document);
        io.AddKeyEvent(ImGuiKey_A, false); io.AddKeyEvent(ImGuiMod_Ctrl, false);
        io.AddKeyEvent(ImGuiKey_Backspace, true); draw(document);
        io.AddKeyEvent(ImGuiKey_Backspace, false); io.AddInputCharactersUTF8(text); draw(document);
        io.AddKeyEvent(ImGuiKey_Enter, true); draw(document);
        io.AddKeyEvent(ImGuiKey_Enter, false); draw(document);
    }
    ImGuiContext* context{};
    application::EditorFeatureGraphPanel panel{};
    std::vector<std::string> errors{};
};

void expect_count(const application::EditorFeatureGraphDocument& document, std::size_t count, const char* message) {
    if (document.snapshot().nodes.size() != count) {
        std::cerr << message << ": expected " << count << ", got " << document.snapshot().nodes.size() << '\n';
        std::exit(1);
    }
}
}

void test_existing_clock_domain_execution_palette() {
    application::EditorFeatureGraphDocument document{graph(), catalog()}; Harness ui;
    ui.draw(document); const auto initial = document.snapshot_owner();
    ui.select(document, "Clock", "Clock " + std::to_string(kHighClock.value()));
    require(document.snapshot_owner() == initial && !document.dirty() && !document.can_undo(), "Clock filter mutated author graph");
    ui.add(document, 1); expect_count(document, 0, "Clock filter admitted another full64 clock sharing low bits");
    ui.add(document, 2); expect_count(document, 1, "Selected clock Process was unavailable");
    ui.add(document, 3); expect_count(document, 2, "Selected clock Continuous was unavailable");

    ui.select(document, "Domain", "Logic", 1);
    ui.select(document, "Execution", "Process", 2);
    ui.add(document, 3); expect_count(document, 2, "Domain filter did not intersect selected clock");
    ui.add(document, 1); expect_count(document, 2, "Execution filter did not intersect selected clock");
    ui.add(document, 2); expect_count(document, 3, "Clock/Domain/Execution intersection excluded correct Feature");

    ui.select(document, "Domain", "All domains", 0); ui.select(document, "Execution", "All execution", 0);
    application::EditorFeatureGraphDocument reordered{graph(), catalog(true)};
    ui.draw(reordered); ui.add(reordered, 1); expect_count(reordered, 0, "Catalog reorder changed clock selection identity");
    ui.add(reordered, 3); expect_count(reordered, 1, "Reordered catalog lost matching clock");

    ui.select(reordered, "Clock", "All clocks"); ui.add(reordered, 1); expect_count(reordered, 2, "All clocks did not restore row");
    ui.select(reordered, "Clock", "Clock " + std::to_string(kHighClock.value()));
    application::EditorFeatureGraphDocument replaced{graph(), catalog(false, true)};
    ui.draw(replaced); require(!replaced.dirty(), "Missing clock reset mutated graph");
    ui.add(replaced, 1); expect_count(replaced, 1, "Removed clock left a stale empty palette filter");
    std::cout << "Clock palette ImGui selection passed\n";
}

namespace {
feature::FeatureDefinition dimensional_definition(uint64_t id, feature::FeatureDomainMask domains,
    feature::ExecutionKind execution, feature::ClockId clock, feature::FeatureStructure structure,
    feature::ScopeId scope) {
    auto result = definition(id, feature::FeatureDomain::kLogic, execution, clock);
    result.contract.domains = domains; result.contract.structure = structure;
    result.contract.clock.work_budget_ns = structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000;
    result.contract.scope.id = scope;
    if (structure == feature::FeatureStructure::kComposite) {
        result.contract.children = {feature::FeatureDefinitionId{201}};
        if ((domains & feature::to_mask(feature::FeatureDomain::kPhysics)) != 0)
            result.contract.children.push_back(feature::FeatureDefinitionId{205});
    }
    return result;
}

feature::FeatureCatalog dimensional_catalog(bool reversed = false, bool only_low = false) {
    using namespace feature;
    const auto logic = to_mask(FeatureDomain::kLogic);
    const auto physics = to_mask(FeatureDomain::kPhysics);
    std::vector definitions{
        dimensional_definition(201, logic, ExecutionKind::kCall, kHighClock, FeatureStructure::kAtomic, kLowScope),
        dimensional_definition(202, logic, ExecutionKind::kCall, kHighClock, FeatureStructure::kComposite, kHighScope),
        dimensional_definition(203, static_cast<FeatureDomainMask>(logic | physics), ExecutionKind::kCall,
            kHighClock, FeatureStructure::kComposite, kHighScope),
        dimensional_definition(204, logic, ExecutionKind::kProcess, kHighClock, FeatureStructure::kAtomic, kHighScope),
        dimensional_definition(205, physics, ExecutionKind::kCall, kHighClock, FeatureStructure::kAtomic, kHighScope),
        dimensional_definition(206, logic, ExecutionKind::kCall, kLowClock, FeatureStructure::kAtomic, kHighScope),
        dimensional_definition(207, logic, ExecutionKind::kCall, kHighClock, FeatureStructure::kAtomic, kLowScope),
        dimensional_definition(208, logic, ExecutionKind::kCall, kHighClock, FeatureStructure::kAtomic, kHighScope),
        dimensional_definition(210, logic, ExecutionKind::kCall, kHighClock, FeatureStructure::kComposite, kLowScope),
    };
    if (reversed) std::reverse(definitions.begin(), definitions.end());
    FeatureCatalog result;
    for (const auto& value : definitions)
        if (!only_low || value.id == FeatureDefinitionId{207})
            require(static_cast<bool>(result.export_definition(value)), "Dimensional UI contract failed");
    if (!only_low) {
        FeatureCatalogEntry summary{};
        summary.definition_id = FeatureDefinitionId{209}; summary.version = {1, 0, 0};
        summary.domains = logic; summary.clock = kHighClock;
        require(static_cast<bool>(result.add_entry(summary)), "Summary-only zero Scope catalog failed");
    }
    return result;
}

void test_scope_combo_uses_full_identity_and_zero_is_an_explicit_selection() {
    application::EditorFeatureGraphDocument document{graph(), dimensional_catalog()}; Harness ui;
    ui.draw(document); const auto original = document.snapshot_owner(); const auto revision = document.revision();
    ui.select(document, "Scope", "Scope " + std::to_string(kHighScope.value()));
    require(document.snapshot_owner() == original && document.revision() == revision &&
                !document.dirty() && !document.can_undo() && !document.can_redo(),
            "Scope chooser changed the author graph/history");
    ui.add(document, 207); expect_count(document, 0, "Scope chooser truncated the full64 identity");
    ui.add(document, 208); expect_count(document, 1, "Selected full64 Scope row was unavailable");
    ui.select(document, "Scope", "Scope 0");
    ui.add(document, 208); expect_count(document, 1, "Explicit zero Scope was treated as All scopes");
    ui.add(document, 209); expect_count(document, 2, "Accepted summary-only zero Scope row was unavailable");
    ui.select(document, "Scope", "All scopes");
    ui.add(document, 207); expect_count(document, 3, "All scopes did not restore the low Scope row");
}

void test_structure_scope_and_search_intersect_real_palette_rows() {
    application::EditorFeatureGraphDocument document{graph(), dimensional_catalog()}; Harness ui;
    ui.draw(document); const auto original = document.snapshot_owner(); const auto revision = document.revision();
    ui.select(document, "Structure", "Composite", 2);
    ui.select(document, "Scope", "Scope " + std::to_string(kHighScope.value()));
    ui.select(document, "Clock", "Clock " + std::to_string(kHighClock.value()));
    ui.select(document, "Domain", "Logic", 1); ui.select(document, "Execution", "Call", 1);
    require(document.snapshot_owner() == original && document.revision() == revision &&
                !document.dirty() && !document.can_undo() && !document.can_redo(),
            "Five palette chooser views changed author graph/history");
    ui.add(document, 208); expect_count(document, 0, "Structure admitted an Atomic row with otherwise matching dimensions");
    ui.add(document, 210); expect_count(document, 0, "Scope admitted a Composite row with otherwise matching dimensions");
    ui.add(document, 202); expect_count(document, 1, "Same-domain Composite was unavailable");
    ui.add(document, 203); expect_count(document, 2, "Cross-domain Composite lost its Logic membership");

    auto before_search = document.snapshot_owner(); auto before_search_revision = document.revision();
    ui.search(document, "202");
    require(document.snapshot_owner() == before_search && document.revision() == before_search_revision,
            "Definition search changed the author graph");
    ui.add(document, 203); expect_count(document, 2, "Search did not intersect Structure/Scope/Clock/Domain/Execution");
    ui.add(document, 202); expect_count(document, 3, "Search excluded the matching five-dimensional row");
    ui.search(document, "");
    ui.select(document, "Structure", "Atomic", 1);
    ui.add(document, 202); expect_count(document, 3, "Atomic structure selection admitted a Composite");
    ui.add(document, 204); expect_count(document, 3, "Execution stopped intersecting new palette dimensions");
    ui.add(document, 205); expect_count(document, 3, "Domain stopped intersecting new palette dimensions");
    ui.add(document, 206); expect_count(document, 3, "Clock stopped intersecting new palette dimensions");
    ui.add(document, 207); expect_count(document, 3, "Scope stopped intersecting Atomic selection");
    ui.add(document, 208); expect_count(document, 4, "Atomic five-dimensional intersection excluded its matching row");

    ui.select(document, "Domain", "All domains", 0); ui.select(document, "Execution", "All execution", 0);
    ui.select(document, "Clock", "All clocks"); ui.select(document, "Structure", "All structures", 0);
    application::EditorFeatureGraphDocument reordered{graph(), dimensional_catalog(true)};
    ui.draw(reordered); ui.add(reordered, 207); expect_count(reordered, 0, "Catalog reorder changed Scope selection identity");
    ui.add(reordered, 208); expect_count(reordered, 1, "Reordered catalog lost the selected Scope");
    application::EditorFeatureGraphDocument replaced{graph(), dimensional_catalog(false, true)};
    ui.draw(replaced);
    require(!replaced.dirty() && !replaced.can_undo(), "Removed Scope reset mutated the replacement graph");
    ui.add(replaced, 207); expect_count(replaced, 1, "Removed Scope left a stale empty palette filter");
}
}  // namespace

int main(int argc, char** argv) {
    // Root can independently observe the missing Scope control on the old panel.
    if (argc == 2 && std::string_view{argv[1]} == "--scope-only") {
        test_scope_combo_uses_full_identity_and_zero_is_an_explicit_selection();
        std::cout << "Scope palette ImGui selection passed\n";
        return 0;
    }
    test_existing_clock_domain_execution_palette();
    test_structure_scope_and_search_intersect_real_palette_rows();
    test_scope_combo_uses_full_identity_and_zero_is_an_explicit_selection();
    std::cout << "Structure/Scope palette ImGui selection passed\n";
}
