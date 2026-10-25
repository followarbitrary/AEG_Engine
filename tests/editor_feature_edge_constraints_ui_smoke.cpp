/** @brief Persisted Snapshot/Queue/Event constraints through the real FeatureGraph panel. */
#include "editor_feature_graph_panel.hpp"
#include "feature_graph_io.hpp"
#include <imgui_internal.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace aeg;

void require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}

void contains(std::string_view text, const std::string& expected, const char* message) {
    if (text.find(expected) == std::string_view::npos) {
        std::cerr << message << ": missing [" << expected << "]\nRendered detail:\n" << text << '\n';
        std::exit(1);
    }
}

struct EdgeFixture final {
    uint64_t source_node, source_port, target_node, target_port;
    feature::EdgeKind kind;
    const char* kind_name;
    uint64_t source_clock, target_clock, schema;
    uint32_t version, capacity, deadline;
    uint32_t source_frequency, source_backlog, target_frequency, target_backlog;
    feature::LateDataPolicy source_late, target_late;
    const char* source_late_name;
    const char* target_late_name;
};

// Non-default values and equal low clock bits expose draft/default and truncation bugs.
constexpr std::array<EdgeFixture, 3> kEdges{{
    {31, 101, 32, 201, feature::EdgeKind::kSnapshot, "Snapshot",
     0x8000000100000007ull, 0x8000000200000007ull, 0x9000000100000021ull,
     7, 17, 13, 97, 41, 127, 43, feature::LateDataPolicy::kUseLatest,
     feature::LateDataPolicy::kDrop, "UseLatest", "Drop"},
    {41, 102, 42, 202, feature::EdgeKind::kQueue, "Queue",
     0x8000000300000007ull, 0x8000000400000007ull, 0x9000000200000021ull,
     11, 23, 29, 101, 47, 131, 53, feature::LateDataPolicy::kQueue,
     feature::LateDataPolicy::kReject, "Queue", "Reject"},
    {51, 103, 52, 203, feature::EdgeKind::kEvent, "Event",
     0x8000000500000007ull, 0x8000000600000007ull, 0x9000000300000021ull,
     19, 31, 37, 103, 59, 137, 61, feature::LateDataPolicy::kReject,
     feature::LateDataPolicy::kUseLatest, "Reject", "UseLatest"},
}};

feature::FeatureDefinition definition(const EdgeFixture& edge, bool source) {
    const auto node = source ? edge.source_node : edge.target_node;
    feature::FeatureDefinition value;
    value.id = feature::FeatureDefinitionId{1000 + node}; value.version = {1, 2, 3};
    auto& contract = value.contract;
    contract.definition_id = value.id; contract.version = value.version;
    contract.domains = feature::to_mask(source ? feature::FeatureDomain::kLogic : feature::FeatureDomain::kPhysics);
    contract.scope = {feature::ScopeId{"World"}};
    contract.clock = feature::make_periodic_clock_contract(feature::ClockId{source ? edge.source_clock : edge.target_clock},
        source ? edge.source_frequency : edge.target_frequency,
        source ? edge.source_backlog : edge.target_backlog,
        source ? edge.source_late : edge.target_late, feature::DeadlinePolicy::kObserve, 250000, 17);
    contract.permissions.version = 1;
    contract.ports = {{feature::PortId{source ? edge.source_port : edge.target_port},
        source ? feature::PortDirection::kOutput : feature::PortDirection::kInput,
        feature::ValueSchemaId{edge.schema}, edge.version}};
    return value;
}

enum class CatalogCase {
    kCurrent, kMissingSource, kMissingPort, kStaleSource,
    kMissingTarget, kMissingTargetPort, kStaleTarget, kNone
};

feature::FeatureCatalog catalog(CatalogCase scenario = CatalogCase::kCurrent) {
    feature::FeatureCatalog result;
    if (scenario == CatalogCase::kNone) return result;
    for (std::size_t index = 0; index < kEdges.size(); ++index) {
        auto source = definition(kEdges[index], true);
        auto target = definition(kEdges[index], false);
        if (index == 0 && scenario == CatalogCase::kMissingPort) source.contract.ports.clear();
        if (index == 0 && scenario == CatalogCase::kMissingTargetPort) target.contract.ports.clear();
        if (index == 0 && scenario == CatalogCase::kStaleSource) {
            source.version = source.contract.version = {2, 0, 0};
            // A current catalog clock must never overwrite the persisted old edge clock.
            source.contract.clock = aeg::feature::make_periodic_clock_contract(feature::ClockId{0xf000000100000007ull}, 211, 71, feature::LateDataPolicy::kReject,
                aeg::feature::DeadlinePolicy::kObserve, source.contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
        }
        if (index == 0 && scenario == CatalogCase::kStaleTarget) {
            target.version = target.contract.version = {2, 0, 0};
            target.contract.clock = aeg::feature::make_periodic_clock_contract(feature::ClockId{0xf000000200000007ull}, 223, 73, feature::LateDataPolicy::kQueue,
                aeg::feature::DeadlinePolicy::kObserve, target.contract.structure == aeg::feature::FeatureStructure::kComposite ? 0 : 250000, 0);
        }
        if (index != 0 || scenario != CatalogCase::kMissingSource)
            require(static_cast<bool>(result.export_definition(source)), "Source UI contract rejected");
        if (index != 0 || scenario != CatalogCase::kMissingTarget)
            require(static_cast<bool>(result.export_definition(target)), "Target UI contract rejected");
    }
    return result;
}

feature::FeatureGraphDocument graph() {
    feature::FeatureGraphDocument result;
    result.asset_uuid = "asset://edge-constraints-ui";
    result.schema_version = 1; result.graph_version = 5; result.draft_revision = 9;
    for (const auto& edge : kEdges) {
        result.nodes.push_back({feature::FeatureNodeId{edge.source_node},
            feature::FeatureDefinitionId{1000 + edge.source_node}, {1, 2, 3}, {}, {}});
        result.nodes.push_back({feature::FeatureNodeId{edge.target_node},
            feature::FeatureDefinitionId{1000 + edge.target_node}, {1, 2, 3}, {}, {}});
        result.edges.push_back({feature::FeatureNodeId{edge.source_node}, feature::PortId{edge.source_port},
            feature::FeatureNodeId{edge.target_node}, feature::PortId{edge.target_port}, edge.kind,
            feature::ClockId{edge.source_clock}, feature::ClockId{edge.target_clock}, edge.version,
            edge.capacity, edge.deadline});
    }
    result.contract_digest = catalog().contract_digest();
    return result;
}

class SavedGraph final {
public:
    SavedGraph() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        file = std::filesystem::temp_directory_path() /
            ("aeg_editor_edge_constraints_ui_" + std::to_string(nonce) + ".aegfeature");
        require(static_cast<bool>(application::authoring::save_feature_graph_document(file, graph())),
                "Edge UI fixture did not save through the formal FeatureGraph codec");
    }
    ~SavedGraph() { std::error_code error; std::filesystem::remove(file, error); }
    feature::FeatureGraphDocument reload() const {
        const auto loaded = application::authoring::load_feature_graph_document(file);
        require(static_cast<bool>(loaded), "Persisted Edge UI fixture did not reload");
        require(loaded.value.edges.size() == 3, "Persisted Edge UI fixture lost a transport");
        require(loaded.value.edges[0].source_clock.value() == 0x8000000100000007ull &&
                    loaded.value.edges[0].target_clock.value() == 0x8000000200000007ull &&
                    loaded.value.edges[0].version == 7 && loaded.value.edges[0].capacity == 17 &&
                    loaded.value.edges[0].deadline_ticks == 13,
                "Codec precondition lost saved full64 clocks/version/capacity/deadline");
        return loaded.value;
    }
    std::filesystem::path file;
};

class Harness final {
public:
    Harness() {
        context = ImGui::CreateContext();
        auto& io = ImGui::GetIO(); io.IniFilename = nullptr;
        io.DisplaySize = {1920, 4200}; io.DeltaTime = 1.0f / 60.0f;
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
        require(errors.empty(), "ImGui edge constraints error");
    }
    std::string draw(application::EditorFeatureGraphDocument& document,
        const application::EditorFeatureGraphPanel::Actions& actions = {}, ImGuiID activate = 0) {
        ImGui::NewFrame();
        if (activate != 0) {
            auto* root = ImGui::FindWindowByName("Flow Graph###FlowGraph");
            require(root != nullptr, "Edge panel root missing before selection");
            context->NavWindow = root; context->NavId = activate;
            context->NavInputSource = ImGuiInputSource_Keyboard;
            context->NavActivateId = context->NavActivateDownId = activate;
        }
        // Capture belongs to an outer window, so panel.End cannot clear its log.
        ImGui::Begin("Edge constraints capture", nullptr, ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer();
        ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        panel.draw(document, "PersistedEdges.aegfeature", actions);
        const std::string rendered{context->LogBuffer.c_str()};
        ImGui::LogFinish(); ImGui::End(); ImGui::Render();
        return rendered;
    }
    std::string select(application::EditorFeatureGraphDocument& document, const EdgeFixture& edge,
        const application::EditorFeatureGraphPanel::Actions& actions = {}) {
        auto* root = ImGui::FindWindowByName("Flow Graph###FlowGraph");
        require(root != nullptr, "Edge panel was not drawn before selecting a saved row");
        const std::string key = std::to_string(edge.source_node) + ":" + std::to_string(edge.source_port) +
            ">" + std::to_string(edge.target_node) + ":" + std::to_string(edge.target_port);
        const auto row_id = ImHashStr(key.c_str(), 0, root->GetID("FeatureGraphEdges"));
        draw(document, actions, row_id);
        auto rendered = draw(document, actions);
        const auto detail = rendered.find("Saved edge");
        if (detail == std::string::npos) {
            std::cerr << "Selecting saved " << edge.kind_name << " row did not show Saved edge details\n";
            std::exit(1);
        }
        return rendered.substr(detail);
    }
    ImGuiContext* context{};
    application::EditorFeatureGraphPanel panel{};
    std::vector<std::string> errors{};
};

void expect_saved(std::string_view text, const EdgeFixture& edge) {
    contains(text, std::string{"Kind: "} + edge.kind_name, "Selected transport kind was not shown");
    contains(text, "Version: " + std::to_string(edge.version), "Saved edge version was hidden or replaced");
    contains(text, "Capacity: " + std::to_string(edge.capacity), "Saved edge capacity was hidden or replaced");
    contains(text, "Deadline ticks: " + std::to_string(edge.deadline), "Saved edge deadline was hidden or replaced");
    contains(text, "Source clock: " + std::to_string(edge.source_clock), "Saved source clock was hidden/truncated/replaced");
    contains(text, "Target clock: " + std::to_string(edge.target_clock), "Saved target clock was hidden/truncated/replaced");
    contains(text, "Deadline unit: source Clock periods", "Saved edge deadline unit was ambiguous");
}

void expect_clock(std::string_view text, const EdgeFixture& edge, bool source) {
    const std::string prefix = source ? "Source Clock " : "Target Clock ";
    contains(text, prefix + "frequency (Hz): " + std::to_string(source ? edge.source_frequency : edge.target_frequency),
             "Current Clock frequency was not shown");
    contains(text, prefix + "max backlog: " + std::to_string(source ? edge.source_backlog : edge.target_backlog),
             "Current Clock backlog was not shown");
    contains(text, prefix + "late data: " + (source ? edge.source_late_name : edge.target_late_name),
             "Current Clock late policy was not shown as a Clock contract");
    contains(text, prefix + "capability version: 1.0.0", "Current Clock capability version was not shown");
    contains(text, prefix + "work budget (ns): 250000", "Current Clock work budget was not shown");
    contains(text, prefix + "phase (ns): 17", "Current Clock phase was not shown");
    const uint64_t frequency = source ? edge.source_frequency : edge.target_frequency;
    const auto deadline = 1'000'000'000ull / frequency + (1'000'000'000ull % frequency != 0);
    contains(text, prefix + "relative deadline (ns): " + std::to_string(deadline),
             "Current Clock relative deadline was not shown");
    contains(text, prefix + "deadline policy: Observe", "Current Clock deadline policy was not shown");
}

void unchanged(const application::EditorFeatureGraphDocument& document,
    const std::shared_ptr<const feature::FeatureGraphDocument>& before, uint64_t revision) {
    require(document.snapshot_owner() == before && document.revision() == revision &&
                !document.dirty() && !document.can_undo() && !document.can_redo(),
            "Read-only saved edge inspection changed graph/history");
}

void test_persisted_transports_show_saved_fields_and_current_contracts(const SavedGraph& saved) {
    application::EditorFeatureGraphDocument document{saved.reload(), catalog()};
    const auto before = document.snapshot_owner(); const auto revision = document.revision(); Harness ui;
    // Document-owned metadata must retain precedence over a caller's different catalog.
    const auto caller_catalog = catalog(CatalogCase::kStaleSource);
    application::EditorFeatureGraphPanel::Actions actions; actions.feature_catalog = &caller_catalog;
    ui.draw(document, actions);
    for (const auto& edge : kEdges) {
        const auto text = ui.select(document, edge, actions);
        expect_saved(text, edge);
        contains(text, "Source schema: " + std::to_string(edge.schema), "Current output schema was not shown");
        contains(text, "Target schema: " + std::to_string(edge.schema), "Current input schema was not shown");
        contains(text, "Source port version: " + std::to_string(edge.version), "Current output version was not shown");
        contains(text, "Target port version: " + std::to_string(edge.version), "Current input version was not shown");
        expect_clock(text, edge, true); expect_clock(text, edge, false);
        unchanged(document, before, revision);
    }
}

void test_missing_or_stale_contract_keeps_saved_edge_facts(const SavedGraph& saved) {
    Harness ui;
    for (const auto scenario : {CatalogCase::kMissingSource, CatalogCase::kMissingPort,
                                CatalogCase::kStaleSource, CatalogCase::kNone}) {
        application::EditorFeatureGraphDocument document{saved.reload(), catalog(scenario)};
        const auto before = document.snapshot_owner(); const auto revision = document.revision();
        ui.draw(document); const auto text = ui.select(document, kEdges.front());
        expect_saved(text, kEdges.front());
        auto lower = text;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        contains(lower, "source contract unavailable", "Missing/stale source contract was not identified");
        require(text.find("Source schema:") == std::string::npos,
                "Unavailable source contract was presented as a current port schema");
        if (scenario == CatalogCase::kStaleSource) {
            contains(lower, "stale", "Definition-version mismatch was not identified as stale");
            require(text.find("Source Clock frequency (Hz):") == std::string::npos,
                    "Stale definition was presented as the saved edge's current Clock contract");
        }
        if (scenario == CatalogCase::kNone) {
            contains(lower, "target contract unavailable", "No-catalog target was not identified");
            require(text.find("Target schema:") == std::string::npos,
                    "Absent catalog fabricated a target schema");
        } else {
            contains(text, "Target schema: " + std::to_string(kEdges.front().schema),
                     "Unavailable source suppressed the independently available target schema");
            expect_clock(text, kEdges.front(), false);
        }
        unchanged(document, before, revision);
    }

    for (const auto scenario : {CatalogCase::kMissingTarget, CatalogCase::kMissingTargetPort,
                                CatalogCase::kStaleTarget}) {
        application::EditorFeatureGraphDocument document{saved.reload(), catalog(scenario)};
        const auto before = document.snapshot_owner(); const auto revision = document.revision();
        ui.draw(document); const auto text = ui.select(document, kEdges.front());
        expect_saved(text, kEdges.front());
        contains(text, "Target contract unavailable", "Missing/stale target contract was not identified");
        require(text.find("Target schema:") == std::string::npos,
                "Unavailable target contract was presented as a current port schema");
        contains(text, "Source schema: " + std::to_string(kEdges.front().schema),
                 "Unavailable target suppressed the independently available source schema");
        expect_clock(text, kEdges.front(), true);
        if (scenario == CatalogCase::kStaleTarget) {
            contains(text, "stale", "Target definition-version mismatch was not identified as stale");
            require(text.find("Target Clock frequency (Hz):") == std::string::npos,
                    "Stale target was presented as the saved edge's current Clock contract");
        }
        unchanged(document, before, revision);
    }

    auto missing_node = saved.reload();
    std::erase_if(missing_node.nodes, [](const auto& node) { return node.node_id == feature::FeatureNodeId{31}; });
    application::EditorFeatureGraphDocument document{std::move(missing_node), catalog()};
    const auto before = document.snapshot_owner(); const auto revision = document.revision();
    ui.draw(document); const auto text = ui.select(document, kEdges.front());
    expect_saved(text, kEdges.front());
    contains(text, "Source contract unavailable", "Missing endpoint node was not identified");
    unchanged(document, before, revision);

    auto no_edges = saved.reload(); no_edges.edges.clear();
    application::EditorFeatureGraphDocument switched{std::move(no_edges), catalog()};
    const auto switched_before = switched.snapshot_owner(); const auto switched_revision = switched.revision();
    const auto switched_text = ui.draw(switched);
    require(switched_text.find("Saved edge") == std::string::npos,
            "Opening a root without the selected edge retained a stale edge detail");
    unchanged(switched, switched_before, switched_revision);
}
}  // namespace

int main() {
    SavedGraph saved;
    test_persisted_transports_show_saved_fields_and_current_contracts(saved);
    test_missing_or_stale_contract_keeps_saved_edge_facts(saved);
    std::cout << "Persisted edge constraints ImGui inspection passed\n";
}
