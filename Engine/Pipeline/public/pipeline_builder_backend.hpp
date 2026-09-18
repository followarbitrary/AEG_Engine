#pragma once

#include "pipeline.hpp"

namespace aeg::pipeline {

/**
 * Backend seam for module-local declarations. Existing PipelineBuilder paths
 * remain unchanged; SDK adapters may forward these operations to a checked
 * local implementation without exposing Registry internals.
 */
class PipelineBuilderBackend {
public:
    virtual ~PipelineBuilderBackend() = default;
    [[nodiscard]] virtual PipelineStageHandle add_stage(const StageDescriptor&) = 0;
    [[nodiscard]] virtual PipelineTaskHandle add_task(const TaskDescriptor&) = 0;
    [[nodiscard]] virtual PipelineArtifactHandle declare_artifact(const ArtifactDescriptor&) = 0;
    [[nodiscard]] virtual PipelineFeaturePointHandle add_feature_point(const FeaturePointDescriptor&) = 0;
    [[nodiscard]] virtual foundation::Result bind_feature(const FeatureBindingDescriptor&) = 0;
    [[nodiscard]] virtual foundation::Result bind_task(PipelineTaskHandle, PipelineStageHandle) = 0;
    [[nodiscard]] virtual foundation::Result reads(PipelineStageHandle, const ArtifactBinding&) = 0;
    [[nodiscard]] virtual foundation::Result writes(PipelineStageHandle, const ArtifactBinding&) = 0;
    [[nodiscard]] virtual foundation::Result reads_writes(PipelineStageHandle, const ArtifactBinding&) = 0;
    [[nodiscard]] virtual foundation::Result publish_snapshot(PipelineStageHandle, const ArtifactBinding&) = 0;
    [[nodiscard]] virtual foundation::Result runs_before(PipelineStageHandle, StageId) = 0;
    [[nodiscard]] virtual foundation::Result runs_after(PipelineStageHandle, StageId) = 0;
    [[nodiscard]] virtual foundation::Result expose_artifact(ArtifactId, PipelineScope) = 0;
    [[nodiscard]] virtual foundation::Result status() const noexcept = 0;
};

/** @brief Stable name used by the module SDK for the local declaration seam. */
using PipelineDeclarationBackend = PipelineBuilderBackend;

/** @brief Immutable identity/configuration supplied to one module-local Feature declaration. */
struct FeatureBuildDescriptor final {
    PipelineScope scope{PipelineScope::kEngine};
    PipelineId pipeline{};
    FeatureId feature{};
    FeatureInstanceId instance{};
    FeaturePartId part{};
    FeaturePointDescriptor point{};
    FeatureDomain domain{FeatureDomain::kLogic};
    std::shared_ptr<const PipelineInstanceConfiguration> configuration{};
};

/** @brief Feature declaration seam with host-owned permission and provenance checks. */
class FeatureDeclarationBackend : public PipelineDeclarationBackend {
public:
    ~FeatureDeclarationBackend() override = default;
    [[nodiscard]] virtual const FeatureBuildDescriptor& feature_descriptor() const noexcept = 0;
    [[nodiscard]] virtual uint32_t stage_count() const noexcept = 0;
    [[nodiscard]] virtual foundation::Result reject(foundation::ResultCode code, std::string message) = 0;
    virtual void on_stage_added(PipelineStageHandle stage, const FeaturePointDescriptor& point) = 0;
    virtual void record_contribution(FeatureDomain domain, uint32_t first_stage_index, uint32_t stage_count) = 0;
};

/** Local adapter used by C++ module SDKs; it borrows a builder for one declaration call. */
class PipelineBuilderBackendAdapter final : public PipelineBuilderBackend {
public:
    explicit PipelineBuilderBackendAdapter(PipelineBuilder& builder) noexcept : builder_(&builder) {}
    [[nodiscard]] PipelineStageHandle add_stage(const StageDescriptor& value) override {
        return builder_->add_stage(value);
    }
    [[nodiscard]] PipelineTaskHandle add_task(const TaskDescriptor& value) override {
        return builder_->add_task(value);
    }
    [[nodiscard]] PipelineArtifactHandle declare_artifact(const ArtifactDescriptor& value) override {
        return builder_->declare_artifact(value);
    }
    [[nodiscard]] PipelineFeaturePointHandle add_feature_point(const FeaturePointDescriptor& value) override {
        return builder_->add_feature_point(value);
    }
    [[nodiscard]] foundation::Result bind_feature(const FeatureBindingDescriptor& value) override {
        return builder_->bind_feature(value);
    }
    [[nodiscard]] foundation::Result bind_task(PipelineTaskHandle task, PipelineStageHandle stage) override {
        return builder_->bind_task(task, stage);
    }
    [[nodiscard]] foundation::Result reads(PipelineStageHandle stage, const ArtifactBinding& value) override {
        return builder_->reads(stage, value);
    }
    [[nodiscard]] foundation::Result writes(PipelineStageHandle stage, const ArtifactBinding& value) override {
        return builder_->writes(stage, value);
    }
    [[nodiscard]] foundation::Result reads_writes(PipelineStageHandle stage, const ArtifactBinding& value) override {
        return builder_->reads_writes(stage, value);
    }
    [[nodiscard]] foundation::Result publish_snapshot(PipelineStageHandle stage,
                                                      const ArtifactBinding& value) override {
        return builder_->publish_snapshot(stage, value);
    }
    [[nodiscard]] foundation::Result runs_before(PipelineStageHandle stage, StageId other) override {
        return builder_->runs_before(stage, other);
    }
    [[nodiscard]] foundation::Result runs_after(PipelineStageHandle stage, StageId other) override {
        return builder_->runs_after(stage, other);
    }
    [[nodiscard]] foundation::Result expose_artifact(ArtifactId artifact, PipelineScope scope) override {
        return builder_->expose_artifact(artifact, scope);
    }
    [[nodiscard]] foundation::Result status() const noexcept override { return builder_->status(); }

private:
    PipelineBuilder* builder_{};
};

}  // namespace aeg::pipeline
