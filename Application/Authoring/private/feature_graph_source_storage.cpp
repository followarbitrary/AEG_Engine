#include "feature_graph_source_storage.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace aeg::application::authoring {
namespace {
constexpr std::size_t kMaxSourceBytes = 16u * 1024u * 1024u;
constexpr std::string_view kIntegrityPrefix = "# AEG FeatureGraph integrity fnv64-v1 ";

uint64_t source_checksum(std::string_view bytes) {
    uint64_t value = 14695981039346656037ull;
    for (const unsigned char byte : bytes) value = (value ^ byte) * 1099511628211ull;
    return value;
}

std::string checksum_text(std::string_view bytes) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << source_checksum(bytes);
    return output.str();
}

class SourceWriterLock final {
public:
    explicit SourceWriterLock(const std::filesystem::path& source) {
        auto path = source;
        path += ".lock";
#if defined(_WIN32)
        handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
#else
        handle_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0600);
        if (handle_ >= 0 && ::flock(handle_, LOCK_EX | LOCK_NB) != 0) {
            ::close(handle_);
            handle_ = -1;
        }
#endif
    }
    ~SourceWriterLock() {
#if defined(_WIN32)
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
        if (handle_ >= 0) ::close(handle_);
#endif
    }
    [[nodiscard]] bool acquired() const {
#if defined(_WIN32)
        return handle_ != INVALID_HANDLE_VALUE;
#else
        return handle_ >= 0;
#endif
    }
private:
#if defined(_WIN32)
    HANDLE handle_{INVALID_HANDLE_VALUE};
#else
    int handle_{-1};
#endif
};

foundation::Result write_complete(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) return foundation::Result::failure(foundation::ResultCode::kBackendError,
        "FeatureGraph source could not be written: " + path.string());
#if defined(_WIN32)
    const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return foundation::invalid_state("FeatureGraph source cannot be opened for durable flush");
    const bool flushed = FlushFileBuffers(handle) != 0;
    CloseHandle(handle);
#else
    const int handle = ::open(path.c_str(), O_RDONLY);
    const bool flushed = handle >= 0 && ::fsync(handle) == 0;
    if (handle >= 0) ::close(handle);
#endif
    if (!flushed) return foundation::Result::failure(foundation::ResultCode::kBackendError,
        "FeatureGraph source durable flush failed");
    return foundation::Result::success();
}

foundation::Result replace_complete(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code error;
#if defined(_WIN32)
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
#else
    std::filesystem::rename(from, to, error);
#endif
    if (error) return foundation::Result::failure(foundation::ResultCode::kBackendError,
        "FeatureGraph source replace failed: " + error.message());
    return foundation::Result::success();
}

struct TemporarySource final {
    std::filesystem::path path{};
    ~TemporarySource() { std::error_code ignored; std::filesystem::remove(path, ignored); }
};
} // namespace

foundation::ResultValue<FeatureGraphFileRevision> inspect_feature_graph_file_revision(
    const std::filesystem::path& file) {
    std::error_code error;
    const bool exists = std::filesystem::exists(file, error);
    if (error) return {foundation::invalid_state("FeatureGraph source cannot be inspected: " + error.message()), {}};
    if (!exists) return {foundation::Result::success(), {}};
    std::ifstream input(file, std::ios::binary | std::ios::ate);
    if (!input) return {foundation::invalid_state("FeatureGraph source cannot be read"), {}};
    const auto size = input.tellg();
    if (size < 0 || static_cast<std::uintmax_t>(size) > kMaxSourceBytes)
        return {foundation::invalid_argument("FeatureGraph source exceeds its size limit"), {}};
    FeatureGraphFileRevision revision{true, std::string(static_cast<std::size_t>(size), '\0')};
    input.seekg(0);
    if (!revision.contents.empty()) input.read(revision.contents.data(), static_cast<std::streamsize>(size));
    if (!input) return {foundation::invalid_state("FeatureGraph source read was incomplete"), {}};
    return {foundation::Result::success(), std::move(revision)};
}

namespace detail {
foundation::Result validate_source_integrity(std::string_view bytes) {
    if (!bytes.starts_with(kIntegrityPrefix)) return foundation::Result::success(); // Existing plain TOML sources.
    const auto newline = bytes.find('\n');
    if (newline == std::string_view::npos || newline != kIntegrityPrefix.size() + 16 ||
        bytes.substr(kIntegrityPrefix.size(), 16) != checksum_text(bytes.substr(newline + 1)))
        return foundation::invalid_argument("FeatureGraph source integrity mismatch; keep the draft and recover explicitly");
    return foundation::Result::success();
}

std::string protect_source_integrity(std::string_view bytes) {
    return std::string(kIntegrityPrefix) + checksum_text(bytes) + "\n" + std::string(bytes);
}

foundation::Result publish_feature_graph_source(const std::filesystem::path& file, std::string_view bytes,
    const std::optional<FeatureGraphFileRevision>& expected,
    const std::function<foundation::Result(const std::filesystem::path&)>& validate, bool preserve_previous) {
    std::error_code error;
    const auto target = std::filesystem::weakly_canonical(file, error);
    if (error || target.empty()) return foundation::invalid_argument("FeatureGraph source path cannot be resolved");
    SourceWriterLock lock(target);
    if (!lock.acquired()) return foundation::Result::failure(foundation::ResultCode::kNotReady,
        "FeatureGraph source is being saved by another writer");
    const auto before = inspect_feature_graph_file_revision(target);
    if (!before) return before.result;
    if (expected && before.value != *expected)
        return foundation::Result::failure(foundation::ResultCode::kOutOfDate,
            "FeatureGraph source changed outside this document; the local draft and disk version are both preserved");

    static std::atomic<uint64_t> sequence{0};
    const auto suffix = ".tmp-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                        "-" + std::to_string(sequence.fetch_add(1));
    auto candidate_path = target;
    candidate_path += suffix;
    TemporarySource candidate{std::move(candidate_path)};
    if (const auto written = write_complete(candidate.path, bytes); !written) return written;
    if (const auto valid = validate(candidate.path); !valid) return valid;

    if (preserve_previous && before.value.exists && validate(target)) {
        auto backup = target;
        backup += ".bak";
        auto backup_candidate_path = backup;
        backup_candidate_path += suffix;
        TemporarySource backup_candidate{std::move(backup_candidate_path)};
        if (const auto written = write_complete(backup_candidate.path, before.value.contents); !written) return written;
        if (const auto valid = validate(backup_candidate.path); !valid) return valid;
        if (const auto replaced = replace_complete(backup_candidate.path, backup); !replaced) return replaced;
    }
    if (!preserve_previous && before.value.exists) {
        // Explicit recovery retains the exact displaced source, including invalid bytes.
        auto archive = target;
        archive += ".recovery-original-" + checksum_text(before.value.contents);
        const auto existing_archive = inspect_feature_graph_file_revision(archive);
        if (!existing_archive) return existing_archive.result;
        if (existing_archive.value.exists) {
            if (existing_archive.value != before.value)
                return foundation::invalid_state("FeatureGraph recovery archive identity collision; source was preserved");
        } else {
            auto archive_candidate_path = archive; archive_candidate_path += suffix;
            TemporarySource archive_candidate{std::move(archive_candidate_path)};
            if (const auto written = write_complete(archive_candidate.path, before.value.contents); !written) return written;
            const auto captured = inspect_feature_graph_file_revision(archive_candidate.path);
            if (!captured) return captured.result;
            if (captured.value != before.value)
                return foundation::invalid_state("FeatureGraph recovery archive did not retain the displaced source");
            if (const auto replaced = replace_complete(archive_candidate.path, archive); !replaced) return replaced;
        }
    }
    // Recheck bytes immediately before publication, including noncooperating external editors.
    const auto current = inspect_feature_graph_file_revision(target);
    if (!current) return current.result;
    if (current.value != before.value)
        return foundation::Result::failure(foundation::ResultCode::kOutOfDate,
            "FeatureGraph source changed during save; source publication was cancelled");
    return replace_complete(candidate.path, target);
}
} // namespace detail
} // namespace aeg::application::authoring
