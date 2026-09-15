/**
 * @file asset_package_smoke.cpp
 * @brief Verifies deterministic asset package indexing and corruption checks.
 */
#include <filesystem>
#include <iostream>

#include "asset_package.hpp"

namespace {
bool require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}
}  // namespace

int main() {
    namespace fs = std::filesystem;
    using aeg::tooling::AssetId;
    using aeg::tooling::AssetKind;
    using aeg::tooling::AssetPackageEntry;
    using aeg::tooling::AssetPackageReader;
    using aeg::tooling::AssetPackageWriter;

    const auto mesh_id = AssetId::parse("1a0729bd-c3ae-4fb8-8d5d-58e26dbab2af");
    const auto texture_id = AssetId::parse("95513e6e-7556-4741-a633-d3f450c5d1f5");
    if (!require(static_cast<bool>(mesh_id) && static_cast<bool>(texture_id),
                 "asset package smoke cannot parse fixture UUIDs")) {
        return 1;
    }

    const fs::path file = fs::temp_directory_path() / "aeg_asset_package_smoke.aegbin";
    std::error_code error;
    fs::remove(file, error);

    const std::vector<AssetPackageEntry> entries = {
        {texture_id.value, AssetKind::kTexture, 3, {std::byte{0xA1}, std::byte{0xB2}}},
        {mesh_id.value, AssetKind::kMesh, 7, {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}}},
    };
    AssetPackageWriter writer;
    if (!require(static_cast<bool>(writer.write(file, entries)), "asset package smoke cannot write package")) {
        fs::remove(file, error);
        return 2;
    }

    const auto reader = AssetPackageReader::open(file);
    if (!require(static_cast<bool>(reader), "asset package smoke cannot open package") ||
        !require(reader.value.entry_count() == entries.size(), "asset package smoke index count is invalid")) {
        fs::remove(file, error);
        return 3;
    }

    const auto loaded_mesh = reader.value.read(mesh_id.value);
    if (!require(static_cast<bool>(loaded_mesh), "asset package smoke cannot resolve mesh UUID") ||
        !require(loaded_mesh.value.kind == AssetKind::kMesh && loaded_mesh.value.schema_version == 7,
                 "asset package smoke entry metadata is invalid") ||
        !require(loaded_mesh.value.bytes == entries[1].bytes, "asset package smoke payload differs")) {
        fs::remove(file, error);
        return 4;
    }

    const auto missing_id = AssetId::parse("3de70f70-b1aa-4a77-9561-a154dd4cbb5e");
    if (!require(!reader.value.read(missing_id.value), "asset package smoke resolved a missing UUID")) {
        fs::remove(file, error);
        return 5;
    }

    std::vector<AssetPackageEntry> duplicate = entries;
    duplicate.push_back(entries.front());
    if (!require(!writer.write(file, duplicate), "asset package smoke accepted duplicate UUIDs")) {
        fs::remove(file, error);
        return 6;
    }

    const std::vector<AssetPackageEntry> replacement = {
        {mesh_id.value, AssetKind::kMesh, 8, {std::byte{0x7F}}},
    };
    if (!require(static_cast<bool>(writer.write(file, replacement)),
                 "asset package smoke cannot atomically replace package") ||
        !require(AssetPackageReader::open(file).value.read(mesh_id.value).value.schema_version == 8,
                 "asset package smoke did not select replacement package")) {
        fs::remove(file, error);
        return 7;
    }

    fs::resize_file(file, 12, error);
    if (!require(!error && !AssetPackageReader::open(file), "asset package smoke accepted a truncated package")) {
        fs::remove(file, error);
        return 8;
    }

    fs::remove(file, error);
    return 0;
}
