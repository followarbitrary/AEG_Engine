#include "feature_clock_test_fixture.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <imgui_internal.h>

#include "dynamic_module.hpp"
#include "editor_feature_definition_panel.hpp"
#include "editor_node_contract_storage.hpp"
#include "feature_contract_json.hpp"
#include "module.hpp"
#include "project_context.hpp"

namespace {
namespace fs = std::filesystem;
using namespace aeg;
constexpr std::string_view kSelectedModule = "com.aeg.generated.project.feature";
constexpr std::string_view kOtherModule = "com.aeg.generated.unselected.project";
constexpr std::string_view kModuleName = "Generated Project Module";
constexpr std::string_view kTarget = "GeneratedProjectModuleTarget";
constexpr std::string_view kPlatform = "GeneratedProjectModuleFixture";
constexpr std::string_view kBuild = "generated-ui";

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void succeeds(const foundation::Result& result, const std::string& phase) {
    require(static_cast<bool>(result), phase + ": " + result.message);
}

template<class T> void succeeds(const foundation::ResultValue<T>& result, const std::string& phase) {
    succeeds(result.result, phase);
}

fs::path project_root(const fs::path& root) { return root / "Source Project"; }

std::string read_text(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "Cannot read fixture file: " + path.string());
    return {std::istreambuf_iterator<char>(input), {}};
}

void write_text(const fs::path& path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    require(static_cast<bool>(output), "Cannot write fixture file: " + path.string());
}

application::NodeContractFile generated_contract(const fs::path& project) {
    const auto files = application::scan_editor_node_contracts(project);
    succeeds(files, "read actual UI generated contract");
    require(files.value.size() == 1 && files.value.front().draft.generated,
            "UI must generate exactly one contract, without a draft sidecar");
    return files.value.front();
}

module::ModuleMetadata metadata_for(const feature::FeatureDefinition& definition) {
    module::ModuleMetadata metadata{};
    metadata.module = {module::ModuleId{kSelectedModule}, std::string{kModuleName}, {1, 0, 0}, {}};
    metadata.build_id = kBuild;
    metadata.target = kPlatform;
    metadata.configuration = "Debug";
    succeeds(metadata.feature_catalog.export_definition(definition), "export generated definition to source metadata");
    const auto digest = module::module_contract_sha256(metadata);
    succeeds(digest, "digest official source metadata");
    metadata.contract_digest = digest.value;
    return metadata;
}

void write_project(const fs::path& root) {
    const auto project = project_root(root);
    require(!fs::exists(project), "The UI generation fixture requires a fresh source project");
    fs::create_directories(project);
    application::project::ProjectConfig selected{};
    selected.name = "Selected Generated Project";
    selected.project_uuid = "00000000-0000-4000-8000-000000000501";
    selected.module.id = kSelectedModule;
    selected.module.target = kTarget;
    selected.target = kPlatform;
    const auto selected_path = project / "Selected.aegproj";
    succeeds(application::project::ProjectFileCodec{}.save(selected_path, selected), "write explicitly selected project");
    auto other = selected;
    other.name = "Unselected Generated Project";
    other.project_uuid = "00000000-0000-4000-8000-000000000502";
    other.module.id = kOtherModule;
    other.module.target = "UnselectedTarget";
    succeeds(application::project::ProjectFileCodec{}.save(project / "Project.aegproj", other),
             "write coexisting unselected project");
    const auto selected_before = read_text(selected_path);
    const auto other_before = read_text(project / "Project.aegproj");

    feature::FeatureGraphDocument graph{};
    graph.asset_uuid = "00000000-0000-4000-8000-000000000503";
    graph.schema_version = 1;
    graph.graph_version = 1;
    application::EditorFeatureGraphDocument document{graph, {}};
    require(document.catalog().entries().empty(), "First-project generation must begin with an empty catalog");
    std::vector<std::string> errors;
    {
        struct ContextOwner {
            ImGuiContext* context{ImGui::CreateContext()};
            ~ContextOwner() { ImGui::DestroyContext(context); }
        } context_owner;
        auto* context = context_owner.context;
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {1600, 5000};
        io.DeltaTime = 1.0f / 60.0f;
        io.ConfigErrorRecoveryEnableAssert = false;
        io.ConfigErrorRecoveryEnableDebugLog = false;
        io.ConfigErrorRecoveryEnableTooltip = false;
        unsigned char* pixels{};
        int width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        context->ErrorCallbackUserData = &errors;
        context->ErrorCallback = [](ImGuiContext*, void* data, const char* message) {
            static_cast<std::vector<std::string>*>(data)->emplace_back(message);
        };
        application::EditorFeatureDefinitionPanel panel;
        std::string rendered_controls;
        std::string controls_before_log_finish;
        std::string last_activation;
        unsigned frame_count{};
        std::size_t log_bytes_after_finish{};
        bool logging_before_finish{}, logging_after_finish{};
        const auto frame = [&](const char* activate = nullptr) {
            ImGui::NewFrame();
            ++frame_count;
            if (activate) {
                last_activation = activate;
                auto* window = ImGui::FindWindowByName("Feature Definitions");
                require(window != nullptr, "Actual Feature Definitions window was not created");
                context->NavWindow = window;
                context->NavId = window->GetID(activate);
                context->NavInputSource = ImGuiInputSource_Keyboard;
                context->NavActivateId = context->NavActivateDownId = context->NavId;
            }
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::LogToBuffer();
            panel.draw(document, project, {}, {}, selected_path);
            controls_before_log_finish = context->LogBuffer.c_str(); // Diagnostic only; the existing oracle remains below.
            logging_before_finish = context->LogEnabled;
            rendered_controls = context->LogBuffer.c_str();
            ImGui::LogFinish();
            logging_after_finish = context->LogEnabled;
            log_bytes_after_finish = static_cast<std::size_t>(context->LogBuffer.size());
            ImGui::Render();
        };
        frame();
        frame("New Feature");
        frame();
        const char* clock_labels[]{"Clock capability version", "Clock timing", "Clock phase (ns)",
            "Clock relative deadline (ns)", "Clock deadline policy", "Clock work budget (ns)"};
        if (std::any_of(std::begin(clock_labels), std::end(clock_labels),
                [&](const char* label) { return rendered_controls.find(label) == std::string::npos; })) {
            const auto* window = ImGui::FindWindowByName("Feature Definitions");
            std::cerr << "CLOCK_UI_DIAGNOSTIC frames=" << frame_count << " last-activation=" << last_activation
                      << " current-context=" << (ImGui::GetCurrentContext() == context)
                      << " display=" << io.DisplaySize.x << 'x' << io.DisplaySize.y
                      << " font-atlas=" << width << 'x' << height << " pixels=" << (pixels != nullptr)
                      << " selected-project-exists=" << fs::is_regular_file(selected_path)
                      << " catalog-entries=" << document.catalog().entries().size()
                      << " imgui-errors=" << errors.size() << " window=" << (window != nullptr) << '\n';
            if (window)
                std::cerr << "CLOCK_UI_WINDOW active=" << window->Active << " hidden=" << window->Hidden
                          << " collapsed=" << window->Collapsed << " skip-items=" << window->SkipItems
                          << " begin-count=" << window->BeginCount << " size=" << window->Size.x << 'x' << window->Size.y
                          << " nav-id=" << context->NavId << '\n';
            std::cerr << "CLOCK_UI_LOG before-finish-bytes=" << controls_before_log_finish.size()
                      << " after-finish-bytes=" << log_bytes_after_finish
                      << " logging-before=" << logging_before_finish << " logging-after=" << logging_after_finish << '\n';
            for (const auto* label : clock_labels)
                std::cerr << "CLOCK_UI_LABEL label=\"" << label << "\" oracle-present="
                          << (rendered_controls.find(label) != std::string::npos) << " before-finish-present="
                          << (controls_before_log_finish.find(label) != std::string::npos) << '\n';
            std::cerr << "CLOCK_UI_RENDERED_ORACLE_BEGIN\n" << rendered_controls << "\nCLOCK_UI_RENDERED_ORACLE_END\n"
                      << "CLOCK_UI_RENDERED_BEFORE_FINISH_BEGIN\n" << controls_before_log_finish
                      << "\nCLOCK_UI_RENDERED_BEFORE_FINISH_END\n";
            for (const auto& error : errors) std::cerr << "CLOCK_UI_IMGUI_ERROR " << error << '\n';
        }
        for (const auto* label : clock_labels)
            require(rendered_controls.find(label) != std::string::npos,
                    "Actual Clock authoring control is missing: " + std::string(label));
        frame("Save and Generate");
        frame();
    }
    for (const auto& error : errors) std::cerr << error << '\n';
    require(errors.empty(), "Actual generation UI reported an ImGui error");
    const auto generated = generated_contract(project);
    const auto& definition = generated.draft.entry;
    const auto& clock = definition.contract.clock;
    require(clock.capability_version == feature::Version{1, 0, 0} && clock.timing &&
            clock.timing->phase_ns == 0 && clock.timing->relative_deadline_ns == 16'666'667 &&
            clock.timing->deadline_policy == feature::DeadlinePolicy::kObserve && clock.work_budget_ns == 250000,
            "Actual UI generation did not preserve the explicit periodic Clock contract");
    require(definition.owner_module == feature::ModuleIdentity{kSelectedModule} &&
            definition.owner_module != feature::ModuleIdentity{kOtherModule} &&
            definition.owner_module != feature::ModuleIdentity{kTarget},
            "Actual UI generation did not retain the explicitly selected project's module.id");
    require(document.catalog().entries().size() == 1 && document.dirty() && document.revision() == 1 &&
            document.catalog().entries().front().owner_module == definition.owner_module &&
            document.catalog().entries().front().definition_id == definition.id,
            "Actual UI generation did not publish the same definition and owner to its author catalog");
    require(read_text(selected_path) == selected_before && read_text(project / "Project.aegproj") == other_before,
            "Generation rewrote a selected or coexisting project file");

    const std::string suffix = ".flowgraph.contract.json";
    const auto filename = generated.path.filename().string();
    require(filename.ends_with(suffix), "Generated contract does not use the formal filename suffix");
    const auto type = filename.substr(0, filename.size() - suffix.size());
    std::vector<std::string> actual_files;
    for (const auto& item : fs::directory_iterator(project / "Source/FlowGraph")) {
        require(item.is_regular_file(), "UI generation retained a lock, temporary file or unexpected directory");
        actual_files.push_back(item.path().filename().string());
    }
    std::vector<std::string> expected_files{type + ".flowgraph.hpp", type + ".flowgraph.cpp",
        type + ".flowgraph.registration.cpp", type + ".flowgraph.contract.json"};
    std::sort(actual_files.begin(), actual_files.end());
    std::sort(expected_files.begin(), expected_files.end());
    require(actual_files == expected_files, "UI generation must install exactly its four declared outputs");

    const auto metadata = module::encode_module_metadata(metadata_for(definition));
    succeeds(metadata, "encode official generated source metadata");
    write_text(project / "Source/GameModule.metadata.json", metadata.value);
    auto mismatch = definition;
    ++mismatch.contract.clock.frequency_hz;
    const auto mismatched_metadata = module::encode_module_metadata(metadata_for(mismatch));
    succeeds(mismatched_metadata, "encode internally valid mismatched source metadata");
    write_text(project / "Source/MismatchedModule.metadata.json", mismatched_metadata.value);

    // These explicit fixture edits model the documented developer integration
    // steps. Production generation does not edit CMake or the module factory.
    const std::string module_source =
        "#if __has_include(\"concurrency.hpp\") || __has_include(\"pipeline.hpp\") || "
        "__has_include(\"serialization.hpp\") || __has_include(\"world_scope.hpp\") || "
        "__has_include(\"rhi.hpp\") || __has_include(\"editor_feature_definition_panel.hpp\")\n"
        "#error Generated game module must compile through the narrow GameSDK facade\n#endif\n"
        "#include \"module_export.hpp\"\n#include \"FlowGraph/" + type + ".flowgraph.hpp\"\n\n"
        "AEG_DEFINE_GAME_MODULE(aeg_define_GeneratedProjectModule) {\n"
        "    aeg::module::FeatureModuleDefinition definition{};\n"
        "    definition.descriptor = {aeg::module::ModuleId{\"" + std::string{kSelectedModule} + "\"}, \"" +
        std::string{kModuleName} + "\", {1, 0, 0}, {}};\n"
        "    definition.status = game::register_" + type + "(definition);\n"
        "    return definition;\n}\nAEG_EXPORT_GAME_MODULE_API(aeg_define_GeneratedProjectModule);\n";
    write_text(project / "Source/GameModule.cpp", module_source);
    const std::string cmake = std::string{R"CMAKE(cmake_minimum_required(VERSION 3.25)
project(GeneratedProjectModuleFixture LANGUAGES CXX)
if(NOT DEFINED ENGINE_DIR OR NOT IS_DIRECTORY "${ENGINE_DIR}")
    message(FATAL_ERROR "Explicit ENGINE_DIR is required")
endif()
set(AEG_BUILD_EDITOR OFF CACHE BOOL "No nested Editor" FORCE)
set(AEG_BUILD_TESTS OFF CACHE BOOL "No nested tests" FORCE)
set(AEG_BUILD_TOOLS ON CACHE BOOL "Build the official metadata tool" FORCE)
include("${ENGINE_DIR}/cmake/EngineTools.cmake")
aeg_add_game_module(GeneratedProjectModuleTarget
    SOURCES Source/GameModule.cpp
)CMAKE"} + "        Source/FlowGraph/" + type + ".flowgraph.cpp\n"
        "        Source/FlowGraph/" + type + ".flowgraph.registration.cpp\n" + R"CMAKE(    METADATA "${CMAKE_CURRENT_SOURCE_DIR}/Source/GameModule.metadata.json"
    BUILD_ID generated-ui
    TARGET_PLATFORM GeneratedProjectModuleFixture
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/ModuleBundle")
)CMAKE";
    write_text(project / "CMakeLists.txt", cmake);
    require(!fs::exists(project / "ModuleBundle"), "Fixture writer must not supply a prebuilt DLL or bundle");
    std::cout << "GENERATED_PROJECT_UI_WRITTEN definition=" << definition.id.value()
              << " owner=" << definition.owner_module.value()
              << " contract=" << feature::feature_contract_digest(definition) << " outputs=4\n";
}

void compare_contract(const feature::FeatureDefinition& expected, const feature::FeatureDefinition& actual,
                      const std::string& phase) {
    const auto expected_json = feature::encode_feature_contract_json(expected.contract);
    const auto actual_json = feature::encode_feature_contract_json(actual.contract);
    succeeds(expected_json, phase + " expected contract codec");
    succeeds(actual_json, phase + " actual contract codec");
    require(actual.id == expected.id && actual.version == expected.version && actual.owner_module == expected.owner_module &&
            actual_json.value == expected_json.value &&
            feature::feature_contract_digest(actual) == feature::feature_contract_digest(expected),
            phase + " differs from the exact UI generated definition, owner, version or full contract");
}

void compare_metadata(const feature::FeatureDefinition& expected, const module::ModuleMetadata& metadata,
                      const std::string& phase) {
    require(metadata.module.id == module::ModuleId{kSelectedModule} && metadata.module.name == kModuleName &&
            metadata.module.version == module::Version{1, 0, 0} && metadata.module.dependencies.empty() &&
            metadata.build_id == kBuild && metadata.target == kPlatform && metadata.configuration == "Debug" &&
            metadata.feature_catalog.entries().size() == 1,
            phase + " module identity or Feature count differs from the selected source project");
    const auto& entry = metadata.feature_catalog.entries().front();
    feature::FeatureDefinition actual{};
    actual.id = entry.definition_id;
    actual.version = entry.version;
    actual.owner_module = entry.owner_module;
    actual.contract = entry.contract;
    compare_contract(expected, actual, phase);
    require(entry.contract_digest == feature::feature_contract_digest(expected), phase + " Feature digest differs");
    const auto digest = module::module_contract_sha256(metadata);
    succeeds(digest, phase + " official Module digest");
    require(digest.value == metadata.contract_digest, phase + " Module digest differs from its declarations");
}

void verify_module(const fs::path& root) {
    const auto project = project_root(root);
    const auto selected = application::project::ProjectFileCodec{}.load(project / "Selected.aegproj");
    succeeds(selected, "read selected project for DLL verification");
    require(selected.value.config.module.id == kSelectedModule && selected.value.config.module.target == kTarget,
            "Selected fixture project identity changed during build");
    const auto expected = generated_contract(project).draft.entry;
    require(expected.owner_module == feature::ModuleIdentity{kSelectedModule}, "Generated author owner is incorrect");
    const auto source = module::read_module_metadata(project / "Source/GameModule.metadata.json");
    succeeds(source, "read official source Module metadata");
    compare_metadata(expected, source.value, "source metadata");
    const auto bundle = module::validate_module_bundle(project / "ModuleBundle/module.manifest.json");
    succeeds(bundle, "formally validate built DLL bundle");
    compare_metadata(expected, bundle.value.metadata, "built bundle metadata");
    require(source.value.contract_digest == bundle.value.metadata.contract_digest,
            "Build publication changed the selected source Module contract");
    const auto good_manifest = read_text(bundle.value.manifest_path);
    const auto good_metadata = read_text(bundle.value.metadata_path);
    const auto good_binary_hash = bundle.value.binary_sha256;

    feature::FeatureRegistry features{aeg::feature::test::clock_test_host()};
    module::ModuleRegistry modules{features};
    module::DynamicModuleLoader loader{modules};
    module::DynamicModuleHandle handle{};
    succeeds(loader.load(bundle.value, handle), "load actual newly compiled generated DLL");
    {
        const auto imported = loader.make_feature_module(handle);
        succeeds(imported, "import actual DLL exported contracts");
        require(imported.value.descriptor.id == module::ModuleId{kSelectedModule} &&
                imported.value.feature_definitions.size() == 1 && imported.value.feature_catalog.entries().size() == 1,
                "Actual DLL does not export exactly the selected project module and generated Feature");
        const auto& actual = imported.value.feature_definitions.front();
        compare_contract(expected, actual, "actual DLL definition");
        require(static_cast<bool>(actual.execute), "Actual generated DLL callback is missing");
        const feature::InvocationToken token{feature::InvocationId{1}, feature::AttemptId{2},
            feature::RuntimeGeneration{3}, 0, feature::ProcessRevision{}, feature::StateVersion{}};
        const auto executed = actual.execute(token, {}, {}, {});
        require(executed.result.code == foundation::ResultCode::kUnsupported &&
                executed.result.message == "Feature implementation is not ready",
                "Actual DLL callback did not execute the generated Unsupported stub");
    }
    succeeds(loader.unload(handle), "release the successful generated DLL and all imported code leases");

    const auto mismatch = module::validate_module_bundle(root / "MismatchBundle/module.manifest.json");
    succeeds(mismatch, "formally validate the internally consistent negative bundle");
    require(mismatch.value.binary_sha256 == good_binary_hash &&
            mismatch.value.metadata.contract_digest != bundle.value.metadata.contract_digest,
            "Negative fixture must use the same real DLL with a different valid metadata contract");
    module::DynamicModuleHandle mismatched_handle{};
    succeeds(loader.load(mismatch.value, mismatched_handle), "negotiate actual DLL from valid negative bundle");
    {
        const auto rejected = loader.make_feature_module(mismatched_handle);
        require(!rejected && rejected.result.code == foundation::ResultCode::kInvalidArgument &&
                rejected.result.message == "DLL Feature contracts differ from the verified sidecar",
                "A validly hashed metadata bundle with a different actual DLL contract was accepted");
    }
    succeeds(loader.unload(mismatched_handle), "release rejected DLL without retaining a failed import lease");
    const auto retained = module::validate_module_bundle(bundle.value.manifest_path);
    succeeds(retained, "revalidate unchanged successful bundle after negative verification");
    require(read_text(bundle.value.manifest_path) == good_manifest && read_text(bundle.value.metadata_path) == good_metadata &&
            retained.value.binary_sha256 == good_binary_hash,
            "Negative verification changed the successful manifest, metadata or DLL");
    std::cout << "GENERATED_PROJECT_MODULE_COMPLETE definition=" << expected.id.value()
              << " owner=" << expected.owner_module.value()
              << " contract=" << feature::feature_contract_digest(expected)
              << " stub=Unsupported metadata_mismatch=rejected\n";
}
}  // namespace

int main(int argc, char** argv) try {
    require(argc == 3, "usage: --write-project TEST_ROOT | --verify-module TEST_ROOT");
    const auto root = fs::absolute(argv[2]).lexically_normal();
    if (std::string_view{argv[1]} == "--write-project") write_project(root);
    else if (std::string_view{argv[1]} == "--verify-module") verify_module(root);
    else throw std::runtime_error("Unknown generated project module fixture command");
    return 0;
} catch (const std::exception& exception) {
    std::cerr << "Generated project module fixture failed: " << exception.what() << '\n';
    return 73;
}
