/**
 * @file render_graph_upload_access.hpp
 * @brief RenderGraph-private access bridge for Resource upload receipts.
 */
#pragma once

#include "../public/render_graph.hpp"

#include <cstdint>
#include <memory>

namespace aeg::render_graph::detail {

/**
 * @brief Keeps upload receipt tracking out of the public RenderGraph contract.
 *
 * Resource uploaders are Engine-private consumers. They need a graph-owned
 * lifetime token, but do not need a public graph method for that internal
 * bookkeeping operation.
 */
struct RenderGraphUploadAccess final {
    [[nodiscard]] static foundation::ResultValue<std::shared_ptr<const void>> track_pass_submission(
        RenderGraph& graph, RenderGraphPass pass, uint64_t frame_index);
};

}  // namespace aeg::render_graph::detail
