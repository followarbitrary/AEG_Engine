#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

#include "asset_handle_table.hpp"
#include "asset_package_reader.hpp"
#include "cooked_asset_generated.h"

namespace {
using aeg::foundation::AssetId;
using aeg::foundation::AssetKind;

AssetId id(std::string_view text) {
    const auto parsed = AssetId::parse(text);
    assert(parsed);
    return parsed.value;
}

std::vector<std::byte> cooked(AssetId value, AssetKind kind, std::vector<std::byte> payload) {
    flatbuffers::FlatBufferBuilder builder;
    const auto uuid = builder.CreateVector(value.value.bytes.data(), value.value.bytes.size());
    const auto bytes = builder.CreateVector(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    const auto root =
        aeg::asset::fb::CreateCookedAsset(builder, uuid, static_cast<aeg::asset::fb::AssetKind>(kind), 9, 0, bytes);
    aeg::asset::fb::FinishCookedAssetBuffer(builder, root);
    return {reinterpret_cast<const std::byte*>(builder.GetBufferPointer()),
            reinterpret_cast<const std::byte*>(builder.GetBufferPointer()) + builder.GetSize()};
}

template <typename T>
void le(std::vector<std::byte>& bytes, std::size_t offset, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        bytes[offset + i] = std::byte((static_cast<uint64_t>(value) >> (i * 8U)) & 0xffU);
    }
}

std::vector<std::byte> package(AssetId value, AssetKind kind, std::vector<std::byte> payload) {
    constexpr std::size_t header = 64;
    constexpr std::size_t index = 48;
    std::vector<std::byte> bytes(header, std::byte{0});
    bytes.resize(header);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    const uint64_t index_offset = bytes.size();
    bytes.resize(bytes.size() + index, std::byte{0});
    const std::array<unsigned char, 16> uuid = value.value.bytes;
    for (std::size_t i = 0; i < uuid.size(); ++i)
        bytes[index_offset + i] = std::byte{uuid[i]};
    le<uint32_t>(bytes, index_offset + 16, static_cast<uint32_t>(kind));
    le<uint32_t>(bytes, index_offset + 20, 9);
    le<uint64_t>(bytes, index_offset + 24, 64);
    le<uint64_t>(bytes, index_offset + 32, payload.size());
    le<uint32_t>(bytes, index_offset + 40, 16);
    const std::array<std::byte, 8> magic{std::byte{'A'}, std::byte{'E'}, std::byte{'G'}, std::byte{'P'},
                                         std::byte{'A'}, std::byte{'C'}, std::byte{'K'}, std::byte{0}};
    std::copy(magic.begin(), magic.end(), bytes.begin());
    le<uint32_t>(bytes, 8, 1U);
    le<uint32_t>(bytes, 12, 64U);
    le<uint64_t>(bytes, 16, bytes.size());
    le<uint64_t>(bytes, 24, index_offset);
    le<uint64_t>(bytes, 32, index);
    le<uint64_t>(bytes, 40, 1U);
    le<uint32_t>(bytes, 48, 48U);
    return bytes;
}
}  // namespace

int main() {
    const AssetId mesh = id("11111111-2222-3333-4444-555555555555");
    const AssetId texture = id("11111111-2222-3333-4444-666666666666");
    aeg::asset::AssetHandleTable handles;
    const auto hm = handles.intern(mesh, AssetKind::kMesh);
    const auto ht = handles.intern(texture, AssetKind::kMesh);
    if (!hm || !ht || hm.value == 0 || hm.value == ht.value || handles.resolve(hm.value, AssetKind::kTexture) ||
        !handles.resolve(ht.value, AssetKind::kMesh) || handles.resolve(ht.value, AssetKind::kMesh).value != texture ||
        !handles.find(mesh, AssetKind::kMesh)) {
        return 1;
    }

    const auto cooked_bytes = cooked(mesh, AssetKind::kFeatureGraph, {std::byte{0xA1}, std::byte{0xB2}});
    const auto package_bytes = package(mesh, AssetKind::kFeatureGraph, cooked_bytes);
    const auto root = std::filesystem::temp_directory_path() / "aeg_runtime_asset_package_smoke.aegbin";
    {
        std::ofstream stream(root, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(package_bytes.data()),
                     static_cast<std::streamsize>(package_bytes.size()));
    }
    const auto reader = aeg::asset::AssetPackageReader::open(root);
    std::filesystem::remove(root);
    if (!reader || reader.value.entry_count() != 1)
        return 2;
    const auto decoded = reader.value.read_cooked(mesh);
    if (!decoded || decoded.value.id != mesh || decoded.value.kind != AssetKind::kFeatureGraph ||
        decoded.value.payload.size() != 2 || decoded.value.payload[0] != std::byte{0xA1}) {
        return 3;
    }
    if (reader.value.read_cooked(texture))
        return 4;
    std::puts("Runtime asset package smoke passed");
    return 0;
}
