/** @file runtime_deferred_contract_smoke.cpp
 *  @brief Validates the runtime deferred-lighting host contract boundary.
 */
#include <iostream>

#include "renderer.hpp"
#include "runtime_asset_resolver.hpp"

namespace {
using aeg::application::runtime::RuntimeAssetResolver;
using aeg::foundation::ResultCode;

bool not_ready(const aeg::foundation::ResultValue<aeg::renderer::RenderDeferredLightingResource>& result) {
    return !result && result.result.code == ResultCode::kNotReady;
}
}  // namespace

int main() {
    RuntimeAssetResolver resolver;
    const aeg::renderer::RenderView view{};

    if (!not_ready(resolver.resolve_deferred_lighting(view))) {
        std::cerr << "missing deferred-lighting contract did not fail closed\n";
        return 1;
    }

    aeg::renderer::RenderDeferredLightingResource pipeline_only{};
    pipeline_only.pipeline = aeg::rhi::PipelineHandle{17};
    resolver.set_upload_context({.deferred_lighting = pipeline_only});
    if (!not_ready(resolver.resolve_deferred_lighting(view))) {
        std::cerr << "pipeline-only deferred-lighting contract did not fail closed\n";
        return 2;
    }

    aeg::renderer::RenderDeferredLightingResource callback_only{};
    callback_only.prepare_inputs = [](const aeg::renderer::RenderDeferredLightingInputs&) {
        return aeg::foundation::ResultValue<aeg::rhi::BindingSetHandle>{aeg::foundation::Result::success(),
                                                                        aeg::rhi::BindingSetHandle{19}};
    };
    resolver.set_upload_context({.deferred_lighting = callback_only});
    if (!not_ready(resolver.resolve_deferred_lighting(view))) {
        std::cerr << "callback-only deferred-lighting contract did not fail closed\n";
        return 3;
    }

    aeg::renderer::RenderDeferredLightingResource complete{};
    complete.pipeline = aeg::rhi::PipelineHandle{23};
    complete.prepare_inputs = [](const aeg::renderer::RenderDeferredLightingInputs&) {
        return aeg::foundation::ResultValue<aeg::rhi::BindingSetHandle>{aeg::foundation::Result::success(),
                                                                        aeg::rhi::BindingSetHandle{29}};
    };
    resolver.set_upload_context({.deferred_lighting = complete});
    const auto resolved = resolver.resolve_deferred_lighting(view);
    if (!resolved || resolved.value.pipeline != complete.pipeline || !resolved.value.prepare_inputs) {
        std::cerr << "complete deferred-lighting contract was not returned\n";
        return 4;
    }
    const auto prepared = resolved.value.prepare_inputs({});
    if (!prepared || prepared.value != aeg::rhi::BindingSetHandle{29}) {
        std::cerr << "deferred-lighting callback was not preserved\n";
        return 5;
    }

    // A frame-aware host can publish a resolver instead of a single borrowed
    // value. The resolver must take precedence so each graph build receives a
    // fresh descriptor callback.
    aeg::renderer::RenderDeferredLightingResource per_frame{};
    per_frame.pipeline = aeg::rhi::PipelineHandle{31};
    per_frame.prepare_inputs = [](const aeg::renderer::RenderDeferredLightingInputs&) {
        return aeg::foundation::ResultValue<aeg::rhi::BindingSetHandle>{aeg::foundation::Result::success(),
                                                                        aeg::rhi::BindingSetHandle{37}};
    };
    resolver.set_upload_context({
        .deferred_lighting = complete,
        .deferred_lighting_resolver =
            [per_frame](const aeg::renderer::RenderView&) {
                return aeg::foundation::ResultValue<aeg::renderer::RenderDeferredLightingResource>{
                    aeg::foundation::Result::success(), per_frame};
            },
    });
    const auto per_frame_result = resolver.resolve_deferred_lighting(view);
    if (!per_frame_result || per_frame_result.value.pipeline != per_frame.pipeline) {
        std::cerr << "per-frame deferred-lighting resolver did not take precedence\n";
        return 6;
    }

    std::cout << "runtime deferred-lighting contract passed\n";
    return 0;
}
