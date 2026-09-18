/** @file pipeline_authoring_catalog_smoke.cpp */
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <pipeline_authoring.hpp>
#include <pipeline_builder_backend.hpp>
#include <string>
#include <type_traits>

namespace {
static_assert(std::is_abstract_v<aeg::pipeline::PipelineBuilderBackend>);
static_assert(std::is_final_v<aeg::pipeline::PipelineBuilderBackendAdapter>);
class FakePipelineFeature final : public aeg::pipeline::PipelineFeature {
public:
    [[nodiscard]] const aeg::pipeline::FeatureDescriptor& descriptor() const noexcept override { return descriptor_; }
    [[nodiscard]] aeg::foundation::Result collect_contributions(aeg::pipeline::FeatureBuildContext&) const override {
        return aeg::foundation::Result::success();
    }

private:
    aeg::pipeline::FeatureDescriptor descriptor_{};
};
class TestLogicFeature final : public aeg::pipeline::LogicFeature {
public:
    [[nodiscard]] const aeg::pipeline::FeatureDescriptor& descriptor() const noexcept override { return descriptor_; }

protected:
    [[nodiscard]] aeg::foundation::Result setup(aeg::pipeline::LogicGraphBuilder&) const override {
        return aeg::foundation::Result::success();
    }

private:
    aeg::pipeline::FeatureDescriptor descriptor_{};
};
class TestRenderFeature final : public aeg::pipeline::RenderFeature {
public:
    [[nodiscard]] const aeg::pipeline::FeatureDescriptor& descriptor() const noexcept override { return descriptor_; }

protected:
    [[nodiscard]] aeg::foundation::Result setup(aeg::pipeline::RenderGraphBuilder&) const override {
        return aeg::foundation::Result::success();
    }

private:
    aeg::pipeline::FeatureDescriptor descriptor_{};
};
static_assert(!aeg::pipeline::SupportedGameFeature<FakePipelineFeature>);
static_assert(aeg::pipeline::SupportedGameFeature<TestLogicFeature>);
static_assert(aeg::pipeline::SupportedGameFeature<TestRenderFeature>);
bool check(bool condition, const char* expression, int line) {
    if (!condition)
        std::cerr << "check failed at line " << line << ": " << expression << '\n';
    return condition;
}
}  // namespace
#define CHECK(expression)                                                 \
    do {                                                                  \
        if (!check(static_cast<bool>(expression), #expression, __LINE__)) \
            return __LINE__;                                              \
    } while (false)

int declaration_validation_cases() {
    using namespace aeg::pipeline;
    TypeCatalogEntry pipeline;
    pipeline.key = {AuthoringTypeKind::kPipeline, 1};
    pipeline.version = {1, 0, 0};
    pipeline.owner_module = ModuleId(1);
    pipeline.name = "Pipeline";
    pipeline.display_name = "Pipeline";
    PipelineAuthoringDeclaration pd;
    pd.stages = {{.id = StageId(1), .name = "A", .phase = PhaseId(1)},
                 {.id = StageId(2), .name = "B", .phase = PhaseId(1)}};
    pd.feature_points = {{.id = FeaturePointId(1),
                          .name = "Point",
                          .anchor_stage = StageId(1),
                          .domains = to_mask(FeatureDomain::kRender),
                          .scopes = to_mask(PipelineScope::kFrame)}};
    pd.artifacts = {{.id = ArtifactId(1), .name = "Data", .type_id = ArtifactTypeId(1)}};
    pd.fixed_orderings = {{StageId(1), StageId(2)}};
    pd.default_features = {{.node_id = "feature", .type = FeatureId(1), .point = FeaturePointId(1)}};
    pipeline.declaration = pd;
    TypeCatalogEntry feature = pipeline;
    feature.key = {AuthoringTypeKind::kFeature, 1};
    feature.name = "Feature";
    feature.domains = to_mask(FeatureDomain::kRender);
    FeatureAuthoringDeclaration fd;
    fd.stages = pd.stages;
    fd.artifacts = pd.artifacts;
    fd.parts = {{.part_index = 0, .domain = FeatureDomain::kRender, .name = "render"}};
    fd.default_passes = {{.node_id = "pass", .type = RenderPassTypeId(1), .part_id = "render"}};
    fd.default_resources = {{.node_id = "resource",
                             .resource_id = RenderResourceId(2),
                             .constraints = {.resource_class = ResourceClass::kTexture}}};
    fd.default_edges = {{.edge_id = "edge",
                         .kind = DocumentEdgeKind::kRenderResource,
                         .source = {"pass", 7},
                         .destination = {"resource", 2},
                         .binding_id = 2}};
    fd.allowed_pass_types = {RenderPassTypeId(1)};
    feature.declaration = fd;
    TypeCatalogEntry pass = pipeline;
    pass.key = {AuthoringTypeKind::kRenderPass, 1};
    pass.name = "Pass";
    pass.declaration = PassAuthoringDeclaration{.type = RenderPassTypeId(1), .allowed_queue_mask = 1};
    auto valid = [](const auto& entry) {
        TypeCatalog c;
        return static_cast<bool>(c.add(entry));
    };
    CHECK(valid(pipeline));
    CHECK(valid(feature));
    CHECK(valid(pass));
    auto reject_pipeline = [&](auto mutate) {
        auto e = pipeline;
        mutate(std::get<PipelineAuthoringDeclaration>(e.declaration));
        return !valid(e);
    };
    auto reject_feature = [&](auto mutate) {
        auto e = feature;
        mutate(std::get<FeatureAuthoringDeclaration>(e.declaration));
        return !valid(e);
    };
    auto reject_entry = [&](auto mutate) {
        auto e = feature;
        mutate(e);
        return !valid(e);
    };
    CHECK(reject_pipeline([](auto& d) { d.stages[0].id = StageId{}; }));
    CHECK(reject_pipeline([](auto& d) { d.stages[0].name.clear(); }));
    CHECK(reject_pipeline([](auto& d) { d.stages[0].phase = PhaseId{}; }));
    CHECK(reject_pipeline([](auto& d) { d.stages[0].execution_domain = static_cast<ExecutionDomain>(255); }));
    CHECK(reject_pipeline([](auto& d) { d.stages[0].time_policy = static_cast<TimePolicy>(255); }));
    CHECK(reject_pipeline([](auto& d) { d.stages[1].id = d.stages[0].id; }));
    CHECK(reject_pipeline([](auto& d) { d.stages[1].name = d.stages[0].name; }));
    CHECK(reject_pipeline([](auto& d) { d.feature_points[0].id = FeaturePointId{}; }));
    CHECK(reject_pipeline([](auto& d) { d.feature_points[0].name.clear(); }));
    CHECK(reject_pipeline([](auto& d) { d.feature_points[0].domains = 0; }));
    CHECK(reject_pipeline([](auto& d) { d.feature_points[0].scopes = 0; }));
    CHECK(reject_pipeline([](auto& d) { d.feature_points[0].anchor_stage = StageId(999); }));
    CHECK(reject_pipeline([](auto& d) { d.feature_points[0].position = static_cast<FeaturePointPosition>(255); }));
    CHECK(reject_pipeline([](auto& d) { d.feature_points[0].domains = 0x80000000; }));
    CHECK(reject_pipeline([](auto& d) { d.feature_points[0].scopes = 0x80000000; }));
    CHECK(reject_pipeline([](auto& d) { d.feature_points[0].flags = static_cast<FeaturePointFlags>(0x80000000); }));
    CHECK(reject_pipeline([](auto& d) {
        auto point = d.feature_points[0];
        point.name = "Other";
        d.feature_points.push_back(point);
    }));
    CHECK(reject_pipeline([](auto& d) {
        auto point = d.feature_points[0];
        point.id = FeaturePointId(2);
        d.feature_points.push_back(point);
    }));
    CHECK(reject_pipeline([](auto& d) { d.artifacts[0].id = ArtifactId{}; }));
    CHECK(reject_pipeline([](auto& d) { d.artifacts[0].name.clear(); }));
    CHECK(reject_pipeline([](auto& d) { d.artifacts[0].type_id = ArtifactTypeId{}; }));
    CHECK(reject_pipeline([](auto& d) { d.artifacts[0].version = 0; }));
    CHECK(reject_pipeline([](auto& d) { d.artifacts[0].owner_scope = static_cast<PipelineScope>(255); }));
    CHECK(reject_pipeline([](auto& d) { d.artifacts[0].visible_to = 0x80000000; }));
    CHECK(reject_pipeline([](auto& d) { d.artifacts[0].storage = static_cast<ArtifactStorage>(255); }));
    CHECK(reject_pipeline([](auto& d) { d.artifacts[0].lifetime = static_cast<ArtifactLifetime>(255); }));
    CHECK(reject_pipeline([](auto& d) { d.artifacts[0].ownership = static_cast<OwnershipPolicy>(255); }));
    CHECK(reject_pipeline([](auto& d) {
        auto a = d.artifacts[0];
        a.name = "Other";
        d.artifacts.push_back(a);
    }));
    CHECK(reject_pipeline([](auto& d) {
        auto a = d.artifacts[0];
        a.id = ArtifactId(2);
        d.artifacts.push_back(a);
    }));
    CHECK(reject_pipeline([](auto& d) { d.fixed_orderings[0].before = StageId(999); }));
    CHECK(reject_pipeline([](auto& d) { d.fixed_orderings[0].after = StageId(999); }));
    CHECK(reject_pipeline([](auto& d) { d.fixed_orderings[0].after = StageId(1); }));
    CHECK(reject_pipeline([](auto& d) { d.fixed_orderings.push_back(d.fixed_orderings[0]); }));
    CHECK(reject_pipeline([](auto& d) { d.fixed_orderings.push_back({StageId(2), StageId(1)}); }));
    CHECK(reject_pipeline([](auto& d) { d.default_features[0].node_id.clear(); }));
    CHECK(reject_pipeline([](auto& d) { d.default_features[0].type = FeatureId{}; }));
    CHECK(reject_pipeline([](auto& d) { d.default_features[0].version = Version{}; }));
    CHECK(reject_pipeline([](auto& d) { d.default_features[0].point = FeaturePointId(999); }));
    CHECK(reject_pipeline([](auto& d) { d.default_features.push_back(d.default_features[0]); }));
    CHECK(reject_pipeline([](auto& d) { d.default_features[0].parameters = {{0, true}}; }));
    CHECK(reject_pipeline([](auto& d) { d.default_features[0].parameters = {{1, NAN}}; }));
    CHECK(reject_pipeline([](auto& d) { d.default_features[0].parameters = {{1, AssetReference{""}}}; }));
    CHECK(reject_feature([](auto& d) { d.stages[0].execution_domain = static_cast<ExecutionDomain>(255); }));
    CHECK(reject_feature([](auto& d) { d.stages[0].time_policy = static_cast<TimePolicy>(255); }));
    CHECK(reject_feature([](auto& d) { d.stages[1].id = d.stages[0].id; }));
    CHECK(reject_feature([](auto& d) { d.stages[1].name = d.stages[0].name; }));
    CHECK(reject_feature([](auto& d) { d.artifacts[0].version = 0; }));
    CHECK(reject_pipeline([](auto& d) {
        d.artifacts[0].owner_scope = PipelineScope::kWorld;
        d.artifacts[0].visible_to = to_mask(PipelineScope::kEngine);
    }));
    CHECK(reject_feature([](auto& d) {
        d.artifacts[0].owner_scope = PipelineScope::kFrame;
        d.artifacts[0].visible_to = to_mask(PipelineScope::kWorld);
    }));
    CHECK(reject_feature([](auto& d) { d.artifacts[0].storage = static_cast<ArtifactStorage>(255); }));
    CHECK(reject_feature([](auto& d) {
        auto a = d.artifacts[0];
        a.name = "Other";
        d.artifacts.push_back(a);
    }));
    CHECK(reject_feature([](auto& d) {
        auto a = d.artifacts[0];
        a.id = ArtifactId(2);
        d.artifacts.push_back(a);
    }));
    CHECK(reject_feature([](auto& d) { d.parts[0].name.clear(); }));
    CHECK(reject_feature([](auto& d) { d.parts[0].domain = FeatureDomain::kComposite; }));
    CHECK(reject_feature([](auto& d) {
        auto p = d.parts[0];
        p.name = "Other";
        d.parts.push_back(p);
    }));
    CHECK(reject_feature([](auto& d) {
        auto p = d.parts[0];
        p.part_index = 1;
        d.parts.push_back(p);
    }));
    CHECK(reject_feature([](auto& d) { d.default_passes[0].node_id.clear(); }));
    CHECK(reject_feature([](auto& d) { d.default_passes[0].type = RenderPassTypeId{}; }));
    CHECK(reject_feature([](auto& d) { d.default_passes[0].version = Version{}; }));
    CHECK(reject_feature([](auto& d) { d.default_passes[0].part_id = "missing"; }));
    CHECK(reject_feature([](auto& d) { d.default_passes[0].part_id.clear(); }));
    CHECK(reject_feature([](auto& d) { d.parts[0].domain = FeatureDomain::kLogic; }));
    CHECK(reject_feature([](auto& d) { d.default_passes[0].type = RenderPassTypeId(999); }));
    CHECK(reject_feature([](auto& d) { d.default_passes.push_back(d.default_passes[0]); }));
    CHECK(reject_feature([](auto& d) { d.default_passes[0].parameters = {{0, true}}; }));
    CHECK(reject_feature([](auto& d) { d.default_passes[0].parameters = {{1, std::vector<double>{NAN}}}; }));
    CHECK(reject_feature([](auto& d) { d.default_passes[0].parameters = {{1, AssetReference{"invalid"}}}; }));
    CHECK(reject_feature([](auto& d) { d.default_resources[0].node_id.clear(); }));
    CHECK(reject_feature([](auto& d) { d.default_resources[0].resource_id = RenderResourceId{}; }));
    CHECK(reject_feature([](auto& d) { d.default_resources[0].constraints.resource_class = ResourceClass::kNone; }));
    CHECK(reject_feature(
        [](auto& d) { d.default_resources[0].constraints.resource_class = static_cast<ResourceClass>(255); }));
    CHECK(reject_feature([](auto& d) { d.default_resources[0].constraints.capability_mask = 1ull << 63; }));
    CHECK(reject_feature([](auto& d) { d.default_resources[0].constraints.usage_mask = 1ull << 63; }));
    CHECK(reject_feature([](auto& d) { d.default_resources[0].constraints.formats = {""}; }));
    CHECK(reject_feature([](auto& d) { d.default_resources[0].constraints.formats = {"rgba8", "rgba8"}; }));
    CHECK(reject_feature([](auto& d) { d.default_resources[0].node_id = "pass"; }));
    CHECK(reject_feature([](auto& d) {
        auto r = d.default_resources[0];
        r.node_id = "Other";
        d.default_resources.push_back(r);
    }));
    CHECK(reject_feature([](auto& d) {
        auto r = d.default_resources[0];
        r.resource_id = RenderResourceId(3);
        d.default_resources.push_back(r);
    }));
    CHECK(reject_feature([](auto& d) { d.default_edges[0].edge_id.clear(); }));
    CHECK(reject_feature([](auto& d) { d.default_edges[0].binding_id = 0; }));
    CHECK(reject_feature([](auto& d) { d.default_edges[0].binding_id = 999; }));
    CHECK(reject_feature([](auto& d) {
        auto e = d.default_edges[0];
        e.binding_id = 1;
        d.default_edges.push_back(e);
    }));
    CHECK(reject_feature([](auto& d) {
        auto e = d.default_edges[0];
        e.edge_id = "Other";
        d.default_edges.push_back(e);
    }));
    CHECK(reject_feature([](auto& d) { d.allowed_pass_types[0] = RenderPassTypeId{}; }));
    CHECK(reject_feature([](auto& d) { d.allowed_pass_types.push_back(d.allowed_pass_types[0]); }));
    CHECK(reject_feature([](auto& d) { d.allowed_pass_types.clear(); }));
    auto invalid_pass = pass;
    std::get<PassAuthoringDeclaration>(invalid_pass.declaration).allowed_queue_mask = 1ull << 63;
    CHECK(!valid(invalid_pass));
    invalid_pass = pass;
    std::get<PassAuthoringDeclaration>(invalid_pass.declaration).type = RenderPassTypeId{};
    CHECK(!valid(invalid_pass));
    feature.ports = {{.id = 1,
                      .name = "Color",
                      .value_kind = PortValueKind::kTexture,
                      .schema_id = 1,
                      .binding_id = 2,
                      .resource_constraints = {.resource_class = ResourceClass::kTexture}}};
    CHECK(valid(feature));
    auto alternate_port = feature;
    alternate_port.ports[0].binding_id = 3;
    alternate_port.ports[0].value_kind = PortValueKind::kBuffer;
    alternate_port.ports[0].resource_constraints.resource_class = ResourceClass::kBuffer;
    CHECK(valid(alternate_port));
    alternate_port.ports[0].value_kind = PortValueKind::kRenderResource;
    CHECK(valid(alternate_port));
    alternate_port.ports[0].value_kind = PortValueKind::kArtifact;
    alternate_port.ports[0].resource_constraints = {};
    CHECK(valid(alternate_port));
    alternate_port.ports[0].value_kind = PortValueKind::kRenderState;
    CHECK(valid(alternate_port));
    auto single_render = feature;
    auto& single_declaration = std::get<FeatureAuthoringDeclaration>(single_render.declaration);
    single_declaration.parts.clear();
    single_declaration.default_passes[0].part_id.clear();
    CHECK(valid(single_render));
    single_render.domains = 0;
    CHECK(valid(single_render));
    single_render.domains = to_mask(FeatureDomain::kLogic);
    CHECK(!valid(single_render));
    TypeCatalog name_catalog;
    CHECK(name_catalog.add(feature));
    auto duplicate_type_name = feature;
    duplicate_type_name.key.id = 999;
    CHECK(!name_catalog.add(duplicate_type_name));
    CHECK(reject_entry([](auto& d) { d.ports[0].id = 0; }));
    CHECK(reject_entry([](auto& d) { d.ports[0].binding_id = 0; }));
    CHECK(reject_entry([](auto& d) { d.ports[0].required = true; }));
    CHECK(reject_entry([](auto& d) { d.ports[0].name.clear(); }));
    CHECK(reject_entry([](auto& d) { d.ports[0].schema_id = 0; }));
    CHECK(reject_entry([](auto& d) { d.ports[0].schema_version = 0; }));
    CHECK(reject_entry([](auto& d) { d.ports[0].direction = static_cast<PortDirection>(255); }));
    CHECK(reject_entry([](auto& d) { d.ports[0].value_kind = static_cast<PortValueKind>(255); }));
    CHECK(reject_entry([](auto& d) { d.ports[0].min_connections = 2; }));
    CHECK(reject_entry([](auto& d) { d.ports[0].resource_constraints.resource_class = ResourceClass::kBuffer; }));
    CHECK(reject_entry([](auto& d) { d.ports[0].resource_constraints.resource_class = ResourceClass::kNone; }));
    CHECK(reject_entry([](auto& d) { d.ports[0].value_kind = PortValueKind::kArtifact; }));
    CHECK(reject_entry([](auto& d) { d.ports[0].value_kind = PortValueKind::kRenderState; }));
    CHECK(reject_entry([](auto& d) { d.ports[0].value_kind = PortValueKind::kBuffer; }));
    CHECK(reject_entry([](auto& d) { d.ports[0].resource_constraints.capability_mask = 1ull << 63; }));
    CHECK(reject_entry([](auto& d) { d.ports[0].resource_constraints.usage_mask = 1ull << 63; }));
    CHECK(reject_entry([](auto& d) {
        auto p = d.ports[0];
        p.name = "Other";
        d.ports.push_back(p);
    }));
    CHECK(reject_entry([](auto& d) {
        auto p = d.ports[0];
        p.id = 2;
        d.ports.push_back(p);
    }));
    return 0;
}

int default_edge_validation_cases() {
    using namespace aeg::pipeline;
    static_assert(std::same_as<DefaultEdge, DocumentEdge>);
    TypeCatalogEntry entry;
    entry.key = {AuthoringTypeKind::kFeature, 10};
    entry.version = {1, 0, 0};
    entry.owner_module = ModuleId(1);
    entry.name = "ConnectedFeature";
    entry.display_name = "Connected Feature";
    entry.domains = to_mask(FeatureDomain::kRender);
    FeatureAuthoringDeclaration declaration;
    declaration.default_passes = {{.node_id = "producer", .type = RenderPassTypeId(1)},
                                  {.node_id = "consumer", .type = RenderPassTypeId(1)},
                                  {.node_id = "other", .type = RenderPassTypeId(1)}};
    declaration.allowed_pass_types = {RenderPassTypeId(1)};
    declaration.default_resources = {{.node_id = "color",
                                      .resource_id = RenderResourceId(2),
                                      .constraints = {.resource_class = ResourceClass::kTexture}}};
    declaration.default_edges = {{.edge_id = "write",
                                  .kind = DocumentEdgeKind::kRenderResource,
                                  .source = {"producer", 7},
                                  .destination = {"color", 2},
                                  .binding_id = 2},
                                 {.edge_id = "read",
                                  .kind = DocumentEdgeKind::kRenderResource,
                                  .source = {"color", 2},
                                  .destination = {"consumer", 9},
                                  .binding_id = 2}};
    entry.declaration = declaration;
    auto valid = [](const auto& value) {
        TypeCatalog catalog;
        return static_cast<bool>(catalog.add(value));
    };
    auto reject = [&](auto mutate) {
        auto invalid = entry;
        mutate(std::get<FeatureAuthoringDeclaration>(invalid.declaration));
        return !valid(invalid);
    };
    // One writer and multiple readers of a binding are a valid default graph.
    CHECK(valid(entry));
    auto fanout = entry;
    std::get<FeatureAuthoringDeclaration>(fanout.declaration)
        .default_edges.push_back({.edge_id = "read-again",
                                  .kind = DocumentEdgeKind::kRenderResource,
                                  .source = {"color", 2},
                                  .destination = {"other", 9},
                                  .binding_id = 2});
    CHECK(valid(fanout));
    CHECK(reject([](auto& d) { d.default_edges[0].source.node_id.clear(); }));
    CHECK(reject([](auto& d) { d.default_edges[0].destination.node_id.clear(); }));
    CHECK(reject([](auto& d) { d.default_edges[0].source.node_id = "missing"; }));
    CHECK(reject([](auto& d) { d.default_edges[0].destination.node_id = "missing"; }));
    CHECK(reject([](auto& d) { d.default_edges[0].source.port_id = 0; }));
    CHECK(reject([](auto& d) { d.default_edges[0].destination.port_id = 999; }));
    CHECK(reject([](auto& d) { d.default_edges[1].source.port_id = 999; }));
    CHECK(reject([](auto& d) { d.default_edges[1].destination.port_id = 0; }));
    CHECK(reject([](auto& d) { d.default_edges[0].kind = static_cast<DocumentEdgeKind>(255); }));
    CHECK(reject([](auto& d) { d.default_edges[0].kind = DocumentEdgeKind::kArtifact; }));
    CHECK(reject([](auto& d) {
        auto edge = d.default_edges[0];
        edge.edge_id = "duplicate-write";
        d.default_edges.push_back(edge);
    }));
    CHECK(reject([](auto& d) {
        auto edge = d.default_edges[0];
        edge.edge_id = "second-producer";
        edge.source = {"other", 7};
        d.default_edges.push_back(edge);
    }));
    // Different destination still cannot create two writers for one binding.
    CHECK(reject([](auto& d) {
        auto edge = d.default_edges[0];
        edge.edge_id = "second-producer-other-destination";
        edge.source = {"other", 7};
        edge.destination = {"consumer", 10};
        d.default_edges.push_back(edge);
    }));
    // A port has one direction; it cannot both consume and produce the same graph value.
    CHECK(reject([](auto& d) { d.default_edges[1].destination = {"producer", 7}; }));
    auto ordering = entry;
    std::get<FeatureAuthoringDeclaration>(ordering.declaration).default_edges = {{.edge_id = "order",
                                                                                  .kind = DocumentEdgeKind::kOrdering,
                                                                                  .source = {"producer", 0},
                                                                                  .destination = {"consumer", 0}}};
    CHECK(valid(ordering));
    std::get<FeatureAuthoringDeclaration>(ordering.declaration).default_edges[0].binding_id = 2;
    CHECK(!valid(ordering));
    auto typed_binding = entry;
    auto& typed_declaration = std::get<FeatureAuthoringDeclaration>(typed_binding.declaration);
    typed_declaration.artifacts = {{.id = ArtifactId(2), .name = "Data", .type_id = ArtifactTypeId(8)}};
    typed_declaration.default_edges.push_back({.edge_id = "artifact",
                                               .kind = DocumentEdgeKind::kArtifact,
                                               .source = {"producer", 20},
                                               .destination = {"other", 21},
                                               .binding_id = 2});
    CHECK(valid(typed_binding));  // Artifact and resource identities occupy separate kinds.
    typed_declaration.default_edges.back().binding_id = 99;
    CHECK(!valid(typed_binding));
    return 0;
}

int main() {
    CHECK(default_edge_validation_cases() == 0);
    CHECK(declaration_validation_cases() == 0);
    using namespace aeg::pipeline;
    TypeCatalog catalog;
    TypeCatalogEntry entry;
    entry.key = {AuthoringTypeKind::kFeature, FeatureId("Game.Outline").value()};
    entry.version = {1, 0, 0};
    entry.owner_module = ModuleId("Game.Main");
    entry.name = "Game.Outline";
    entry.display_name = "Outline";
    entry.domains = to_mask(FeatureDomain::kRender);
    entry.scopes = to_mask(PipelineScope::kFrame);
    entry.parameters = {{.id = 1,
                         .name = "Width",
                         .display_name = "Width",
                         .kind = ParameterKind::kFloat64,
                         .default_value = 1.0,
                         .minimum = 0.0,
                         .maximum = 8.0,
                         .schema_version = 1}};
    entry.ports = {{.id = 1,
                    .name = "Color",
                    .direction = PortDirection::kOutput,
                    .value_kind = PortValueKind::kRenderResource,
                    .schema_id = 1,
                    .schema_version = 1,
                    .required = true,
                    .min_connections = 1,
                    .max_connections = 1,
                    .binding_id = 42,
                    .resource_constraints = {.resource_class = ResourceClass::kTexture}}};
    entry.declaration = FeatureAuthoringDeclaration{};
    CHECK(catalog.add(entry));
    entry.name = "Changed";
    CHECK(catalog.find(entry.key)->name == "Game.Outline");
    CHECK(!catalog.add(entry));
    const std::array schema{entry.parameters.front()};
    CHECK(validate_parameter_block(ParameterBlock{{1, 2.0}}, schema));
    CHECK(!validate_parameter_block(ParameterBlock{{1, std::string("wide")}}, schema));
    CHECK(!validate_parameter_block(ParameterBlock{{1, 9.0}}, schema));
    const std::array scalar_schema{ParameterDescriptor{.id = 10,
                                                       .name = "Enabled",
                                                       .display_name = "Enabled",
                                                       .kind = ParameterKind::kBool,
                                                       .default_value = false},
                                   ParameterDescriptor{.id = 11,
                                                       .name = "Count",
                                                       .display_name = "Count",
                                                       .kind = ParameterKind::kInt64,
                                                       .default_value = int64_t{0}},
                                   ParameterDescriptor{.id = 12,
                                                       .name = "Flags",
                                                       .display_name = "Flags",
                                                       .kind = ParameterKind::kUInt64,
                                                       .default_value = uint64_t{0}},
                                   ParameterDescriptor{.id = 13,
                                                       .name = "Text",
                                                       .display_name = "Text",
                                                       .kind = ParameterKind::kString,
                                                       .default_value = std::string{}}};
    CHECK(validate_parameter_block({{10, true}, {11, int64_t{-5}}, {12, uint64_t{5}}, {13, std::string("hello")}},
                                   scalar_schema));
    CHECK(!validate_parameter_block({{10, uint64_t{1}}}, scalar_schema));
    CHECK(!validate_parameter_block({{11, uint64_t{1}}}, scalar_schema));
    CHECK(!validate_parameter_block({{12, int64_t{1}}}, scalar_schema));
    CHECK(!validate_parameter_block({{13, true}}, scalar_schema));
    ParameterDescriptor mode{.id = 2,
                             .name = "Mode",
                             .display_name = "Mode",
                             .kind = ParameterKind::kEnum,
                             .default_value = std::string("solid"),
                             .enum_values = {"solid", "wire"},
                             .schema_version = 1};
    ParameterDescriptor asset{.id = 3,
                              .name = "Asset",
                              .display_name = "Asset",
                              .kind = ParameterKind::kAsset,
                              .default_value = AssetReference{"12AB5678-1234-1234-1234-123456789abc"},
                              .schema_version = 1};
    ParameterDescriptor vector{.id = 4,
                               .name = "Offset",
                               .display_name = "Offset",
                               .kind = ParameterKind::kVector,
                               .default_value = std::vector<double>{0.0, 0.0, 0.0},
                               .schema_version = 1};
    auto invalid_enum = mode;
    invalid_enum.enum_values.push_back("");
    CHECK(!validate_parameter_block({}, std::array{invalid_enum}));
    invalid_enum = mode;
    invalid_enum.enum_values.push_back("wire");
    CHECK(!validate_parameter_block({}, std::array{invalid_enum}));
    auto duplicate_name = mode;
    duplicate_name.id = 99;
    CHECK(!validate_parameter_block({}, std::array{mode, duplicate_name}));
    CHECK(validate_parameter_block({{1, 0.0}}, schema));
    CHECK(validate_parameter_block({{1, 8.0}}, schema));
    auto bounds = schema[0];
    bounds.default_value = 0.0;
    CHECK(validate_parameter_block({}, std::array{bounds}));
    bounds.default_value = 8.0;
    CHECK(validate_parameter_block({}, std::array{bounds}));
    bounds.default_value = -1.0;
    CHECK(!validate_parameter_block({}, std::array{bounds}));
    bounds = schema[0];
    bounds.minimum = 9.0;
    CHECK(!validate_parameter_block({}, std::array{bounds}));
    bounds = schema[0];
    bounds.minimum = NAN;
    CHECK(!validate_parameter_block({}, std::array{bounds}));
    bounds = schema[0];
    bounds.maximum = INFINITY;
    CHECK(!validate_parameter_block({}, std::array{bounds}));
    bounds = schema[0];
    bounds.default_value = NAN;
    CHECK(!validate_parameter_block({}, std::array{bounds}));
    auto invalid_parameter = mode;
    invalid_parameter.kind = static_cast<ParameterKind>(255);
    CHECK(!validate_parameter_block({}, std::array{invalid_parameter}));
    invalid_parameter = mode;
    invalid_parameter.schema_version = 0;
    CHECK(!validate_parameter_block({}, std::array{invalid_parameter}));
    CHECK(!validate_parameter_block({}, std::array{mode, mode}));
    const std::array extended_schema{mode, asset, vector};
    CHECK(validate_parameter_block(ParameterBlock{{2, std::string("wire")},
                                                  {3, AssetReference{"12345678-1234-1234-1234-123456789abc"}},
                                                  {4, std::vector<double>{1.0, 2.0, 3.0}}},
                                   extended_schema));
    for (const auto uuid :
         {"", " ", "mesh-uuid", "12345678_1234-1234-1234-123456789abc", "12345678-1234-1234-1234-123456789abg"}) {
        auto invalid_asset = asset;
        invalid_asset.default_value = AssetReference{uuid};
        CHECK(!validate_parameter_block({}, std::array{invalid_asset}));
        CHECK(!validate_parameter_block({{3, AssetReference{uuid}}}, extended_schema));
    }
    CHECK(!validate_parameter_block(ParameterBlock{{2, std::string("unknown")}}, extended_schema));
    CHECK(!validate_parameter_block(ParameterBlock{{4, std::vector<double>{NAN}}}, extended_schema));
    CHECK(!validate_parameter_block(ParameterBlock{{99, true}}, extended_schema));
    TypeCatalogEntry first = entry;
    first.key = {AuthoringTypeKind::kPipeline, PipelineId("Game.A").value()};
    first.name = "Game.A";
    first.declaration = PipelineAuthoringDeclaration{};
    TypeCatalogEntry second = first;
    second.key = {AuthoringTypeKind::kRenderPass, RenderPassTypeId("Game.Pass").value()};
    second.name = "Game.Pass";
    second.declaration = PassAuthoringDeclaration{.type = RenderPassTypeId("Game.Pass"), .allowed_queue_mask = 1};
    CHECK(catalog.add(second));
    CHECK(catalog.add(first));
    const auto entries = catalog.entries();
    CHECK(entries.size() == 3);
    CHECK(entries[0].key < entries[1].key);
    CHECK(entries[1].key < entries[2].key);
    TypeCatalogEntry zero_domains = first;
    zero_domains.key.id = PipelineId("Game.ZeroDomains").value();
    zero_domains.name = "Game.ZeroDomains";
    zero_domains.domains = 0;
    CHECK(catalog.add(zero_domains));
    TypeCatalogEntry mismatched_pass = second;
    mismatched_pass.key.id = RenderPassTypeId("Game.Other.Pass").value();
    mismatched_pass.name = "Game.Other.Pass";
    CHECK(!catalog.add(mismatched_pass));
    TypeCatalogEntry invalid_default = entry;
    invalid_default.key.id = FeatureId("Game.InvalidDefault").value();
    invalid_default.name = "Game.InvalidDefault";
    invalid_default.parameters.front().default_value = 9.0;
    CHECK(!catalog.add(invalid_default));
    PipelineDocument document;
    document.schema_version = 1;
    document.asset_uuid = "document-uuid";
    document.name = "Game Pipeline";
    document.module_requirements.push_back(ModuleId("Game.Main"));
    document.pipeline_nodes.push_back({.node_id = "pipeline",
                                       .type = PipelineId("Game.A"),
                                       .version = {1, 0, 0},
                                       .scope = PipelineScope::kFrame,
                                       .owner_binding = "world"});
    document.feature_nodes.push_back({.node_id = "feature",
                                      .type = FeatureId("Game.Outline"),
                                      .version = {1, 0, 0},
                                      .pipeline_node = "pipeline",
                                      .point = FeaturePointId("Render.Outline"),
                                      .enabled = true});
    document.pass_nodes.push_back({.node_id = "pass",
                                   .type = RenderPassTypeId("Game.Pass"),
                                   .version = {1, 0, 0},
                                   .feature_node = "feature",
                                   .part_id = "render",
                                   .enabled = true});
    document.resources.push_back({.node_id = "color", .resource_id = RenderResourceId("SceneColor")});
    document.edges.push_back({.edge_id = "edge",
                              .kind = DocumentEdgeKind::kRenderResource,
                              .source = {"pass", 1},
                              .destination = {"feature", 1},
                              .binding_id = 42});
    document.resources.front().parameters = {{1, std::string("SceneColor")}};
    document.layouts.push_back({.node_id = "pass", .x = 10.0, .y = 20.0, .expanded = true});
    document.extensions.push_back({.owner_node = "pass", .encoding = OpaqueAuthoringEncoding::kJson, .payload = "{}"});
    CHECK(document.pipeline_nodes.size() == 1 && document.extensions.size() == 1);
    return 0;
}
