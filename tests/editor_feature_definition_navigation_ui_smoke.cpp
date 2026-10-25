#include "editor_feature_definition_panel.hpp"
#include "feature_implementation_navigation_fixture.hpp"

#include <imgui_internal.h>
#include <functional>
#include <iostream>

namespace {
using namespace aeg;
using tests::navigation::require;
using Launcher = std::function<foundation::Result(const application::EditorIdeLaunchRequest&)>;

template<class Panel>
bool request_open(Panel& panel, feature::FeatureDefinitionId id) {
    if constexpr (requires { panel.request_open(id); }) {
        panel.request_open(id); return true;
    } else {
        std::cerr << "FEATURE_DEFINITION_REQUEST_OPEN_MISSING\n";
        return false;
    }
}

template<class Panel>
bool draw_panel(Panel& panel, application::EditorFeatureGraphDocument& document,
                const tests::navigation::Fixture& fixture,
                std::span<const feature::FeatureDefinitionId> compiled, const Launcher& launch) {
    if constexpr (requires { panel.draw(document, fixture.root, compiled, feature::AssetKindResolver{}, fixture.project_file, launch); }) {
        panel.draw(document, fixture.root, compiled, {}, fixture.project_file, launch);
        return true;
    } else {
        panel.draw(document, fixture.root, compiled, {}, fixture.project_file);
        return false;
    }
}

class Harness final {
public:
    Harness(application::EditorFeatureGraphDocument& document, const tests::navigation::Fixture& fixture,
            std::vector<feature::FeatureDefinitionId> compiled, Launcher launcher)
        : document_(document), fixture_(fixture), compiled_(std::move(compiled)), launcher_(std::move(launcher)) {
        context_ = ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr; io.DisplaySize = {1500, 4000}; io.DeltaTime = 1.0f / 60.0f;
        io.ConfigErrorRecoveryEnableAssert = io.ConfigErrorRecoveryEnableDebugLog = io.ConfigErrorRecoveryEnableTooltip = false;
        unsigned char* pixels{}; int width{}, height{};
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        context_->ErrorCallbackUserData = &errors;
        context_->ErrorCallback = [](ImGuiContext*, void* data, const char* message) {
            static_cast<std::vector<std::string>*>(data)->emplace_back(message);
        };
    }
    ~Harness() { ImGui::DestroyContext(context_); }
    void frame(const char* activate = nullptr) {
        ImGui::NewFrame();
        if (activate) {
            auto* window = ImGui::FindWindowByName("Feature Definitions");
            require(window != nullptr, "actual Feature Definition workspace is missing");
            context_->NavWindow = window; context_->NavId = window->GetID(activate);
            context_->NavInputSource = ImGuiInputSource_Keyboard;
            context_->NavActivateId = context_->NavActivateDownId = context_->NavId;
        }
        ImGui::Begin("Definition navigation capture", nullptr, ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer();
        ImGui::SetNextWindowPos({0, 0}); ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::SetNextWindowCollapsed(false);
        injectable = draw_panel(panel, document_, fixture_, compiled_, launcher_);
        rendered = context_->LogBuffer.c_str();
        ImGui::LogFinish(); ImGui::End(); ImGui::Render();
        require(errors.empty(), "ImGui reported a definition navigation error");
    }
    application::EditorFeatureDefinitionPanel panel;
    std::string rendered;
    std::vector<std::string> errors;
    bool injectable{};
private:
    ImGuiContext* context_{};
    application::EditorFeatureGraphDocument& document_;
    const tests::navigation::Fixture& fixture_;
    std::vector<feature::FeatureDefinitionId> compiled_;
    Launcher launcher_;
};

void test_request_open_and_explicit_source_action() {
    tests::navigation::Fixture fixture;
    const auto low = fixture.generate(feature::FeatureDefinitionId{uint64_t{7}}, "LowIdentity");
    const auto high = fixture.generate(feature::FeatureDefinitionId{uint64_t{0x8000000100000007ull}}, "HighIdentity");
    auto catalog = tests::feature_catalog(low.draft.entry);
    require(bool(catalog.export_definition(high.draft.entry)), "second navigation catalog entry is invalid");
    auto graph = tests::feature_graph(high.draft.entry);
    application::EditorFeatureGraphDocument document{graph, catalog};
    const auto original = document.snapshot_owner();
    const auto original_catalog = document.catalog().contract_digest();
    std::vector<std::pair<std::filesystem::path, std::string>> protected_sources;
    for (const auto& file : high.files.files) protected_sources.emplace_back(file, tests::read_text(file));
    std::vector<application::EditorIdeLaunchRequest> launches;
    bool reject_launch = false;
    Harness ui{document, fixture, {low.draft.entry.id, high.draft.entry.id}, [&](const auto& request) {
        launches.push_back(request);
        return reject_launch ? foundation::Result::failure(foundation::ResultCode::kBackendError, "navigation IDE adapter unavailable")
                             : foundation::Result::success();
    }};
    ui.frame();
    const bool queued = request_open(ui.panel, high.draft.entry.id);
    ui.frame(); ui.frame();
    require(queued && ui.rendered.find("Definition " + std::to_string(high.draft.entry.id.value())) != std::string::npos,
            "request_open did not open the exact high-ID Definition workspace");
    require(ui.rendered.find("read-only") != std::string::npos,
            "compiled Definition workspace is not explicitly read-only");
    require(ui.injectable && launches.empty(), "opening a Definition launched an IDE without an explicit action or lacks the launch seam");
    require(ui.rendered.find("Open Implementation in IDE") != std::string::npos,
            "compiled project Definition has no explicit implementation navigation action");
    ui.frame("Open Implementation in IDE"); ui.frame();
    require(launches.size() == 1, "explicit implementation action did not issue exactly one adapter request");
    const auto& launched = launches.front();
    require(std::filesystem::equivalent(launched.project_root, fixture.root) &&
                std::filesystem::equivalent(launched.source_file, high.files.implementation_target.path) &&
                launched.line == high.execute_line && launched.column == 1,
            "actual UI launch request selected the display-name peer, descriptor reference or wrong source line");
    ui.frame("Save Draft"); ui.frame("Save and Generate"); ui.frame();
    require(document.snapshot_owner() == original && document.revision() == 0 && !document.dirty() &&
                !document.can_undo() && document.catalog().contract_digest() == original_catalog,
            "compiled Definition navigation or disabled Save changed the author graph/catalog/history");
    for (const auto& [file, bytes] : protected_sources)
        require(tests::read_text(file) == bytes, "compiled Definition navigation rewrote generated user source");
    reject_launch = true;
    ui.frame("Open Implementation in IDE"); ui.frame();
    require(launches.size() == 2 && ui.rendered.find("navigation IDE adapter unavailable") != std::string::npos,
            "the actual workspace did not surface the injected adapter failure");
}

void test_system_definition_has_no_fabricated_source() {
    tests::navigation::Fixture fixture;
    auto definition = tests::feature_definition();
    definition.id = feature::FeatureDefinitionId{"System.NoProjectImplementation"};
    definition.contract.definition_id = definition.id;
    application::EditorFeatureGraphDocument document{tests::feature_graph(definition), tests::feature_catalog(definition)};
    const auto original = document.snapshot_owner();
    unsigned launches = 0;
    Harness ui{document, fixture, {definition.id}, [&](const auto&) {
        ++launches; return foundation::Result::success();
    }};
    ui.frame();
    const bool queued = request_open(ui.panel, definition.id);
    ui.frame(); ui.frame();
    require(queued && ui.rendered.find("Definition " + std::to_string(definition.id.value())) != std::string::npos &&
                ui.rendered.find("read-only") != std::string::npos,
            "system Definition did not open its read-only contract workspace");
    require(ui.rendered.find("No project") != std::string::npos,
            "system/no-source Definition offers no explicit source-unavailable explanation");
    ui.frame("Open Implementation in IDE"); ui.frame();
    require(launches == 0 && document.snapshot_owner() == original && !document.dirty(),
            "system Definition fabricated an IDE target or mutated the author graph");
}
}  // namespace

int main() {
    unsigned failures = 0;
    const auto run = [&](const char* name, auto test) {
        try { test(); std::cout << name << " passed\n"; }
        catch (const std::exception& error) { ++failures; std::cerr << name << ": " << error.what() << '\n'; }
    };
    run("compiled-workspace-and-explicit-ide-action", test_request_open_and_explicit_source_action);
    run("system-workspace-no-source-explanation", test_system_definition_has_no_fabricated_source);
    std::cout << "FEATURE_DEFINITION_NAVIGATION_OBSERVATIONS=2 failed=" << failures << '\n';
    return failures ? 1 : 0;
}
