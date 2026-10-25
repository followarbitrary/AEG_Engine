#include "editor_project_operations.hpp"

#include <exception>
#include <new>
#include <string>
#include <utility>

#include <imgui.h>

#include "composition/editor_parameter_widget.hpp"
#include "project_context.hpp"

namespace aeg::application {
namespace {

tooling::ProjectOperationStatus idle_status() {
    tooling::ProjectOperationStatus status;
    status.state = tooling::ProjectOperationState::kSucceeded;
    status.execution_complete = true;
    return status;
}

std::string path_text(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

bool edit_path(const char* label, std::filesystem::path& path) {
    auto text = path_text(path);
    if (!draw_parameter_text(label, text)) return false;
    path = std::filesystem::path{std::u8string{text.begin(), text.end()}};
    return true;
}

const char* operation_state(const tooling::ProjectOperationStatus& status) noexcept {
    switch (status.state) {
    case tooling::ProjectOperationState::kRunning: return "Running";
    case tooling::ProjectOperationState::kSucceeded: return "Succeeded";
    case tooling::ProjectOperationState::kFailed: return "Failed";
    case tooling::ProjectOperationState::kCancelled:
        return status.execution_complete ? "Cancelled" : "Cancelling";
    }
    return "Unknown";
}

const char* process_state(feature::ProcessStatus state) noexcept {
    switch (state) {
    case feature::ProcessStatus::kReady: return "Ready";
    case feature::ProcessStatus::kSuspended: return "Suspended";
    case feature::ProcessStatus::kCancelled: return "Cancelled";
    case feature::ProcessStatus::kCompleted: return "Completed";
    }
    return "Unknown";
}

void draw_feature_execution(const tooling::ProjectOperationFeatureExecution& execution) {
    const auto& source = execution.source;
    ImGui::Text("Definition: %llu", static_cast<unsigned long long>(source.definition_id.value()));
    ImGui::Text("Node: %llu  Instance: %llu", static_cast<unsigned long long>(source.node_id.value()),
                static_cast<unsigned long long>(source.instance_id.value()));
    ImGui::Text("Owner module: %llu  Generation: %llu",
                static_cast<unsigned long long>(source.owner_module.value()),
                static_cast<unsigned long long>(execution.generation.value()));
    ImGui::Text("Definition digest: %llu", static_cast<unsigned long long>(execution.definition_digest));
    ImGui::Text("Process: %llu  %s", static_cast<unsigned long long>(execution.process.id.value()),
                process_state(execution.process.status));
    if (!execution.process.current_state.empty())
        ImGui::Text("Process state: %s", execution.process.current_state.c_str());
    if (const auto& issued = execution.invocation.last_issued; issued)
        ImGui::Text("Invocation: %llu  Attempt: %llu",
                    static_cast<unsigned long long>(issued->token.invocation_id.value()),
                    static_cast<unsigned long long>(issued->token.attempt_id.value()));
    if (execution.invocation.last_committed)
        ImGui::TextUnformatted("Invocation committed");
    if (const auto& failure = execution.invocation.last_failed; failure)
        ImGui::TextWrapped("Invocation failure: %s", failure->result.message.c_str());
    for (const auto& effect : execution.side_effects)
        ImGui::Text("Effect: %llu v%u  kind=%u  commit=%u",
                    static_cast<unsigned long long>(effect.id.value()), effect.version,
                    static_cast<unsigned>(effect.kind), static_cast<unsigned>(effect.commit));
}

}  // namespace

EditorProjectOperations::EditorProjectOperations() : status_(idle_status()) {}
EditorProjectOperations::~EditorProjectOperations() { shutdown(); }

bool EditorProjectOperations::active() const noexcept {
    return operation_id_ != 0 && !status_.execution_complete;
}

foundation::Result EditorProjectOperations::set_project(const std::filesystem::path& project_file) {
    try {
        tooling::ProjectOperationRequest next;
        std::filesystem::path selected;
        if (!project_file.empty()) {
            const auto parsed = project::ProjectFileCodec{}.load(project_file);
            if (!parsed) return parsed.result;
            selected = project_file;
            next.project_file = selected;
            next.target = parsed.value.config.module.target;
        }
        // Parse and allocate the replacement before retiring the old session.
        // A bad selection must leave its current request and operation intact.
        shutdown();
        project_file_ = std::move(selected);
        request_ = std::move(next);
        status_ = idle_status();
        action_error_ = {};
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor project operation selection allocation failed");
    } catch (const std::exception& error) {
        return foundation::unknown_exception(error);
    } catch (...) {
        return foundation::invalid_state("Editor project operation selection threw an exception");
    }
}

foundation::Result EditorProjectOperations::set_request(const tooling::ProjectOperationRequest& request) {
    if (active()) return foundation::invalid_state("A project operation is still active");
    if (project_file_.empty() || request.project_file != project_file_)
        return foundation::invalid_argument("Project operation request must name the exact selected project file");
    try {
        auto next = request;
        request_ = std::move(next);
        action_error_ = {};
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor project operation request allocation failed");
    } catch (const std::exception& error) {
        return foundation::unknown_exception(error);
    } catch (...) {
        return foundation::invalid_state("Editor project operation request threw an exception");
    }
}

foundation::Result EditorProjectOperations::start(tooling::ProjectOperation operation) {
    if (active()) return foundation::invalid_state("A project operation is still active");
    if (project_file_.empty() || request_.project_file != project_file_)
        return foundation::invalid_state("Select the saved project file before starting an operation");
    if (operation != tooling::ProjectOperation::kBuild && operation != tooling::ProjectOperation::kCook &&
        operation != tooling::ProjectOperation::kBuildAndCook)
        return foundation::invalid_argument("Project operation is invalid");
    try {
        auto next = request_;
        next.operation = operation;
        if (!service_) service_ = std::make_unique<tooling::ProjectOperationService>();
        const auto started = service_->start(next);
        if (!started) return started.result;
        request_ = std::move(next);
        operation_id_ = started.value;
        status_ = {};
        action_error_ = {};
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor project operation startup allocation failed");
    } catch (const std::exception& error) {
        return foundation::unknown_exception(error);
    } catch (...) {
        return foundation::invalid_state("Editor project operation startup threw an exception");
    }
}

foundation::Result EditorProjectOperations::poll() {
    if (!service_ || operation_id_ == 0) return foundation::Result::success();
    try {
        auto current = service_->poll(operation_id_);
        if (!current) return current.result;
        status_ = std::move(current.value);
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor project operation status allocation failed");
    } catch (const std::exception& error) {
        return foundation::unknown_exception(error);
    } catch (...) {
        return foundation::invalid_state("Editor project operation polling threw an exception");
    }
}

foundation::Result EditorProjectOperations::cancel() {
    if (!active()) return foundation::Result::success();
    try {
        const auto cancelled = service_->cancel(operation_id_);
        if (!cancelled) return cancelled;
        return poll();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("Editor project operation cancellation allocation failed");
    } catch (const std::exception& error) {
        return foundation::unknown_exception(error);
    } catch (...) {
        return foundation::invalid_state("Editor project operation cancellation threw an exception");
    }
}

void EditorProjectOperations::shutdown() noexcept {
    if (service_ && operation_id_ != 0) {
        try { static_cast<void>(service_->cancel(operation_id_)); } catch (...) {}
        try {
            auto completed = service_->wait(operation_id_);
            if (completed) status_ = std::move(completed.value);
            else action_error_ = std::move(completed.result);
        } catch (...) {}
    }
    // Keep the actual post-worker commits and publication paths. Destruction
    // still joins if cancellation or the owning final-status copy failed.
    service_.reset();
    operation_id_ = 0;
}

void EditorProjectOperations::draw() {
    ImGui::TextWrapped("Build and Cook use the saved project, Scene and FeatureGraph files. Unsaved edits are not saved by these actions.");
    const auto selected = path_text(project_file_);
    ImGui::TextWrapped("Project file: %s", selected.empty() ? "No project selected" : selected.c_str());

    auto draft = request_;
    bool changed = false;
    ImGui::BeginDisabled(active() || project_file_.empty());
    changed |= edit_path("Engine directory", draft.engine_directory);
    changed |= edit_path("CMake executable", draft.cmake_executable);
    changed |= edit_path("Module manifest", draft.module_manifest);
    ImGui::TextDisabled("Select the exact module manifest; custom absolute paths are supported.");
    changed |= edit_path("Output directory", draft.output_directory);
    changed |= draw_parameter_text("Configuration", draft.configuration);
    changed |= draw_parameter_text("Target", draft.target);
    ImGui::EndDisabled();
    if (changed) action_error_ = set_request(draft);

    ImGui::BeginDisabled(active() || project_file_.empty());
    if (ImGui::Button("Build")) action_error_ = start(tooling::ProjectOperation::kBuild);
    ImGui::SameLine();
    if (ImGui::Button("Cook")) action_error_ = start(tooling::ProjectOperation::kCook);
    ImGui::SameLine();
    if (ImGui::Button("Build and Cook")) action_error_ = start(tooling::ProjectOperation::kBuildAndCook);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!active() || status_.state == tooling::ProjectOperationState::kCancelled);
    if (ImGui::Button("Cancel")) action_error_ = cancel();
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::Text("Status: %s", operation_id_ == 0 ? "Idle" : operation_state(status_));
    if (!action_error_) ImGui::TextWrapped("Operation request: %s", action_error_.message.c_str());
    if (!status_.result.message.empty()) ImGui::TextWrapped("%s", status_.result.message.c_str());
    for (const auto& diagnostic : status_.diagnostics)
        ImGui::TextWrapped("%s: %s", diagnostic.code.c_str(), diagnostic.message.c_str());
    if (!status_.module_manifest.empty())
        ImGui::TextWrapped("Module manifest: %s", path_text(status_.module_manifest).c_str());
    if (!status_.game_manifest.empty())
        ImGui::TextWrapped("Game manifest: %s", path_text(status_.game_manifest).c_str());
    if (status_.feature_module.id.is_valid())
        ImGui::Text("Feature module: %s (%llu)", status_.feature_module.name.c_str(),
                    static_cast<unsigned long long>(status_.feature_module.id.value()));
    for (const auto& stage : status_.feature_stages) ImGui::BulletText("%s", stage.c_str());
    if (!status_.feature_executions.empty() && ImGui::CollapsingHeader("Feature executions")) {
        for (std::size_t index = 0; index != status_.feature_executions.size(); ++index) {
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::TreeNode("Execution", "Execution %zu", index + 1)) {
                draw_feature_execution(status_.feature_executions[index]);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }
}

}  // namespace aeg::application
