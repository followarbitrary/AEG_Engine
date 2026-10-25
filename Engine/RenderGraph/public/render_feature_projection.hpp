/**
 * @file render_feature_projection.hpp
 * @brief Frame-local RenderFeature projection into internal work items.
 */
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "feature_graph.hpp"
#include "render_intent_plan.hpp"

namespace aeg::render_graph {

struct RenderWorkItemIdTag;
using RenderWorkItemId = foundation::StableId<RenderWorkItemIdTag>;

/**
 * @brief Frame-local operation emitted for a Render-domain Feature.
 *
 * Work item kinds describe operations for the internal RenderGraph adapter. They are
 * intentionally typed instead of encoded as author-facing pass names so that
 * a Feature projection cannot become a second render-pass authoring API.
 * Module ownership is retained for diagnostics and hot-reload provenance.
 */
enum class RenderWorkItemKind : uint8_t {
    kPrepare = 0,
    kExecute = 1,
};

struct RenderWorkItem final {
    feature::FeatureDomain domain{feature::FeatureDomain::kRender};
    feature::FeatureDefinitionId feature_definition_id{};
    feature::ModuleIdentity owner_module{};
    feature::FeatureNodeId node_id{};
    feature::FeatureInstanceId instance_id{};
    uint64_t frame_index{0};
    RenderWorkItemId id{};
    RenderWorkItemKind kind{RenderWorkItemKind::kPrepare};
    feature::InvocationToken invocation{};
    uint32_t operation_index{0};
};

}  // namespace aeg::render_graph
