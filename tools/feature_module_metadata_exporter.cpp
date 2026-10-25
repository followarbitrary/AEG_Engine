#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "feature_module_metadata_export.hpp"

#if !defined(AEG_METADATA_FACTORY_HEADER) || !defined(AEG_METADATA_FACTORY)
#error "The metadata exporter requires its module factory header and function"
#endif
#include AEG_METADATA_FACTORY_HEADER

namespace {
struct Options final {
    std::filesystem::path output;
    std::string build_id;
    std::string target;
    std::string configuration;
};

bool parse_options(const std::vector<std::string>& arguments, Options& options) {
    for (std::size_t index = 1; index < arguments.size(); index += 2) {
        if (index + 1 >= arguments.size() || arguments[index + 1].empty()) return false;
        const std::string_view option{arguments[index]};
        const std::string& value = arguments[index + 1];
        if (option == "--output" && options.output.empty()) options.output = std::filesystem::u8path(value);
        else if (option == "--build-id" && options.build_id.empty()) options.build_id = value;
        else if (option == "--target" && options.target.empty()) options.target = value;
        else if (option == "--config" && options.configuration.empty()) options.configuration = value;
        else return false;
    }
    return !options.output.empty() && !options.build_id.empty() && !options.target.empty() &&
           !options.configuration.empty();
}

bool write_metadata(const std::filesystem::path& path, std::string_view contents) {
    std::error_code error;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.close();
        if (!output) {
            std::filesystem::remove(temporary, error);
            return false;
        }
    }
    // Replace only after validation, encoding and the complete write succeeded.
    // In particular, never remove a last-good destination before a rename.
#if defined(_WIN32)
    const bool replaced = MoveFileExW(temporary.c_str(), path.c_str(),
                                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::filesystem::rename(temporary, path, error);
    const bool replaced = !error;
#endif
    if (!replaced) std::filesystem::remove(temporary, error);
    return replaced;
}
}  // namespace

int run(const std::vector<std::string>& arguments) {
    Options options;
    if (!parse_options(arguments, options)) {
        std::cerr << "usage: module_contract_exporter --output <metadata.json> --build-id <id> "
                     "--target <target> --config <configuration>\n";
        return 2;
    }
    try {
        const auto definition = AEG_METADATA_FACTORY();
        const auto metadata = aeg::module::export_feature_module_metadata(
            definition, options.build_id, options.target, options.configuration);
        if (!metadata) { std::cerr << metadata.result.message << '\n'; return 3; }
        const auto encoded = aeg::module::encode_module_metadata(metadata.value);
        if (!encoded) { std::cerr << encoded.result.message << '\n'; return 4; }
        if (!write_metadata(options.output, encoded.value)) {
            std::cerr << "could not publish source module metadata: " << options.output << '\n';
            return 5;
        }
        std::cout << "Exported module contracts: " << metadata.value.feature_catalog.entries().size() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "module contract factory failed: " << error.what() << '\n';
        return 6;
    } catch (...) {
        std::cerr << "module contract factory failed with an unknown exception\n";
        return 6;
    }
}

#if defined(_WIN32)
std::string utf8_from_wide_exact(std::wstring_view value) {
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                             static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                            result.data(), required, nullptr, nullptr) <= 0)
        return {};
    return result;
}

std::string utf8_from_wide(const wchar_t* value) {
    const std::wstring original{value};
    // CMake's Windows process launcher may pass UTF-8 script arguments
    // through the active ACP before CreateProcessW.  If the resulting wide
    // text can be round-tripped as ACP bytes and those bytes are valid UTF-8,
    // recover the original text before constructing filesystem paths.
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
                                    repaired.data(), repaired_size) > 0 && repaired != original)
                return utf8_from_wide_exact(repaired);
        }
    }
    return utf8_from_wide_exact(original);
}

int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        const auto value = utf8_from_wide(argv[index]);
        if (value.empty() && argv[index][0] != L'\0') return 2;
        arguments.push_back(value);
    }
    return run(arguments);
}
#else
int main(int argc, char** argv) {
    return run(std::vector<std::string>(argv, argv + argc));
}
#endif
