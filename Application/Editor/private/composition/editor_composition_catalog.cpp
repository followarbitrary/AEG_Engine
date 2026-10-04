#include "editor_composition_catalog.hpp"

#include <algorithm>
#include <variant>

namespace aeg::application {
namespace {
using namespace pipeline;

bool compatible(FeatureDomainMask domains, PipelineScopeMask scopes, const AuthoringFeaturePoint& point) {
    return (domains & point.domains) != 0 && (scopes & point.scopes) != 0;
}

const PipelineNodeDocument* find_pipeline(const PipelineDocument& document, std::string_view id) {
    const auto found = std::find_if(document.pipeline_nodes.begin(), document.pipeline_nodes.end(),
                                    [&](const auto& node) { return node.node_id == id; });
    return found == document.pipeline_nodes.end() ? nullptr : &*found;
}

const FeatureNodeDocument* find_feature(const PipelineDocument& document, std::string_view id) {
    const auto found = std::find_if(document.feature_nodes.begin(), document.feature_nodes.end(),
                                    [&](const auto& node) { return node.node_id == id; });
    return found == document.feature_nodes.end() ? nullptr : &*found;
}

const PassNodeDocument* find_pass(const PipelineDocument& document, std::string_view id) {
    const auto found = std::find_if(document.pass_nodes.begin(), document.pass_nodes.end(),
                                    [&](const auto& node) { return node.node_id == id; });
    return found == document.pass_nodes.end() ? nullptr : &*found;
}

void sort_palette(std::vector<EditorPaletteEntry>& entries) {
    std::stable_sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        if (left.label != right.label)
            return left.label < right.label;
        if (left.type.kind != right.type.kind)
            return left.type.kind < right.type.kind;
        return left.type.id < right.type.id;
    });
}
}  // namespace

foundation::ResultValue<std::vector<EditorPaletteEntry>> build_composition_palette(
    const pipeline::PipelineDocument& document, const pipeline::TypeCatalog& catalog,
    const EditorGraphAddress& address) {
    std::vector<EditorPaletteEntry> result;
    if (address.kind == EditorGraphKind::kPipeline) {
        for (const auto& entry : catalog.entries()) {
            if (entry.key.kind == pipeline::AuthoringTypeKind::kPipeline)
                result.push_back({entry.key, entry.display_name, true, {}});
        }
    } else if (address.kind == EditorGraphKind::kFeature) {
        const auto* pipeline_node = find_pipeline(document, address.pipeline_node);
        if (!pipeline_node)
            return {foundation::not_found("Pipeline graph address does not exist"), {}};
        const auto* pipeline_entry =
            catalog.find({pipeline::AuthoringTypeKind::kPipeline, pipeline_node->type.value()});
        if (!pipeline_entry)
            return {foundation::not_found("Pipeline type is unavailable"), {}};
        const auto* declaration = std::get_if<pipeline::PipelineAuthoringDeclaration>(&pipeline_entry->declaration);
        if (!declaration)
            return {foundation::invalid_argument("Pipeline catalog declaration is invalid"), {}};
        for (const auto& entry : catalog.entries()) {
            if (entry.key.kind != pipeline::AuthoringTypeKind::kFeature)
                continue;
            bool allowed = false;
            for (const auto& point : declaration->feature_points)
                allowed |= compatible(entry.domains, entry.scopes, point);
            if (allowed)
                result.push_back({entry.key, entry.display_name, true, {}});
        }
    } else {
        const auto* feature_node = find_feature(document, address.feature_node);
        if (!feature_node)
            return {foundation::not_found("Feature graph address does not exist"), {}};
        const auto* feature_entry = catalog.find({pipeline::AuthoringTypeKind::kFeature, feature_node->type.value()});
        if (!feature_entry)
            return {foundation::not_found("Feature type is unavailable"), {}};
        const auto* declaration = std::get_if<pipeline::FeatureAuthoringDeclaration>(&feature_entry->declaration);
        if (!declaration)
            return {foundation::invalid_argument("Feature catalog declaration is invalid"), {}};
        if (!declaration->allow_add_passes)
            return {foundation::Result::success(), std::move(result)};
        if (address.part_id.empty()) {
            std::size_t render_parts = 0;
            for (const auto& part : declaration->parts)
                render_parts += part.domain == pipeline::FeatureDomain::kRender ? 1U : 0U;
            if (render_parts > 1U || (!declaration->parts.empty() && render_parts == 0U))
                return {foundation::Result::success(), std::move(result)};
        }
        for (const auto type : declaration->allowed_pass_types) {
            const auto* pass = catalog.find({pipeline::AuthoringTypeKind::kRenderPass, type.value()});
            if (pass)
                result.push_back({pass->key, pass->display_name, true, {}});
        }
    }
    sort_palette(result);
    return {foundation::Result::success(), std::move(result)};
}

pipeline::TypeCatalogEntry make_empty_custom_type(pipeline::AuthoringTypeKind kind, std::string_view name_view) {
    using namespace pipeline;
    const std::string name{name_view};
    TypeCatalogEntry entry;
    entry.version = {1, 0, 0};
    entry.owner_module = ModuleId("Editor.FlowGraph");
    entry.name = name;
    const auto separator = name.find_last_of('.');
    entry.display_name = "Empty " + name.substr(separator == std::string::npos ? 0 : separator + 1);
    entry.domains = to_mask(FeatureDomain::kRender);
    entry.scopes = kAllPipelineScopes;
    if (kind == AuthoringTypeKind::kPipeline) {
        const StageId stage_id(name + ".Stage");
        const FeaturePointId point_id(name + ".Point");
        entry.key = {kind, PipelineId(name).value()};
        entry.domains = kAllFeatureDomains;
        entry.declaration = PipelineAuthoringDeclaration{
            .stages = {{.id = stage_id,
                        .name = "Build",
                        .phase = PhaseId("RenderBuild"),
                        .execution_domain = ExecutionDomain::kRenderThread,
                        .time_policy = TimePolicy::kFrame,
                        .editable = true}},
            .feature_points = {{.id = point_id,
                                .name = "Feature Point",
                                .anchor_stage = stage_id,
                                .position = FeaturePointPosition::kAfter,
                                .domains = kAllFeatureDomains,
                                .scopes = kAllPipelineScopes,
                                .flags = FeaturePointFlags::kMayCreateArtifacts |
                                         FeaturePointFlags::kMayCreateGraphicsPass |
                                         FeaturePointFlags::kMayCreateComputePass |
                                         FeaturePointFlags::kMayCreateCopyPass}}};
    } else if (kind == AuthoringTypeKind::kFeature) {
        entry.key = {kind, FeatureId(name).value()};
        entry.declaration = FeatureAuthoringDeclaration{
            .parts = {{.part_index = 0, .domain = FeatureDomain::kRender, .name = "render"}},
            .allow_add_passes = true};
    } else {
        const RenderPassTypeId pass_id(name);
        entry.key = {kind, pass_id.value()};
        entry.declaration = PassAuthoringDeclaration{.type = pass_id, .allowed_queue_mask = 1};
    }
    return entry;
}

foundation::ResultValue<EditorDiagnosticTarget> resolve_composition_diagnostic(
    const pipeline::PipelineDocument& document, const pipeline::DocumentDiagnostic& diagnostic) {
    EditorDiagnosticTarget target;
    target.node_id = diagnostic.node_id;
    target.edge_id = diagnostic.edge_id;
    target.port_id = diagnostic.port_id;
    target.parameter_id = diagnostic.parameter_id;
    if (!diagnostic.node_id.empty()) {
        for (const auto& node : document.pipeline_nodes)
            if (node.node_id == diagnostic.node_id)
                target.graph = {EditorGraphKind::kPipeline, node.node_id, {}, {}};
        for (const auto& node : document.feature_nodes)
            if (node.node_id == diagnostic.node_id)
                target.graph = {EditorGraphKind::kFeature, node.pipeline_node, node.node_id, {}};
        for (const auto& node : document.pass_nodes)
            if (node.node_id == diagnostic.node_id) {
                const auto* feature = find_feature(document, node.feature_node);
                target.graph = {EditorGraphKind::kPass, feature ? feature->pipeline_node : std::string{},
                                node.feature_node, node.part_id};
            }
        for (const auto& node : document.resources) {
            if (node.node_id != diagnostic.node_id)
                continue;
            const auto separator = node.node_id.find('/');
            const std::string owner = separator == std::string::npos ? std::string{} : node.node_id.substr(0, separator);
            const auto feature = find_feature(document, owner);
            target.graph = {EditorGraphKind::kPass, feature ? feature->pipeline_node : std::string{}, owner, {}};
            // Resource cards are intentionally hidden in the strict Pass
            // canvas. Locate the nearest visible Pass so diagnostics still
            // have a useful target instead of selecting an ID that cannot be
            // rendered by the current graph projection.
            const auto pass_for_resource = std::find_if(
                document.edges.begin(), document.edges.end(), [&](const auto& edge) {
                    const std::string_view candidate = edge.source.node_id == node.node_id
                                                            ? edge.destination.node_id
                                                            : edge.destination.node_id == node.node_id
                                                                ? edge.source.node_id
                                                                : std::string_view{};
                    return !candidate.empty() && std::any_of(
                                                     document.pass_nodes.begin(), document.pass_nodes.end(),
                                                     [&](const auto& pass) {
                                                         return pass.node_id == candidate &&
                                                                (feature == nullptr || pass.feature_node == feature->node_id);
                                                     });
                });
            if (pass_for_resource != document.edges.end()) {
                target.node_id = pass_for_resource->source.node_id == node.node_id
                                     ? pass_for_resource->destination.node_id
                                     : pass_for_resource->source.node_id;
                const auto pass = find_pass(document, target.node_id);
                if (pass != nullptr)
                    target.graph = {EditorGraphKind::kPass, feature ? feature->pipeline_node : std::string{},
                                    pass->feature_node, pass->part_id};
            } else if (feature != nullptr) {
                const auto fallback_pass = std::find_if(document.pass_nodes.begin(), document.pass_nodes.end(),
                                                       [&](const auto& pass) {
                                                           return pass.feature_node == feature->node_id;
                                                       });
                if (fallback_pass != document.pass_nodes.end()) {
                    target.node_id = fallback_pass->node_id;
                    target.graph = {EditorGraphKind::kPass, feature->pipeline_node, fallback_pass->feature_node,
                                    fallback_pass->part_id};
                } else {
                    target.node_id = feature->node_id;
                    target.graph = {EditorGraphKind::kFeature, feature->pipeline_node, {}, {}};
                }
            } else {
                target.node_id.clear();
            }
        }
    }
    if (target.node_id.empty() && target.edge_id.empty())
        return {foundation::Result::success(), std::move(target)};
    if (!target.node_id.empty() && target.graph.pipeline_node.empty() && target.graph.feature_node.empty())
        return {foundation::not_found("Authored diagnostic source node does not exist"), {}};
    return {foundation::Result::success(), std::move(target)};
}

}  // namespace aeg::application
