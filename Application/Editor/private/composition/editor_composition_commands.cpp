#include "editor_composition_commands.hpp"

#include <algorithm>
#include <exception>
#include <new>

namespace aeg::application {
namespace {
using namespace pipeline;

bool has_node(const PipelineDocument& document, std::string_view id) {
    return std::any_of(document.pipeline_nodes.begin(), document.pipeline_nodes.end(),
                       [&](const auto& node) { return node.node_id == id; }) ||
           std::any_of(document.feature_nodes.begin(), document.feature_nodes.end(),
                       [&](const auto& node) { return node.node_id == id; }) ||
           std::any_of(document.pass_nodes.begin(), document.pass_nodes.end(),
                       [&](const auto& node) { return node.node_id == id; }) ||
           std::any_of(document.resources.begin(), document.resources.end(),
                       [&](const auto& node) { return node.node_id == id; });
}

const TypeCatalogEntry* find_type(const TypeCatalog& catalog, AuthoringTypeKey key) {
    return catalog.find(key);
}

PipelineScope first_scope(PipelineScopeMask mask) {
    for (uint8_t value = 0; value <= static_cast<uint8_t>(PipelineScope::kEditor); ++value)
        if (mask & (PipelineScopeMask{1u} << value))
            return static_cast<PipelineScope>(value);
    return PipelineScope::kFrame;
}

const FeatureNodeDocument* find_feature(const PipelineDocument& document, std::string_view id) {
    const auto found = std::find_if(document.feature_nodes.begin(), document.feature_nodes.end(),
                                    [&](const auto& node) { return node.node_id == id; });
    return found == document.feature_nodes.end() ? nullptr : &*found;
}

const PipelineNodeDocument* find_pipeline(const PipelineDocument& document, std::string_view id) {
    const auto found = std::find_if(document.pipeline_nodes.begin(), document.pipeline_nodes.end(),
                                    [&](const auto& node) { return node.node_id == id; });
    return found == document.pipeline_nodes.end() ? nullptr : &*found;
}

}  // namespace

foundation::Result EditorCompositionCommands::add_type(const EditorGraphAddress& address, AuthoringTypeKey key,
                                                       std::string node_id, std::string attachment_id) {
    if (node_id.empty())
        return foundation::invalid_argument("authored node ID is empty");
    const auto* entry = find_type(document_.catalog(), key);
    if (!entry)
        return foundation::not_found("authoring type is unavailable");
    return document_.execute("Add " + entry->display_name, [&](PipelineDocument& next) {
        if (has_node(next, node_id))
            return foundation::invalid_state("authored node ID is already in use");
        if (address.kind == EditorGraphKind::kPipeline) {
            if (key.kind != AuthoringTypeKind::kPipeline || !address.pipeline_node.empty())
                return foundation::invalid_argument("Pipeline insertion address or type is invalid");
            PipelineNodeDocument node;
            node.node_id = node_id;
            node.type = PipelineId(entry->key.id);
            node.version = entry->version;
            node.scope = first_scope(entry->scopes);
            node.parameters = {};
            next.pipeline_nodes.push_back(std::move(node));
        } else if (address.kind == EditorGraphKind::kFeature) {
            if (key.kind != AuthoringTypeKind::kFeature || !find_pipeline(next, address.pipeline_node))
                return foundation::invalid_argument("Feature insertion address or type is invalid");
            if (attachment_id.empty())
                return foundation::invalid_argument("Feature insertion requires a FeaturePoint");
            const auto* pipeline_node = find_pipeline(next, address.pipeline_node);
            const auto* pipeline_entry =
                find_type(document_.catalog(), {AuthoringTypeKind::kPipeline, pipeline_node->type.value()});
            const auto* pipeline_decl =
                pipeline_entry ? std::get_if<PipelineAuthoringDeclaration>(&pipeline_entry->declaration) : nullptr;
            const auto point =
                pipeline_decl
                    ? std::find_if(pipeline_decl->feature_points.begin(), pipeline_decl->feature_points.end(),
                                   [&](const auto& value) { return value.id == FeaturePointId(attachment_id); })
                    : std::vector<AuthoringFeaturePoint>::const_iterator{};
            if (!pipeline_decl || point == pipeline_decl->feature_points.end() ||
                (entry->domains & point->domains) == 0 || (entry->scopes & point->scopes) == 0)
                return foundation::invalid_argument("FeaturePoint is not compatible with the Feature");
            FeatureNodeDocument node;
            node.node_id = node_id;
            node.type = FeatureId(entry->key.id);
            node.version = entry->version;
            node.pipeline_node = address.pipeline_node;
            node.point = FeaturePointId(attachment_id);
            if (const auto* feature_decl = std::get_if<FeatureAuthoringDeclaration>(&entry->declaration))
                if (!feature_decl->default_passes.empty()) {
                    // Defaults are materialized by the shared document preparer.
                }
            next.feature_nodes.push_back(std::move(node));
        } else {
            if (key.kind != AuthoringTypeKind::kRenderPass || address.feature_node.empty())
                return foundation::invalid_argument("Pass insertion address or type is invalid");
            const auto* owner = find_feature(next, address.feature_node);
            if (!owner || address.part_id.empty())
                return foundation::invalid_argument("Pass owner or part is invalid");
            const auto* owner_entry =
                find_type(document_.catalog(), {AuthoringTypeKind::kFeature, owner->type.value()});
            const auto* declaration =
                owner_entry ? std::get_if<FeatureAuthoringDeclaration>(&owner_entry->declaration) : nullptr;
            if (!declaration ||
                std::find(declaration->allowed_pass_types.begin(), declaration->allowed_pass_types.end(),
                          RenderPassTypeId(key.id)) == declaration->allowed_pass_types.end())
                return foundation::invalid_argument("Pass type is not allowed by the Feature");
            PassNodeDocument node;
            node.node_id = node_id;
            node.type = RenderPassTypeId(entry->key.id);
            node.version = entry->version;
            node.feature_node = address.feature_node;
            node.part_id = address.part_id;
            next.pass_nodes.push_back(std::move(node));
        }
        return foundation::Result::success();
    });
}

foundation::Result EditorCompositionCommands::erase_node(std::string_view node_id) {
    if (node_id.empty())
        return foundation::invalid_argument("authored node ID is empty");
    return document_.execute("Delete authored node", [&](PipelineDocument& next) {
        const auto pipeline = std::find_if(next.pipeline_nodes.begin(), next.pipeline_nodes.end(),
                                           [&](const auto& node) { return node.node_id == node_id; });
        if (pipeline != next.pipeline_nodes.end()) {
            std::vector<std::string> children;
            for (const auto& feature : next.feature_nodes)
                if (feature.pipeline_node == node_id)
                    children.push_back(feature.node_id);
            next.pipeline_nodes.erase(pipeline);
            for (const auto& child : children) {
                next.feature_nodes.erase(std::remove_if(next.feature_nodes.begin(), next.feature_nodes.end(),
                                                        [&](const auto& node) { return node.node_id == child; }),
                                         next.feature_nodes.end());
                next.pass_nodes.erase(std::remove_if(next.pass_nodes.begin(), next.pass_nodes.end(),
                                                     [&](const auto& node) { return node.feature_node == child; }),
                                      next.pass_nodes.end());
            }
        } else {
            const auto feature = std::find_if(next.feature_nodes.begin(), next.feature_nodes.end(),
                                              [&](const auto& node) { return node.node_id == node_id; });
            if (feature != next.feature_nodes.end()) {
                next.pass_nodes.erase(std::remove_if(next.pass_nodes.begin(), next.pass_nodes.end(),
                                                     [&](const auto& node) { return node.feature_node == node_id; }),
                                      next.pass_nodes.end());
                next.feature_nodes.erase(feature);
            } else {
                const auto pass = std::find_if(next.pass_nodes.begin(), next.pass_nodes.end(),
                                               [&](const auto& node) { return node.node_id == node_id; });
                if (pass == next.pass_nodes.end())
                    return foundation::not_found("authored node does not exist");
                next.pass_nodes.erase(pass);
            }
        }
        next.edges.erase(std::remove_if(next.edges.begin(), next.edges.end(),
                                        [&](const auto& edge) {
                                            return edge.source.node_id == node_id ||
                                                   edge.destination.node_id == node_id;
                                        }),
                         next.edges.end());
        return foundation::Result::success();
    });
}

foundation::Result EditorCompositionCommands::connect(const EditorConnectionRequest& request) {
    if (request.edge_id.empty() || request.source_node.empty() || request.destination_node.empty() ||
        !request.source_port || !request.destination_port || !request.binding_id)
        return foundation::invalid_argument("connection request is incomplete");
    return document_.execute("Connect authored ports", [&](PipelineDocument& next) {
        if (!has_node(next, request.source_node) || !has_node(next, request.destination_node))
            return foundation::not_found("connection endpoint does not exist");
        if (std::any_of(next.edges.begin(), next.edges.end(),
                        [&](const auto& edge) { return edge.edge_id == request.edge_id; }))
            return foundation::invalid_state("connection edge ID is already in use");
        next.edges.push_back({request.edge_id,
                              request.kind,
                              {request.source_node, request.source_port},
                              {request.destination_node, request.destination_port},
                              request.binding_id});
        const auto diagnostics = validate_pipeline_document(next, document_.catalog());
        if (!diagnostics.empty())
            return foundation::invalid_argument(diagnostics.front().message);
        return foundation::Result::success();
    });
}

foundation::Result EditorCompositionCommands::disconnect(std::string_view edge_id) {
    return document_.execute("Disconnect authored ports", [&](PipelineDocument& next) {
        const auto found = std::find_if(next.edges.begin(), next.edges.end(),
                                        [&](const auto& edge) { return edge.edge_id == edge_id; });
        if (found == next.edges.end())
            return foundation::not_found("connection edge does not exist");
        next.edges.erase(found);
        return foundation::Result::success();
    });
}

foundation::Result EditorCompositionCommands::set_parameter(std::string_view node_id, uint64_t parameter_id,
                                                            ParameterValue value) {
    if (node_id.empty() || !parameter_id)
        return foundation::invalid_argument("parameter request is incomplete");
    return document_.execute("Set authored parameter", [&](PipelineDocument& next) {
        auto set = [&](auto& node, AuthoringTypeKind kind, uint64_t type_id) -> foundation::Result {
            const auto* entry = find_type(document_.catalog(), {kind, type_id});
            if (!entry)
                return foundation::not_found("parameter owner type is unavailable");
            ParameterBlock candidate = node.parameters;
            candidate[parameter_id] = value;
            if (const auto result = validate_parameter_block(candidate, entry->parameters); !result)
                return result;
            node.parameters = std::move(candidate);
            return foundation::Result::success();
        };
        for (auto& node : next.pipeline_nodes)
            if (node.node_id == node_id)
                return set(node, AuthoringTypeKind::kPipeline, node.type.value());
        for (auto& node : next.feature_nodes)
            if (node.node_id == node_id)
                return set(node, AuthoringTypeKind::kFeature, node.type.value());
        for (auto& node : next.pass_nodes)
            if (node.node_id == node_id)
                return set(node, AuthoringTypeKind::kRenderPass, node.type.value());
        return foundation::not_found("parameter owner node does not exist");
    });
}

foundation::Result EditorCompositionCommands::begin_transaction(std::string label) {
    if (transaction_)
        return foundation::invalid_state("an editor composition transaction is already active");
    if (label.empty())
        return foundation::invalid_argument("editor composition transaction label is empty");
    transaction_ = Transaction{std::move(label), document_.snapshot(), *document_.snapshot()};
    return foundation::Result::success();
}

foundation::Result EditorCompositionCommands::update_transaction(
    std::function<foundation::Result(PipelineDocument&)> mutation) {
    if (!transaction_)
        return foundation::invalid_state("no editor composition transaction is active");
    if (!mutation)
        return foundation::invalid_argument("editor composition transaction mutation is empty");
    try {
        return mutation(transaction_->draft);
    } catch (const std::exception& exception) {
        return foundation::unknown_exception(exception);
    }
}

foundation::Result EditorCompositionCommands::commit_transaction() {
    if (!transaction_)
        return foundation::invalid_state("no editor composition transaction is active");
    if (document_.snapshot() != transaction_->before)
        return foundation::invalid_state("editor composition changed during transaction");
    if (const auto diagnostics = validate_pipeline_document(transaction_->draft, document_.catalog());
        !diagnostics.empty())
        return foundation::invalid_argument(diagnostics.front().message);
    const auto label = transaction_->label;
    auto draft = std::move(transaction_->draft);
    const auto result = document_.execute(label, [draft = std::move(draft)](PipelineDocument& next) mutable {
        next = std::move(draft);
        return foundation::Result::success();
    });
    if (result)
        transaction_.reset();
    return result;
}

foundation::Result EditorCompositionCommands::cancel_transaction() noexcept {
    if (!transaction_)
        return foundation::invalid_state("no editor composition transaction is active");
    transaction_.reset();
    return foundation::Result::success();
}

}  // namespace aeg::application
