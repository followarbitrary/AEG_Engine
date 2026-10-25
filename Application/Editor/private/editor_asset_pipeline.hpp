/**
 * @file editor_asset_pipeline.hpp
 * @brief CPU import, fast-path loading, background baking, and file watching.
 */
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "concurrency.hpp"
#include "editor_asset_database.hpp"
#include "result.hpp"

namespace aeg::application::asset_pipeline {

struct CpuAsset final {
    uint64_t asset_id{0};
    uint64_t source_hash{0};
    EditorAssetKind kind{EditorAssetKind::kUnknown};
    bool success{false};
    bool from_baked_format{false};
    bool transient{false};
    std::string diagnostic{};

    // Mesh payload uses the renderer's interleaved position/uv/normal contract:
    // float4 position, float2 uv, float3 normal.
    std::vector<std::byte> vertex_data{};
    uint32_t vertex_stride{0};
    uint32_t vertex_count{0};
    std::vector<std::byte> index_data{};
    uint32_t index_count{0};
    bool indices_are_uint32{false};
    std::array<float, 3> bounds_center{0.0f, 0.0f, 0.0f};
    float bounds_radius{0.0f};

    uint32_t width{0};
    uint32_t height{0};
    std::vector<std::byte> pixels{};

    uint64_t texture_asset_id{0};
    std::array<float, 4> tint{1.0f, 1.0f, 1.0f, 1.0f};
    std::vector<std::byte> font_data{};
};

/** @brief Owns no GPU state; safe to run beside the render thread. */
class EditorAssetPipeline final {
public:
    EditorAssetPipeline() = default;
    ~EditorAssetPipeline();

    EditorAssetPipeline(const EditorAssetPipeline&) = delete;
    EditorAssetPipeline& operator=(const EditorAssetPipeline&) = delete;

    [[nodiscard]] foundation::Result initialize(const std::filesystem::path& project_root);
    [[nodiscard]] foundation::Result synchronize(std::span<const EditorAssetRecord> records);
    [[nodiscard]] foundation::Result request_reimport(uint64_t asset_id);
    [[nodiscard]] foundation::Result load_fast_path(uint64_t asset_id, CpuAsset& output) const;
    [[nodiscard]] foundation::Result poll(std::vector<CpuAsset>& completed);
    void shutdown() noexcept;

    [[nodiscard]] const std::filesystem::path& derived_root() const noexcept { return derived_root_; }

private:
    struct WorkItem final {
        EditorAssetRecord record{};
        bool force{false};
        uint64_t revision{0};
    };

    [[nodiscard]] foundation::Result queue_record(const EditorAssetRecord& record, bool force);
    void worker_loop(concurrency::CancellationToken cancellation);
    void watcher_loop(concurrency::CancellationToken cancellation);

    [[nodiscard]] static foundation::Result decode_record(const std::filesystem::path& project_root,
                                                          const std::filesystem::path& derived_root,
                                                          const EditorAssetRecord& record, CpuAsset& output,
                                                          bool allow_baked);
    [[nodiscard]] static foundation::Result write_baked(const std::filesystem::path& derived_root,
                                                        const CpuAsset& asset);

    std::filesystem::path project_root_{};
    std::filesystem::path derived_root_{};
    mutable std::mutex mutex_{};
    std::condition_variable condition_{};
    std::vector<EditorAssetRecord> records_{};
    std::vector<WorkItem> work_{};
    std::vector<CpuAsset> completed_{};
    std::unordered_map<uint64_t, uint64_t> latest_revisions_{};
    uint64_t next_revision_{1};
    // Worker and watcher exceptions must be surfaced to the editor thread;
    // letting one escape an executor callback terminates the process, while
    // completion alone would only report failure after a loop has stopped.
    std::atomic<bool> background_failure_{false};
    std::atomic<bool> stopping_{false};
    concurrency::CancellationSource cancellation_{};
    std::shared_ptr<concurrency::WorkerExecutor> io_executor_{};
    concurrency::Completion worker_completion_{};
    concurrency::Completion watcher_completion_{};
};

}  // namespace aeg::application::asset_pipeline
