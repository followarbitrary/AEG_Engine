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

}  // namespace aeg::tooling
