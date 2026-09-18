#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#include "project_operation.hpp"

int main() {
    using namespace aeg;
    const auto root = std::filesystem::temp_directory_path() / "aeg_project_operation_smoke";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    const auto project = root / "Project.aegproj";
    std::ofstream(project)
        << "format_version = 2\n[project]\nname = \"Smoke\"\nuuid = \"00000000-0000-0000-0000-000000000001\"\n"
        << "[engine]\nassociation = \"AEG\"\nsdk_requirement = \"1.0\"\n"
        << "[module]\nid = \"Smoke.GameModule\"\ntarget = \"GameModule\"\n"
        << "[paths]\nassets = \"Assets\"\nsource = \"Source\"\nbuild = \"Build\"\ncache = \"Cache\"\nsaved = "
           "\"Saved\"\nbinaries = \"Binaries\"\n"
        << "[build]\ntarget = \"windows-x64-vulkan\"\nstartup_scene = "
           "\"Assets/Scenes/Main.aegscene\"\nstartup_pipelines = []\n";

    tooling::ProjectOperationService service;
    tooling::ProjectOperationRequest request{project, root, "Debug", "Game", tooling::ProjectOperation::kCook};
    const auto started = service.start(request);
    if (!started || service.start(request))
        return 1;
    tooling::ProjectOperationStatus terminal{};
    bool done = false;
    for (int i = 0; i < 100000 && !done; ++i) {
        const auto polled = service.poll(started.value);
        if (!polled)
            return 2;
        terminal = polled.value;
        done = terminal.state != tooling::ProjectOperationState::kRunning;
        if (!done)
            std::this_thread::yield();
    }
    if (!done || terminal.state != tooling::ProjectOperationState::kSucceeded) {
        std::cerr << "state=" << static_cast<int>(terminal.state) << " done=" << done
                  << " msg=" << terminal.result.message << "\n";
        return 3;
    }
    if (terminal.pipeline_stages.size() != 3 || terminal.pipeline_stages[0] != "ProjectValidationFeature" ||
        terminal.pipeline_stages[1] != "ProjectConfigureFeature" ||
        terminal.pipeline_stages[2] != "ProjectCompileFeature")
        return 30;
    tooling::ProjectOperationRequest cook_request{
        project,        root, "Debug", "Game", tooling::ProjectOperation::kCook, root / "missing.module.manifest.json",
        root / "Cooked"};
    const auto cook_started = service.start(cook_request);
    if (!cook_started)
        return 31;
    done = false;
    for (int i = 0; i < 100000 && !done; ++i) {
        const auto polled = service.poll(cook_started.value);
        if (!polled)
            return 32;
        terminal = polled.value;
        done = terminal.state != tooling::ProjectOperationState::kRunning;
        if (!done)
            std::this_thread::yield();
    }
    if (!done || terminal.state != tooling::ProjectOperationState::kFailed || terminal.diagnostics.empty() ||
        terminal.diagnostics.front().code != "cook.module" || terminal.pipeline_stages.size() != 3)
        return 33;
    const auto missing =
        service.start({root / "missing.aegproj", root, "Debug", "Game", tooling::ProjectOperation::kBuild});
    if (!missing)
        return 4;
    if (!service.cancel(missing.value))
        return 5;
    const auto cancelled = service.poll(missing.value);
    if (!cancelled || cancelled.value.state != tooling::ProjectOperationState::kCancelled)
        return 6;
    if (service.poll(999999))
        return 7;
    std::filesystem::remove_all(root, error);
    std::cout << "Project operation smoke passed\n";
    return 0;
}
