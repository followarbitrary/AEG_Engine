#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <chrono>
#include <vector>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

#include "module_bundle.hpp"

namespace {
using Json = nlohmann::json;

struct Options final {
    std::filesystem::path module{};
    std::filesystem::path binary_output{};
    std::filesystem::path metadata_input{};
    std::filesystem::path output{};
    std::filesystem::path manifest{};
    std::string build_id{};
    std::string target{};
    std::string configuration{};
};

void usage() {
    std::cerr << "usage: aeg_module_metadata_tool --module <dll> --metadata <metadata.json> "
                 "[--binary-output <bundle-dll>] --output <metadata.json> --manifest <manifest.json> "
                 "--build-id <id> --target <target> --config <configuration>\n";
}

bool next_value(std::size_t& index, const std::vector<std::string>& arguments, std::string& value) {
    if (index + 1 >= arguments.size())
        return false;
    value = arguments[++index];
    return !value.empty();
}

bool parse_options(const std::vector<std::string>& arguments, Options& options) {
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        std::string value;
        const std::string_view argument(arguments[index]);
        if (argument == "--module" && next_value(index, arguments, value))
            options.module = std::filesystem::u8path(value);
        else if (argument == "--binary-output" && next_value(index, arguments, value))
            options.binary_output = std::filesystem::u8path(value);
        else if ((argument == "--metadata" || argument == "--input") && next_value(index, arguments, value))
            options.metadata_input = std::filesystem::u8path(value);
        else if (argument == "--output" && next_value(index, arguments, value))
            options.output = std::filesystem::u8path(value);
        else if (argument == "--manifest" && next_value(index, arguments, value))
            options.manifest = std::filesystem::u8path(value);
        else if (argument == "--build-id" && next_value(index, arguments, value))
            options.build_id = value;
        else if (argument == "--target" && next_value(index, arguments, value))
            options.target = value;
        else if (argument == "--config" && next_value(index, arguments, value))
            options.configuration = value;
        else
            return false;
    }
    return !options.module.empty() && !options.output.empty() && !options.manifest.empty();
}

bool write_file(const std::filesystem::path& path, std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.close();
    if (!output)
        return false;
    return true;
}

bool atomic_replace(const std::filesystem::path& temporary, const std::filesystem::path& path) {
#ifdef _WIN32
    return MoveFileExW(temporary.wstring().c_str(), path.wstring().c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
#else
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    return !error;
#endif
}

std::filesystem::path temporary_path(const std::filesystem::path& path, std::size_t ordinal) {
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    auto temporary = path;
    temporary += ".aeg-stage-" + std::to_string(stamp) + "-" + std::to_string(ordinal);
    return temporary;
}

struct PublishedFile final {
    std::filesystem::path destination{};
    std::filesystem::path backup{};
    bool existed{false};
    bool replaced{false};
};

bool stage_backup(PublishedFile& file, std::size_t ordinal) {
    std::error_code error;
    file.existed = std::filesystem::exists(file.destination, error);
    if (error) return false;
    if (!file.existed || !std::filesystem::is_regular_file(file.destination, error))
        return !error;
    file.backup = temporary_path(file.destination, 100 + ordinal);
    std::filesystem::copy_file(file.destination, file.backup,
                               std::filesystem::copy_options::overwrite_existing, error);
    return !error;
}

void discard_file(const std::filesystem::path& path) {
    if (path.empty()) return;
    std::error_code error;
    std::filesystem::remove(path, error);
}

void rollback_file(PublishedFile& file) {
    if (!file.replaced) return;
    if (file.existed && !file.backup.empty()) {
        static_cast<void>(atomic_replace(file.backup, file.destination));
    } else if (!file.existed) {
        discard_file(file.destination);
    }
    file.replaced = false;
}

std::string path_text(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::filesystem::path relative_bundle_path(const std::filesystem::path& base, const std::filesystem::path& file) {
    std::error_code error;
    const auto relative =
        std::filesystem::absolute(file, error).lexically_relative(std::filesystem::weakly_canonical(base, error));
    if (error || relative.empty() || relative.is_absolute())
        return {};
    for (const auto& component : relative)
        if (component == "..")
            return {};
    return relative;
}
int run(const std::vector<std::string>& arguments) {
    Options options;
    if (!parse_options(arguments, options)) {
        usage();
        return 2;
    }
    if (options.metadata_input.empty()) {
        options.metadata_input = options.module.parent_path() / "module.metadata.json";
    }

    const auto metadata_result = aeg::module::read_module_metadata(options.metadata_input);
    if (!metadata_result) {
        std::cerr << "metadata read failed: " << metadata_result.result.message << '\n';
        return 3;
    }
    auto metadata = metadata_result.value;
    const auto apply_identity = [](std::string& actual, const std::string& expected, const char* name) {
        if (expected.empty())
            return true;
        if (!actual.empty() && actual != expected)
            return false;
        actual = expected;
        static_cast<void>(name);
        return true;
    };
    if (!apply_identity(metadata.build_id, options.build_id, "build_id") ||
        !apply_identity(metadata.target, options.target, "target") ||
        !apply_identity(metadata.configuration, options.configuration, "configuration")) {
        std::cerr << "metadata identity does not match command line\n";
        return 4;
    }
    if (metadata.build_id.empty() || metadata.target.empty() || metadata.configuration.empty()) {
        std::cerr << "build id, target and configuration are required\n";
        return 5;
    }

    const auto contract = aeg::module::module_contract_sha256(metadata);
    if (!contract) {
        std::cerr << "contract digest failed: " << contract.result.message << '\n';
        return 6;
    }
    if (!metadata.contract_digest.empty() && metadata.contract_digest != contract.value) {
        std::cerr << "metadata contract digest does not match declarations\n";
        return 7;
    }
    metadata.contract_digest = contract.value;
    const auto encoded = aeg::module::encode_module_metadata(metadata);
    if (!encoded) {
        std::cerr << "metadata encoding failed: " << encoded.result.message << '\n';
        return 8;
    }
    const auto metadata_hash = aeg::module::module_content_sha256(encoded.value);
    if (!metadata_hash)
        return 9;

    std::ifstream binary(options.module, std::ios::binary);
    if (!binary) {
        std::cerr << "module binary is missing: " << options.module.string() << '\n';
        return 10;
    }
    const std::string binary_bytes((std::istreambuf_iterator<char>(binary)), {});
    const auto binary_hash = aeg::module::module_content_sha256(binary_bytes);
    if (!binary_hash)
        return 11;

    const auto base = options.manifest.parent_path();
    const auto metadata_relative = relative_bundle_path(base, options.output);
    const auto binary_path = options.binary_output.empty() ? options.module : options.binary_output;
    const auto binary_relative = relative_bundle_path(base, binary_path);
    if (metadata_relative.empty() || binary_relative.empty()) {
        std::cerr << "output and module must be inside the manifest directory\n";
        return 12;
    }
    Json manifest{{"binary_path", path_text(binary_relative)},
                  {"binary_sha256", binary_hash.value},
                  {"build_id", metadata.build_id},
                  {"configuration", metadata.configuration},
                  {"contract_digest", contract.value},
                  {"manifest_schema_version", 1},
                  {"metadata_path", path_text(metadata_relative)},
                  {"metadata_sha256", metadata_hash.value},
                  {"target", metadata.target}};
    std::error_code error;
    std::filesystem::create_directories(options.output.parent_path(), error);
    if (error)
        return 13;
    if (!options.binary_output.empty()) {
        std::filesystem::create_directories(options.binary_output.parent_path(), error);
        if (error)
            return 13;
    }
    const auto metadata_stage = temporary_path(options.output, 0);
    const auto binary_stage = temporary_path(options.binary_output.empty() ? options.module : options.binary_output, 1);
    const auto manifest_stage = temporary_path(options.manifest, 2);
    PublishedFile metadata_file{options.output};
    PublishedFile binary_file{options.binary_output.empty() ? options.module : options.binary_output};
    PublishedFile manifest_file{options.manifest};
    if (!stage_backup(metadata_file, 0) ||
        (!options.binary_output.empty() && !stage_backup(binary_file, 1)) ||
        !stage_backup(manifest_file, 2)) {
        discard_file(metadata_file.backup);
        discard_file(binary_file.backup);
        discard_file(manifest_file.backup);
        std::cerr << "could not stage last-good metadata bundle\n";
        return 13;
    }
    bool staged = write_file(metadata_stage, encoded.value);
    if (staged && !options.binary_output.empty())
        staged = write_file(binary_stage, binary_bytes);
    if (staged)
        staged = write_file(manifest_stage, manifest.dump() + "\n");
    bool published = staged;
    if (published) {
        published = atomic_replace(metadata_stage, metadata_file.destination);
        metadata_file.replaced = published;
    }
    if (published && !options.binary_output.empty()) {
        published = atomic_replace(binary_stage, binary_file.destination);
        binary_file.replaced = published;
    }
    if (published) {
        published = atomic_replace(manifest_stage, manifest_file.destination);
        manifest_file.replaced = published;
    }
    if (!published) {
        // A bundle is a single selector plus its payloads.  If any final
        // replacement fails, restore every payload that was already replaced
        // so the active selector continues to describe the last-good bundle.
        rollback_file(manifest_file);
        rollback_file(binary_file);
        rollback_file(metadata_file);
        discard_file(metadata_stage);
        discard_file(binary_stage);
        discard_file(manifest_stage);
        discard_file(metadata_file.backup);
        discard_file(binary_file.backup);
        discard_file(manifest_file.backup);
        std::cerr << "could not write staged metadata bundle\n";
        return 13;
    }
    discard_file(metadata_file.backup);
    discard_file(binary_file.backup);
    discard_file(manifest_file.backup);
    std::cout << "metadata bundle written: " << path_text(options.manifest) << '\n';
    return 0;
}
}  // namespace

#ifdef _WIN32
std::string utf8_from_wide_exact(std::wstring_view value) {
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                             static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0)
        return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                            result.data(), required, nullptr, nullptr) <= 0)
        return {};
    return result;
}

std::string utf8_from_wide(const wchar_t* value) {
    const std::wstring original{value};
    // CMake can pass UTF-8 script arguments through the active ACP before
    // CreateProcessW. Recover that reversible mojibake before path parsing.
    const UINT code_pages[] = {CP_ACP, GetACP(), GetConsoleCP(), GetConsoleOutputCP(), 936u};
    for (const UINT code_page : code_pages) {
        if (code_page == 0 || code_page == CP_UTF8) continue;
        BOOL used_default = FALSE;
        const int acp_size = WideCharToMultiByte(code_page, WC_NO_BEST_FIT_CHARS, original.data(),
                                                 static_cast<int>(original.size()), nullptr, 0, nullptr,
                                                 &used_default);
        if (acp_size <= 0 || used_default) continue;
        std::string acp(static_cast<std::size_t>(acp_size), '\0');
        if (WideCharToMultiByte(code_page, WC_NO_BEST_FIT_CHARS, original.data(),
                                static_cast<int>(original.size()), acp.data(), acp_size, nullptr,
                                &used_default) <= 0) continue;
        const int repaired_size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                                       acp.data(), acp_size, nullptr, 0);
        if (repaired_size > 0) {
            std::wstring repaired(static_cast<std::size_t>(repaired_size), L'\0');
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, acp.data(), acp_size,
                                    repaired.data(), repaired_size) > 0 && repaired != original) {
                return utf8_from_wide_exact(repaired);
                }
        }
    }
    return utf8_from_wide_exact(original);
}

int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        const auto value = utf8_from_wide(argv[index]);
        if (value.empty() && argv[index][0] != L'\0') {
            usage();
            return 2;
        }
        arguments.push_back(value);
    }
    return run(arguments);
}
#else
int main(int argc, char** argv) {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index)
        arguments.emplace_back(argv[index]);
    return run(arguments);
}
#endif
