#include "editor_composition_preview.hpp"

#include "pipeline_authoring.hpp"

namespace aeg::application {

EditorCompositionPreviewController::EditorCompositionPreviewController(
    pipeline::PipelineRegistry& registry, pipeline::RenderPassRegistry& render_pass_registry,
    engine::PipelineCompositionRuntime& runtime) noexcept
    : registry_(&registry), render_pass_registry_(&render_pass_registry), runtime_(&runtime) {}

foundation::Result EditorCompositionPreviewController::request(
    std::shared_ptr<const pipeline::PipelineDocument> document, pipeline::TypeCatalog catalog, uint64_t revision,
    std::span<const engine::PipelineHostBinding> bindings) {
    if (!document)
        return foundation::invalid_argument("Preview request has no Pipeline document");
    if (revision == 0)
        return foundation::invalid_argument("Preview request revision is invalid");
    if (!registry_ || !render_pass_registry_ || !runtime_)
        return foundation::invalid_state("Preview services are unavailable");
    const auto prepared = pipeline::prepare_pipeline_document(*document, catalog, *registry_, *render_pass_registry_);
    if (!prepared) {
        runtime_->discard_pending();
        state_.draft_revision = revision;
        state_.rebuilding = false;
        state_.stale = true;
        state_.diagnostics = pipeline::validate_pipeline_document(*document, catalog);
        return prepared.result;
    }
    if (const auto result = runtime_->stage(prepared.value, bindings); !result) {
        runtime_->discard_pending();
        state_.draft_revision = revision;
        state_.rebuilding = false;
        state_.stale = true;
        state_.diagnostics.clear();
        return result;
    }
    state_.draft_revision = revision;
    state_.rebuilding = true;
    state_.stale = false;
    state_.stopped = false;
    state_.diagnostics.clear();
    return foundation::Result::success();
}

foundation::Result EditorCompositionPreviewController::apply_at_frame_boundary() {
    if (!runtime_)
        return foundation::invalid_state("Preview services are unavailable");
    if (state_.stale && !state_.rebuilding)
        return foundation::invalid_state("the latest FlowGraph draft is stale; repair it before applying");
    if (const auto result = runtime_->apply_pending(true); !result)
        return result;
    if (state_.rebuilding) {
        state_.active_revision = state_.draft_revision;
        state_.rebuilding = false;
    }
    state_.stopped = false;
    state_.stale = false;
    return foundation::Result::success();
}

foundation::Result EditorCompositionPreviewController::stop() noexcept {
    if (runtime_ != nullptr) {
        if (const auto result = runtime_->stop_and_retire(); !result)
            return result;
    }
    state_.rebuilding = false;
    state_.stopped = true;
    state_.stale = false;
    state_.diagnostics.clear();
    return foundation::Result::success();
}

}  // namespace aeg::application
