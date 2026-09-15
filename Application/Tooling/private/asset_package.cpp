/**
 * @file asset_package.cpp
 * @brief Implements checked little-endian asset package encoding.
 */
#include "asset_package.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace aeg::tooling {
namespace {
constexpr std::array<std::byte, 8> kPackageMagic{
    std::byte{static_cast<unsigned char>('A')}, std::byte{static_cast<unsigned char>('E')},
    std::byte{static_cast<unsigned char>('G')}, std::byte{static_cast<unsigned char>('P')},
    std::byte{static_cast<unsigned char>('A')}, std::byte{static_cast<unsigned char>('C')},
    std::byte{static_cast<unsigned char>('K')}, std::byte{0},
};
constexpr uint32_t kPackageVersion = 1;
constexpr uint32_t kHeaderBytes = 64;
constexpr uint32_t kIndexEntryBytes = 48;
constexpr uint32_t kPayloadAlignment = 16;
constexpr uint64_t kMaxEntryCount = 1'000'000;

struct EncodedIndexEntry final {
    AssetId id{};
    AssetKind kind{AssetKind::kMesh};
    uint32_t schema_version{};
    uint64_t offset{};
    uint64_t byte_count{};
};

[[nodiscard]] bool is_known_kind(uint32_t value) noexcept {
    return value <= static_cast<uint32_t>(AssetKind::kFont);
}

[[nodiscard]] bool is_valid_range(uint64_t offset, uint64_t size, uint64_t total) noexcept {
    return offset <= total && size <= total - offset;
}

[[nodiscard]] uint64_t align_up(uint64_t value, uint32_t alignment) noexcept {
    const uint64_t mask = static_cast<uint64_t>(alignment - 1U);
    return (value + mask) & ~mask;
}

template <typename Value>
void write_le(std::vector<std::byte>& output, std::size_t offset, Value value) {
    for (std::size_t index = 0; index < sizeof(Value); ++index) {
        output[offset + index] = std::byte((static_cast<uint64_t>(value) >> (index * 8U)) & 0xffU);
    }
}

template <typename Value>
[[nodiscard]] bool read_le(std::span<const std::byte> input, std::size_t offset, Value& output) noexcept {
    if (offset > input.size() || sizeof(Value) > input.size() - offset) {
        return false;
    }
    uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(Value); ++index) {
        value |= static_cast<uint64_t>(std::to_integer<uint8_t>(input[offset + index])) << (index * 8U);
    }
    output = static_cast<Value>(value);
    return true;
}

[[nodiscard]] foundation::Result write_file(const std::filesystem::path& file, std::span<const std::byte> bytes) {
    const std::filesystem::path temporary = file.string() + ".tmp";
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot open asset package for writing");
    }
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream) {
        std::error_code error;
        std::filesystem::remove(temporary, error);
        return foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot write asset package");
    }
#if defined(_WIN32)
    if (::MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        std::error_code error;
        std::filesystem::remove(temporary, error);
        return foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot replace asset package");
    }
#else
    std::error_code error;
    std::filesystem::rename(temporary, file, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot replace asset package");
    }
#endif
    return foundation::Result::success();
}

[[nodiscard]] foundation::ResultValue<std::vector<std::byte>> read_file(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary | std::ios::ate);
    if (!stream) {
        return {foundation::not_found("Asset package file is missing"), {}};
    }
    const std::streamsize size = stream.tellg();
    if (size < 0) {
        return {foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot inspect asset package size"), {}};
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()), size);
    return stream ? foundation::ResultValue<std::vector<std::byte>>{foundation::Result::success(), std::move(bytes)}
                  : foundation::ResultValue<std::vector<std::byte>>{
                        foundation::Result::failure(foundation::ResultCode::kUnknown, "Cannot read asset package"), {}};
}
}  // namespace

foundation::Result AssetPackageWriter::write(const std::filesystem::path& file,
                                             std::vector<AssetPackageEntry> entries) const {
    if (file.empty() || file.filename().empty() || entries.size() > kMaxEntryCount) {
        return foundation::invalid_argument("Asset package output path or entry count is invalid");
    }
    std::sort(entries.begin(), entries.end(),
              [](const AssetPackageEntry& left, const AssetPackageEntry& right) { return left.id < right.id; });
    if (std::adjacent_find(entries.begin(), entries.end(),
                           [](const AssetPackageEntry& left, const AssetPackageEntry& right) {
                               return left.id == right.id;
                           }) != entries.end()) {
        return foundation::invalid_argument("Asset package contains duplicate asset UUIDs");
    }

    std::vector<std::byte> bytes(kHeaderBytes, std::byte{0});
    std::vector<EncodedIndexEntry> index;
    index.reserve(entries.size());
    for (const AssetPackageEntry& entry : entries) {
        const uint64_t aligned_offset = align_up(bytes.size(), kPayloadAlignment);
        if (aligned_offset > std::numeric_limits<std::size_t>::max() ||
            entry.bytes.size() > std::numeric_limits<uint64_t>::max() - aligned_offset) {
            return foundation::out_of_memory("Asset package payload size exceeds supported limits");
        }
        bytes.resize(static_cast<std::size_t>(aligned_offset), std::byte{0});
        bytes.insert(bytes.end(), entry.bytes.begin(), entry.bytes.end());
        index.push_back({entry.id, entry.kind, entry.schema_version, aligned_offset, entry.bytes.size()});
    }

    const uint64_t index_offset = bytes.size();
    const uint64_t index_bytes = static_cast<uint64_t>(index.size()) * kIndexEntryBytes;
    if (index.size() > std::numeric_limits<std::size_t>::max() / kIndexEntryBytes ||
        index_bytes > std::numeric_limits<std::size_t>::max() - bytes.size()) {
        return foundation::out_of_memory("Asset package index exceeds supported limits");
    }
    bytes.resize(bytes.size() + static_cast<std::size_t>(index_bytes), std::byte{0});
    for (std::size_t index_number = 0; index_number < index.size(); ++index_number) {
        const EncodedIndexEntry& entry = index[index_number];
        const std::size_t offset = static_cast<std::size_t>(index_offset) + index_number * kIndexEntryBytes;
        for (std::size_t byte_index = 0; byte_index < entry.id.value.bytes.size(); ++byte_index) {
            bytes[offset + byte_index] = std::byte{entry.id.value.bytes[byte_index]};
        }
        write_le<uint32_t>(bytes, offset + 16, static_cast<uint32_t>(entry.kind));
        write_le<uint32_t>(bytes, offset + 20, entry.schema_version);
        write_le<uint64_t>(bytes, offset + 24, entry.offset);
        write_le<uint64_t>(bytes, offset + 32, entry.byte_count);
        write_le<uint32_t>(bytes, offset + 40, kPayloadAlignment);
    }

    std::copy(kPackageMagic.begin(), kPackageMagic.end(), bytes.begin());
    write_le<uint32_t>(bytes, 8, kPackageVersion);
    write_le<uint32_t>(bytes, 12, kHeaderBytes);
    write_le<uint64_t>(bytes, 16, bytes.size());
    write_le<uint64_t>(bytes, 24, index_offset);
    write_le<uint64_t>(bytes, 32, index_bytes);
    write_le<uint64_t>(bytes, 40, index.size());
    write_le<uint32_t>(bytes, 48, kIndexEntryBytes);
    return write_file(file, bytes);
}

foundation::ResultValue<AssetPackageReader> AssetPackageReader::open(const std::filesystem::path& file) {
    const auto bytes = read_file(file);
    if (!bytes) {
        return {bytes.result, {}};
    }
    if (bytes.value.size() < kHeaderBytes ||
        !std::equal(kPackageMagic.begin(), kPackageMagic.end(), bytes.value.begin())) {
        return {foundation::invalid_argument("Asset package header is invalid"), {}};
    }

    const std::span<const std::byte> input(bytes.value);
    uint32_t version = 0;
    uint32_t header_bytes = 0;
    uint64_t file_bytes = 0;
    uint64_t index_offset = 0;
    uint64_t index_bytes = 0;
    uint64_t entry_count = 0;
    uint32_t entry_bytes = 0;
    if (!read_le(input, 8, version) || !read_le(input, 12, header_bytes) || !read_le(input, 16, file_bytes) ||
        !read_le(input, 24, index_offset) || !read_le(input, 32, index_bytes) || !read_le(input, 40, entry_count) ||
        !read_le(input, 48, entry_bytes) || version != kPackageVersion || header_bytes != kHeaderBytes ||
        entry_bytes != kIndexEntryBytes || file_bytes != input.size() || entry_count > kMaxEntryCount ||
        index_bytes != entry_count * kIndexEntryBytes || !is_valid_range(index_offset, index_bytes, file_bytes)) {
        return {foundation::invalid_argument("Asset package index metadata is invalid"), {}};
    }

    AssetPackageReader reader{};
    reader.bytes_ = bytes.value;
    reader.index_.reserve(static_cast<std::size_t>(entry_count));
    for (uint64_t index_number = 0; index_number < entry_count; ++index_number) {
        const std::size_t offset = static_cast<std::size_t>(index_offset + index_number * kIndexEntryBytes);
        IndexEntry entry{};
        for (std::size_t byte_index = 0; byte_index < entry.id.value.bytes.size(); ++byte_index) {
            entry.id.value.bytes[byte_index] = std::to_integer<uint8_t>(input[offset + byte_index]);
        }
        uint32_t kind = 0;
        uint32_t alignment = 0;
        if (!read_le(input, offset + 16, kind) || !read_le(input, offset + 20, entry.schema_version) ||
            !read_le(input, offset + 24, entry.offset) || !read_le(input, offset + 32, entry.byte_count) ||
            !read_le(input, offset + 40, alignment) || !is_known_kind(kind) || alignment != kPayloadAlignment ||
            entry.offset % kPayloadAlignment != 0 || !is_valid_range(entry.offset, entry.byte_count, index_offset)) {
            return {foundation::invalid_argument("Asset package entry is invalid"), {}};
        }
        entry.kind = static_cast<AssetKind>(kind);
        reader.index_.push_back(entry);
    }
    if (!std::is_sorted(reader.index_.begin(), reader.index_.end(),
                        [](const IndexEntry& left, const IndexEntry& right) { return left.id < right.id; }) ||
        std::adjacent_find(reader.index_.begin(), reader.index_.end(),
                           [](const IndexEntry& left, const IndexEntry& right) { return left.id == right.id; }) !=
            reader.index_.end()) {
        return {foundation::invalid_argument("Asset package index IDs are not unique and sorted"), {}};
    }
    return {foundation::Result::success(), std::move(reader)};
}

std::size_t AssetPackageReader::entry_count() const noexcept {
    return index_.size();
}

foundation::ResultValue<PackagedAsset> AssetPackageReader::read(AssetId id) const {
    const auto found = std::lower_bound(index_.begin(), index_.end(), id,
                                        [](const IndexEntry& entry, AssetId needle) { return entry.id < needle; });
    if (found == index_.end() || found->id != id) {
        return {foundation::not_found("Asset UUID is not present in package"), {}};
    }
    const std::size_t offset = static_cast<std::size_t>(found->offset);
    const std::size_t size = static_cast<std::size_t>(found->byte_count);
    return {foundation::Result::success(),
            {found->kind, found->schema_version, {bytes_.begin() + offset, bytes_.begin() + offset + size}}};
}
}  // namespace aeg::tooling
