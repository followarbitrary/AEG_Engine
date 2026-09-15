/**
 * @file editor_system_font_catalog.cpp
 * @brief Builds editor font assets from the Application platform font snapshot.
 */
#include "editor_system_font_catalog.hpp"

#include "application_platform.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <new>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aeg::application {
namespace {
using aeg::editor::EditorFontDescriptor;
using aeg::editor::EditorFontKind;

constexpr std::array<std::string_view, 7> kPreferredFamilies{
    "Segoe UI", "Inter", "Noto Sans SC", "IBM Plex Sans", "JetBrains Mono", "Fira Code", "Source Code Pro"};

[[nodiscard]] std::string lower_ascii(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char character : value) {
        result.push_back(character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a')
                                                               : static_cast<char>(character));
    }
    return result;
}

[[nodiscard]] bool is_preferred_family(std::string_view family_name) {
    const std::string lowered = lower_ascii(family_name);
    return std::any_of(kPreferredFamilies.begin(), kPreferredFamilies.end(), [&lowered](std::string_view preferred) {
        return lowered == lower_ascii(preferred);
    });
}

[[nodiscard]] std::string make_font_key(std::string_view family_name) {
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char character : family_name) {
        hash ^= character;
        hash *= 1099511628211ull;
    }
    std::ostringstream stream;
    stream << "system_" << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
}

[[nodiscard]] EditorFontKind infer_kind(std::string_view family_name) {
    const std::string lowered = lower_ascii(family_name);
    return lowered.find("mono") != std::string::npos || lowered.find("code") != std::string::npos ||
                   lowered.find("console") != std::string::npos
               ? EditorFontKind::kMonospace
               : EditorFontKind::kInterface;
}

[[nodiscard]] foundation::ResultValue<std::vector<std::byte>> read_font_file(std::string_view path) {
    try {
        const std::filesystem::path file_path = std::filesystem::u8path(std::string{path});
        std::ifstream stream(file_path, std::ios::binary);
        if (!stream) {
            return {foundation::unsupported("System font file could not be opened"), {}};
        }
        stream.seekg(0, std::ios::end);
        const std::streampos end = stream.tellg();
        if (end <= 0 || static_cast<uint64_t>(end) > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max()) ||
            static_cast<uint64_t>(end) > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max())) {
            return {foundation::invalid_argument("System font file size is invalid"), {}};
        }
        const std::size_t size = static_cast<std::size_t>(end);
        std::vector<std::byte> data(size);
        stream.seekg(0, std::ios::beg);
        stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
        if (!stream) {
            return {foundation::unsupported("System font file could not be read"), {}};
        }
        return {foundation::Result::success(), std::move(data)};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("System font file allocation failed"), {}};
    } catch (const std::exception& exception) {
        return {foundation::unknown_exception(exception), {}};
    }
}
}  // namespace

foundation::ResultValue<EditorSystemFontCatalogData> EditorSystemFontCatalog::load() const {
    try {
        const foundation::ResultValue<std::vector<SystemFontRecord>> records_result = enumerate_system_fonts();
        if (!records_result) {
            return {records_result.result, {}};
        }

        EditorSystemFontCatalogData result{};
        std::string fallback_key;
        std::size_t preferred_rank = kPreferredFamilies.size();
        for (const SystemFontRecord& record : records_result.value) {
            EditorFontDescriptor descriptor{};
            descriptor.key = make_font_key(record.family_name);
            descriptor.display_name = record.family_name;
            descriptor.kind = infer_kind(record.family_name);
            descriptor.supports_chinese = record.supports_chinese;
            descriptor.supports_ligatures = record.supports_ligatures;
            descriptor.is_default = is_preferred_family(record.family_name);
            descriptor.is_protected = true;

            const foundation::ResultValue<std::vector<std::byte>> data_result = read_font_file(record.file_path);
            if (!data_result) {
                continue;
            }
            if (const foundation::Result register_result =
                    result.catalog.register_font(descriptor, data_result.value, record.face_index);
                !register_result) {
                continue;
            }
            if (fallback_key.empty()) {
                fallback_key = descriptor.key;
            }
            const auto preferred = std::find(kPreferredFamilies.begin(), kPreferredFamilies.end(), record.family_name);
            const auto rank = static_cast<std::size_t>(preferred - kPreferredFamilies.begin());
            if (rank < preferred_rank) {
                result.default_font_key = descriptor.key;
                preferred_rank = rank;
            }
        }
        if (result.catalog.font_count() == 0) {
            return {foundation::unsupported("No readable system fonts were found"), {}};
        }
        if (result.default_font_key.empty()) {
            result.default_font_key = std::move(fallback_key);
        }
        return {foundation::Result::success(), std::move(result)};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("System font catalog allocation failed"), {}};
    } catch (const std::exception& exception) {
        return {foundation::unknown_exception(exception), {}};
    } catch (...) {
        return {foundation::Result::failure(foundation::ResultCode::kUnknown,
                                            "System font catalog construction threw an unknown exception"), {}};
    }
}
}  // namespace aeg::application
