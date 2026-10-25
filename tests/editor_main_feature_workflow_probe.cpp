#include "editor_main_feature_workflow_probe.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include <imgui_internal.h>

#include "../Application/Editor/private/editor_scene_runtime.hpp"
#include "../Application/Editor/private/composition/editor_feature_graph_document.hpp"
#include "feature_graph_io.hpp"
#include "feature_parameters.hpp"
#include "project_operation.hpp"

namespace aeg::editor_test {
namespace {
using namespace aeg;
using Snapshot = application::runtime::FeatureRuntimeNodeSnapshot;
constexpr std::string_view kAsset = "00000000-0000-4000-8000-000000000203";
constexpr feature::FeatureNodeId kNode{"Operation.Logic"};
constexpr feature::FeatureDefinitionId kDefinition{"Dynamic.ProcessLogic"};
constexpr feature::ModuleIdentity kModule{"com.aeg.dynamic.fixture"};
constexpr std::string_view kEdited = "MainEdited";

std::filesystem::path normalized(const std::filesystem::path& path) {
    return std::filesystem::absolute(path).lexically_normal();
}
std::string file_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
bool contains(const std::vector<std::string>& values, std::string_view value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}
const feature::ProcessSubgraph* process(const feature::FeatureGraphDocument& graph) {
    const auto found = std::find_if(graph.process_subgraphs.begin(), graph.process_subgraphs.end(),
        [](const auto& candidate) { return candidate.owner_node == kNode; });
    return found == graph.process_subgraphs.end() ? nullptr : &*found;
}
bool committed(const Snapshot& snapshot) {
    if (snapshot.owners.size() != 1 || snapshot.owners.front().processes.size() != 1) return false;
    const auto& owner = snapshot.owners.front();
    const auto& state = owner.processes.front();
    if (!owner.invocations.last_committed || !owner.invocations.last_issued ||
        state.state.commit_count == 0 || state.state.payload.size() != 1 || state.state.payload.front() == 0)
        return false;
    const auto& commit = *owner.invocations.last_committed;
    // Main uses its real elapsed clock, so several legitimate ticks may precede this UI observation.
    return commit.attempt.token.generation == snapshot.generation &&
        commit.attempt.token.invocation_id.is_valid() && commit.attempt.token.attempt_id.is_valid() &&
        commit.attempt.process == state.id && commit.state_version == state.state.version &&
        commit.process_revision == state.revision &&
        commit.state_version == feature::StateVersion(commit.attempt.token.state_version.value() + 1);
}
}  // namespace

struct MainFeatureWorkflowProbe::Impl final {
    enum class Phase {
        kWaitOperation, kFocusManifest, kTypeManifest, kFocusConfiguration, kTypeConfiguration,
        kLoad, kLoadClicked, kWaitLoad, kSelect, kSelectClicked, kPlay, kWaitCommit, kWaitPause,
        kProcessOwner, kOwnerClicked, kFocusState, kTypeState, kAddState, kStateClicked,
        kFocusFrom, kTypeFrom, kFocusTo, kTypeTo, kAddTransition, kTransitionClicked,
        kSave, kSaveClicked, kApply, kApplyClicked, kWaitApply, kSelectApplied,
        kSelectAppliedClicked, kPlayApplied, kWaitAppliedCommit, kWaitFinalPause, kDone
    };
    Phase phase{Phase::kWaitOperation};
    std::chrono::steady_clock::time_point phase_started{std::chrono::steady_clock::now()};
    foundation::Result error{};
    std::optional<Snapshot> inspected{}, retained{};
    feature::RuntimeGeneration loaded_generation{}, applied_generation{};
    std::vector<uint8_t> original_parameters{};
    std::string original_disk{};
    std::filesystem::path source_path{};
    uint64_t inspections{0}, selection_baseline{0};
    bool queued_load{false}, queued_apply{false}, saved{false};

    void advance(Phase next) { phase = next; phase_started = std::chrono::steady_clock::now(); }
    foundation::Result fail(std::string message) {
        if (error) {
            error = foundation::invalid_state("Main Feature workflow phase " +
                std::to_string(static_cast<unsigned>(phase)) + ": " + message);
            std::cerr << error.message << '\n';
        }
        return error;
    }
    ImGuiWindow* window() const { return ImGui::FindWindowByName("Flow Graph###FlowGraph"); }
    bool scroll(bool bottom) const {
        auto* target = window();
        if (!target) return false;
        ImGui::FocusWindow(target);
        const float position = bottom ? target->ScrollMax.y : 0.f;
        if (std::abs(target->Scroll.y - position) > 1.f) {
            ImGui::SetScrollY(target, position);
            return false;
        }
        return true;
    }
    void activate(std::string_view label, bool table_node = false, bool input = false) const {
        auto* target = window();
        auto* context = ImGui::GetCurrentContext();
        const std::string text(label);
        const auto id = table_node
            ? ImGui::GetIDWithSeed(text.c_str(), nullptr, target->GetID("FeatureGraphNodes"))
            : target->GetID(text.c_str());
        context->NavWindow = target;
        context->NavId = id;
        context->NavInputSource = ImGuiInputSource_Keyboard;
        context->NavActivateId = context->NavActivateDownId = id;
        context->NavActivateFlags = input ? ImGuiActivateFlags_PreferInput : ImGuiActivateFlags_None;
    }
    bool type(std::string_view label, const std::string& value) const {
        auto* target = window();
        auto* input = target ? ImGui::GetInputTextState(target->GetID(std::string(label).c_str())) : nullptr;
        if (!input || ImGui::GetCurrentContext()->ActiveId != input->ID) {
            if (target) activate(label, false, true);
            return false;
        }
        input->SelectAll();
        ImGui::GetIO().AddInputCharactersUTF8(value.c_str());
        return true;
    }
    foundation::Result source(const FeatureWorkflowContext& context, const Snapshot& snapshot,
                               bool edited) {
        if (snapshot.asset_uuid != kAsset || snapshot.node_id != kNode || !snapshot.generation.is_valid() ||
            normalized(snapshot.source_path) != source_path || snapshot.owners.size() != 1)
            return fail("actual UI Inspect lost the exact asset/node/source/generation");
        const auto& owner = snapshot.owners.front();
        if (owner.definition_id != kDefinition || owner.owner_module != kModule ||
            !owner.owner.instance_id.is_valid() || owner.owner.definition_digest == 0 ||
            owner.contract.execution != feature::ExecutionKind::kProcess ||
            owner.contract.state != feature::StatePolicy::kProcessState ||
            owner.parameters != original_parameters || owner.processes.size() != 1 ||
            !owner.processes.front().id.is_valid() || owner.processes.front().owner != owner.owner.instance_id ||
            owner.processes.front().owner_module != kModule ||
            contains(owner.processes.front().states, kEdited) != edited)
            return fail("actual UI Inspect lost the executable Module/Process/parameters or authored states");
        const auto execution = context.scene.inspect_feature_execution(kAsset);
        if (!execution || execution.value.generation != snapshot.generation ||
            normalized(execution.value.source_path) != source_path || execution.value.provenance.size() != 1)
            return fail("UI owner has no matching published Scene execution");
        const auto& row = execution.value.provenance.front();
        auto path = row.runtime.owner_path.nodes;
        path.push_back(row.runtime.node_id);
        if (row.authored_node_id != kNode || !row.authored_owner_path.nodes.empty() ||
            row.runtime.definition_id != kDefinition || row.runtime.owner_module != kModule ||
            row.runtime.instance_id != owner.owner.instance_id || row.runtime.node_id != owner.owner.node_id ||
            row.runtime.process_id != owner.processes.front().id || owner.owner_path.nodes != path ||
            row.runtime.owner_path.nodes != std::vector<feature::FeatureNodeId>{feature::FeatureNodeId("asset:" + std::string(kAsset))})
            return fail("actual runtime owner/path differs from the actual published source row");
        return foundation::Result::success();
    }
    foundation::Result unchanged(const FeatureWorkflowContext& context) {
        const auto active = context.scene.inspect_feature_node(kAsset, kNode);
        if (!active || active.value.generation != loaded_generation || !source(context, active.value, false))
            return fail("author editing or Save replaced the active Scene generation");
        return foundation::Result::success();
    }
};

MainFeatureWorkflowProbe::MainFeatureWorkflowProbe() : impl_(std::make_unique<Impl>()) {}
MainFeatureWorkflowProbe::~MainFeatureWorkflowProbe() = default;

foundation::Result MainFeatureWorkflowProbe::before_ui(const FeatureWorkflowContext& context) {
    using Phase = Impl::Phase;
    auto& probe = *impl_;
    if (!probe.error || complete()) return probe.error;
    try {
        if (probe.phase != Phase::kWaitOperation &&
            std::chrono::steady_clock::now() - probe.phase_started > std::chrono::seconds(20))
            return probe.fail("actual UI action did not complete within its bounded frame pump");
        if (probe.phase == Phase::kWaitOperation) {
            if (!context.operation.execution_complete) return foundation::Result::success();
            if (context.operation.state != tooling::ProjectOperationState::kSucceeded ||
                context.request.operation != tooling::ProjectOperation::kBuildAndCook ||
                context.operation.feature_stages.size() != 5 || context.operation.feature_executions.size() != 5 ||
                !std::filesystem::is_regular_file(context.request.module_manifest) || !context.document ||
                normalized(context.scene.project_file()) != normalized(context.request.project_file))
                return probe.fail("the actual Main source BuildAndCook did not complete in this project session");
            const auto& graph = context.document->snapshot();
            if (graph.asset_uuid != kAsset || graph.nodes.size() != 1 || graph.nodes.front().node_id != kNode ||
                graph.nodes.front().definition_id != kDefinition || !process(graph) || contains(process(graph)->states, kEdited))
                return probe.fail("source fixture does not contain the expected untouched authored Process");
            const auto& entries = context.document->catalog().entries();
            const auto entry = std::find_if(entries.begin(), entries.end(), [](const auto& value) {
                return value.definition_id == kDefinition;
            });
            if (entry == entries.end()) return probe.fail("the source metadata did not supply the real Process contract");
            const auto parameters = feature::decode_feature_parameters(graph.nodes.front().parameter_blob, entry->contract.parameters);
            if (!parameters || parameters.value.size() != 1 || parameters.value.front().id != feature::ParameterId("Channel") ||
                parameters.value.front().payload != std::vector<uint8_t>{4})
                return probe.fail("source Process Channel is not the literal authored byte 4");
            for (const auto& execution : context.operation.feature_executions) {
                if (!execution.invocation.last_committed || execution.process.state.commit_count != 1 ||
                    execution.process.status != feature::ProcessStatus::kCompleted ||
                    execution.source.owner_module != feature::ModuleIdentity("Engine.Tooling.ProjectOperations") ||
                    execution.invocation.last_committed->attempt.process != execution.process.id ||
                    execution.invocation.last_committed->attempt.token.generation != execution.generation)
                    return probe.fail("source BuildAndCook omitted a real Tooling Process commit");
            }
            if (context.scene.inspect_feature_node(kAsset, kNode))
                return probe.fail("source-only project already had an executable Scene before explicit Load Module");
            probe.source_path = normalized(context.document_path);
            probe.original_disk = file_bytes(probe.source_path);
            probe.original_parameters = graph.nodes.front().parameter_blob;
            if (probe.original_disk.empty()) return probe.fail("authored source file was not readable");
            probe.advance(Phase::kFocusManifest);
            std::cout << "MAIN_FEATURE_WORKFLOW_BUILD_COMPLETE commits=5 module="
                      << context.request.module_manifest.string() << '\n';
        }
        if (!probe.window()) return foundation::Result::success();
        const auto focus = [&](const char* label, Phase next, bool bottom) {
            if (!probe.scroll(bottom)) return;
            probe.activate(label, false, true); probe.advance(next);
        };
        switch (probe.phase) {
        case Phase::kFocusManifest: focus("Module manifest", Phase::kTypeManifest, false); break;
        case Phase::kTypeManifest:
            if (probe.type("Module manifest", context.request.module_manifest.generic_string())) probe.advance(Phase::kFocusConfiguration);
            break;
        case Phase::kFocusConfiguration: focus("Configuration", Phase::kTypeConfiguration, false); break;
        case Phase::kTypeConfiguration:
            if (probe.type("Configuration", context.request.configuration)) probe.advance(Phase::kLoad);
            break;
        case Phase::kLoad:
            if (probe.scroll(false)) { probe.activate("Load Module"); probe.advance(Phase::kLoadClicked); }
            break;
        case Phase::kWaitLoad: {
            if (context.scene.feature_publication_pending()) break;
            const auto active = context.scene.inspect_feature_node(kAsset, kNode);
            if (!active) break;
            if (!probe.source(context, active.value, false)) return probe.error;
            probe.loaded_generation = active.value.generation;
            probe.advance(Phase::kSelect);
            break;
        }
        case Phase::kSelect:
        case Phase::kSelectApplied:
            if (probe.scroll(false)) {
                probe.selection_baseline = probe.inspections;
                probe.activate(std::to_string(kNode.value()), true);
                probe.advance(probe.phase == Phase::kSelect ? Phase::kSelectClicked : Phase::kSelectAppliedClicked);
            }
            break;
        case Phase::kPlay:
        case Phase::kPlayApplied:
            if (!context.invoke_view_action) return probe.fail("Main's actual workspace dispatcher is absent");
            if (const auto result = context.invoke_view_action("play"); !result) return probe.fail(result.message);
            probe.advance(probe.phase == Phase::kPlay ? Phase::kWaitCommit : Phase::kWaitAppliedCommit);
            break;
        case Phase::kWaitCommit:
        case Phase::kWaitAppliedCommit:
            if (probe.inspected && committed(*probe.inspected) &&
                probe.inspected->generation == (probe.phase == Phase::kWaitCommit ? probe.loaded_generation : probe.applied_generation)) {
                const bool edited = probe.phase == Phase::kWaitAppliedCommit;
                if (!probe.source(context, *probe.inspected, edited)) return probe.error;
                if (const auto result = context.invoke_view_action("pause"); !result) return probe.fail(result.message);
                probe.advance(edited ? Phase::kWaitFinalPause : Phase::kWaitPause);
            }
            break;
        case Phase::kWaitPause:
            if (context.scene.mode() == application::EditorSceneRuntime::Mode::kPause && probe.inspected &&
                committed(*probe.inspected) && probe.inspected->owners.front().invocations.open_attempts.empty()) {
                if (!probe.source(context, *probe.inspected, false)) return probe.error;
                probe.retained = *probe.inspected;
                probe.advance(Phase::kProcessOwner);
            }
            break;
        case Phase::kProcessOwner:
            if (probe.scroll(true)) { probe.activate("Use selected as Process owner"); probe.advance(Phase::kOwnerClicked); }
            break;
        case Phase::kFocusState: focus("New state", Phase::kTypeState, true); break;
        case Phase::kTypeState:
            if (probe.type("New state", std::string(kEdited))) probe.advance(Phase::kAddState);
            break;
        case Phase::kAddState:
            if (probe.scroll(true)) { probe.activate("Add state"); probe.advance(Phase::kStateClicked); }
            break;
        case Phase::kFocusFrom: focus("Transition from", Phase::kTypeFrom, true); break;
        case Phase::kTypeFrom:
            if (probe.type("Transition from", "Idle")) probe.advance(Phase::kFocusTo);
            break;
        case Phase::kFocusTo: focus("Transition to", Phase::kTypeTo, true); break;
        case Phase::kTypeTo:
            if (probe.type("Transition to", std::string(kEdited))) probe.advance(Phase::kAddTransition);
            break;
        case Phase::kAddTransition:
            if (probe.scroll(true)) { probe.activate("Add transition"); probe.advance(Phase::kTransitionClicked); }
            break;
        case Phase::kSave:
            if (probe.scroll(false)) { probe.activate("Save"); probe.advance(Phase::kSaveClicked); }
            break;
        case Phase::kApply:
            if (probe.scroll(false)) { probe.activate("Apply"); probe.advance(Phase::kApplyClicked); }
            break;
        case Phase::kWaitApply: {
            if (context.scene.feature_publication_pending()) break;
            const auto active = context.scene.inspect_feature_node(kAsset, kNode);
            if (!active || active.value.generation == probe.loaded_generation) break;
            if (!probe.source(context, active.value, true)) return probe.error;
            if (!probe.retained || active.value.owners.front().owner.instance_id == probe.retained->owners.front().owner.instance_id ||
                active.value.owners.front().processes.front().id == probe.retained->owners.front().processes.front().id)
                return probe.fail("Apply reused the retired Runtime owner/Process identity");
            probe.applied_generation = active.value.generation;
            probe.advance(Phase::kSelectApplied);
            break;
        }
        case Phase::kWaitFinalPause:
            if (context.scene.mode() == application::EditorSceneRuntime::Mode::kPause && probe.inspected &&
                probe.inspected->generation == probe.applied_generation && committed(*probe.inspected) &&
                probe.inspected->owners.front().invocations.open_attempts.empty()) {
                if (!probe.source(context, *probe.inspected, true)) return probe.error;
                if (!probe.retained || !committed(*probe.retained) || probe.retained->generation != probe.loaded_generation ||
                    contains(probe.retained->owners.front().processes.front().states, kEdited) ||
                    !probe.queued_load || !probe.queued_apply || !probe.saved)
                    return probe.fail("retained old owning Inspect snapshot changed during Apply/new DLL execution");
                const auto& owner = probe.inspected->owners.front();
                std::cout << "MAIN_FEATURE_WORKFLOW_COMPLETE asset=" << kAsset << " node=" << kNode.value()
                          << " module=" << owner.owner_module.value() << " instance=" << owner.owner.instance_id.value()
                          << " process=" << owner.processes.front().id.value()
                          << " old_generation=" << probe.loaded_generation.value()
                          << " generation=" << probe.applied_generation.value()
                          << " invocation=" << owner.invocations.last_committed->attempt.token.invocation_id.value()
                          << " edited_state=MainEdited saved=1 inspections=" << probe.inspections << '\n';
                probe.advance(Phase::kDone);
            }
            break;
        default: break;
        }
        return foundation::Result::success();
    } catch (const std::exception& exception) { return probe.fail(exception.what()); }
}

foundation::Result MainFeatureWorkflowProbe::after_ui(const FeatureWorkflowContext& context) {
    using Phase = Impl::Phase;
    auto& probe = *impl_;
    if (!probe.error || complete()) return probe.error;
    try {
        switch (probe.phase) {
        case Phase::kLoadClicked:
            if (!context.scene.feature_publication_pending() || context.scene.inspect_feature_node(kAsset, kNode))
                return probe.fail("actual Load Module button did not queue a frame-boundary publication");
            probe.queued_load = true; probe.advance(Phase::kWaitLoad); break;
        case Phase::kSelectClicked:
        case Phase::kSelectAppliedClicked: {
            const bool edited = probe.phase == Phase::kSelectAppliedClicked;
            if (probe.inspections <= probe.selection_baseline || !probe.inspected ||
                probe.inspected->generation != (edited ? probe.applied_generation : probe.loaded_generation))
                return probe.fail("actual selected-node UI did not call Main's real Inspect binding");
            if (!probe.source(context, *probe.inspected, edited)) return probe.error;
            probe.advance(edited ? Phase::kPlayApplied : Phase::kPlay); break;
        }
        case Phase::kOwnerClicked: probe.advance(Phase::kFocusState); break;
        case Phase::kStateClicked:
            if (!context.document || !context.document->dirty() || !process(context.document->snapshot()) ||
                std::count(process(context.document->snapshot())->states.begin(),
                           process(context.document->snapshot())->states.end(), kEdited) != 1)
                return probe.fail("actual Add state button did not author the unique MainEdited state");
            if (!probe.unchanged(context)) return probe.error;
            probe.advance(Phase::kFocusFrom); break;
        case Phase::kTransitionClicked:
            if (!context.document || !process(context.document->snapshot()) ||
                !contains(process(context.document->snapshot())->transitions, "Idle->MainEdited"))
                return probe.fail("actual Add transition button did not make MainEdited reachable");
            if (!probe.unchanged(context)) return probe.error;
            probe.advance(Phase::kSave); break;
        case Phase::kSaveClicked: {
            const auto disk = application::authoring::load_feature_graph_document(probe.source_path);
            if (!context.document || context.document->dirty() || !disk || file_bytes(probe.source_path) == probe.original_disk ||
                disk.value.asset_uuid != kAsset || disk.value.nodes.size() != 1 || disk.value.nodes.front().node_id != kNode ||
                disk.value.nodes.front().parameter_blob != probe.original_parameters || !process(disk.value) ||
                !contains(process(disk.value)->states, kEdited) || !contains(process(disk.value)->transitions, "Idle->MainEdited") ||
                disk.value.graph_version != context.document->snapshot().graph_version ||
                disk.value.draft_revision != context.document->snapshot().draft_revision)
                return probe.fail("actual Save button did not persist only the authored Process edit");
            if (!probe.unchanged(context)) return probe.error;
            probe.saved = true; probe.advance(Phase::kApply); break;
        }
        case Phase::kApplyClicked:
            if (!context.scene.feature_publication_pending() || !probe.unchanged(context))
                return probe.fail("actual Apply button did not retain the old generation until the next Scene frame");
            probe.queued_apply = true; probe.advance(Phase::kWaitApply); break;
        default: break;
        }
        return foundation::Result::success();
    } catch (const std::exception& exception) { return probe.fail(exception.what()); }
}

void MainFeatureWorkflowProbe::record_inspection(const foundation::ResultValue<Snapshot>& observed) {
    if (!observed || observed.value.asset_uuid != kAsset || observed.value.node_id != kNode) return;
    try { impl_->inspected = observed.value; ++impl_->inspections; }
    catch (const std::exception& exception) { static_cast<void>(impl_->fail(exception.what())); }
}
bool MainFeatureWorkflowProbe::complete() const noexcept { return impl_->phase == Impl::Phase::kDone; }
foundation::Result MainFeatureWorkflowProbe::finish() const {
    if (!impl_->error) return impl_->error;
    if (complete()) return foundation::Result::success();
    return foundation::invalid_state("Main Feature workflow did not finish: phase=" +
        std::to_string(static_cast<unsigned>(impl_->phase)) + " inspections=" + std::to_string(impl_->inspections) +
        " queued_load=" + std::to_string(impl_->queued_load) + " saved=" + std::to_string(impl_->saved) +
        " queued_apply=" + std::to_string(impl_->queued_apply));
}
}  // namespace aeg::editor_test
