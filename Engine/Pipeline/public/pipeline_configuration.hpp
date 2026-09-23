#pragma once

#include <memory>
#include <string>
#include <vector>

#include "render_pass.hpp"

namespace aeg::pipeline {

struct RenderPassInstanceConfiguration final {
    RenderPassTypeId type{};
    RenderPassInstanceId instance{};
    rhi::QueueClass queue{rhi::QueueClass::kGraphics};
    bool enabled{true};
    ParameterBlock parameters{};
    std::unordered_map<uint64_t, uint64_t> resource_bindings{};
    std::string part{};
    std::string source{};
    std::vector<RenderPassInstanceId> ordered_after{};
    FeaturePartId resolved_part{};
};

struct FeatureInstanceConfiguration final {
    FeatureInstanceId instance{};
    FeatureId feature{};
    FeaturePointId point{};
    ParameterBlock parameters{};
    /** @brief Enables declarative Pass expansion authorized by the Feature authoring declaration. */
    bool expand_authored_passes{false};
    std::vector<RenderPassInstanceConfiguration> passes{};
    std::string part{};
    std::string source{};
};

struct PipelineInstanceConfiguration final {
    PipelineInstanceId instance{};
    PipelineId pipeline{};
    /** @brief Scope authored by the Pipeline document and used to create its runtime host. */
    PipelineScope scope{PipelineScope::kEngine};
    std::vector<FeatureInstanceConfiguration> features{};
    uint64_t revision{0};
    std::string source{};
};

}  // namespace aeg::pipeline
