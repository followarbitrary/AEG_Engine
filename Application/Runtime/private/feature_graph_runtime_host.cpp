#include "feature_graph_runtime_host.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "feature_graph_asset.hpp"

namespace aeg::application::runtime {

namespace {

feature::FeatureNodeId namespaced_node_id(std::string_view asset_uuid, feature::FeatureNodeId node_id) {
    return feature::FeatureNodeId(std::string(asset_uuid) + "#" + std::to_string(node_id.value()));
}

std::string scoped_metadata(std::string_view asset_uuid, std::string_view value) {
    return std::string(asset_uuid) + "::" + std::string(value);
}

foundation::ResultValue<feature::FeatureGraphDocument> merge_documents(
    const std::vector<feature::FeatureGraphDocument>& documents, const feature::FeatureRegistry& registry) {
    if (documents.empty()) {
        return {foundation::Result::failure(foundation::ResultCode::kNotReady,
                                             "no staged FeatureGraphs are ready"), {}};
    }
    feature::FeatureGraphDocument merged{};
    merged.asset_uuid = "runtime://feature-batch";
    merged.schema_version = 1;
    merged.graph_version = 1;
    merged.contract_digest = registry.contract_digest();
    std::unordered_set<std::string> asset_ids;
    std::unordered_set<std::string> module_requirements;
    std::unordered_set<std::string> input_bindings;
    std::unordered_set<std::string> variadic_slots;
    std::unordered_set<std::string> extensions;
    for (const auto& document : documents) {
        if (!asset_ids.insert(document.asset_uuid).second) {
            return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                 "FeatureGraph asset ids must be unique within a runtime batch"), {}};
        }
        merged.graph_version = std::max(merged.graph_version, document.graph_version);
        merged.draft_revision = std::max(merged.draft_revision, document.draft_revision);
        merged.layout_revision = std::max(merged.layout_revision, document.layout_revision);
        for (const auto& value : document.module_requirements)
            if (module_requirements.insert(value).second) merged.module_requirements.push_back(value);
        // These fields belong to an authored asset.  Preserve their scope in
        // the synthetic batch instead of silently merging two assets that use
        // the same binding or extension name.
        for (const auto& value : document.input_bindings) {
            const auto scoped = scoped_metadata(document.asset_uuid, value);
            if (input_bindings.insert(scoped).second) merged.input_bindings.push_back(scoped);
        }
        for (const auto& value : document.variadic_slots) {
            const auto scoped = scoped_metadata(document.asset_uuid, value);
            if (variadic_slots.insert(scoped).second) merged.variadic_slots.push_back(scoped);
        }
        for (const auto& value : document.extensions) {
            const auto scoped = scoped_metadata(document.asset_uuid, value);
            if (extensions.insert(scoped).second) merged.extensions.push_back(scoped);
        }

        std::unordered_map<feature::FeatureNodeId, feature::FeatureNodeId,
                           feature::StableIdHash<feature::FeatureNodeIdTag>> remap;
        for (const auto& node : document.nodes) {
            const auto mapped = namespaced_node_id(document.asset_uuid, node.node_id);
            if (!remap.emplace(node.node_id, mapped).second) {
                return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                     "FeatureGraph node ids must be unique within an asset"), {}};
            }
        }
        for (const auto& node : document.nodes) {
            feature::FeatureNode copy = node;
            copy.node_id = remap.at(node.node_id);
            for (auto& owner : copy.owner_path.nodes) {
                if (const auto mapped_owner = remap.find(owner); mapped_owner != remap.end())
                    owner = mapped_owner->second;
            }
            copy.owner_path.nodes.insert(copy.owner_path.nodes.begin(),
                                         feature::FeatureNodeId(std::string("asset:") + document.asset_uuid));
            merged.nodes.push_back(std::move(copy));
        }
        for (const auto& edge : document.edges) {
            const auto source = remap.find(edge.source_node);
            const auto target = remap.find(edge.target_node);
            if (source == remap.end() || target == remap.end()) {
                return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                     "FeatureGraph edge references an unknown node"), {}};
            }
            feature::FeatureEdge copy = edge;
            copy.source_node = source->second;
            copy.target_node = target->second;
            merged.edges.push_back(std::move(copy));
        }
        for (const auto& process : document.process_subgraphs) {
            const auto owner = remap.find(process.owner_node);
            if (owner == remap.end()) {
                return {foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                                     "FeatureGraph process references an unknown node"), {}};
            }
            feature::ProcessSubgraph copy = process;
            copy.owner_node = owner->second;
            merged.process_subgraphs.push_back(std::move(copy));
        }
    }
    return {foundation::Result::success(), std::move(merged)};
}

}  // namespace

foundation::Result FeatureGraphRuntimeHost::stage(const feature::FeatureGraphDocument& graph) {
    if (!graph.nodes.empty() && graph.contract_digest == 0) {
        pending_documents_.clear();
        return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                           "FeatureGraph contract digest is required for runtime staging");
    }
    if (graph.contract_digest != 0 && graph.contract_digest != registry_.contract_digest()) {
        pending_documents_.clear();
        return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                           "FeatureGraph contract digest is stale");
    }
    auto result = feature::FeatureGraphCompiler{}.compile(graph, registry_);
    if (!result) {
        pending_documents_.clear();
        return result.result;
    }
    for (const auto& pending : pending_documents_) {
        if (pending.asset_uuid == graph.asset_uuid) {
            pending_documents_.clear();
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                               "FeatureGraph asset ids must be unique within a runtime batch");
        }
    }
    pending_documents_.push_back(graph);
    return foundation::Result::success();
}

foundation::Result FeatureGraphRuntimeHost::stage_asset(std::span<const std::byte> bytes) {
    const auto decoded = aeg::asset::decode_feature_graph_asset(bytes);
    if (!decoded) {
        pending_documents_.clear();
        return decoded.result;
    }
    return stage(decoded.value);
}

foundation::Result FeatureGraphRuntimeHost::apply_pending() {
    if (pending_documents_.empty()) {
        return foundation::Result::failure(foundation::ResultCode::kNotReady,
                                           "no staged FeatureGraphs are ready");
    }
    for (const auto& pending : pending_documents_) {
        if (!pending.nodes.empty() && pending.contract_digest == 0) {
            pending_documents_.clear();
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                               "FeatureGraph contract digest is required for runtime staging");
        }
        if (pending.contract_digest != 0 && pending.contract_digest != registry_.contract_digest()) {
            pending_documents_.clear();
            return foundation::Result::failure(foundation::ResultCode::kInvalidArgument,
                                               "FeatureGraph contract digest is stale");
        }
    }
    auto merged = merge_documents(pending_documents_, registry_);
    if (!merged) {
        pending_documents_.clear();
        return merged.result;
    }
    auto compiled = feature::FeatureGraphCompiler{}.compile(merged.value, registry_);
    if (!compiled) {
        pending_documents_.clear();
        return compiled.result;
    }
    if (const auto result = runtime_.publish_graph(merged.value, compiled.value); !result) {
        return result;
    }
    active_ = std::move(compiled.value);
    active_documents_ = pending_documents_;
    pending_documents_.clear();
    generation_ = runtime_.generation();
    return foundation::Result::success();
}

foundation::Result FeatureGraphRuntimeHost::validate_active() const {
    if (!active_) return foundation::not_ready("FeatureGraph host has no active graph");
    if (generation_ != runtime_.generation())
        return foundation::invalid_state("FeatureGraph host generation has retired");
    return foundation::Result::success();
}

foundation::ResultValue<FeatureRuntimeNodeSnapshot> FeatureGraphRuntimeHost::inspect_node(
    std::string_view asset_uuid, feature::FeatureNodeId node_id) const {
    if (const auto result = validate_active(); !result) return {result, {}};
    const auto document = std::find_if(active_documents_.begin(), active_documents_.end(),
        [&](const auto& item) { return item.asset_uuid == asset_uuid; });
    if (document == active_documents_.end() || std::none_of(document->nodes.begin(), document->nodes.end(),
        [&](const auto& node) { return node.node_id == node_id; }))
        return {foundation::invalid_argument("FeatureGraph authored node is not active"), {}};
    const auto mapped_node = namespaced_node_id(asset_uuid, node_id);
    const auto root = std::find_if(active_->provenance.begin(), active_->provenance.end(),
        [&](const auto& row) { return row.node_id == mapped_node; });
    if (root == active_->provenance.end())
        return {foundation::invalid_state("FeatureGraph authored node has no published owner"), {}};
    FeatureRuntimeNodeSnapshot snapshot{generation_, std::string(asset_uuid), node_id};
    std::vector<feature::FeatureInstanceId> pending{root->instance_id};
    for (size_t index = 0; index != pending.size(); ++index) {
        const auto* instance = runtime_.find_instance(pending[index]);
        if (!instance) return {foundation::invalid_state("FeatureGraph owner has retired"), {}};
        const auto* definition = registry_.find_definition(instance->definition_id);
        if (!definition || feature::feature_contract_digest(*definition) != instance->definition_digest)
            return {foundation::invalid_state("FeatureGraph owner definition has retired"), {}};
        FeatureRuntimeOwnerSnapshot owner{};
        owner.owner = {instance->id, mapped_node, instance->definition_digest, instance->render_resource_epoch};
        owner.definition_id = instance->definition_id;
        owner.owner_module = instance->owner_module;
        owner.scope = instance->scope;
        owner.owner_path = instance->owner_path;
        owner.contract = definition->contract;
        owner.state = instance->state;
        owner.parameters = instance->parameters;
        owner.children = instance->children;
        owner.render_output = instance->render_output;
        auto invocations = runtime_.inspect_invocations(instance->id);
        if (!invocations) return {invocations.result, {}};
        owner.invocations = std::move(invocations.value);
        for (const auto id : instance->processes) {
            const auto* process = runtime_.find_process(id);
            if (!process || process->owner != instance->id)
                return {foundation::invalid_state("FeatureGraph Process owner has retired"), {}};
            owner.processes.push_back(*process);
        }
        pending.insert(pending.end(), instance->children.begin(), instance->children.end());
        snapshot.owners.push_back(std::move(owner));
    }
    return {foundation::Result::success(), std::move(snapshot)};
}

foundation::ResultValue<FeatureRuntimeExecutionSnapshot> FeatureGraphRuntimeHost::inspect_execution(
    std::string_view asset_uuid) const {
    if (const auto valid = validate_active(); !valid) return {valid, {}};
    const auto document = std::find_if(active_documents_.begin(), active_documents_.end(),
        [&](const auto& item) { return item.asset_uuid == asset_uuid; });
    if (document == active_documents_.end())
        return {foundation::invalid_argument("FeatureGraph authored root is not active"), {}};
    FeatureRuntimeExecutionSnapshot snapshot;
    snapshot.generation = generation_; snapshot.asset_uuid = document->asset_uuid;
    snapshot.graph_version = document->graph_version; snapshot.draft_revision = document->draft_revision;
    snapshot.contract_digest = document->contract_digest;
    std::unordered_set<feature::FeatureInstanceId, feature::StableIdHash<feature::FeatureInstanceIdTag>> owned;
    for (const auto& node : document->nodes) {
        const auto mapped_node = namespaced_node_id(asset_uuid, node.node_id);
        const auto published_root = std::find_if(active_->provenance.begin(), active_->provenance.end(),
            [&](const auto& item) { return item.node_id == mapped_node; });
        if (published_root == active_->provenance.end())
            return {foundation::invalid_state("FeatureGraph authored node has no published owner"), {}};
        const auto* root_instance = runtime_.find_instance(published_root->instance_id);
        if (!root_instance)
            return {foundation::invalid_state("FeatureGraph authored root has retired"), {}};
        std::vector<feature::FeatureInstanceId> pending{published_root->instance_id};
        for (std::size_t index = 0; index < pending.size(); ++index) {
            const auto* owner = runtime_.find_instance(pending[index]);
            if (!owner) return {foundation::invalid_state("FeatureGraph owner has retired"), {}};
            const auto* definition = registry_.find_definition(owner->definition_id);
            if (!definition || feature::feature_contract_digest(*definition) != owner->definition_digest)
                return {foundation::invalid_state("FeatureGraph owner definition has retired"), {}};
            for (auto id : owner->processes) {
                const auto* process = runtime_.find_process(id);
                if (!process || process->owner != owner->id)
                    return {foundation::invalid_state("FeatureGraph Process owner has retired"), {}};
            }
            if (!owned.insert(owner->id).second)
                return {foundation::invalid_state("Published Feature owner attribution is ambiguous"), {}};
            FeatureRuntimeExecutionProvenance row;
            row.authored_node_id = node.node_id; row.authored_owner_path = node.owner_path;
            row.authored_instance_path = node.owner_path;
            if (owner->id != root_instance->id) {
                const auto& root_path = root_instance->owner_path.nodes;
                const auto& owner_path = owner->owner_path.nodes;
                if (owner_path.size() <= root_path.size() ||
                    !std::equal(root_path.begin(), root_path.end(), owner_path.begin()))
                    return {foundation::invalid_state("FeatureGraph descendant has foreign root ancestry"), {}};
                row.authored_instance_path.nodes.push_back(node.node_id);
                row.authored_instance_path.nodes.insert(row.authored_instance_path.nodes.end(),
                    owner_path.begin() + static_cast<std::ptrdiff_t>(root_path.size()), owner_path.end());
            }
            const auto root = std::find_if(active_->provenance.begin(), active_->provenance.end(),
                [&](const auto& item) { return item.instance_id == owner->id; });
            if (root != active_->provenance.end()) row.runtime = *root;
            else {
                const auto expanded = registry_.expand(owner->definition_id, owner->owner_path);
                if (!expanded) return {expanded.result, {}};
                row.runtime.definition_id = owner->definition_id;
                row.runtime.node_id = mapped_node;
                row.runtime.instance_id = owner->id;
                row.runtime.owner_module = owner->owner_module;
                row.runtime.owner_path = owner->owner_path;
                row.runtime.clock = definition->contract.clock.id;
                row.runtime.domains = expanded.value.domains;
                if (!owner->processes.empty()) row.runtime.process_id = owner->processes.front();
            }
            if (owner->last_invocation_issued) row.runtime.invocation_id = owner->last_invocation_issued->token.invocation_id;
            snapshot.provenance.push_back(std::move(row));
            pending.insert(pending.end(), owner->children.begin(), owner->children.end());
        }
    }
    for (const auto& published : active_->plans) {
        feature::ExecutionPlan plan; plan.clock = published.clock;
        for (auto instance : published.instances)
            if (owned.contains(instance)) plan.instances.push_back(instance);
        if (!plan.instances.empty()) snapshot.plans.push_back(std::move(plan));
    }
    return {foundation::Result::success(), std::move(snapshot)};
}

foundation::Result FeatureGraphRuntimeHost::control_process(const RuntimeProcessCommand& command) {
    if (command.generation != generation_)
        return foundation::invalid_state("FeatureGraph Process command generation is stale");
    const auto snapshot = inspect_node(command.asset_uuid, command.node_id);
    if (!snapshot) return snapshot.result;
    const auto owner = std::find_if(snapshot.value.owners.begin(), snapshot.value.owners.end(),
        [&](const auto& value) { return value.owner == command.owner; });
    if (owner == snapshot.value.owners.end())
        return foundation::invalid_argument("FeatureGraph Process command owner is not in the selected authored node");
    const auto process = std::find_if(owner->processes.begin(), owner->processes.end(),
        [&](const auto& value) { return value.id == command.process; });
    if (process == owner->processes.end() || process->revision != command.expected_revision)
        return foundation::invalid_state("FeatureGraph Process command identity or revision is stale");
    switch (command.action) {
        case RuntimeProcessAction::kSuspend: return runtime_.suspend(command.process);
        case RuntimeProcessAction::kResume: return runtime_.resume(command.process);
        case RuntimeProcessAction::kCancel: return runtime_.cancel(command.process);
    }
    return foundation::invalid_argument("unknown FeatureGraph Process command action");
}

foundation::ResultValue<std::vector<feature::RenderFeatureOutput>> FeatureGraphRuntimeHost::capture_render_outputs() const {
    if (const auto result = validate_active(); !result) return {result, {}};
    return runtime_.capture_render_outputs(*active_);
}

foundation::Result FeatureGraphRuntimeHost::render_publication_status(feature::ClockId clock) const {
    if (const auto result = validate_active(); !result) return result;
    return runtime_.render_publication_status(clock);
}
}  // namespace aeg::application::runtime
