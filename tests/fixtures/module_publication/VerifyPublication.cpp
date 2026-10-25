#include "dynamic_module.hpp"
#include "feature_clock_admission.hpp"
#include "feature_runtime.hpp"
#include "module_bundle.hpp"
#include "module.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void succeeds(const aeg::foundation::Result& result, const char* operation) {
    require(bool(result), std::string{operation} + ": " + result.message);
}

std::filesystem::path utf8_path(std::string_view value) {
    return std::filesystem::u8path(value.begin(), value.end());
}

int verify(const std::vector<std::string>& arguments) {
    using namespace aeg;
    require(arguments.size() == 6, "--verify manifest expected-build expected-target expected-revision");
    const auto revision = static_cast<uint16_t>(std::stoul(arguments[5]));
    const auto bundle = module::validate_module_bundle(utf8_path(arguments[2]));
    succeeds(bundle.result, "validate active manifest and all referenced payload hashes");
    require(bundle.value.metadata.build_id == arguments[3] && bundle.value.metadata.target == arguments[4] &&
                bundle.value.metadata.configuration == "Debug",
            "published metadata identity did not preserve the exact requested UTF8 bytes");
    const auto host = feature::detail::make_clock_host_context(feature::detail::ClockHostLimits{});
    succeeds(host.result, "create bounded publication verifier host");
    feature::FeatureRegistry registry{host.value};
    module::ModuleRegistry modules{registry};
    module::DynamicModuleLoader loader{modules};
    module::DynamicModuleHandle handle;
    succeeds(loader.load(bundle.value, handle), "load the manifest's actual DLL");
    auto imported = loader.make_feature_module(handle);
    succeeds(imported.result, "compare actual DLL contracts with published metadata");
    require(imported.value.feature_definitions.size() == 1, "fixture must import one executable definition");
    const auto definition_id = imported.value.feature_definitions.front().id;
    succeeds(module::register_static_module(modules, std::move(imported.value)), "admit the actual DLL definition");
    imported = {};
    {
        feature::FeatureRuntime runtime{registry};
        const auto expanded = registry.expand(definition_id, {});
        succeeds(expanded.result, "expand published definition");
        const auto owner = runtime.activate(expanded.value, feature::ScopeId{"World"});
        succeeds(owner.result, "activate published definition");
        succeeds(runtime.dispatch_call(owner.value), "execute published DLL callback");
        const auto* instance = runtime.find_instance(owner.value);
        require(instance && instance->state.payload == std::vector<uint8_t>{static_cast<uint8_t>(revision)} &&
                    instance->state.commit_count == 1,
                "active DLL did not commit its expected revision exactly once");
        succeeds(runtime.shutdown_execution(), "retire verifier Runtime work");
    }
    succeeds(modules.unregister_module(bundle.value.metadata.module.id), "unregister verifier module");
    succeeds(loader.unload(handle), "release DLL callback leases");
    std::cout << "PUBLICATION_BUNDLE_VERIFIED callback-commits=1\n";
    return 0;
}

#if defined(_WIN32)
std::wstring quoted_argument(std::wstring_view value) {
    std::wstring result{L"\""};
    std::size_t backslashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        result.append(character == L'\"' ? 2 * backslashes + 1 : backslashes, L'\\');
        result.push_back(character);
        backslashes = 0;
    }
    result.append(2 * backslashes, L'\\');
    result.push_back(L'\"');
    return result;
}

int locked_publish(const std::vector<std::string>& arguments) {
    const bool build = arguments[1] == "--locked-build";
    require(arguments.size() == (build ? 6u : 10u),
            "--locked-publish tool module source-metadata output manifest build-id target config; "
            "--locked-build cmake build-root manifest target");
    const auto manifest = utf8_path(arguments[build ? 4 : 6]);
    const auto handle = CreateFileW(manifest.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "could not hold the last-good manifest against replacement");
    struct Handle final {
        HANDLE value;
        ~Handle() { CloseHandle(value); }
    } held{handle};
    // Read sharing keeps the selector usable; withholding DELETE deterministically
    // fails its final replacement after the real tool has prepared its payloads.
    const auto tool = utf8_path(arguments[2]);
    const std::vector<std::wstring> command = build ? std::vector<std::wstring>{
        tool.native(), L"--build", utf8_path(arguments[3]).native(), L"--config", L"Debug",
        L"--target", utf8_path(arguments[5]).native(), L"--parallel", L"4"} : std::vector<std::wstring>{
        tool.native(), L"--module", utf8_path(arguments[3]).native(),
        L"--metadata", utf8_path(arguments[4]).native(),
        L"--output", utf8_path(arguments[5]).native(), L"--manifest", manifest.native(),
        L"--build-id", utf8_path(arguments[7]).native(),
        L"--target", utf8_path(arguments[8]).native(), L"--config", utf8_path(arguments[9]).native()};
    std::wstring command_line;
    for (const auto& value : command) {
        if (!command_line.empty()) command_line.push_back(L' ');
        command_line += quoted_argument(value);
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    require(CreateProcessW(tool.c_str(), command_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &startup, &process) != 0,
            "could not launch the real metadata publisher while holding its selector");
    Handle thread{process.hThread};
    Handle child{process.hProcess};
    require(WaitForSingleObject(child.value, INFINITE) == WAIT_OBJECT_0, "could not wait for actual publisher");
    DWORD result = 0;
    require(GetExitCodeProcess(child.value, &result) != 0, "could not read the actual publisher's native exit code");
    std::cout << "PUBLICATION_LOCKED_CHILD_EXIT " << result << '\n';
    return 0;
}
#endif

int run(const std::vector<std::string>& arguments) {
    try {
        require(arguments.size() > 1, "a publication verification mode is required");
        if (arguments[1] == "--verify") return verify(arguments);
#if defined(_WIN32)
        if (arguments[1] == "--locked-publish" || arguments[1] == "--locked-build")
            return locked_publish(arguments);
#endif
        throw std::runtime_error("unknown publication verification mode");
    } catch (const std::exception& error) {
        std::cerr << "publication fixture error: " << error.what() << '\n';
        return 20;
    }
}
} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[index], -1,
                                             nullptr, 0, nullptr, nullptr);
        if (size <= 0) return 21;
        std::string value(static_cast<std::size_t>(size), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[index], -1, value.data(), size,
                                nullptr, nullptr) != size) return 21;
        value.pop_back();
        arguments.push_back(std::move(value));
    }
    return run(arguments);
}
#else
int main(int argc, char** argv) {
    return run(std::vector<std::string>(argv, argv + argc));
}
#endif
