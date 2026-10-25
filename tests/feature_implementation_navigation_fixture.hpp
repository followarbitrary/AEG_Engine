#pragma once

#include "editor_custom_node_workflow.hpp"
#include "editor_feature_definition_fixture.hpp"
#include "editor_node_contract_storage.hpp"
#include "project_context.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace aeg::tests::navigation {
inline void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
inline void write(const std::filesystem::path& file, std::string_view bytes) {
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    require(bool(stream), "navigation fixture could not write its source");
}

struct Generated final {
    application::NodeDefinitionDraft draft;
    application::CustomNodeScaffoldResult files;
    uint32_t execute_line{};
    std::string symbol;
};

class Fixture final {
public:
    Fixture() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() /
            ("aeg_feature_implementation_navigation_" + std::to_string(nonce)) /
            std::filesystem::path{u8"项目 源码"};
        std::filesystem::create_directories(root);
        application::project::ProjectConfig config;
        config.name = "Implementation navigation";
        config.module.id = "com.aeg.editor.fixture";
        project_file = root / "Selected.aegproj";
        require(bool(application::project::ProjectFileCodec{}.save(project_file, config)), "navigation project is invalid");
    }
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root.parent_path(), error);
    }
    Generated generate(feature::FeatureDefinitionId id, std::string stem) const {
        Generated result;
        result.draft = feature_definition_draft();
        result.draft.entry.id = id; result.draft.entry.contract.definition_id = id;
        // Both definitions deliberately have the same author name and display
        // label. Only the canonical contract identity selects their source.
        result.draft.name = "Game.SameName"; result.draft.display_name = "Same display name";
        result.draft.generated = true;
        application::CustomNodeScaffoldRequest request;
        request.project_root = root;
        request.source_subdirectory = std::filesystem::path{"Source/FlowGraph"} /
            std::filesystem::path{u8"定义文件"};
        request.namespace_name = "game::navigation";
        request.type_name = stem;
        request.owner_module = result.draft.entry.owner_module;
        const auto generated = application::scaffold_custom_node(request, result.draft);
        require(bool(generated), "actual four-file scaffolder failed: " + generated.result.message);
        result.files = generated.value;
        require(result.files.files.size() == 4, "actual scaffolder did not produce four outputs");
        const auto source = read_text(result.files.implementation_target.path);
        result.symbol = stem + "::execute";
        const std::string signature = "aeg::foundation::ResultValue<aeg::feature::StateDelta> " + result.symbol + "(";
        const auto actual = source.find(signature);
        require(actual != std::string::npos, "generated execute definition is absent");
        const std::string prefix = "// " + signature + " fake();\n" +
            "const char* navigation_decoy = R\"nav(" + signature + " fake() {})nav\";\n" +
            "/* " + signature + " fake() {} */\n\n";
        result.execute_line = 1u + static_cast<uint32_t>(std::count(prefix.begin(), prefix.end(), '\n') +
            std::count(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(actual), '\n'));
        write(result.files.implementation_target.path, prefix + source + "\n// " + signature + " another fake() {}\n");
        // The generated descriptor also has an earlier &Type::execute
        // reference. Navigation must reach the function definition, not it.
        const auto scanned = application::scan_editor_node_contracts(root);
        require(bool(scanned) && std::any_of(scanned.value.begin(), scanned.value.end(), [&](const auto& file) {
            return file.draft.entry.id == id && file.draft.generated;
        }), "formal contract scan did not retain the generated Definition identity");
        return result;
    }
    std::filesystem::path root;
    std::filesystem::path project_file;
};
}  // namespace aeg::tests::navigation
