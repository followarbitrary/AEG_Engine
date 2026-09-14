/**
 * @file editor_asset_database.cpp
 * @brief Implements the project-local Asset Browser index.
 */
#include "editor_asset_database.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_set>
#include <utility>

namespace aeg::application {
namespace {
using foundation::Result;

constexpr uint64_t kFnvOffset = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;
constexpr uint64_t kDynamicIdBit = 0x8000000000000000ull;
constexpr std::string_view kManifestHeader{"AEG_ASSET_DATABASE\t1"};

[[nodiscard]] std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

[[nodiscard]] EditorAssetKind kind_for_extension(const std::string& extension) noexcept {
    if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".jpe" ||
        extension == ".jif" || extension == ".jfif" || extension == ".bmp" || extension == ".tga" ||
        extension == ".gif" || extension == ".psd" || extension == ".pic" || extension == ".pnm" ||
        extension == ".pbm" || extension == ".pgm" || extension == ".ppm" || extension == ".pam" ||
        extension == ".hdr" || extension == ".aegtex") {
        return EditorAssetKind::kTexture;
    }
    if (extension == ".mesh" || extension == ".obj" || extension == ".fbx" || extension == ".gltf" ||
        extension == ".glb" || extension == ".aegmesh") {
        return EditorAssetKind::kMesh;
    }
    if (extension == ".mat" || extension == ".material" || extension == ".aegmat" || extension == ".aegmatbin") {
        return EditorAssetKind::kMaterial;
    }
    if (extension == ".aegscene" || extension == ".scene") {
        return EditorAssetKind::kScene;
    }
    if (extension == ".ttf" || extension == ".otf" || extension == ".ttc" || extension == ".otc" ||
        extension == ".aegfont") {
        return EditorAssetKind::kFont;
    }
    return EditorAssetKind::kUnknown;
}

[[nodiscard]] int64_t modified_ticks(const std::filesystem::path& path) noexcept {
    std::error_code error;
    const auto value = std::filesystem::last_write_time(path, error);
    return error ? 0 : static_cast<int64_t>(value.time_since_epoch().count());
}

[[nodiscard]] uint64_t hash_file(const std::filesystem::path& path, uint64_t seed) noexcept {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return 0;
    }
    uint64_t hash = kFnvOffset ^ seed;
    std::array<char, 4096> buffer{};
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = stream.gcount();
        for (std::streamsize index = 0; index < count; ++index) {
            hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(index)]);
            hash *= kFnvPrime;
        }
    }
    return hash;
}

[[nodiscard]] uint64_t stable_id(std::string_view relative_path) noexcept {
    uint64_t hash = kFnvOffset;
    for (const unsigned char character : relative_path) {
        hash ^= character;
        hash *= kFnvPrime;
    }
    hash |= kDynamicIdBit;
    return hash == 0 ? kDynamicIdBit : hash;
}

[[nodiscard]] std::vector<std::string> split_tab(std::string_view line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (start <= line.size()) {
        const std::size_t end = line.find('\t', start);
        fields.emplace_back(line.substr(start, end == std::string_view::npos ? line.size() - start : end - start));
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return fields;
}

template <typename Value>
[[nodiscard]] bool parse_integer(std::string_view text, Value& value) noexcept {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

[[nodiscard]] bool validate_import(const std::filesystem::path& path, EditorAssetKind kind, std::string& error) {
    std::error_code size_error;
    const uintmax_t size = std::filesystem::file_size(path, size_error);
    if (size_error || size == 0) {
        error = "Asset file is empty or cannot be read.";
        return false;
    }
    if (kind == EditorAssetKind::kMesh && lower(path.extension().string()) == ".obj") {
        std::ifstream stream(path);
        bool has_vertex = false;
        bool has_face = false;
        std::string line;
        while (std::getline(stream, line)) {
            has_vertex = has_vertex || line.rfind("v ", 0) == 0;
            has_face = has_face || line.rfind("f ", 0) == 0;
        }
        if (!has_vertex || !has_face) {
            error = "OBJ mesh must contain at least one vertex and one face.";
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string replace_all(std::string value, std::string_view from, std::string_view to) {
    if (from.empty()) {
        return value;
    }
    std::size_t offset = 0;
    while ((offset = value.find(from, offset)) != std::string::npos) {
        value.replace(offset, from.size(), to);
        offset += to.size();
    }
    return value;
}

[[nodiscard]] foundation::Result rewrite_asset_references(const std::filesystem::path& project_root,
                                                          std::span<const EditorAssetRecord> records,
                                                          uint64_t excluded_id, std::string_view old_path,
                                                          std::string_view new_path) {
    const std::string old_without_root =
        old_path.starts_with("Assets/") ? std::string(old_path.substr(7)) : std::string(old_path);
    const std::string new_without_root =
        new_path.starts_with("Assets/") ? std::string(new_path.substr(7)) : std::string(new_path);
    for (const auto& record : records) {
        if (record.id == excluded_id || record.status != EditorAssetStatus::kImported ||
            (record.kind != EditorAssetKind::kMaterial && record.kind != EditorAssetKind::kScene)) {
            continue;
        }
        const std::filesystem::path path = project_root / record.relative_path;
        std::ifstream source(path, std::ios::binary);
        if (!source) {
            continue;
        }
        const std::string original((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
        std::string updated = replace_all(original, old_path, new_path);
        updated = replace_all(std::move(updated), old_without_root, new_without_root);
        if (updated == original) {
            continue;
        }
        const std::filesystem::path temporary = path.string() + ".aeg_asset_tmp";
        std::ofstream destination(temporary, std::ios::binary | std::ios::trunc);
        if (!destination) {
            return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                               "Cannot update a dependent asset reference");
        }
        destination.write(updated.data(), static_cast<std::streamsize>(updated.size()));
        destination.close();
        std::error_code error;
        std::filesystem::copy_file(temporary, path, std::filesystem::copy_options::overwrite_existing, error);
        const bool copied = !error;
        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
        if (!copied || cleanup_error) {
            return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                               "Cannot replace a dependent asset reference");
        }
    }
    return foundation::Result::success();
}

}  // namespace

std::string_view editor_asset_kind_name(EditorAssetKind kind) noexcept {
    switch (kind) {
        case EditorAssetKind::kTexture:
            return "Texture";
        case EditorAssetKind::kMesh:
            return "Mesh";
        case EditorAssetKind::kMaterial:
            return "Material";
        case EditorAssetKind::kScene:
            return "Scene";
        case EditorAssetKind::kFont:
            return "Font";
        case EditorAssetKind::kUnknown:
            return "Unknown";
    }
    return "Unknown";
}

std::string_view editor_asset_status_name(EditorAssetStatus status) noexcept {
    switch (status) {
        case EditorAssetStatus::kImported:
            return "Imported";
        case EditorAssetStatus::kMissing:
            return "Missing";
        case EditorAssetStatus::kUnsupported:
            return "Unsupported";
        case EditorAssetStatus::kFailed:
            return "Failed";
    }
    return "Failed";
}

foundation::Result EditorAssetDatabase::initialize(const std::filesystem::path& project_root) {
    std::error_code error;
    project_root_ = std::filesystem::weakly_canonical(project_root, error);
    if (error) {
        project_root_ = std::filesystem::absolute(project_root, error);
    }
    assets_root_ = project_root_ / "Assets";
    manifest_path_ = project_root_ / ".aeg" / "asset_database.tsv";
    assets_.clear();
    if (const Result result = load_manifest(); !result) {
        return result;
    }
    // A default editor session may run without a project Assets directory. Keep
    // that session in-memory until the user creates or refreshes the directory.
    if (error) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot inspect project Assets directory");
    }
    if (!std::filesystem::exists(assets_root_)) {
        return Result::success();
    }
    return refresh();
}

foundation::Result EditorAssetDatabase::load_manifest() {
    std::ifstream stream(manifest_path_);
    if (!stream) {
        return Result::success();
    }
    std::string line;
    if (!std::getline(stream, line) || line != kManifestHeader) {
        return foundation::invalid_argument("Asset database manifest header is invalid");
    }
    while (std::getline(stream, line)) {
        if (line.empty()) {
            continue;
        }
        const auto fields = split_tab(line);
        if (fields.size() != 8 && fields.size() != 9) {
            return foundation::invalid_argument("Asset database manifest record is malformed");
        }
        EditorAssetRecord record{};
        uint8_t kind = 0;
        uint8_t status = 0;
        if (!parse_integer(fields[0], record.id) || !parse_integer(fields[1], kind) ||
            !parse_integer(fields[2], status) || !parse_integer(fields[4], record.file_size) ||
            !parse_integer(fields[5], record.modified_ticks) || !parse_integer(fields[6], record.content_hash)) {
            return foundation::invalid_argument("Asset database manifest number is invalid");
        }
        record.kind = static_cast<EditorAssetKind>(kind);
        record.status = static_cast<EditorAssetStatus>(status);
        record.relative_path = fields[3];
        record.display_name = fields[7];
        record.extension = lower(std::filesystem::path(record.relative_path).extension().string());
        if (fields.size() == 9 && !fields[8].empty()) {
            std::size_t start = 0;
            while (start < fields[8].size()) {
                const std::size_t end = fields[8].find(',', start);
                uint64_t dependency = 0;
                const std::string_view value(fields[8].data() + start,
                                             end == std::string::npos ? fields[8].size() - start : end - start);
                if (parse_integer(value, dependency)) {
                    record.dependencies.push_back(dependency);
                }
                if (end == std::string::npos) {
                    break;
                }
                start = end + 1;
            }
        }
        assets_.push_back(std::move(record));
    }
    return Result::success();
}

foundation::Result EditorAssetDatabase::save_manifest() const {
    std::error_code error;
    std::filesystem::create_directories(manifest_path_.parent_path(), error);
    if (error) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot create Asset database directory");
    }
    const auto temporary = manifest_path_.string() + ".tmp";
    std::ofstream stream(temporary, std::ios::trunc);
    if (!stream) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot write Asset database manifest");
    }
    stream << kManifestHeader << '\n';
    for (const auto& asset : assets_) {
        stream << asset.id << '\t' << static_cast<unsigned>(asset.kind) << '\t' << static_cast<unsigned>(asset.status)
               << '\t' << asset.relative_path << '\t' << asset.file_size << '\t' << asset.modified_ticks << '\t'
               << asset.content_hash << '\t' << asset.display_name << '\t';
        for (std::size_t index = 0; index < asset.dependencies.size(); ++index) {
            if (index != 0) {
                stream << ',';
            }
            stream << asset.dependencies[index];
        }
        stream << '\n';
    }
    stream.close();
    std::filesystem::rename(temporary, manifest_path_, error);
    if (error) {
        std::filesystem::remove(manifest_path_, error);
        error.clear();
        std::filesystem::rename(temporary, manifest_path_, error);
    }
    return error
               ? foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot replace Asset database manifest")
               : Result::success();
}

foundation::Result EditorAssetDatabase::refresh() {
    std::error_code error;
    std::filesystem::create_directories(assets_root_, error);
    if (error) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot access project Assets directory");
    }
    const std::vector<EditorAssetRecord> previous = assets_;
    std::vector<EditorAssetRecord> scanned;
    std::unordered_set<uint64_t> used_ids;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(assets_root_, error)) {
        if (error) {
            return foundation::Result::failure(foundation::ResultCode::kUnknown,
                                               "Cannot scan project Assets directory");
        }
        if (!entry.is_regular_file(error) || error) {
            continue;
        }
        const std::string relative = std::filesystem::relative(entry.path(), project_root_, error).generic_string();
        if (error || relative.empty()) {
            continue;
        }
        EditorAssetRecord record{};
        record.relative_path = relative;
        record.display_name = entry.path().stem().string();
        record.extension = lower(entry.path().extension().string());
        record.kind = kind_for_extension(record.extension);
        record.file_size = static_cast<uint64_t>(entry.file_size(error));
        record.modified_ticks = modified_ticks(entry.path());
        record.content_hash = hash_file(entry.path(), static_cast<uint8_t>(record.kind));
        const auto by_path = std::find_if(previous.begin(), previous.end(), [&](const auto& value) {
            return value.relative_path == record.relative_path && value.kind == record.kind;
        });
        const auto by_content = std::find_if(previous.begin(), previous.end(), [&](const auto& value) {
            return value.kind == record.kind && value.content_hash == record.content_hash &&
                   value.status != EditorAssetStatus::kMissing && !used_ids.contains(value.id);
        });
        record.id = by_path != previous.end() ? by_path->id
                                              : (by_content != previous.end() ? by_content->id : stable_id(relative));
        while (!used_ids.emplace(record.id).second) {
            record.id = (record.id + 1u) | kDynamicIdBit;
        }
        if (record.kind == EditorAssetKind::kUnknown) {
            record.status = EditorAssetStatus::kUnsupported;
            record.error = "No importer is registered for this file type.";
        } else if (validate_import(entry.path(), record.kind, record.error)) {
            record.status = EditorAssetStatus::kImported;
        } else {
            record.status = EditorAssetStatus::kFailed;
        }
        scanned.push_back(std::move(record));
    }
    for (const auto& old : previous) {
        if (std::find_if(scanned.begin(), scanned.end(), [&](const auto& value) { return value.id == old.id; }) ==
            scanned.end()) {
            EditorAssetRecord missing = old;
            missing.status = EditorAssetStatus::kMissing;
            missing.file_size = 0;
            missing.modified_ticks = 0;
            missing.content_hash = 0;
            missing.dependencies.clear();
            missing.error = "Asset file is missing from the project Assets directory.";
            scanned.push_back(std::move(missing));
        }
    }
    std::sort(scanned.begin(), scanned.end(),
              [](const auto& left, const auto& right) { return left.relative_path < right.relative_path; });
    for (auto& asset : scanned) {
        if ((asset.kind != EditorAssetKind::kMaterial && asset.kind != EditorAssetKind::kScene) ||
            asset.status != EditorAssetStatus::kImported) {
            continue;
        }
        std::ifstream source(project_root_ / asset.relative_path, std::ios::binary);
        if (!source) {
            continue;
        }
        const std::string contents((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
        for (const auto& dependency : scanned) {
            if (dependency.id == asset.id || dependency.relative_path.empty()) {
                continue;
            }
            if (contents.find(dependency.relative_path) != std::string::npos) {
                asset.dependencies.push_back(dependency.id);
            }
        }
    }
    assets_ = std::move(scanned);
    return save_manifest();
}

foundation::Result EditorAssetDatabase::reimport(uint64_t id) {
    if (find(id) == nullptr) {
        return foundation::not_found("Asset ID is not present in the database");
    }
    return refresh();
}

foundation::Result EditorAssetDatabase::rename(uint64_t id, std::string_view new_name) {
    const EditorAssetRecord* asset = find(id);
    if (asset == nullptr) {
        return foundation::not_found("Asset ID is not present in the database");
    }
    if (new_name.empty() || new_name.find_first_of("/\\\t\r\n") != std::string_view::npos) {
        return foundation::invalid_argument("Asset name is invalid");
    }
    const std::string old_relative_path = asset->relative_path;
    const std::filesystem::path source = project_root_ / asset->relative_path;
    const std::filesystem::path target = source.parent_path() / (std::string(new_name) + source.extension().string());
    std::error_code error;
    std::filesystem::rename(source, target, error);
    if (error) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown, "Asset rename failed");
    }
    const std::string new_relative_path = std::filesystem::relative(target, project_root_, error).generic_string();
    if (error || new_relative_path.empty()) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown, "Asset rename path is invalid");
    }
    const Result rewrite = rewrite_asset_references(project_root_, assets_, id, old_relative_path, new_relative_path);
    const Result refreshed = refresh();
    return rewrite ? refreshed : rewrite;
}

foundation::Result EditorAssetDatabase::remove(uint64_t id) {
    const EditorAssetRecord* asset = find(id);
    if (asset == nullptr) {
        return foundation::not_found("Asset ID is not present in the database");
    }
    std::error_code error;
    std::filesystem::remove(project_root_ / asset->relative_path, error);
    if (error) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown, "Asset delete failed");
    }
    return refresh();
}

const EditorAssetRecord* EditorAssetDatabase::find(uint64_t id) const noexcept {
    const auto found = std::find_if(assets_.begin(), assets_.end(), [id](const auto& asset) { return asset.id == id; });
    return found == assets_.end() ? nullptr : &*found;
}

const EditorAssetRecord* EditorAssetDatabase::find_by_relative_path(std::string_view path) const noexcept {
    const auto found = std::find_if(assets_.begin(), assets_.end(),
                                    [path](const EditorAssetRecord& record) { return record.relative_path == path; });
    return found == assets_.end() ? nullptr : &*found;
}

bool EditorAssetDatabase::is_valid(uint64_t id, EditorAssetKind kind) const noexcept {
    const auto* asset = find(id);
    return asset != nullptr && asset->kind == kind && asset->status == EditorAssetStatus::kImported;
}

}  // namespace aeg::application
