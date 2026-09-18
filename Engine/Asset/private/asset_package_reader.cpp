/** @file asset_package_reader.cpp */
#include "asset_package_reader.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <span>
#include <utility>

namespace aeg::asset {
namespace {
constexpr std::array<std::byte, 8> kMagic{std::byte{'A'}, std::byte{'E'}, std::byte{'G'}, std::byte{'P'},
                                          std::byte{'A'}, std::byte{'C'}, std::byte{'K'}, std::byte{0}};
constexpr uint32_t kVersion = 1;
constexpr uint32_t kHeaderBytes = 64;
constexpr uint32_t kEntryBytes = 48;
constexpr uint32_t kAlignment = 16;
constexpr uint64_t kMaxEntries = 1'000'000;

template <typename T>
[[nodiscard]] bool read_le(std::span<const std::byte> input, std::size_t offset, T& output) noexcept {
    if (offset > input.size() || sizeof(T) > input.size() - offset)
        return false;
    uint64_t value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<uint64_t>(std::to_integer<uint8_t>(input[offset + i])) << (i * 8U);
    }
    output = static_cast<T>(value);
    return true;
}
[[nodiscard]] bool valid_range(uint64_t offset, uint64_t size, uint64_t total) noexcept {
    return offset <= total && size <= total - offset;
}
[[nodiscard]] foundation::ResultValue<std::vector<std::byte>> read_file(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary | std::ios::ate);
    if (!stream)
        return {foundation::not_found("Asset package file is missing"), {}};
    const auto end = stream.tellg();
    if (end < 0)
        return {foundation::invalid_argument("Asset package size is invalid"), {}};
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()), end);
    if (!stream)
        return {foundation::invalid_argument("Asset package cannot be read"), {}};
    return {foundation::Result::success(), std::move(bytes)};
}
}  // namespace

foundation::ResultValue<AssetPackageReader> AssetPackageReader::open(const std::filesystem::path& file) {
    const auto loaded = read_file(file);
    if (!loaded)
        return {loaded.result, {}};
    if (loaded.value.size() < kHeaderBytes || !std::equal(kMagic.begin(), kMagic.end(), loaded.value.begin())) {
        return {foundation::invalid_argument("Asset package header is invalid"), {}};
    }
    const std::span<const std::byte> input(loaded.value);
    uint32_t version = 0, header_bytes = 0, entry_bytes = 0;
    uint64_t file_bytes = 0, index_offset = 0, index_bytes = 0, entry_count = 0;
    if (!read_le(input, 8, version) || !read_le(input, 12, header_bytes) || !read_le(input, 16, file_bytes) ||
        !read_le(input, 24, index_offset) || !read_le(input, 32, index_bytes) || !read_le(input, 40, entry_count) ||
        !read_le(input, 48, entry_bytes) || version != kVersion || header_bytes != kHeaderBytes ||
        entry_bytes != kEntryBytes || file_bytes != input.size() || entry_count > kMaxEntries ||
        entry_count > std::numeric_limits<uint64_t>::max() / kEntryBytes || index_bytes != entry_count * kEntryBytes ||
        !valid_range(index_offset, index_bytes, file_bytes)) {
        return {foundation::invalid_argument("Asset package index metadata is invalid"), {}};
    }

    AssetPackageReader reader{};
    reader.bytes_ = loaded.value;
    reader.index_.reserve(static_cast<std::size_t>(entry_count));
    for (uint64_t i = 0; i < entry_count; ++i) {
        const std::size_t offset = static_cast<std::size_t>(index_offset + i * kEntryBytes);
        IndexEntry entry{};
        for (std::size_t byte = 0; byte < entry.id.value.bytes.size(); ++byte) {
            entry.id.value.bytes[byte] = std::to_integer<uint8_t>(input[offset + byte]);
        }
        uint32_t kind = 0, alignment = 0;
        if (!read_le(input, offset + 16, kind) || !read_le(input, offset + 20, entry.schema_version) ||
            !read_le(input, offset + 24, entry.offset) || !read_le(input, offset + 32, entry.byte_count) ||
            !read_le(input, offset + 40, alignment) || kind > static_cast<uint32_t>(AssetKind::kShader) ||
            alignment != kAlignment || entry.offset % kAlignment != 0 ||
            !valid_range(entry.offset, entry.byte_count, index_offset)) {
            return {foundation::invalid_argument("Asset package entry is invalid"), {}};
        }
        entry.kind = static_cast<AssetKind>(kind);
        reader.index_.push_back(entry);
    }
    if (!std::is_sorted(reader.index_.begin(), reader.index_.end(),
                        [](const IndexEntry& a, const IndexEntry& b) { return a.id < b.id; }) ||
        std::adjacent_find(reader.index_.begin(), reader.index_.end(), [](const IndexEntry& a, const IndexEntry& b) {
            return a.id == b.id;
        }) != reader.index_.end()) {
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
    const auto offset = static_cast<std::size_t>(found->offset);
    const auto size = static_cast<std::size_t>(found->byte_count);
    return {foundation::Result::success(),
            {found->id, found->kind, found->schema_version, {bytes_.begin() + offset, bytes_.begin() + offset + size}}};
}

foundation::ResultValue<CookedAsset> AssetPackageReader::read_cooked(AssetId id) const {
    const auto packaged = read(id);
    if (!packaged)
        return {packaged.result, {}};
    const auto decoded = decode_cooked_asset(packaged.value.bytes);
    if (!decoded)
        return {decoded.result, {}};
    if (decoded.value.id != packaged.value.id || decoded.value.kind != packaged.value.kind ||
        decoded.value.schema_version != packaged.value.schema_version) {
        return {foundation::invalid_argument("Cooked asset envelope does not match package index"), {}};
    }
    return decoded;
}
}  // namespace aeg::asset
