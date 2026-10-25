/** @brief Transactional Engine-private access to the fixed native graph backend. */
#pragma once

#include <memory>
#include <span>

#include "render_feature_runtime.hpp"
#include "pipeline.hpp"

namespace aeg::render_graph {

/**
 * Borrows a graph for one fixed native build. Uncommitted destruction restores
 * resources, passes, dependencies and execution leases to the initial checkpoint.
 * No Pipeline or Feature registration is involved in this technical adapter.
 */
class NativeGraphBuildSession final {
public:
    [[nodiscard]] static foundation::ResultValue<std::unique_ptr<NativeGraphBuildSession>> begin(
        RenderGraph& graph, std::span<const RenderGraphFeatureTextureBinding> textures,
        pipeline::RenderGraphBuildFlags flags, const void* context_data = nullptr);
    ~NativeGraphBuildSession();

    NativeGraphBuildSession(const NativeGraphBuildSession&) = delete;
    NativeGraphBuildSession& operator=(const NativeGraphBuildSession&) = delete;

    [[nodiscard]] pipeline::RenderGraphBuilder& builder() noexcept;
    void commit() noexcept;

private:
    struct Impl;
    explicit NativeGraphBuildSession(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace aeg::render_graph
