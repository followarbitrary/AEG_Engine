/** @brief Verifies malformed caches and failed source imports remain recoverable. */
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#include "editor_asset_pipeline.hpp"

namespace {
using aeg::application::EditorAssetKind;
using aeg::application::EditorAssetRecord;
using aeg::application::EditorAssetStatus;
using aeg::application::asset_pipeline::CpuAsset;
using aeg::application::asset_pipeline::EditorAssetPipeline;

uint64_t hash(std::string_view text, EditorAssetKind kind) {
    uint64_t result = 14695981039346656037ull ^ static_cast<uint8_t>(kind);
    for (unsigned char value : text) {
        result = (result ^ value) * 1099511628211ull;
    }
    return result;
}

bool await_completion(EditorAssetPipeline& pipeline, bool success) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < deadline) {
        std::vector<CpuAsset> completed;
        const auto result = pipeline.poll(completed);
        if (!result && !success) {
            return true;  // The watcher can consume the one-shot allocation failure first.
        }
        for (const auto& asset : completed) {
            if (asset.asset_id == 1 && asset.success == success && (success || !asset.diagnostic.empty())) {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
}  // namespace

int main() {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("aeg_asset_recovery_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "Assets");
    EditorAssetPipeline pipeline;
    if (!pipeline.initialize(root)) {
        return 1;
    }
    // The products wrap the old unchecked addition to a 34-byte payload:
    // (2^32-1)^2 + 3*(2863311531) = 2^64+2.
    struct Header {
        std::array<char, 8> magic;
        uint32_t version;
        uint64_t id;
        uint64_t hash;
        uint64_t bytes;
    };
    struct Mesh {
        uint32_t stride;
        uint32_t vertices;
        uint32_t indices;
        uint32_t width;
        float center[3];
        float radius;
    };
    const Header header{{'A', 'E', 'G', 'M', 'E', 'S', 'H', 1}, 1, 2, 0, sizeof(Mesh) + 2};
    const Mesh mesh{0xFFFFFFFFu, 0xFFFFFFFFu, 2863311531u, 3, {}, 0};
    {
        std::ofstream file(root / "Assets" / "bad.aegmesh", std::ios::binary);
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        file.write(reinterpret_cast<const char*>(&mesh), sizeof(mesh));
        file.write("xx", 2);
    }
    EditorAssetRecord malformed{};
    malformed.id = 2;
    malformed.relative_path = "Assets/bad.aegmesh";
    malformed.kind = EditorAssetKind::kMesh;
    malformed.status = EditorAssetStatus::kFailed;  // Exercise the synchronous fast path before queuing work.
    if (!pipeline.synchronize(std::span{&malformed, 1})) {
        return 2;
    }
    CpuAsset output{};
    try {
        if (pipeline.load_fast_path(2, output) || output.success) {
            std::cerr << "Malformed baked mesh must fail without publishing a successful asset\n";
            return 3;
        }
    } catch (const std::exception& exception) {
        std::cerr << "Malformed baked mesh escaped Result: " << exception.what() << '\n';
        return 4;
    }
    const std::string triangle = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    std::ofstream(root / "Assets" / "triangle.obj") << triangle;
    EditorAssetRecord valid{};
    valid.id = 1;
    valid.relative_path = "Assets/triangle.obj";
    valid.kind = EditorAssetKind::kMesh;
    valid.status = EditorAssetStatus::kImported;
    valid.content_hash = hash(triangle, valid.kind);
    if (!pipeline.synchronize(std::span{&valid, 1}) || !await_completion(pipeline, true)) {
        return 5;
    }
    const std::string invalid_triangle = "f 1 2\n";
    std::ofstream(root / "Assets" / "triangle.obj", std::ios::trunc) << invalid_triangle;
    valid.content_hash = hash(invalid_triangle, valid.kind);
    if (!pipeline.synchronize(std::span{&valid, 1}) || !await_completion(pipeline, false)) {
        std::cerr << "Invalid source did not become a reported import failure\n";
        return 6;
    }
    std::ofstream(root / "Assets" / "triangle.obj", std::ios::trunc) << triangle;
    valid.content_hash = hash(triangle, valid.kind);
    if (!pipeline.synchronize(std::span{&valid, 1}) || !await_completion(pipeline, true)) {
        std::cerr << "Asset worker did not recover for the next valid job\n";
        return 7;
    }
    pipeline.shutdown();
    std::filesystem::remove_all(root);
    return 0;
}
