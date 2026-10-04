#include <algorithm>
#include <iostream>
#include <limits>

#include "editor_composition_test_support.hpp"

namespace {
using namespace aeg::pipeline;

int failures{};
void check(bool ok, const char* name) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << name << '\n';
    }
}
bool has_code(const std::vector<DocumentDiagnostic>& diagnostics, std::string_view code, uint64_t slot = 0) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const DocumentDiagnostic& diagnostic) {
        return diagnostic.code == code && (slot == 0 || diagnostic.slot_id == slot);
    });
}

struct Fixture {
    TypeCatalog catalog{};
    PipelineDocument document{};
    Fixture() {
        aeg::tests::EditorCompositionFixture base;
        catalog = std::move(base.catalog);
        document = std::move(base.document);
        TypeCatalog replacement;
        for (auto entry : catalog.entries()) {
            if (entry.key.kind == AuthoringTypeKind::kFeature) {
                PortDescriptor asset = entry.ports.front();
                asset.id = 23;
                asset.name = "TextureAsset";
                asset.required = false;  // The explicit contract is authoritative.
                asset.input = InputPortContract{.allowed_sources = input_source_mask(InputSourceKind::kAsset),
                                                 .presence = InputPresenceRequirement::kRequired};
                entry.ports.push_back(asset);
                PortDescriptor gain = asset;
                gain.id = 24;
                gain.name = "Gain";
                gain.value_kind = PortValueKind::kArtifact;
                gain.resource_constraints = {};
                gain.input = InputPortContract{
                    .allowed_sources = input_source_mask(InputSourceKind::kLiteral) |
                                       input_source_mask(InputSourceKind::kDefault) |
                                       input_source_mask(InputSourceKind::kContext),
                    .presence = InputPresenceRequirement::kOptional,
                    .literal_schema = ParameterDescriptor{.id = 24, .name = "Gain", .display_name = "Gain",
                                                           .kind = ParameterKind::kFloat64, .default_value = 1.0,
                                                           .minimum = 0.0, .maximum = 2.0},
                    .default_value = InputDefaultDescriptor{.id = 7, .revision = 5, .value = 1.0},
                    .context_keys = {"Exposure"}};
                entry.ports.push_back(gain);
                PortDescriptor layer = asset;
                layer.id = 25;
                layer.name = "Layer";
                layer.input = InputPortContract{.allowed_sources = input_source_mask(InputSourceKind::kEdge) |
                                                                   input_source_mask(InputSourceKind::kAsset),
                                                 .presence = InputPresenceRequirement::kRequired,
                                                 .min_edges = 1, .max_edges = 1, .variadic_group_id = 100};
                entry.ports.push_back(layer);
                entry.variadic_inputs = {{.id = 100, .name = "Layers", .member_port_id = 25,
                                          .min_slots = 0, .max_slots = 3, .ordered = true, .empty_is_valid = true}};
            }
            if (!replacement.add(entry))
                throw std::runtime_error("FlowGraph fixture contract rejected");
        }
        catalog = std::move(replacement);
        document.input_bindings = {{.node_id = "feature", .port_id = 23, .source = InputSourceKind::kAsset,
                                     .asset = AssetReference{"00000000-0000-0000-0000-000000000011"},
                                     .content_revision = 3, .status = InputBindingStatus::kBound}};
    }
    std::vector<DocumentDiagnostic> diagnostics() const { return validate_pipeline_document(document, catalog); }
};

void sources() {
    Fixture fixture;
    check(fixture.diagnostics().empty(), "required Asset input accepts no edge");
    fixture.document.input_bindings.clear();
    check(has_code(fixture.diagnostics(), "input.required"), "missing required source rejected");

    fixture = Fixture{};
    fixture.document.input_bindings[0].source = InputSourceKind::kNone;
    check(has_code(fixture.diagnostics(), "input.required"), "None does not satisfy required input");
    fixture = Fixture{};
    fixture.document.input_bindings[0].asset.uuid = "invalid";
    check(has_code(fixture.diagnostics(), "input.asset"), "bad AssetId rejected");
    fixture = Fixture{};
    fixture.document.input_bindings[0].asset.uuid = "00000000-0000-0000-0000-000000000000";
    check(has_code(fixture.diagnostics(), "input.asset"), "null AssetId rejected");
    fixture = Fixture{};
    fixture.document.input_bindings[0].source = InputSourceKind::kLiteral;
    check(has_code(fixture.diagnostics(), "input.source"), "source whitelist enforced");
    fixture = Fixture{};
    fixture.document.input_bindings.push_back(fixture.document.input_bindings[0]);
    check(has_code(fixture.diagnostics(), "input.duplicate"), "duplicate active sources rejected");

    for (const auto status : {InputBindingStatus::kPending, InputBindingStatus::kInvalid,
                              InputBindingStatus::kCancelled}) {
        fixture = Fixture{};
        fixture.document.input_bindings[0].status = status;
        const char* code = status == InputBindingStatus::kPending ? "input.pending" :
                           status == InputBindingStatus::kInvalid ? "input.invalid" : "input.cancelled";
        check(has_code(fixture.diagnostics(), code), "unready binding never considered Ready");
    }
    fixture = Fixture{};
    fixture.document.input_bindings.push_back({.node_id = "feature", .port_id = 24, .source = InputSourceKind::kNone});
    check(fixture.diagnostics().empty(), "optional None independent of source whitelist");
    fixture.document.input_bindings.back().source = InputSourceKind::kLiteral;
    fixture.document.input_bindings.back().literal = 1.5;
    check(has_code(fixture.diagnostics(), "input.unset"), "active source must transition through Bound");
    fixture.document.input_bindings.back().status = InputBindingStatus::kBound;
    fixture.document.input_bindings.back().literal = std::string("1");
    check(has_code(fixture.diagnostics(), "input.literal"), "Literal must match value schema");
    fixture.document.input_bindings.back().literal = 1.5;
    check(fixture.diagnostics().empty(), "typed Literal accepted");
    fixture.document.input_bindings.back().literal = 3.0;
    check(has_code(fixture.diagnostics(), "input.literal"), "Literal bounds enforced");
    fixture.document.input_bindings.back().literal = std::numeric_limits<double>::infinity();
    check(has_code(fixture.diagnostics(), "input.literal"), "nonfinite Literal rejected");
    fixture.document.input_bindings.back() = {.node_id = "feature", .port_id = 24,
                                              .source = InputSourceKind::kDefault, .default_id = 7,
                                              .default_revision = 4, .status = InputBindingStatus::kBound};
    check(has_code(fixture.diagnostics(), "input.default"), "stale default revision rejected");
    fixture.document.input_bindings.back().default_revision = 5;
    check(fixture.diagnostics().empty(), "contract default identity accepted");
    fixture.document.input_bindings.back() = {.node_id = "feature", .port_id = 24,
                                              .source = InputSourceKind::kContext, .context_key = "Other"};
    fixture.document.input_bindings.back().status = InputBindingStatus::kBound;
    check(has_code(fixture.diagnostics(), "input.context"), "undeclared Context key rejected");
    fixture.document.input_bindings.back().context_key = "Exposure";
    check(fixture.diagnostics().empty(), "declared Context accepted");
    fixture.document.input_bindings.back().port_id = 99;
    check(has_code(fixture.diagnostics(), "input.port"), "undeclared port rejected");
    fixture.document.input_bindings.back().node_id = "missing";
    check(has_code(fixture.diagnostics(), "input.node"), "unknown node binding rejected");

    fixture = Fixture{};
    fixture.document.resources.push_back({.node_id = "feature/ExternalTexture",
                                          .resource_id = RenderResourceId("Game.ExternalTexture"),
                                          .type = ArtifactTypeId("Pipeline.DefaultTexture"),
                                          .version = 1,
                                          .owner_scope = PipelineScope::kFrame,
                                          .visible_to = to_mask(PipelineScope::kFrame),
                                          .external_input = true,
                                          .storage = ArtifactStorage::kGpuLogicalResource,
                                          .lifetime = ArtifactLifetime::kFrame,
                                          .ownership = OwnershipPolicy::kGraphOwned,
                                          .resource_constraints = {.resource_class = ResourceClass::kTexture}});
    fixture.document.input_bindings.push_back({.node_id = "feature/ExternalTexture",
                                               .port_id = fixture.document.resources.back().resource_id.value(),
                                               .source = InputSourceKind::kAsset,
                                               .asset = AssetReference{"00000000-0000-0000-0000-000000000013"},
                                               .status = InputBindingStatus::kBound});
    check(fixture.diagnostics().empty(), "external resource Asset source accepts no upstream node");
    fixture.document.input_bindings.back().source = InputSourceKind::kLiteral;
    check(has_code(fixture.diagnostics(), "input.source"), "resource input rejects undeclared Literal source");
}

void slots_and_edges() {
    Fixture fixture;
    fixture.document.variadic_slots = {{.node_id = "feature", .group_id = 100, .slot_id = 301, .order = 0},
                                       {.node_id = "feature", .group_id = 100, .slot_id = 302, .order = 1}};
    fixture.document.edges.push_back({.edge_id = "layer", .kind = DocumentEdgeKind::kRenderResource,
                                      .source = {"pass", 11}, .destination = {"feature", 25, 301}, .binding_id = 77});
    fixture.document.input_bindings.push_back({.node_id = "feature", .port_id = 25, .slot_id = 301,
                                               .source = InputSourceKind::kEdge, .edge_ids = {"layer"},
                                               .status = InputBindingStatus::kBound});
    fixture.document.input_bindings.push_back({.node_id = "feature", .port_id = 25, .slot_id = 302,
                                               .source = InputSourceKind::kAsset,
                                               .asset = AssetReference{"00000000-0000-0000-0000-000000000012"},
                                               .status = InputBindingStatus::kBound});
    check(fixture.diagnostics().empty(), "two slots with one edge and one Asset accepted");
    {
        auto unbound_edge = Fixture{};
        unbound_edge.document.variadic_slots = {{.node_id = "feature", .group_id = 100, .slot_id = 301, .order = 0}};
        unbound_edge.document.edges.push_back({.edge_id = "unbound-layer", .kind = DocumentEdgeKind::kRenderResource,
                                               .source = {"pass", 11}, .destination = {"feature", 25, 301},
                                               .binding_id = 77});
        check(has_code(unbound_edge.diagnostics(), "input.binding", 301),
              "every explicit input edge requires an Edge binding");
    }
    {
        auto two_edges = fixture;
        two_edges.document.edges.push_back({.edge_id = "layer2", .kind = DocumentEdgeKind::kRenderResource,
                                            .source = {"pass", 11}, .destination = {"feature", 25, 302}, .binding_id = 77});
        two_edges.document.input_bindings[2] = {.node_id = "feature", .port_id = 25, .slot_id = 302,
                                                .source = InputSourceKind::kEdge, .edge_ids = {"layer2"},
                                                .status = InputBindingStatus::kBound};
        check(two_edges.diagnostics().empty(), "edge limit is per slot, not summed across a variadic group");
    }
    {
        auto orphan = fixture;
        orphan.document.variadic_slots.pop_back();
        check(has_code(orphan.diagnostics(), "input.slot", 302), "non-edge binding cannot create an undeclared slot");
    }
    {
        auto uncovered = fixture;
        uncovered.document.input_bindings[1].edge_ids.clear();
        check(has_code(uncovered.diagnostics(), "input.edge", 301), "all incoming edges must belong to the active source");
        uncovered.document.input_bindings[1] = {.node_id = "feature", .port_id = 25, .slot_id = 301,
                                                .source = InputSourceKind::kAsset,
                                                .asset = AssetReference{"00000000-0000-0000-0000-000000000012"},
                                                .status = InputBindingStatus::kBound};
        check(has_code(uncovered.diagnostics(), "input.source", 301), "asset source with an unlisted incoming edge is rejected");
    }
    std::swap(fixture.document.variadic_slots[0].order, fixture.document.variadic_slots[1].order);
    check(fixture.diagnostics().empty(), "slot sorting preserves SlotId");
    fixture.document.variadic_slots.push_back({.node_id = "feature", .group_id = 100, .slot_id = 303, .order = 2});
    check(has_code(fixture.diagnostics(), "input.required", 303), "new empty required slot is a valid draft error");
    fixture.document.variadic_slots.push_back({.node_id = "feature", .group_id = 100, .slot_id = 304, .order = 3});
    check(has_code(fixture.diagnostics(), "input.multiplicity"), "node slot limit enforced without counting edges");
    fixture.document.variadic_slots.resize(2);
    fixture.document.variadic_slots[1].slot_id = 301;
    check(has_code(fixture.diagnostics(), "input.slot"), "duplicate SlotId rejected");
    fixture.document.variadic_slots[1].slot_id = 302;
    fixture.document.input_bindings[1].edge_ids = {"color"}; // The same type is not the same destination.
    check(has_code(fixture.diagnostics(), "input.edge", 301), "edge must target the bound SlotId");
    fixture.document.input_bindings[1].edge_ids = {"layer"};
    fixture.document.input_bindings[1].source = InputSourceKind::kAsset;
    check(has_code(fixture.diagnostics(), "input.source", 301), "Asset cannot coexist with an active edge");
    fixture.document.input_bindings[1].source = InputSourceKind::kEdge;
    fixture.document.edges.back().destination.slot_id = 999;
    check(has_code(fixture.diagnostics(), "edge.slot"), "edge cannot create an undeclared slot");
    fixture.document.edges.back().destination.slot_id = 301;
    fixture.document.edges.push_back({.edge_id = "extra", .kind = DocumentEdgeKind::kRenderResource,
                                      .source = {"pass", 11}, .destination = {"feature", 25, 301}, .binding_id = 77});
    fixture.document.input_bindings[1].edge_ids.push_back("extra");
    check(has_code(fixture.diagnostics(), "input.multiplicity", 301), "per-slot edge limit enforced");
    fixture.document.edges.pop_back();
    fixture.document.input_bindings[1].edge_ids = {"layer"};
    fixture.document.input_bindings[1].slot_id = 0;
    check(has_code(fixture.diagnostics(), "input.slot"), "Variadic binding cannot omit SlotId");
    fixture.document.input_bindings[1].port_id = 23;
    fixture.document.input_bindings[1].slot_id = 301;
    check(has_code(fixture.diagnostics(), "input.slot"), "fixed port cannot acquire a SlotId");
}

void legacy_migration() {
    Fixture fixture;
    TypeCatalog migration_catalog;
    for (auto entry : fixture.catalog.entries()) {
        if (entry.key.kind == AuthoringTypeKind::kFeature)
            entry.ports.resize(1);
        if (!migration_catalog.add(entry))
            return;
    }
    fixture.document.edges = {{.edge_id = "legacy-flow", .kind = DocumentEdgeKind::kOrdering,
                               .source = {"pass", 0}, .destination = {"feature", 0}, .binding_id = 0}};
    const auto migrated = migrate_legacy_ordering_edges(fixture.document, migration_catalog);
    if (!migrated)
        std::cerr << "migration failure: " << migrated.result.message << '\n';
    check(static_cast<bool>(migrated), "legacy ordering edge migrates when typed ports are unique");
    if (migrated) {
        const auto& edge = migrated.value.edges.front();
        check(edge.kind == DocumentEdgeKind::kRenderResource && edge.source.port_id == 11 &&
                  edge.destination.port_id == 22 && edge.binding_id == 77,
              "legacy migration preserves the typed data contract");
        check(!migrated.value.input_bindings.empty() &&
                  migrated.value.input_bindings.front().source == InputSourceKind::kEdge,
              "legacy migration creates an explicit Edge input binding");
    }
}

void absent_variadic_group() {
    Fixture fixture;
    TypeCatalog constrained;
    for (auto entry : fixture.catalog.entries()) {
        if (entry.key.kind == AuthoringTypeKind::kFeature) {
            entry.variadic_inputs[0].min_slots = 2;
            entry.variadic_inputs[0].empty_is_valid = false;
        }
        check(static_cast<bool>(constrained.add(entry)), "constrained variadic contract is valid");
    }
    check(has_code(validate_pipeline_document(fixture.document, constrained), "input.required"),
          "zero declared slots must still enforce the group's minimum");
}
}  // namespace

int main() {
    try {
        sources();
        slots_and_edges();
        legacy_migration();
        absent_variadic_group();
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
    if (failures)
        return 1;
    std::cout << "FlowGraph input contracts passed\n";
    return 0;
}
