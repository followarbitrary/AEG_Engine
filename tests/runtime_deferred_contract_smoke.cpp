/** @file runtime_deferred_contract_smoke.cpp
 *  @brief Validates the runtime deferred-lighting host contract boundary.
 */
#include <iostream>

#include "renderer.hpp"
#include "runtime_asset_resolver.hpp"
#include "runtime_asset_upload_host.hpp"

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
    RuntimeAssetResolver::UploadContext context{};
    context.deferred_lighting = pipeline_only;
    resolver.set_upload_context(context);
    if (!not_ready(resolver.resolve_deferred_lighting(view))) {
        std::cerr << "pipeline-only deferred-lighting contract did not fail closed\n";
        return 2;
    }

    aeg::renderer::RenderDeferredLightingResource callback_only{};
    callback_only.prepare_inputs = [](const aeg::renderer::RenderDeferredLightingInputs&) {
        return aeg::foundation::ResultValue<aeg::rhi::BindingSetHandle>{aeg::foundation::Result::success(),
                                                                        aeg::rhi::BindingSetHandle{19}};
    };
    context = {};
    context.deferred_lighting = callback_only;
    resolver.set_upload_context(context);
    if (!not_ready(resolver.resolve_deferred_lighting(view))) {
        std::cerr << "callback-only deferred-lighting contract did not fail closed\n";
        return 3;
    }

    aeg::renderer::RenderDeferredLightingResource invalid_profile{};
    invalid_profile.pipeline = aeg::rhi::PipelineHandle{23};
    invalid_profile.prepare_inputs = [](const aeg::renderer::RenderDeferredLightingInputs&) {
        return aeg::foundation::ResultValue<aeg::rhi::BindingSetHandle>{aeg::foundation::Result::success(),
                                                                        aeg::rhi::BindingSetHandle{29}};
    };
    context = {};
    context.deferred_lighting = invalid_profile;
    resolver.set_upload_context(context);
    if (!not_ready(resolver.resolve_deferred_lighting(view))) {
        std::cerr << "invalid deferred-lighting Shader/PSO contract did not fail closed\n";
        return 4;
    }

    aeg::renderer::RenderDeferredLightingResource complete{};
    complete.pipeline = aeg::rhi::PipelineHandle{31};
    complete.prepare_inputs = [](const aeg::renderer::RenderDeferredLightingInputs&) {
        return aeg::foundation::ResultValue<aeg::rhi::BindingSetHandle>{aeg::foundation::Result::success(),
                                                                        aeg::rhi::BindingSetHandle{37}};
    };
    complete.profile_contract = {
        {aeg::renderer::kRendererProfileContractVersion, aeg::renderer::RendererProfile::kDeferredLighting,
         0x1122334455667788ull, 1},
        {aeg::rhi::Format::kRgba16Float, aeg::rhi::SampleCount::kX1, aeg::rhi::Format::kUnknown},
        1};
    context = {};
    context.deferred_lighting = complete;
    resolver.set_upload_context(context);
    const auto resolved = resolver.resolve_deferred_lighting(view);
    if (!resolved || resolved.value.pipeline != complete.pipeline || !resolved.value.prepare_inputs) {
        std::cerr << "complete deferred-lighting contract was not returned\n";
        return 5;
    }
    const auto prepared = resolved.value.prepare_inputs({});
    if (!prepared || prepared.value != aeg::rhi::BindingSetHandle{37}) {
        std::cerr << "deferred-lighting callback was not preserved\n";
        return 6;
    }

    // A frame-aware host can publish a resolver instead of a single borrowed
    // value. The resolver must take precedence so each graph build receives a
    // fresh descriptor callback.
    aeg::renderer::RenderDeferredLightingResource per_frame{};
    per_frame.pipeline = aeg::rhi::PipelineHandle{41};
    per_frame.prepare_inputs = [](const aeg::renderer::RenderDeferredLightingInputs&) {
        return aeg::foundation::ResultValue<aeg::rhi::BindingSetHandle>{aeg::foundation::Result::success(),
                                                                        aeg::rhi::BindingSetHandle{43}};
    };
    per_frame.profile_contract = complete.profile_contract;
    context = {};
    context.deferred_lighting = complete;
    context.deferred_lighting_resolver = [per_frame](const aeg::renderer::RenderView&) {
        return aeg::foundation::ResultValue<aeg::renderer::RenderDeferredLightingResource>{
            aeg::foundation::Result::success(), per_frame};
    };
    resolver.set_upload_context(context);
    const auto per_frame_result = resolver.resolve_deferred_lighting(view);
    if (!per_frame_result || per_frame_result.value.pipeline != per_frame.pipeline) {
        std::cerr << "per-frame deferred-lighting resolver did not take precedence\n";
        return 7;
    }

    std::cout << "runtime deferred-lighting contract passed\n";
    return 0;
}
