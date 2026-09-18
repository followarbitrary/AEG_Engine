/** @file pipeline_authoring_types.hpp @brief Owned authoring contracts. */
#pragma once
#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "pipeline_types.hpp"
namespace aeg::pipeline {
enum class AuthoringTypeKind : uint8_t { kPipeline, kFeature, kRenderPass };
struct AuthoringTypeKey final {
    AuthoringTypeKind kind{};
    uint64_t id{};
    friend constexpr auto operator<=>(AuthoringTypeKey, AuthoringTypeKey) = default;
};
struct AssetReference final {
    std::string uuid{};
    friend bool operator==(const AssetReference&, const AssetReference&) = default;
};
using ParameterValue = std::variant<bool, int64_t, uint64_t, double, std::string, std::vector<double>, AssetReference>;
using ParameterBlock = std::map<uint64_t, ParameterValue>;
enum class ParameterKind : uint8_t { kBool, kInt64, kUInt64, kFloat64, kString, kVector, kEnum, kAsset };
struct ParameterDescriptor final {
    uint64_t id{};
    std::string name{};
    std::string display_name{};
    ParameterKind kind{};
    ParameterValue default_value{false};
    std::optional<double> minimum{};
    std::optional<double> maximum{};
    std::vector<std::string> enum_values{};
    std::string unit{};
    uint32_t schema_version{1};
    bool affects_topology{false};
};
enum class PortDirection : uint8_t { kInput, kOutput };
enum class PortValueKind : uint8_t { kArtifact, kRenderResource, kRenderState, kBuffer, kTexture };
enum class ResourceClass : uint8_t { kNone, kBuffer, kTexture };
// capability_mask uses the matching RHI BufferCapability/TextureCapability bits;
// usage_mask uses 1 << ResourceUsage. Zero sample_count means unspecified.
struct ResourceConstraints final {
    ResourceClass resource_class{ResourceClass::kNone};
    std::vector<std::string> formats{};
    std::string size_rule{};
    uint32_t sample_count{};
    uint64_t capability_mask{};
    uint64_t usage_mask{};
};
struct PortDescriptor final {
    uint64_t id{};
    std::string name{};
    PortDirection direction{};
    PortValueKind value_kind{};
    uint64_t schema_id{};
    uint32_t schema_version{1};
    bool required{false};
    uint32_t min_connections{};
    uint32_t max_connections{1};
    uint64_t binding_id{};
    ResourceConstraints resource_constraints{};
};
struct AuthoringStage final {
    StageId id{};
    std::string name{};
    PhaseId phase{};
    ExecutionDomain execution_domain{};
    TimePolicy time_policy{};
    bool editable{false};
};
struct AuthoringFeaturePoint final {
    FeaturePointId id{};
    std::string name{};
    StageId anchor_stage{};
    FeaturePointPosition position{};
    FeatureDomainMask domains{};
    PipelineScopeMask scopes{};
    FeaturePointFlags flags{};
};
struct AuthoringArtifact final {
    ArtifactId id{};
    std::string name{};
    ArtifactTypeId type_id{};
    uint32_t version{1};
    PipelineScope owner_scope{};
    PipelineScopeMask visible_to{};
    bool external_input{};
    ArtifactStorage storage{};
    ArtifactLifetime lifetime{};
    OwnershipPolicy ownership{};
};
struct AuthoringStageOrdering final {
    StageId before{};
    StageId after{};
};
struct AuthoringPart final {
    uint32_t part_index{};
    FeatureDomain domain{};
    std::string name{};
};
struct DefaultFeatureNode final {
    std::string node_id{};
    FeatureId type{};
    Version version{1, 0, 0};
    FeaturePointId point{};
    ParameterBlock parameters{};
};
struct DefaultPassNode final {
    std::string node_id{};
    RenderPassTypeId type{};
    Version version{1, 0, 0};
    std::string part_id{};
    ParameterBlock parameters{};
    bool enabled{true};
};
struct DefaultResourceNode final {
    std::string node_id{};
    RenderResourceId resource_id{};
    ResourceConstraints constraints{};
};
// Shared by authored documents and type-declared default graphs. Ordering edges
// address nodes (port_id/binding_id == 0); data edges address stable ports.
// A resource node's intrinsic port_id is its resource_id; it relays its value
// between a producing Pass and consuming Passes, and is not another producer.
enum class DocumentEdgeKind : uint8_t { kArtifact, kOrdering, kRenderResource, kRenderState };
struct DocumentEndpoint final {
    std::string node_id{};
    uint64_t port_id{};
};
struct DocumentEdge final {
    std::string edge_id{};
    DocumentEdgeKind kind{};
    DocumentEndpoint source{};
    DocumentEndpoint destination{};
    uint64_t binding_id{};
};
using DefaultEdge = DocumentEdge;
struct PipelineAuthoringDeclaration final {
    std::vector<AuthoringStage> stages{};
    std::vector<AuthoringFeaturePoint> feature_points{};
    std::vector<AuthoringArtifact> artifacts{};
    std::vector<AuthoringStageOrdering> fixed_orderings{};
    std::vector<DefaultFeatureNode> default_features{};
};
struct FeatureAuthoringDeclaration final {
    std::vector<AuthoringStage> stages{};
    std::vector<AuthoringArtifact> artifacts{};
    std::vector<AuthoringPart> parts{};
    std::vector<DefaultPassNode> default_passes{};
    std::vector<DefaultResourceNode> default_resources{};
    std::vector<DefaultEdge> default_edges{};
    std::vector<RenderPassTypeId> allowed_pass_types{};
    bool allow_add_passes{false};
};
// Queue masks use 1 << rhi::QueueClass; zero leaves queue selection unspecified.
struct PassAuthoringDeclaration final {
    RenderPassTypeId type{};
    uint64_t allowed_queue_mask{};
};
using AuthoringDeclaration =
    std::variant<PipelineAuthoringDeclaration, FeatureAuthoringDeclaration, PassAuthoringDeclaration>;
struct TypeCatalogEntry final {
    AuthoringTypeKey key{};
    Version version{};
    ModuleId owner_module{};
    std::string name{};
    std::string display_name{};
    FeatureDomainMask domains{};
    PipelineScopeMask scopes{};
    std::vector<ParameterDescriptor> parameters{};
    std::vector<PortDescriptor> ports{};
    AuthoringDeclaration declaration{PipelineAuthoringDeclaration{}};
};
}  // namespace aeg::pipeline
