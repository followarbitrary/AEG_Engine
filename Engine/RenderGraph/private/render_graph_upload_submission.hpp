/**
 * @file render_graph_upload_submission.hpp
 * @brief RenderGraph-private upload recording/submission receipt.
 */
#pragma once

#include <cstdint>
#include <memory>

namespace aeg::render_graph {
namespace detail {
struct RenderGraphUploadAccess;
}

/**
 * @brief Internal upload receipt kept behind opaque Resource/Runtime fields.
 *
 * Submission means the graph accepted the batch, not GPU completion. A
 * recording expires when its pass is removed, reset, or rolled back.
 */
class RenderGraphPassSubmission final {
public:
    [[nodiscard]] bool was_submitted() const noexcept { return submitted_; }
    [[nodiscard]] bool is_recorded_for(uint64_t frame_index) const noexcept {
        return frame_index_ == frame_index && !recording_.expired();
    }

private:
    friend class RenderGraph;
    friend struct detail::RenderGraphUploadAccess;
    std::weak_ptr<const void> recording_{};
    uint64_t frame_index_{0};
    bool submitted_{false};
};
}  // namespace aeg::render_graph
