/**
 * @file project_smoke.cpp
 * @brief Verifies project configuration persistence and path isolation.
 */
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string_view>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

#include "../ThirdParty/toml11/include/toml.hpp"

#include "project_context.hpp"

namespace {
class TestParticipant final : public aeg::application::project::ProjectLifecycleParticipant {
public:
    [[nodiscard]] aeg::foundation::Result on_project_loaded(const aeg::application::project::ProjectContext&) override {
        ++load_count;
        return aeg::foundation::Result::success();
    }

    [[nodiscard]] aeg::foundation::Result on_project_unloading(
        const aeg::application::project::ProjectContext&) override {
        ++unload_count;
        return aeg::foundation::Result::success();
    }

    uint32_t load_count{};
    uint32_t unload_count{};
};

class FailingParticipant final : public aeg::application::project::ProjectLifecycleParticipant {
public:
    [[nodiscard]] aeg::foundation::Result on_project_loaded(const aeg::application::project::ProjectContext&) override {
        return aeg::foundation::invalid_state("intentional lifecycle failure");
    }

    [[nodiscard]] aeg::foundation::Result on_project_unloading(
        const aeg::application::project::ProjectContext&) override {
        return aeg::foundation::Result::success();
    }
};

bool require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::string modern_project_source(std::string_view format = "2") {
    return (format.empty() ? std::string{} : "format_version = " + std::string{format} + "\n") +
        "[project]\nname = \"Modern Source\"\nuuid = \"source-uuid\"\n"
        "[engine]\nassociation = \"AEG\"\nsdk_requirement = \"1.0\"\n"
        "[module]\nid = \"Modern.Game\"\ntarget = \"GameModule\"\n"
        "[paths]\nassets = \"Assets\"\nsource = \"Source\"\nbuild = \"Build\"\n"
        "cache = \"Cache\"\nsaved = \"Saved\"\nbinaries = \"Binaries\"\n"
        "[build]\ntarget = \"windows-x64-vulkan\"\nstartup_scene = \"Assets/Scenes/Main.aegscene\"\n"
        "startup_feature_graphs = [\"Assets/Features/First.aegfeature\", \"Assets/Features/Second.aegfeature\"]\n"
        "[vendor]\nkeep = \"source metadata\"\n";
}

bool retired_project_formats_are_rejected(const std::filesystem::path& root) {
    using namespace aeg::application::project;
    using aeg::foundation::ResultCode;
    const ProjectFileCodec codec;
    const auto source = root / "RetiredFormat.aegproj";
    const auto target = root / "RetiredFormatSave.aegproj";
    const auto temporary = std::filesystem::path{target.string() + ".tmp"};
    constexpr std::string_view target_bytes{"existing target must survive format rejection\n"};
    constexpr std::string_view temporary_bytes{"existing temporary must survive format rejection\n"};
    bool passed = true;
    const auto rejected_save = [&](const auto& candidate, std::string_view scenario) {
        std::ofstream(target, std::ios::binary | std::ios::trunc) << target_bytes;
        std::ofstream(temporary, std::ios::binary | std::ios::trunc) << temporary_bytes;
        const auto original = read_bytes(source);
        const auto result = codec.save(target, candidate);
        if (result || result.code != ResultCode::kUnsupported || result.message.find("format_version") == std::string::npos ||
            read_bytes(target) != target_bytes || read_bytes(temporary) != temporary_bytes || read_bytes(source) != original) {
            std::cerr << "Retired Project format save accepted/restamped: " << scenario << " code="
                      << static_cast<unsigned>(result.code) << '\n';
            passed = false;
        }
    };
    for (const auto version : {"", "1", "0", "3", "-1", "4294967296"}) {
        std::ofstream(source, std::ios::binary | std::ios::trunc) << modern_project_source(version);
        const auto original = read_bytes(source);
        const auto loaded = codec.load(source);
        if (loaded || loaded.result.code != ResultCode::kUnsupported ||
            loaded.result.message.find("format_version") == std::string::npos || read_bytes(source) != original) {
            std::cerr << "Retired Project format load accepted or lacked typed diagnostic: "
                      << (std::string_view{version}.empty() ? "missing" : version) << " code="
                      << static_cast<unsigned>(loaded.result.code) << '\n';
            passed = false;
        }
        const auto discovered = discover_project_file(source);
        if (discovered || discovered.result.code != ResultCode::kUnsupported ||
            discovered.result.message.find("format_version") == std::string::npos || read_bytes(source) != original) {
            std::cerr << "Retired Project format explicit discovery accepted: "
                      << (std::string_view{version}.empty() ? "missing" : version) << '\n';
            passed = false;
        }
        ProjectFile retained;
        retained.config.name = "Modern Overlay Must Not Upgrade Source";
        retained.retained_source = std::make_shared<const toml::value>(toml::parse(source));
        rejected_save(retained, std::string{"retained source "} + (std::string_view{version}.empty() ? "missing" : version));
    }
    for (const auto version : {0u, 1u, 3u, 999u, uint32_t{0xffffffffu}}) {
        ProjectConfig invalid;
        invalid.name = "Invalid Format";
        invalid.format_version = version;
        rejected_save(invalid, "typed config " + std::to_string(version));
        ProjectFile retained;
        retained.config = invalid;
        retained.retained_source = std::make_shared<const toml::value>(toml::parse_str(modern_project_source()));
        rejected_save(retained, "retained config " + std::to_string(version));
    }
    return passed;
}

bool retired_project_files_are_rejected(const std::filesystem::path& root) {
    using namespace aeg::application::project;
    using aeg::foundation::ResultCode;
    const ProjectFileCodec codec;
    ProjectConfig modern;
    modern.name = "Modern Save Must Not Convert Old File";
    ProjectFile retained;
    retained.config = modern;
    retained.retained_source = std::make_shared<const toml::value>(toml::parse_str(modern_project_source()));
    bool passed = true;
    const auto reject_file = [&](const auto& file, std::string_view bytes, std::string_view expected) {
        const auto temporary = std::filesystem::path{file.string() + ".tmp"};
        constexpr std::string_view temporary_bytes{"existing file temporary must survive rejection\n"};
        const auto restore = [&] {
            std::ofstream(file, std::ios::binary | std::ios::trunc) << bytes;
            std::ofstream(temporary, std::ios::binary | std::ios::trunc) << temporary_bytes;
        };
        restore();
        const auto check = [&](const auto& result, std::string_view operation) {
            if (result || result.code != ResultCode::kUnsupported || result.message.find(expected) == std::string::npos ||
                read_bytes(file) != bytes || read_bytes(temporary) != temporary_bytes) {
                std::cerr << "Retired Project file " << operation << " accepted/restamped: " << file.filename().string()
                          << " code=" << static_cast<unsigned>(result.code) << '\n';
                passed = false;
            }
        };
        check(codec.load(file).result, "load");
        check(discover_project_file(file).result, "explicit discovery");
        check(codec.save(file, modern), "typed save");
        restore();
        check(codec.save(file, retained), "retained save");
    };
    for (const auto extension : {".aegproject", ".AEGPROJECT", ".AeGpRoJeCt"})
        reject_file(root / (std::string{"ModernRenamed"} + extension), modern_project_source(), ".aegproject");
    for (const auto version : {1, 2}) {
        reject_file(root / "OldHeader.aegproj", "AEG_PROJECT\t" + std::to_string(version) +
            "\nOld header\nassets=Assets\nscenes=Scenes\ndefault_scene=Scenes/Main.aegscene\n", "AEG_PROJECT");
    }
    const auto legacy_only = root / "LegacyOnly";
    std::filesystem::create_directories(legacy_only);
    const auto legacy_file = legacy_only / "project.aegproject";
    std::ofstream(legacy_file, std::ios::binary | std::ios::trunc) << modern_project_source();
    const auto legacy_bytes = read_bytes(legacy_file);
    const auto rejected = discover_project_file(legacy_only);
    if (rejected || rejected.result.code != ResultCode::kUnsupported ||
        rejected.result.message.find(".aegproject") == std::string::npos || read_bytes(legacy_file) != legacy_bytes ||
        std::filesystem::exists(legacy_only / "Project.aegproj")) {
        std::cerr << "Legacy-only Project discovery accepted or converted source\n";
        passed = false;
    }
    const auto canonical = legacy_only / "Project.aegproj";
    std::ofstream(canonical, std::ios::binary | std::ios::trunc) << modern_project_source();
    const auto selected = discover_project_file(legacy_only);
    passed = require(selected && selected.value.filename() == canonical.filename() && read_bytes(legacy_file) == legacy_bytes,
                     "Canonical Project discovery did not retain modern precedence without modifying legacy file") && passed;
    const auto explicit_legacy = discover_project_file(legacy_file);
    passed = require(!explicit_legacy && explicit_legacy.result.code == ResultCode::kUnsupported,
                     "Explicit legacy file bypassed refusal when a canonical project coexists") && passed;
    const auto explicit_modern = legacy_only / "Vendor.modernproject";
    std::ofstream(explicit_modern, std::ios::binary | std::ios::trunc) << modern_project_source();
    const auto selected_modern = discover_project_file(explicit_modern);
    const auto loaded_modern = codec.load(explicit_modern);
    passed = require(selected_modern && loaded_modern && selected_modern.value.filename() == explicit_modern.filename(),
                     "Explicit modern vendor Project was rejected") && passed;
    return passed;
}

bool modern_unicode_configuration_is_preserved(const std::filesystem::path& root) {
    using namespace aeg::application::project;
    const std::u8string unicode{u8"\u6e38\u620f\U0001f680"};
    const std::string text(unicode.begin(), unicode.end());
    ProjectConfig config;
    config.name = text;
    config.project_uuid = "unicode-project";
    config.engine = {text, "2.0", text};
    config.module = {text, "UnicodeModule"};
    config.paths.source = std::filesystem::path{u8"Source/\u4ee3\u7801"};
    config.startup_feature_graphs = {std::filesystem::path{u8"Assets/Features/\u9996.aegfeature"},
                                   std::filesystem::path{u8"Assets/Features/\u6b21.aegfeature"}};
    const auto file = root / "Unicode.aegproj";
    const ProjectFileCodec codec;
    if (!require(static_cast<bool>(codec.save(file, config)), "Modern Unicode Project typed save failed")) return false;
    { std::ofstream out(file, std::ios::binary | std::ios::app); out << "[vendor]\nvalue = \"" << text << "\"\n"; }
    auto loaded = codec.load(file);
    if (!require(loaded && loaded.value.config.name == text && loaded.value.config.engine.association == text &&
                     loaded.value.config.engine.template_id == text && loaded.value.config.module.id == text &&
                     loaded.value.config.paths.source == config.paths.source &&
                     loaded.value.config.startup_feature_graphs == config.startup_feature_graphs,
                 "Modern Unicode Project full configuration did not round-trip")) return false;
    loaded.value.config.name += " retained";
    if (!require(static_cast<bool>(codec.save(file, loaded.value)), "Modern Unicode retained save failed")) return false;
    const auto saved = codec.load(file);
    return require(saved && saved.value.config.name == loaded.value.config.name &&
                       saved.value.config.paths.source == config.paths.source &&
                       saved.value.config.startup_feature_graphs == config.startup_feature_graphs &&
                       toml::find<std::string>(toml::parse(file), "vendor", "value") == text,
                   "Modern Unicode retained save changed source paths, roots or vendor metadata");
}

bool native_unicode_project_paths_are_supported(const std::filesystem::path& root) {
    using namespace aeg::application::project;
    using aeg::foundation::ResultCode;
    const ProjectFileCodec codec;
    const auto source = root / "NativePathSource.aegproj";
    const auto source_bytes = modern_project_source();
    std::ofstream(source, std::ios::binary | std::ios::trunc) << source_bytes;
    const auto retained_source = codec.load(source);
    if (!require(static_cast<bool>(retained_source), "Native path fixture requires a real modern retained source")) return false;
    struct Case final {
        const char* name;
        std::filesystem::path directory, filename;
        bool canonical;
    };
    const Case cases[] = {
        {"emoji filename", root / "NativeFilename", std::filesystem::path{u8"Selected-\U0001f680.aegproj"}, false},
        {"emoji root", root / std::filesystem::path{u8"NativeRoot-\U0001f680"}, "Project.aegproj", true},
        {"emoji root and filename", root / std::filesystem::path{u8"NativeBoth-\U0001f680"},
         std::filesystem::path{u8"Selected-\U0001f680.aegproj"}, false},
    };
    bool passed = true;
    for (const auto& test : cases) {
        std::error_code error;
        std::filesystem::create_directories(test.directory, error);
        if (!require(!error, "Native path fixture directory creation failed")) return false;
        const auto file = test.directory / test.filename;
        std::ofstream(file, std::ios::binary | std::ios::trunc) << source_bytes;
        const auto check = [&](bool condition, const char* operation, const aeg::foundation::Result& result) {
            if (!condition) {
                std::cerr << "Native Unicode Project " << operation << " failed: " << test.name
                          << " code=" << static_cast<unsigned>(result.code) << " message=" << result.message << '\n';
                passed = false;
            }
        };
        const auto loaded = codec.load(file);
        check(loaded && loaded.value.config.name == retained_source.value.config.name && read_bytes(file) == source_bytes,
              "load", loaded.result);
        const auto selected = discover_project_file(file);
        check(selected && selected.value == std::filesystem::weakly_canonical(file), "explicit discovery", selected.result);
        if (test.canonical) {
            const auto directory = discover_project_file(test.directory);
            check(directory && directory.value == std::filesystem::weakly_canonical(file), "directory discovery", directory.result);
        }
        auto typed = retained_source.value.config;
        typed.name = "Native Typed Update";
        const auto typed_update = codec.save(file, typed);
        check(static_cast<bool>(typed_update), "existing-target typed update", typed_update);
        auto overlay = retained_source.value;
        overlay.config.name = "Native Retained Update";
        const auto retained_update = codec.save(file, overlay);
        check(static_cast<bool>(retained_update) && read_bytes(source) == source_bytes,
              "existing-target retained update", retained_update);
        auto live = codec.load(file);
        if (live) {
            live.value.config.name = "Native Actual Source Update";
            const auto actual_update = codec.save(file, live.value);
            check(static_cast<bool>(actual_update), "actual native retained update", actual_update);
            const auto updated = codec.load(file);
            check(updated && updated.value.config.name == live.value.config.name &&
                      updated.value.config.startup_feature_graphs == overlay.config.startup_feature_graphs &&
                      toml::find<std::string>(*std::static_pointer_cast<const toml::value>(updated.value.retained_source),
                                              "vendor", "keep") == "source metadata",
                  "retained metadata round-trip", updated.result);
        } else {
            check(false, "actual native retained source", live.result);
        }
        const auto create = test.directory / std::filesystem::path{u8"Created-\U0001f680.aegproj"};
        const auto created = codec.save(create, typed);
        const auto created_source = codec.load(create);
        check(created && created_source && created_source.value.config.name == typed.name,
              "absent-target typed create/load", created ? created_source.result : created);
        check(!std::filesystem::exists(std::filesystem::path{file}.concat(".tmp")) && read_bytes(source) == source_bytes,
              "successful update cleanup", retained_update);
    }
    const auto target = cases[2].directory / std::filesystem::path{u8"Rejected-\U0001f680.aegproj"};
    auto temporary = target;
    temporary += ".tmp";
    constexpr std::string_view temporary_bytes{"native temporary sentinel\n"};
    struct RejectedTarget final {
        const char* name;
        std::string payload, diagnostic;
        ResultCode code;
    };
    auto removed = modern_project_source();
    removed.insert(removed.find("[build]\n") + std::string_view{"[build]\n"}.size(), "startup_pipelines = []\n");
    const auto target_utf8 = target.generic_u8string();
    const std::string target_text{target_utf8.begin(), target_utf8.end()};
    const RejectedTarget rejected[] = {
        {"missing version", modern_project_source(""), "format_version", ResultCode::kUnsupported},
        {"version 1", modern_project_source("1"), "format_version", ResultCode::kUnsupported},
        {"removed startup field", std::move(removed), "build.startup_pipelines", ResultCode::kUnsupported},
        {"malformed UTF-8 source diagnostic", "format_version = 2\n[project\n", target_text, ResultCode::kInvalidArgument},
    };
    for (const auto& test : rejected) {
        std::ofstream(target, std::ios::binary | std::ios::trunc) << test.payload;
        std::ofstream(temporary, std::ios::binary | std::ios::trunc) << temporary_bytes;
        const auto loaded = codec.load(target);
        const auto reject = [&](const aeg::foundation::Result& result, const char* operation) {
            if (result || result.code != test.code || result.message.find(test.diagnostic) == std::string::npos ||
                read_bytes(target) != test.payload || read_bytes(temporary) != temporary_bytes || read_bytes(source) != source_bytes) {
                std::cerr << "Native Unicode Project " << operation << " changed rejected files or lost diagnostic: "
                          << test.name << " code=" << static_cast<unsigned>(result.code)
                          << " message=" << result.message << '\n';
                passed = false;
            }
        };
        reject(loaded.result, "load");
        reject(codec.save(target, retained_source.value.config), "typed target validation");
        reject(codec.save(target, retained_source.value), "retained target validation");
    }
    return passed;
}

bool modern_saves_reject_retired_targets(const std::filesystem::path& root) {
    using namespace aeg::application::project;
    using aeg::foundation::ResultCode;
    struct Case final {
        std::string name, payload, diagnostic;
        ResultCode code{ResultCode::kUnsupported};
    };
    std::vector<Case> cases;
    for (const auto version : {"", "1", "0", "3", "-1", "4294967296", "\"2\"", "true", "[2]"})
        cases.push_back({std::string_view{version}.empty() ? "missing" : version,
                         modern_project_source(version), "format_version"});
    for (const auto key : {"startup_pipelines", "startup_frame_projections"}) {
        auto payload = modern_project_source();
        payload.insert(payload.find("[build]\n") + std::string_view{"[build]\n"}.size(), std::string{key} + " = []\n");
        cases.push_back({key, std::move(payload), std::string{"build."} + key});
    }
    for (const auto extension : {".aegpipe", ".AEGFRAME"}) {
        auto payload = modern_project_source();
        payload.insert(payload.find("[build]\n") + std::string_view{"[build]\n"}.size(),
                       "startup_feature_graphs = [\"Assets/Old" + std::string{extension} + "\"]\n");
        const auto first = payload.find("startup_feature_graphs = [\"Assets/Features/First.aegfeature\"");
        payload.erase(first, payload.find('\n', first) - first + 1);
        cases.push_back({extension, std::move(payload), "build.startup_feature_graphs"});
    }
    cases.push_back({"malformed", "format_version = 2\n[project\n", "Existing project target", ResultCode::kInvalidArgument});
    const auto source = root / "ModernSource.aegproj";
    const auto target = root / "ExistingTarget.aegproj";
    const auto temporary = std::filesystem::path{target.string() + ".tmp"};
    const auto source_bytes = modern_project_source();
    std::ofstream(source, std::ios::binary | std::ios::trunc) << source_bytes;
    const ProjectFileCodec codec;
    auto retained = codec.load(source);
    if (!require(static_cast<bool>(retained), "Target rejection fixture needs a real modern retained source")) return false;
    retained.value.config.name = "Modern Retained Configuration";
    ProjectConfig typed = retained.value.config;
    typed.name = "Modern Typed Configuration";
    constexpr std::string_view temporary_bytes{"pre-existing target temporary must survive rejection\n"};
    bool passed = true;
    for (const auto& test : cases) {
        const auto reject = [&](const auto& candidate, const char* operation) {
            std::ofstream(target, std::ios::binary | std::ios::trunc) << test.payload;
            std::ofstream(temporary, std::ios::binary | std::ios::trunc) << temporary_bytes;
            const auto saved = codec.save(target, candidate);
            if (saved || saved.code != test.code || saved.message.find(test.diagnostic) == std::string::npos ||
                read_bytes(target) != test.payload || read_bytes(temporary) != temporary_bytes || read_bytes(source) != source_bytes) {
                std::cerr << "Modern " << operation << " overwrote retired TOML target or lacked concrete rejection: "
                          << test.name << " code=" << static_cast<unsigned>(saved.code) << '\n';
                passed = false;
            }
        };
        reject(typed, "typed save");
        reject(retained.value, "retained save");
    }
    const auto create = root / "ModernCreate.aegproj";
    if (!require(!std::filesystem::exists(create) && static_cast<bool>(codec.save(create, typed)),
                 "Modern typed configuration could not create an absent target")) return false;
    typed.name = "Supported Typed Update";
    if (!require(static_cast<bool>(codec.save(create, typed)) && codec.load(create).value.config.name == typed.name,
                 "Modern typed configuration could not overwrite a supported modern target")) return false;
    auto live = codec.load(create);
    if (!require(static_cast<bool>(live), "Modern retained update source is unavailable")) return false;
    live.value.config.name = "Supported Retained Update";
    if (!require(static_cast<bool>(codec.save(create, live.value)) &&
                     codec.load(create).value.config.name == live.value.config.name,
                 "Modern retained configuration could not update its supported modern target")) return false;
    // Validate the caller's retained source before an unrelated invalid target.
    auto invalid_source = source_bytes;
    invalid_source.insert(invalid_source.find("[build]\n") + std::string_view{"[build]\n"}.size(), "startup_pipelines = []\n");
    ProjectFile invalid = retained.value;
    invalid.retained_source = std::make_shared<const toml::value>(toml::parse_str(invalid_source));
    const auto target_bytes = modern_project_source("1");
    std::ofstream(target, std::ios::binary | std::ios::trunc) << target_bytes;
    std::ofstream(temporary, std::ios::binary | std::ios::trunc) << temporary_bytes;
    const auto source_failure = codec.save(target, invalid);
    passed = require(!source_failure && source_failure.code == ResultCode::kUnsupported &&
                         source_failure.message.find("build.startup_pipelines") != std::string::npos &&
                         read_bytes(target) == target_bytes && read_bytes(temporary) == temporary_bytes &&
                         read_bytes(source) == source_bytes,
                     "Existing target validation masked the retained-source error or wrote rejected bytes") && passed;
    return passed;
}

bool removed_startup_keys_are_rejected(const std::filesystem::path& root) {
    using namespace aeg::application::project;
    using aeg::foundation::ResultCode;
    const ProjectFileCodec codec;
    bool passed = true;
    for (const unsigned version : {1u, 2u}) {
        for (const auto key : {std::string_view{"startup_frame_projections"}, std::string_view{"startup_pipelines"}}) {
            for (const auto value : {std::string_view{"[]"}, std::string_view{"[\"Assets/Legacy.aegpipe\"]"}, std::string_view{"7"}}) {
                const auto source = root / "RemovedKey.aegproj";
                std::ofstream(source, std::ios::binary | std::ios::trunc)
                    << "format_version = " << version << "\n"
                       "[project]\nname = \"Removed Field\"\n"
                       "[engine]\nassociation = \"AEG\"\nsdk_requirement = \"1.0\"\n"
                       "[module]\nid = \"Removed.Game\"\ntarget = \"GameModule\"\n"
                       "[paths]\nassets = \"Assets\"\nsource = \"Source\"\nbuild = \"Build\"\n"
                       "cache = \"Cache\"\nsaved = \"Saved\"\nbinaries = \"Binaries\"\n"
                       "[build]\ntarget = \"windows-x64-vulkan\"\nstartup_scene = \"Assets/Scenes/Main.aegscene\"\n"
                       "startup_feature_graphs = [\"Assets/Features/Default.aegfeature\"]\n"
                    << key << " = " << value << "\n[custom]\nkeep_me = \"vendor\"\n";
                const auto original_source = read_bytes(source);
                const auto expected_path = version == 1 ? std::string{"format_version"} : "build." + std::string{key};
                const auto loaded = codec.load(source);
                if (loaded || loaded.result.code != ResultCode::kUnsupported ||
                    loaded.result.message.find(expected_path) == std::string::npos || read_bytes(source) != original_source) {
                    std::cerr << "Removed startup key load not explicitly rejected: format=" << version << ' '
                              << expected_path << '=' << value << " code=" << static_cast<unsigned>(loaded.result.code) << '\n';
                    passed = false;
                }
                ProjectFile retained;
                retained.config.name = "Retained Source";
                retained.config.format_version = version;
                retained.retained_source = std::make_shared<const toml::value>(toml::parse(source));
                const auto target = root / "RemovedKeySave.aegproj";
                const auto temporary = std::filesystem::path{target.string() + ".tmp"};
                const std::string target_bytes = "existing project bytes must survive rejection\n";
                const std::string temporary_bytes = "existing temporary bytes must survive rejection\n";
                std::ofstream(target, std::ios::binary | std::ios::trunc) << target_bytes;
                std::ofstream(temporary, std::ios::binary | std::ios::trunc) << temporary_bytes;
                const auto saved = codec.save(target, retained);
                if (saved || saved.code != ResultCode::kUnsupported || saved.message.find(expected_path) == std::string::npos ||
                    read_bytes(target) != target_bytes || read_bytes(temporary) != temporary_bytes || read_bytes(source) != original_source) {
                    std::cerr << "Removed retained startup key save not explicitly rejected without writes: format=" << version << ' '
                              << expected_path << '=' << value << " code=" << static_cast<unsigned>(saved.code) << '\n';
                    passed = false;
                }
            }
        }
    }
    return passed;
}

bool removed_startup_graph_extensions_are_rejected(const std::filesystem::path& root) {
    using namespace aeg::application::project;
    using aeg::foundation::ResultCode;
    const ProjectFileCodec codec;
    bool passed = true;
    for (const auto path : {"Assets/Old.aegpipe", "Assets/Old.AEGPIPE", "Assets/Old.AeGpIpE",
                           "Assets/Old.aegframe", "Assets/Old.AEGFRAME", "Assets/Old.AeGfRaMe"}) {
        const auto source = root / "RemovedExtension.aegproj";
        std::ofstream(source, std::ios::binary | std::ios::trunc)
            << "format_version = 2\n[project]\nname = \"Removed Extension\"\n"
               "[paths]\nassets = \"Assets\"\nsource = \"Source\"\nbuild = \"Build\"\n"
               "cache = \"Cache\"\nsaved = \"Saved\"\nbinaries = \"Binaries\"\n"
               "[build]\ntarget = \"windows-x64-vulkan\"\nstartup_scene = \"Assets/Scenes/Main.aegscene\"\n"
               "startup_feature_graphs = [\"Assets/Features/Good.aegfeature\", \""
            << path << "\"]\n";
        const auto original = read_bytes(source);
        const auto rejected_load = codec.load(source);
        if (rejected_load || rejected_load.result.code != ResultCode::kUnsupported ||
            rejected_load.result.message.find("build.startup_feature_graphs") == std::string::npos || read_bytes(source) != original) {
            std::cerr << "Removed startup graph extension load accepted or lacked typed diagnostic: " << path << '\n';
            passed = false;
        }
        ProjectConfig config;
        config.name = "Removed Extension";
        config.startup_feature_graphs = {"Assets/Features/Good.aegfeature", path};
        const auto target = root / "RemovedExtensionSave.aegproj";
        const auto temporary = std::filesystem::path{target.string() + ".tmp"};
        const std::string target_bytes = "existing extension target must survive\n";
        const std::string temporary_bytes = "existing extension temporary must survive\n";
        const auto rejected_save = [&](const auto& candidate, const char* operation) {
            std::ofstream(target, std::ios::binary | std::ios::trunc) << target_bytes;
            std::ofstream(temporary, std::ios::binary | std::ios::trunc) << temporary_bytes;
            const auto result = codec.save(target, candidate);
            if (result || result.code != ResultCode::kUnsupported ||
                result.message.find("build.startup_feature_graphs") == std::string::npos ||
                read_bytes(target) != target_bytes || read_bytes(temporary) != temporary_bytes || read_bytes(source) != original) {
                std::cerr << "Removed startup graph extension " << operation << " accepted/restamped: " << path << '\n';
                passed = false;
            }
        };
        rejected_save(config, "typed save");
        ProjectFile retained;
        retained.config.name = "Valid Overlay";
        // A valid replacement must not silently scrub an obsolete retained source.
        retained.config.startup_feature_graphs = {"Assets/Features/Good.aegfeature"};
        retained.retained_source = std::make_shared<const toml::value>(toml::parse(source));
        rejected_save(retained, "retained save");
    }
    ProjectConfig vendor;
    vendor.name = "Non-legacy Vendor Extension";
    vendor.startup_feature_graphs = {"Assets/Features/Good.aegfeature", "Vendor/Scene.customgraph"};
    const auto vendor_path = root / "VendorExtension.aegproj";
    const auto saved = codec.save(vendor_path, vendor);
    const auto loaded = codec.load(vendor_path);
    passed = require(saved && loaded && loaded.value.config.startup_feature_graphs == vendor.startup_feature_graphs,
                     "Old-extension rejection over-constrained non-legacy vendor paths") && passed;
    return passed;
}
}  // namespace

int main() {
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    namespace fs = std::filesystem;
    using aeg::application::project::ProjectConfig;
    using aeg::application::project::ProjectFileCodec;
    using aeg::application::project::ProjectPaths;

    const fs::path root = fs::temp_directory_path() / "aeg_project_smoke";
    std::error_code error;
    fs::remove_all(root, error);
    fs::create_directories(root / "Assets", error);
    fs::create_directories(root / "Cache", error);
    fs::create_directories(root / "Saved", error);
    fs::create_directories(root / "Binaries", error);
    if (!require(!error, "project smoke cannot create fixture directories")) {
        return 1;
    }
    const bool native_paths_supported = native_unicode_project_paths_are_supported(root);
    const bool targets_rejected = modern_saves_reject_retired_targets(root);
    const bool unicode_preserved = modern_unicode_configuration_is_preserved(root);
    const bool formats_rejected = retired_project_formats_are_rejected(root);
    const bool files_rejected = retired_project_files_are_rejected(root);
    if (!native_paths_supported) return 47;
    if (!targets_rejected) return 46;
    if (!unicode_preserved || !formats_rejected || !files_rejected) return 45;
    if (!removed_startup_keys_are_rejected(root)) return 42;
    if (!removed_startup_graph_extensions_are_rejected(root)) return 44;

    ProjectConfig config{};
    config.name = "Project Smoke";
    config.project_uuid = "project-smoke-uuid";
    config.target = "windows-x64-vulkan";
    config.module.id = "ProjectSmoke.GameModule";
    config.startup_feature_graphs.emplace_back("Assets/Features/Default.aegfeature");
    config.startup_feature_graphs.emplace_back("Assets/Features/Extra.aegfeature");

    const auto paths = ProjectPaths::resolve(root / "ProjectSmoke.aegproj", config.paths);
    if (!require(static_cast<bool>(paths), "project smoke cannot resolve valid directories") ||
        !require(paths.value.directory(aeg::application::project::ProjectDirectory::kAssets) == root / "Assets",
                 "project smoke Assets mapping is invalid") ||
        !require(
            paths.value.resolve_under(aeg::application::project::ProjectDirectory::kAssets, "Meshes/Cube.gltf").value ==
                root / "Assets" / "Meshes" / "Cube.gltf",
            "project smoke relative path mapping is invalid") ||
        !require(
            !paths.value.resolve_under(aeg::application::project::ProjectDirectory::kAssets, "../Saved/escape.txt"),
            "project smoke accepted a path outside Assets") ||
        !require(!paths.value.resolve_under(aeg::application::project::ProjectDirectory::kAssets,
                                            fs::temp_directory_path() / "outside.txt"),
                 "project smoke accepted an absolute path")) {
        fs::remove_all(root, error);
        return 2;
    }

    const fs::path project_file = root / "Project.aegproj";
    ProjectFileCodec codec;
    if (!require(static_cast<bool>(codec.save(project_file, config)), "project smoke cannot save TOML project") ||
        !require(static_cast<bool>(codec.load(project_file)), "project smoke cannot load TOML project")) {
        fs::remove_all(root, error);
        return 3;
    }

    const auto loaded = codec.load(project_file);
    if (!require(loaded.value.config.name == config.name, "project smoke name did not round-trip") ||
        !require(loaded.value.config.target == config.target, "project smoke target did not round-trip") ||
        !require(loaded.value.config.project_uuid == config.project_uuid, "project smoke uuid did not round-trip") ||
        !require(loaded.value.config.module.id == config.module.id, "project smoke module did not round-trip") ||
        !require(loaded.value.config.startup_feature_graphs == config.startup_feature_graphs,
                 "project smoke ordered startup FeatureGraphs did not round-trip") ||
        !require(loaded.value.config.paths.source == "Source" && loaded.value.config.paths.build == "Build",
                 "project smoke v2 path defaults did not round-trip")) {
        fs::remove_all(root, error);
        return 4;
    }

    std::ofstream(project_file, std::ios::trunc)
        << "format_version = 2\n"
           "[project]\nname = \"Project Smoke\"\nuuid = \"project-smoke-uuid\"\n"
           "[engine]\nassociation = \"AEG\"\nsdk_requirement = \"1.0\"\n"
           "[module]\nid = \"ProjectSmoke.GameModule\"\ntarget = \"GameModule\"\n"
           "[paths]\nassets = \"Assets\"\nsource = \"Source\"\nbuild = \"Build\"\n"
           "cache = \"Cache\"\nsaved = \"Saved\"\nbinaries = \"Binaries\"\n"
           "[build]\ntarget = \"windows-x64-vulkan\"\nstartup_scene = \"Assets/Scenes/Main.aegscene\"\n"
           "startup_feature_graphs = [\"Assets/Features/Default.aegfeature\", \"Assets/Features/Extra.aegfeature\"]\n"
           "[custom]\nkeep_me = \"yes\"\nstartup_pipelines = [\"vendor-only\"]\n"
           "startup_frame_projections = \"vendor-frame-value\"\n"
           "[custom.nested]\nkeep_nested = \"preserved\"\n";
    auto overlay = codec.load(project_file);
    if (!require(static_cast<bool>(overlay), "project smoke cannot load overlay fixture")) {
        fs::remove_all(root, error);
        return 8;
    }
    overlay.value.config.name = "Project Smoke Renamed";
    if (!require(static_cast<bool>(codec.save(project_file, overlay.value)),
                 "project smoke cannot save retained TOML document")) {
        fs::remove_all(root, error);
        return 9;
    }
    std::ifstream overlay_contents(project_file);
    const std::string overlay_text((std::istreambuf_iterator<char>(overlay_contents)),
                                   std::istreambuf_iterator<char>());
    if (!require(overlay_text.find("keep_me = \"yes\"") != std::string::npos,
                 "project smoke save dropped unknown TOML metadata")) {
        fs::remove_all(root, error);
        return 10;
    }
    overlay_contents.close();
    const auto saved_overlay = codec.load(project_file);
    const auto saved_vendor = toml::parse(project_file);
    if (!require(saved_overlay && saved_overlay.value.config.startup_feature_graphs == config.startup_feature_graphs,
                 "retained save changed startup FeatureGraph ordering") ||
        !require(toml::find<std::vector<std::string>>(saved_vendor, "custom", "startup_pipelines") ==
                     std::vector<std::string>{"vendor-only"} &&
                     toml::find<std::string>(saved_vendor, "custom", "startup_frame_projections") == "vendor-frame-value" &&
                     toml::find<std::string>(saved_vendor, "custom", "nested", "keep_nested") == "preserved",
                 "save removed unrelated vendor fields with legacy-looking names")) {
        fs::remove_all(root, error);
        return 43;
    }

    const auto discovered = aeg::application::project::discover_project_file(root);
    if (!require(static_cast<bool>(discovered) && discovered.value.filename() == project_file.filename(),
                 "project smoke did not discover canonical project file")) {
        fs::remove_all(root, error);
        return 11;
    }

    aeg::application::project::ProjectManager manager;
    const auto participant = std::make_shared<TestParticipant>();
    if (!require(static_cast<bool>(manager.register_participant(participant)),
                 "project smoke cannot register lifecycle participant") ||
        !require(static_cast<bool>(manager.load(root)),
                 "project smoke cannot load a project session from project root") ||
        !require(manager.loaded(), "project smoke did not mark the project session as loaded") ||
        !require(manager.context() != nullptr && manager.context()->config.name == "Project Smoke Renamed",
                 "project smoke context does not retain the loaded configuration") ||
        !require(participant->load_count == 1U, "project smoke did not notify lifecycle participant of loading") ||
        !require(static_cast<bool>(manager.save()), "project smoke cannot save a loaded project session") ||
        !require(static_cast<bool>(manager.unload()), "project smoke cannot unload a project session") ||
        !require(participant->unload_count == 1U, "project smoke did not notify lifecycle participant of unloading") ||
        !require(!manager.loaded() && manager.context() == nullptr,
                 "project smoke did not release the unloaded project context")) {
        fs::remove_all(root, error);
        return 5;
    }

    aeg::application::project::ProjectManager rollback_manager;
    const auto rollback_participant = std::make_shared<TestParticipant>();
    if (!require(static_cast<bool>(rollback_manager.register_participant(rollback_participant)),
                 "project smoke cannot register rollback participant") ||
        !require(static_cast<bool>(rollback_manager.register_participant(std::make_shared<FailingParticipant>())),
                 "project smoke cannot register failing participant") ||
        !require(!rollback_manager.load(project_file), "project smoke accepted failed lifecycle load") ||
        !require(!rollback_manager.loaded() && rollback_manager.context() == nullptr,
                 "project smoke retained context after lifecycle rollback") ||
        !require(rollback_participant->load_count == 1U && rollback_participant->unload_count == 1U,
                 "project smoke did not roll back the loaded lifecycle participant")) {
        fs::remove_all(root, error);
        return 6;
    }

    std::ofstream(project_file, std::ios::trunc) << "format_version = 999\n";
    if (!require(!codec.load(project_file), "project smoke accepted an unsupported format version")) {
        fs::remove_all(root, error);
        return 7;
    }

    fs::remove_all(root, error);
    return 0;
}
