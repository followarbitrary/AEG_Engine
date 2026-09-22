/** @file pipeline_document.hpp @brief Fully-owned editable pipeline document. */
#pragma once
#include "pipeline_authoring_types.hpp"
namespace aeg::pipeline {
struct PipelineNodeDocument final {
    std::string node_id{};
    PipelineId type{};
    Version version{};
    PipelineScope scope{};
    std::string owner_binding{};
    ParameterBlock parameters{};
};
struct FeatureNodeDocument final {
    std::string node_id{};
    FeatureId type{};
    Version version{};
    std::string pipeline_node{};
    FeaturePointId point{};
    bool enabled{true};
    ParameterBlock parameters{};
};
struct PassNodeDocument final {
    std::string node_id{};
    RenderPassTypeId type{};
    Version version{};
    std::string feature_node{};
    std::string part_id{};
    ParameterBlock parameters{};
    bool enabled{true};
};
struct ResourceNodeDocument final {
    std::string node_id{};
    RenderResourceId resource_id{};
    ArtifactTypeId type{};
    uint32_t version{1};
    ParameterBlock parameters{};
    PipelineScope owner_scope{};
    PipelineScopeMask visible_to{};
    bool external_input{};
    ArtifactStorage storage{};
    ArtifactLifetime lifetime{};
    OwnershipPolicy ownership{};
    ResourceConstraints resource_constraints{};
};
struct DocumentNodeLayout final {
    std::string node_id{};
    double x{};
    double y{};
    bool expanded{true};
};
enum class OpaqueAuthoringEncoding : uint8_t { kToml, kJson };
struct OpaqueAuthoringRecord final {
    std::string owner_node{};
    OpaqueAuthoringEncoding encoding{};
    std::string payload{};
};
struct PipelineDocument final {
    uint32_t schema_version{1};
    std::string asset_uuid{};
    std::string name{};
    std::vector<ModuleId> module_requirements{};
    std::vector<PipelineNodeDocument> pipeline_nodes{};
    std::vector<FeatureNodeDocument> feature_nodes{};
    std::vector<PassNodeDocument> pass_nodes{};
    std::vector<ResourceNodeDocument> resources{};
    std::vector<DocumentEdge> edges{};
    /** Editor display-consumer links targeting the mandatory Present terminal. */
    std::vector<std::string> present_connections{};
    std::vector<DocumentNodeLayout> layouts{};
    std::vector<OpaqueAuthoringRecord> extensions{};
};
}  // namespace aeg::pipeline
