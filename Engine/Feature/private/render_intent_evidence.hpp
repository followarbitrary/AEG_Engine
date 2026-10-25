#pragma once
#include "feature_runtime.hpp"

namespace aeg::feature::detail {
/** Immutable typed-export evidence shared by routed deliveries, with no nested receipts. */
struct RenderLightListEvidence final {
    RenderFeatureOutput source{};
    ProcessRevision process_revision{};
};
/** Returns the same-invocation declaration; non-LightList messages return nullptr. */
[[nodiscard]] foundation::ResultValue<const RenderLightCullingScene*> light_list_export_declaration(
    const RenderIntent* intent, const PortMessage& message, const InvocationToken& token);
}  // namespace aeg::feature::detail
