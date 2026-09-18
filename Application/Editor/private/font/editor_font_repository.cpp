#include "editor_font_repository.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <iterator>
#include <limits>

namespace aeg::application {
namespace {
std::atomic<uint64_t> g_transaction_id{1};
constexpr std::size_t kMaximumFontBytes = 64u * 1024u * 1024u;

[[nodiscard]] foundation::ResultValue<std::vector<std::byte>> read_bytes(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > kMaximumFontBytes)
        return {foundation::invalid_argument("user font file size is invalid"), {}};
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {foundation::not_found("user font file cannot be opened"), {}};
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input && !input.eof())
        return {foundation::invalid_state("user font file could not be read"), {}};
    return {foundation::Result::success(), std::move(bytes)};
}

}  // namespace

EditorFontRepository::~EditorFontRepository() {
    rollback();
}

foundation::Result EditorFontRepository::initialize(std::filesystem::path root_directory) {
    if (root_directory.empty() || root_directory.is_relative())
        return foundation::invalid_argument("user font repository root must be an absolute path");
    std::error_code error;
    if (std::filesystem::exists(root_directory, error) && !std::filesystem::is_directory(root_directory, error))
        return foundation::invalid_argument("user font repository root is not a directory");
    if (!std::filesystem::create_directories(root_directory, error) && error)
        return foundation::Result::failure(foundation::ResultCode::kBackendError,
                                           "user font repository directory could not be created");
    rollback();
    root_directory_ = std::move(root_directory);
    initialized_ = true;
    return foundation::Result::success();
}

bool EditorFontRepository::accepted_extension(const std::filesystem::path& path) noexcept {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return extension == ".ttf" || extension == ".otf";
}

std::string EditorFontRepository::user_key(std::string_view inspected_key) {
    constexpr std::string_view prefix = "font_";
    if (inspected_key.starts_with(prefix))
        return "user_" + std::string(inspected_key.substr(prefix.size()));
    return "user_" + std::string(inspected_key);
}

foundation::ResultValue<editor::EditorFontAsset> EditorFontRepository::inspect_file(
    const std::filesystem::path& path) const {
    const auto bytes = read_bytes(path);
    if (!bytes)
        return {bytes.result, {}};
    const auto inspected = editor::EditorFontInspector{}.inspect(bytes.value);
    if (!inspected)
        return {inspected.result, {}};
    editor::EditorFontAsset asset{};
    asset.descriptor = inspected.value;
    asset.descriptor.key = user_key(inspected.value.key);
    asset.descriptor.source = editor::EditorFontSource::kUser;
    asset.descriptor.is_default = false;
    asset.descriptor.is_protected = false;
    asset.descriptor.is_user_font = true;
    asset.font_data = bytes.value;
    return {foundation::Result::success(), std::move(asset)};
}

foundation::ResultValue<editor::EditorFontCatalog> EditorFontRepository::scan() const {
    if (!initialized_)
        return {foundation::invalid_state("user font repository is not initialized"), {}};
    editor::EditorFontCatalog catalog;
    std::error_code error;
    std::vector<std::filesystem::path> files;
    for (std::filesystem::directory_iterator it(root_directory_, error), end; !error && it != end;
         it.increment(error)) {
        if (it->is_regular_file(error) && accepted_extension(it->path()))
            files.push_back(it->path());
    }
    if (error)
        return {foundation::Result::failure(foundation::ResultCode::kBackendError, "user font repository scan failed"),
                {}};
    std::sort(files.begin(), files.end());
    for (const auto& path : files) {
        const auto asset = inspect_file(path);
        if (!asset)
            continue;  // Invalid user files are ignored during startup; import reports the error.
        if (const auto result =
                catalog.register_font(asset.value.descriptor, asset.value.font_data, asset.value.face_index);
            !result)
            return {result, {}};
    }
    return {foundation::Result::success(), std::move(catalog)};
}

foundation::ResultValue<std::vector<EditorFontFileRecord>> EditorFontRepository::list_records() const {
    if (!initialized_)
        return {foundation::invalid_state("user font repository is not initialized"), {}};
    const auto catalog = scan();
    if (!catalog)
        return {catalog.result, {}};
    std::vector<EditorFontFileRecord> records;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(root_directory_, error)) {
        if (error)
            break;
        if (!entry.is_regular_file(error) || !accepted_extension(entry.path()))
            continue;
        const auto asset = inspect_file(entry.path());
        if (!asset)
            continue;
        if (catalog.value.find(asset.value.descriptor.key) == nullptr)
            continue;
        records.push_back({asset.value.descriptor.key, entry.path().filename().string()});
    }
    if (error)
        return {foundation::Result::failure(foundation::ResultCode::kBackendError, "user font record scan failed"), {}};
    std::sort(records.begin(), records.end(), [](const auto& left, const auto& right) { return left.key < right.key; });
    return {foundation::Result::success(), std::move(records)};
}

foundation::Result EditorFontRepository::ensure_transaction_directory() {
    if (!initialized_)
        return foundation::invalid_state("user font repository is not initialized");
    if (!transaction_directory_.empty())
        return foundation::Result::success();
    const auto id = g_transaction_id.fetch_add(1, std::memory_order_relaxed);
    transaction_directory_ = root_directory_ / ".transactions" / ("tx_" + std::to_string(id));
    std::error_code error;
    if (!std::filesystem::create_directories(transaction_directory_, error) && error) {
        transaction_directory_.clear();
        return foundation::Result::failure(foundation::ResultCode::kBackendError,
                                           "user font transaction directory could not be created");
    }
    return foundation::Result::success();
}

foundation::ResultValue<editor::EditorFontAsset> EditorFontRepository::stage_import(
    const std::filesystem::path& source_path) {
    if (!initialized_)
        return {foundation::invalid_state("user font repository is not initialized"), {}};
    if (source_path.empty() || !source_path.is_absolute() || !accepted_extension(source_path))
        return {foundation::invalid_argument("user font source path or extension is invalid"), {}};
    std::error_code error;
    if (!std::filesystem::is_regular_file(source_path, error) || error)
        return {foundation::not_found("user font source file is missing"), {}};
    const auto inspected = inspect_file(source_path);
    if (!inspected)
        return inspected;
    const auto scanned = scan();
    if (!scanned)
        return {scanned.result, {}};
    if (scanned.value.find(inspected.value.descriptor.key) != nullptr ||
        std::any_of(additions_.begin(), additions_.end(),
                    [&](const auto& item) { return item.key == inspected.value.descriptor.key; }))
        return {foundation::invalid_argument("user font content is already imported"), {}};
    if (const auto result = ensure_transaction_directory(); !result)
        return {result, {}};

    const std::string extension = [&] {
        std::string value = source_path.extension().string();
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        return value;
    }();
    const std::string relative_name = inspected.value.descriptor.key + extension;
    const auto staged_path = transaction_directory_ / relative_name;
    std::ofstream output(staged_path, std::ios::binary | std::ios::trunc);
    if (!output)
        return {foundation::Result::failure(foundation::ResultCode::kBackendError,
                                            "user font transaction file could not be created"),
                {}};
    output.write(reinterpret_cast<const char*>(inspected.value.font_data.data()),
                 static_cast<std::streamsize>(inspected.value.font_data.size()));
    if (!output)
        return {foundation::Result::failure(foundation::ResultCode::kBackendError,
                                            "user font transaction file could not be written"),
                {}};
    additions_.push_back({inspected.value.descriptor.key, relative_name, staged_path});
    return inspected;
}

foundation::Result EditorFontRepository::stage_remove(std::string_view font_key) {
    if (!initialized_)
        return foundation::invalid_state("user font repository is not initialized");
    if (!font_key.starts_with("user_") || font_key.size() != 21)
        return foundation::invalid_argument("only user font keys can be removed");
    if (std::find(removals_.begin(), removals_.end(), font_key) != removals_.end())
        return foundation::Result::success();
    const auto scanned = scan();
    if (!scanned)
        return scanned.result;
    if (scanned.value.find(font_key) == nullptr &&
        std::none_of(additions_.begin(), additions_.end(), [&](const auto& item) { return item.key == font_key; }))
        return foundation::not_found("user font key is not registered");
    removals_.emplace_back(font_key);
    return foundation::Result::success();
}

foundation::Result EditorFontRepository::commit() {
    if (!initialized_)
        return foundation::invalid_state("user font repository is not initialized");
    std::error_code error;
    for (const auto& addition : additions_) {
        const auto destination = root_directory_ / addition.relative_file_name;
        const auto temporary = destination.string() + ".tmp";
        std::filesystem::copy_file(addition.staged_path, temporary, std::filesystem::copy_options::overwrite_existing,
                                   error);
        if (error)
            return foundation::Result::failure(foundation::ResultCode::kBackendError, "user font commit copy failed");
        std::filesystem::rename(temporary, destination, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            return foundation::Result::failure(foundation::ResultCode::kBackendError,
                                               "user font commit replace failed");
        }
    }
    if (!removals_.empty()) {
        for (std::filesystem::directory_iterator it(root_directory_, error), end; !error && it != end;
             it.increment(error)) {
            if (!it->is_regular_file(error) || !accepted_extension(it->path()))
                continue;
            const auto asset = inspect_file(it->path());
            if (!asset || std::find(removals_.begin(), removals_.end(), asset.value.descriptor.key) == removals_.end())
                continue;
            std::filesystem::remove(it->path(), error);
            if (error)
                return foundation::Result::failure(foundation::ResultCode::kBackendError, "user font removal failed");
        }
    }
    if (error)
        return foundation::Result::failure(foundation::ResultCode::kBackendError, "user font commit scan failed");
    rollback();
    return foundation::Result::success();
}

void EditorFontRepository::rollback() noexcept {
    std::error_code error;
    if (!transaction_directory_.empty())
        std::filesystem::remove_all(transaction_directory_, error);
    additions_.clear();
    removals_.clear();
    transaction_directory_.clear();
}

}  // namespace aeg::application
