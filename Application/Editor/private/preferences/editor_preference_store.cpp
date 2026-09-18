#include "editor_preference_store.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <span>
#include <unordered_set>

namespace aeg::application {
namespace {
constexpr std::array<char, 8> kMagic{'A', 'E', 'G', 'E', 'P', 'R', 'E', 'F'};
constexpr uint32_t kVersion = 1;
constexpr std::size_t kMaximumFileBytes = 1024u * 1024u;
constexpr std::size_t kMaximumStringBytes = 256;
constexpr uint32_t kActiveFontField = 1;
constexpr uint32_t kFontSizeField = 2;
constexpr uint32_t kUserFontsField = 3;

void append_u32(std::vector<std::byte>& bytes, uint32_t value) {
    for (uint32_t shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffu));
}

[[nodiscard]] bool read_u32(std::span<const std::byte> bytes, std::size_t& cursor, uint32_t& value) noexcept {
    if (cursor > bytes.size() || bytes.size() - cursor < 4)
        return false;
    value = static_cast<uint32_t>(static_cast<uint8_t>(bytes[cursor])) |
            (static_cast<uint32_t>(static_cast<uint8_t>(bytes[cursor + 1])) << 8u) |
            (static_cast<uint32_t>(static_cast<uint8_t>(bytes[cursor + 2])) << 16u) |
            (static_cast<uint32_t>(static_cast<uint8_t>(bytes[cursor + 3])) << 24u);
    cursor += 4;
    return true;
}

void append_field(std::vector<std::byte>& payload, uint32_t id, std::span<const std::byte> value) {
    append_u32(payload, id);
    append_u32(payload, static_cast<uint32_t>(value.size()));
    payload.insert(payload.end(), value.begin(), value.end());
}

[[nodiscard]] bool valid_record(const EditorFontFileRecord& record) noexcept {
    if (!record.key.starts_with("user_") || record.key.size() != 21 || record.relative_file_name.empty() ||
        record.relative_file_name.size() > kMaximumStringBytes)
        return false;
    const std::filesystem::path path(record.relative_file_name);
    if (path.has_root_path() || path.has_parent_path() || path.filename().string() != record.relative_file_name)
        return false;
    const auto extension = path.extension().string();
    return extension == ".ttf" || extension == ".otf";
}

[[nodiscard]] foundation::Result validate_data(const EditorPreferenceFileData& data) {
    if (data.active_font_key.size() > kMaximumStringBytes || data.active_font_key.empty() ||
        !std::isfinite(data.font_size_pixels) || data.font_size_pixels < 10.0f || data.font_size_pixels > 48.0f)
        return foundation::invalid_argument("editor preference values are invalid");
    if (data.user_fonts.size() > 256)
        return foundation::invalid_argument("editor preference font count is too large");
    std::unordered_set<std::string> keys;
    for (const auto& record : data.user_fonts) {
        if (!valid_record(record) || !keys.insert(record.key).second)
            return foundation::invalid_argument("editor preference user font record is invalid or duplicated");
    }
    return foundation::Result::success();
}

[[nodiscard]] foundation::ResultValue<std::string> read_string(std::span<const std::byte> bytes, std::size_t& cursor) {
    uint32_t size = 0;
    if (!read_u32(bytes, cursor, size) || size > kMaximumStringBytes || bytes.size() - cursor < size)
        return {foundation::invalid_argument("editor preference string field is truncated"), {}};
    std::string value(reinterpret_cast<const char*>(bytes.data() + cursor), size);
    cursor += size;
    return {foundation::Result::success(), std::move(value)};
}
}  // namespace

foundation::ResultValue<EditorPreferenceFileData> EditorPreferenceStore::load() const {
    if (file_path_.empty())
        return {foundation::invalid_argument("editor preference path is empty"), {}};
    std::ifstream input(file_path_, std::ios::binary);
    if (!input)
        return {foundation::not_found("editor preference file is missing"), {}};
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < 0 || static_cast<uint64_t>(end) > kMaximumFileBytes)
        return {foundation::invalid_argument("editor preference file is too large"), {}};
    input.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input && !input.eof())
        return {foundation::invalid_argument("editor preference file could not be read"), {}};
    if (bytes.size() < kMagic.size() + 8 ||
        !std::equal(kMagic.begin(), kMagic.end(), reinterpret_cast<const char*>(bytes.data())))
        return {foundation::invalid_argument("editor preference magic is invalid"), {}};
    std::size_t cursor = kMagic.size();
    uint32_t version = 0;
    uint32_t payload_size = 0;
    if (!read_u32(bytes, cursor, version) || !read_u32(bytes, cursor, payload_size) || version != kVersion ||
        payload_size != bytes.size() - cursor)
        return {foundation::invalid_argument("editor preference header is invalid or truncated"), {}};
    const auto payload = std::span<const std::byte>(bytes).subspan(cursor, payload_size);
    EditorPreferenceFileData result{};
    bool active_seen = false;
    bool size_seen = false;
    bool users_seen = false;
    cursor = 0;
    while (cursor < payload.size()) {
        uint32_t id = 0;
        uint32_t size = 0;
        if (!read_u32(payload, cursor, id) || !read_u32(payload, cursor, size) || size > payload.size() - cursor)
            return {foundation::invalid_argument("editor preference field header is truncated"), {}};
        const auto field = payload.subspan(cursor, size);
        cursor += size;
        if (id == kActiveFontField) {
            if (active_seen || size > kMaximumStringBytes)
                return {foundation::invalid_argument("active font field is invalid"), {}};
            result.active_font_key.assign(reinterpret_cast<const char*>(field.data()), field.size());
            active_seen = true;
        } else if (id == kFontSizeField) {
            if (size != sizeof(float) || size_seen)
                return {foundation::invalid_argument("font size field is invalid"), {}};
            std::memcpy(&result.font_size_pixels, field.data(), sizeof(float));
            size_seen = true;
        } else if (id == kUserFontsField) {
            if (users_seen)
                return {foundation::invalid_argument("user font field is duplicated"), {}};
            std::size_t field_cursor = 0;
            uint32_t count = 0;
            if (!read_u32(field, field_cursor, count) || count > 256)
                return {foundation::invalid_argument("user font count is invalid"), {}};
            result.user_fonts.reserve(count);
            for (uint32_t index = 0; index < count; ++index) {
                const auto key = read_string(field, field_cursor);
                const auto file = read_string(field, field_cursor);
                if (!key || !file)
                    return {foundation::invalid_argument("user font record is truncated"), {}};
                result.user_fonts.push_back({std::move(key.value), std::move(file.value)});
            }
            if (field_cursor != field.size())
                return {foundation::invalid_argument("user font field has trailing bytes"), {}};
            users_seen = true;
        }
    }
    if (!active_seen || !size_seen)
        return {foundation::invalid_argument("editor preference required field is missing"), {}};
    if (const auto validation = validate_data(result); !validation)
        return {validation, {}};
    return {foundation::Result::success(), std::move(result)};
}

foundation::Result EditorPreferenceStore::write_atomic(const EditorPreferenceFileData& data) const {
    if (file_path_.empty())
        return foundation::invalid_argument("editor preference path is empty");
    if (const auto validation = validate_data(data); !validation)
        return validation;
    try {
        std::vector<std::byte> payload;
        std::vector<std::byte> active(data.active_font_key.size());
        std::memcpy(active.data(), data.active_font_key.data(), active.size());
        append_field(payload, kActiveFontField, active);
        std::vector<std::byte> size(sizeof(float));
        std::memcpy(size.data(), &data.font_size_pixels, sizeof(float));
        append_field(payload, kFontSizeField, size);
        std::vector<std::byte> users;
        append_u32(users, static_cast<uint32_t>(data.user_fonts.size()));
        std::vector<EditorFontFileRecord> sorted = data.user_fonts;
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& left, const auto& right) { return left.key < right.key; });
        for (const auto& record : sorted) {
            append_u32(users, static_cast<uint32_t>(record.key.size()));
            users.insert(users.end(), reinterpret_cast<const std::byte*>(record.key.data()),
                         reinterpret_cast<const std::byte*>(record.key.data() + record.key.size()));
            append_u32(users, static_cast<uint32_t>(record.relative_file_name.size()));
            users.insert(users.end(), reinterpret_cast<const std::byte*>(record.relative_file_name.data()),
                         reinterpret_cast<const std::byte*>(record.relative_file_name.data() +
                                                            record.relative_file_name.size()));
        }
        append_field(payload, kUserFontsField, users);
        if (payload.size() > std::numeric_limits<uint32_t>::max())
            return foundation::invalid_argument("editor preference payload is too large");
        std::vector<std::byte> bytes(kMagic.size() + 8 + payload.size());
        std::memcpy(bytes.data(), kMagic.data(), kMagic.size());
        std::size_t cursor = kMagic.size();
        auto write_u32 = [&bytes, &cursor](uint32_t value) {
            for (uint32_t shift = 0; shift < 32; shift += 8)
                bytes[cursor++] = static_cast<std::byte>((value >> shift) & 0xffu);
        };
        write_u32(kVersion);
        write_u32(static_cast<uint32_t>(payload.size()));
        std::memcpy(bytes.data() + cursor, payload.data(), payload.size());

        std::error_code error;
        std::filesystem::create_directories(file_path_.parent_path(), error);
        if (error)
            return foundation::Result::failure(foundation::ResultCode::kBackendError,
                                               "editor preference directory could not be created");
        const auto temporary = file_path_.string() + ".tmp";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            return foundation::Result::failure(foundation::ResultCode::kBackendError,
                                               "editor preference temporary file could not be opened");
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        if (!output)
            return foundation::Result::failure(foundation::ResultCode::kBackendError,
                                               "editor preference temporary file could not be written");
        output.close();
        if (output.fail())
            return foundation::Result::failure(foundation::ResultCode::kBackendError,
                                               "editor preference temporary file could not be closed");
        const auto backup = file_path_.string() + ".bak";
        std::filesystem::remove(backup, error);
        if (std::filesystem::exists(file_path_, error)) {
            std::filesystem::rename(file_path_, backup, error);
            if (error) {
                std::filesystem::remove(temporary, error);
                return foundation::Result::failure(foundation::ResultCode::kBackendError,
                                                   "editor preference backup failed");
            }
        }
        std::filesystem::rename(temporary, file_path_, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            if (std::filesystem::exists(backup, error))
                std::filesystem::rename(backup, file_path_, error);
            return foundation::Result::failure(foundation::ResultCode::kBackendError,
                                               "editor preference atomic replace failed");
        }
        std::filesystem::remove(backup, error);
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("editor preference serialization allocation failed");
    }
}

}  // namespace aeg::application
