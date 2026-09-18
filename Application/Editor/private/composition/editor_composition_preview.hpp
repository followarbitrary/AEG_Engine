#pragma once

#include <memory>
#include <span>
#include <vector>

#include "pipeline_composition_runtime.hpp"

namespace aeg::application {

struct EditorCompositionPreviewState final {
    uint64_t draft_revision{};
    uint64_t active_revision{};
    bool rebuilding{};
    bool stopped{true};
    bool stale{};
    std::vector<pipeline::DocumentDiagnostic> diagnostics{};
};

class EditorCompositionPreviewController final {
public:
    EditorCompositionPreviewController(pipeline::PipelineRegistry& registry,
                                       pipeline::RenderPassRegistry& render_pass_registry,
                                       engine::PipelineCompositionRuntime& runtime) noexcept;

    [[nodiscard]] foundation::Result request(std::shared_ptr<const pipeline::PipelineDocument> document,
                                             pipeline::TypeCatalog catalog, uint64_t revision,
                                             std::span<const engine::PipelineHostBinding> bindings);
    [[nodiscard]] foundation::Result apply_at_frame_boundary();
    [[nodiscard]] foundation::Result stop() noexcept;
    [[nodiscard]] const EditorCompositionPreviewState& state() const noexcept { return state_; }

private:
    pipeline::PipelineRegistry* registry_{};
    pipeline::RenderPassRegistry* render_pass_registry_{};
    engine::PipelineCompositionRuntime* runtime_{};
    EditorCompositionPreviewState state_{};
};

}  // namespace aeg::application
