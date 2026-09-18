#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

#include "module_bundle.hpp"

namespace {
using Json = nlohmann::json;

struct Options final {
    std::filesystem::path module{};
    std::filesystem::path metadata_input{};
    std::filesystem::path output{};
    std::filesystem::path manifest{};
    std::string build_id{};
    std::string target{};
    std::string configuration{};
};

void usage() {
    std::cerr << "usage: aeg_module_metadata_tool --module <dll> --metadata <metadata.json> "
                 "--output <metadata.json> --manifest <manifest.json> "
                 "--build-id <id> --target <target> --config <configuration>\n";
}

bool next_value(int& index, int argc, char** argv, std::string& value) {
    if (index + 1 >= argc)
        return false;
    value = argv[++index];
    return !value.empty();
}

bool parse_options(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        std::string value;
        const std::string_view argument(argv[index]);
        if (argument == "--module" && next_value(index, argc, argv, value))
            options.module = value;
        else if ((argument == "--metadata" || argument == "--input") && next_value(index, argc, argv, value))
            options.metadata_input = value;
        else if (argument == "--output" && next_value(index, argc, argv, value))
            options.output = value;
        else if (argument == "--manifest" && next_value(index, argc, argv, value))
            options.manifest = value;
        else if (argument == "--build-id" && next_value(index, argc, argv, value))
            options.build_id = value;
        else if (argument == "--target" && next_value(index, argc, argv, value))
            options.target = value;
        else if (argument == "--config" && next_value(index, argc, argv, value))
            options.configuration = value;
        else
            return false;
    }
    return !options.module.empty() && !options.output.empty() && !options.manifest.empty();
}

bool write_text(const std::filesystem::path& path, std::string_view contents) {
    const auto temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.close();
    if (!output)
        return false;
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::rename(temporary, path, error);
    if (error)
        std::filesystem::remove(temporary, error);
    return !error;
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
}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
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
    const auto binary_relative = relative_bundle_path(base, options.module);
    if (metadata_relative.empty() || binary_relative.empty()) {
        std::cerr << "output and module must be inside the manifest directory\n";
        return 12;
    }
    Json manifest{{"binary_path", binary_relative.generic_string()},
                  {"binary_sha256", binary_hash.value},
                  {"build_id", metadata.build_id},
                  {"configuration", metadata.configuration},
                  {"contract_digest", contract.value},
                  {"manifest_schema_version", 1},
                  {"metadata_path", metadata_relative.generic_string()},
                  {"metadata_sha256", metadata_hash.value},
                  {"target", metadata.target}};
    std::error_code error;
    std::filesystem::create_directories(options.output.parent_path(), error);
    if (error || !write_text(options.output, encoded.value) || !write_text(options.manifest, manifest.dump() + "\n")) {
        std::cerr << "could not write staged metadata bundle\n";
        return 13;
    }
    std::cout << "metadata bundle written: " << options.manifest.string() << '\n';
    return 0;
}
