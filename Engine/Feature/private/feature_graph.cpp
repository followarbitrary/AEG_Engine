#include "feature_graph.hpp"
#include "feature_process.hpp"

#include <algorithm>
#include <bit>
#include <functional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

namespace aeg::feature {
namespace {
class GraphSemanticDigest final {
public:
    void number(uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8)
            byte(static_cast<uint8_t>(value >> shift));
    }

    void text(std::string_view value) {
        number(value.size());
        for (const unsigned char character : value) byte(character);
    }

    void bytes(const std::vector<uint8_t>& value) {
        number(value.size());
        for (const uint8_t element : value) byte(element);
    }

    void ordered_strings(const std::vector<std::string>& values) {
        number(values.size());
        for (const auto& value : values) text(value);
    }

    void unordered_strings(const std::vector<std::string>& values) {
        std::vector<std::string_view> ordered;
        ordered.reserve(values.size());
        for (const auto& value : values) ordered.emplace_back(value);
        strings(std::move(ordered));
    }

    void extensions(const std::vector<std::string>& values) {
        std::vector<std::string_view> ordered;
        ordered.reserve(values.size());
        for (const auto& value : values) {
            const std::string_view record(value);
            // Match the reserved Process layout namespace used by Cook and
            // the Feature canvas namespace. Similar editor/vendor names are
            // business data, including the existing aeg.editor.canvas.v1.
            if (!record.starts_with("aeg.editor.process-layout.") &&
                !record.starts_with("aeg.editor.feature-layout.v1:"))
                ordered.push_back(record);
        }
        strings(std::move(ordered));
    }

    [[nodiscard]] uint64_t value() const noexcept { return value_ == 0 ? 1 : value_; }

private:
    void byte(uint8_t value) {
        value_ ^= value;
        value_ *= 1099511628211ull;
    }

    void strings(std::vector<std::string_view> values) {
        std::sort(values.begin(), values.end());
        number(values.size());
        for (const auto value : values) text(value);
    }

    uint64_t value_{14695981039346656037ull};
};

template <typename Value, typename Compare>
std::vector<const Value*> canonical_graph_collection(const std::vector<Value>& values, Compare compare) {
    std::vector<const Value*> ordered;
    ordered.reserve(values.size());
    for (const auto& value : values) ordered.push_back(&value);
    // Preserve multiplicity. A digest must not turn duplicate declarations
    // into a valid-looking set; the compiler still rejects duplicate ids.
    std::sort(ordered.begin(), ordered.end(), [&](const auto* left, const auto* right) {
        return compare(*left, *right);
    });
    return ordered;
}

foundation::Result invalid(std::string message) {
    return foundation::Result::failure(foundation::ResultCode::kInvalidArgument, std::move(message));
}

const FeatureNode* find_node(const FeatureGraphDocument& graph, FeatureNodeId id) {
    const auto it = std::find_if(graph.nodes.begin(), graph.nodes.end(),
                                 [id](const FeatureNode& node) { return node.node_id == id; });
    return it == graph.nodes.end() ? nullptr : &*it;
}

foundation::ResultValue<ProcessStateMachine> make_process_state_machine(
    const ProcessSubgraph& process, const FeatureContract& contract) {
    if (process.states.empty() || process.state_schema_version == 0)
        return {invalid("process subgraph requires states and a schema version"), {}};

    ProcessStateMachine machine{};
    machine.initial_state = process.initial_state.empty() ? process.states.front() : process.initial_state;
    std::unordered_set<std::string> state_names;
    for (const auto& state_name : process.states) {
        if (state_name.empty() || !state_names.insert(state_name).second)
            return {invalid("process states must be unique nonempty names"), {}};
        machine.states.push_back({.name = state_name, .is_entry = state_name == machine.initial_state});
    }

    std::unordered_set<std::string> terminal_names;
    for (const auto& terminal : process.terminal_states) {
        if (!state_names.contains(terminal) || !terminal_names.insert(terminal).second)
            return {invalid("process terminal states must be declared and unique"), {}};
    }
    std::unordered_set<std::string> transition_names;
    for (const auto& encoded : process.transitions) {
        const auto separator = encoded.find("->");
        if (separator == std::string::npos || encoded.find("->", separator + 2) != std::string::npos)
            return {invalid("process transitions must use one source->target separator"), {}};
        const std::string from = encoded.substr(0, separator);
        const std::string to = encoded.substr(separator + 2);
        if (!state_names.contains(from) || !state_names.contains(to) ||
            !transition_names.insert(encoded).second)
            return {invalid("process transitions must reference unique declared states"), {}};
        machine.transitions.push_back({from, to, {}, {TransitionConditionType::kAlways}, 0});
    }
    if (process.terminal_states.empty()) {
        std::unordered_set<std::string> sources;
        for (const auto& transition : machine.transitions) sources.insert(transition.from_state);
        for (const auto& state : machine.states)
            if (!sources.contains(state.name)) terminal_names.insert(state.name);
    }
    for (auto& state : machine.states) state.is_terminal = terminal_names.contains(state.name);
    for (const auto& transition : machine.transitions) {
        if (const auto result = validate_transition_condition(transition.condition, contract.ports); !result)
            return {result, {}};
    }
    if (const auto result = validate_process_state_machine(machine); !result)
        return {result, {}};
    return {foundation::Result::success(), std::move(machine)};
}

const PortContract* find_port(const FeatureContract& contract, PortId id, PortDirection direction) {
    const auto it = std::find_if(contract.ports.begin(), contract.ports.end(),
                                 [id, direction](const PortContract& port) {
                                     return port.id == id && port.direction == direction;
                                 });
    return it == contract.ports.end() ? nullptr : &*it;
}

const FeatureCatalogEntry* find_catalog_entry(const FeatureCatalog& catalog, FeatureDefinitionId id) {
    const auto it = std::find_if(catalog.entries().begin(), catalog.entries().end(),
                                 [id](const FeatureCatalogEntry& entry) { return entry.definition_id == id; });
    return it == catalog.entries().end() ? nullptr : &*it;
}

FeatureDomainMask declared_domains(const FeatureDefinition& definition) {
    return definition.contract.domains;
}

FeatureDomainMask declared_domains(const FeatureCatalogEntry& entry) {
    // Metadata may carry a legacy summary-only catalog entry.  Full Feature
    // contracts take precedence, while summary entries retain their declared
    // domain for editor validation.
    return entry.contract.definition_id.is_valid() ? entry.contract.domains : entry.domains;
}

template <typename DefinitionLookup>
FeatureDomainMask port_domains(const DefinitionLookup& lookup, FeatureDefinitionId definition_id,
                               PortId port_id, PortDirection direction,
                               std::unordered_set<FeatureDefinitionId, StableIdHash<FeatureDefinitionIdTag>>& visiting) {
    if (!visiting.insert(definition_id).second) {
        return 0;
    }
    const auto* definition = lookup(definition_id);
    if (definition == nullptr) {
        visiting.erase(definition_id);
        return 0;
    }
    if (definition->contract.structure == FeatureStructure::kAtomic) {
        visiting.erase(definition_id);
        return declared_domains(*definition);
    }

    FeatureDomainMask domains = 0;
    for (const auto& mapping : definition->contract.child_port_mappings) {
        if (mapping.parent_port != port_id || mapping.direction != direction) {
            continue;
        }
        domains = static_cast<FeatureDomainMask>(
            domains | port_domains(lookup, mapping.child_definition, mapping.child_port, direction, visiting));
    }
    visiting.erase(definition_id);
    return domains == 0 ? declared_domains(*definition) : domains;
}

template <typename DefinitionLookup>
FeatureDomainMask port_domains(const DefinitionLookup& lookup, FeatureDefinitionId definition_id,
                               PortId port_id, PortDirection direction) {
    std::unordered_set<FeatureDefinitionId, StableIdHash<FeatureDefinitionIdTag>> visiting;
    return port_domains(lookup, definition_id, port_id, direction, visiting);
}

template <typename DefinitionLookup>
foundation::Result validate_port_projection(
    const DefinitionLookup& lookup, FeatureDefinitionId definition_id, PortId port_id,
    PortDirection direction,
    std::unordered_set<FeatureDefinitionId, StableIdHash<FeatureDefinitionIdTag>>& visiting) {
    if (!visiting.insert(definition_id).second)
        return invalid("recursive composite port mapping is not executable");
    const auto* definition = lookup(definition_id);
    if (definition == nullptr) {
        visiting.erase(definition_id);
        return invalid("composite port mapping references an unknown child definition");
    }
    const auto* parent_port = find_port(definition->contract, port_id, direction);
    if (parent_port == nullptr) {
        visiting.erase(definition_id);
        return invalid("composite port mapping references an undeclared parent port");
    }
    if (definition->contract.structure == FeatureStructure::kAtomic) {
        visiting.erase(definition_id);
        return foundation::Result::success();
    }
    for (const auto& mapping : definition->contract.child_port_mappings) {
        if (mapping.parent_port != port_id || mapping.direction != direction) continue;
        const auto* child_definition = lookup(mapping.child_definition);
        if (child_definition == nullptr) {
            visiting.erase(definition_id);
            return invalid("composite port mapping references an unknown child definition");
        }
        const auto* child_port = find_port(child_definition->contract, mapping.child_port, direction);
        if (child_port == nullptr) {
            visiting.erase(definition_id);
            return invalid("composite port mapping references an undeclared child port");
        }
        if (parent_port->schema != child_port->schema || parent_port->version != child_port->version) {
            visiting.erase(definition_id);
            return invalid("composite port mapping schema and version must match the parent port");
        }
        if (const auto result = validate_port_projection(lookup, mapping.child_definition,
                                                         mapping.child_port, direction, visiting);
            !result) {
            visiting.erase(definition_id);
            return result;
        }
    }
    visiting.erase(definition_id);
    return foundation::Result::success();
}

template <typename DefinitionLookup>
foundation::Result validate_port_projection(const DefinitionLookup& lookup,
                                            FeatureDefinitionId definition_id, PortId port_id,
                                            PortDirection direction) {
    std::unordered_set<FeatureDefinitionId, StableIdHash<FeatureDefinitionIdTag>> visiting;
    return validate_port_projection(lookup, definition_id, port_id, direction, visiting);
}

const PortContract* find_catalog_port(const FeatureCatalogEntry& entry, PortId id, PortDirection direction) {
    const auto it = std::find_if(entry.ports.begin(), entry.ports.end(), [id, direction](const PortContract& port) {
        return port.id == id && port.direction == direction;
    });
    return it == entry.ports.end() ? nullptr : &*it;
}
}

uint64_t graph_semantic_digest(const FeatureGraphDocument& graph) {
    GraphSemanticDigest digest;
    digest.text("AEG.FeatureGraph.Semantic.v1");
    digest.text(graph.asset_uuid);
    digest.number(graph.schema_version);
    digest.number(graph.graph_version);
    digest.number(graph.contract_digest);
    digest.unordered_strings(graph.module_requirements);

    const auto nodes = canonical_graph_collection(graph.nodes, [](const FeatureNode& left, const FeatureNode& right) {
        return std::tie(left.node_id, left.definition_id, left.definition_version, left.owner_path.nodes,
                        left.parameter_blob) <
               std::tie(right.node_id, right.definition_id, right.definition_version, right.owner_path.nodes,
                        right.parameter_blob);
    });
    digest.number(nodes.size());
    for (const auto* node : nodes) {
        digest.number(node->node_id.value());
        digest.number(node->definition_id.value());
        digest.number(node->definition_version.major);
        digest.number(node->definition_version.minor);
        digest.number(node->definition_version.patch);
        digest.number(node->owner_path.nodes.size());
        for (const auto owner : node->owner_path.nodes) digest.number(owner.value());
        digest.bytes(node->parameter_blob);
    }

    const auto edges = canonical_graph_collection(graph.edges, [](const FeatureEdge& left, const FeatureEdge& right) {
        return std::tie(left.source_node, left.source_port, left.target_node, left.target_port, left.kind,
                        left.source_clock, left.target_clock, left.version, left.capacity, left.deadline_ticks) <
               std::tie(right.source_node, right.source_port, right.target_node, right.target_port, right.kind,
                        right.source_clock, right.target_clock, right.version, right.capacity, right.deadline_ticks);
    });
    digest.number(edges.size());
    for (const auto* edge : edges) {
        digest.number(edge->source_node.value());
        digest.number(edge->source_port.value());
        digest.number(edge->target_node.value());
        digest.number(edge->target_port.value());
        digest.number(static_cast<uint8_t>(edge->kind));
        digest.number(edge->source_clock.value());
        digest.number(edge->target_clock.value());
        digest.number(edge->version);
        digest.number(edge->capacity);
        digest.number(edge->deadline_ticks);
    }

    const auto processes = canonical_graph_collection(graph.process_subgraphs,
        [](const ProcessSubgraph& left, const ProcessSubgraph& right) {
            return std::tie(left.owner_node, left.states, left.transitions, left.state_schema_version,
                            left.initial_state, left.terminal_states) <
                   std::tie(right.owner_node, right.states, right.transitions, right.state_schema_version,
                            right.initial_state, right.terminal_states);
        });
    digest.number(processes.size());
    for (const auto* process : processes) {
        digest.number(process->owner_node.value());
        // Process declarations retain order: an omitted entry selects the
        // first state. Do not apply the graph's collection sorting to them.
        digest.ordered_strings(process->states);
        digest.ordered_strings(process->transitions);
        digest.number(process->state_schema_version);
        digest.text(process->initial_state);
        digest.ordered_strings(process->terminal_states);
    }

    digest.unordered_strings(graph.input_bindings);
    digest.ordered_strings(graph.variadic_slots);
    digest.extensions(graph.extensions);
    return digest.value();
}

foundation::ResultValue<ProcessStateMachine> canonicalize_process_subgraph(
    const ProcessSubgraph& process, const FeatureContract& contract) {
    return make_process_state_machine(process, contract);
}

foundation::Result validate_feature_edge(const FeatureGraphDocument& graph, const FeatureEdge& edge,
                                         const FeatureCatalog& catalog) {
    const auto source_node = find_node(graph, edge.source_node);
    const auto target_node = find_node(graph, edge.target_node);
    if (source_node == nullptr || target_node == nullptr)
        return invalid("FeatureGraph edge references an unknown node");
    const auto* source = find_catalog_entry(catalog, source_node->definition_id);
    const auto* target = find_catalog_entry(catalog, target_node->definition_id);
    if (source == nullptr || target == nullptr || source_node->definition_version != source->version ||
        target_node->definition_version != target->version)
        return invalid("FeatureGraph edge definitions are missing or stale");
    const auto* source_port = find_catalog_port(*source, edge.source_port, PortDirection::kOutput);
    const auto* target_port = find_catalog_port(*target, edge.target_port, PortDirection::kInput);
    if (source_port == nullptr || target_port == nullptr || source_port->schema != target_port->schema ||
        source_port->version != target_port->version || edge.version == 0 || edge.version != source_port->version)
        return invalid("FeatureGraph edge ports must have matching versioned schemas");
    const auto lookup = [&catalog](FeatureDefinitionId id) -> const FeatureCatalogEntry* {
        return find_catalog_entry(catalog, id);
    };
    if (const auto result = validate_port_projection(lookup, source_node->definition_id, edge.source_port,
                                                     PortDirection::kOutput);
        !result)
        return result;
    if (const auto result = validate_port_projection(lookup, target_node->definition_id, edge.target_port,
                                                     PortDirection::kInput);
        !result)
        return result;
    if (edge.source_clock != source->clock || edge.target_clock != target->clock)
        return invalid("FeatureGraph edge clocks must match catalog contracts");
    if (edge.source_clock != edge.target_clock && edge.kind == EdgeKind::kData)
        return invalid("cross-clock data edges require an explicit transport policy");
    const auto source_domains = port_domains(lookup, source_node->definition_id, edge.source_port,
                                             PortDirection::kOutput);
    const auto target_domains = port_domains(lookup, target_node->definition_id, edge.target_port,
                                             PortDirection::kInput);
    if (source_domains != target_domains && edge.kind == EdgeKind::kData)
        return invalid("cross-domain data edges require an explicit transport policy");
    if (edge.kind == EdgeKind::kQueue && edge.capacity == 0)
        return invalid("queue edges require capacity");
    if (edge.kind == EdgeKind::kSnapshot && edge.deadline_ticks == 0)
        return invalid("snapshot edges require a deadline");
    const auto duplicate = std::find_if(graph.edges.begin(), graph.edges.end(), [&edge](const FeatureEdge& value) {
        return value.source_node == edge.source_node && value.source_port == edge.source_port &&
               value.target_node == edge.target_node && value.target_port == edge.target_port;
    });
    if (duplicate != graph.edges.end())
        return foundation::invalid_state("FeatureGraph edge already exists");
    return foundation::Result::success();
}

foundation::ResultValue<CompiledFeatureGraph> FeatureGraphCompiler::compile(
    const FeatureGraphDocument& graph, const FeatureRegistry& registry) const {
    // Failure diagnostics retain authored identities; no partially compiled plan can escape.
    const auto fail = [&graph](foundation::Result result, const FeatureNode* node = nullptr,
                              PortId port = {}, FeatureNodeId missing_node = {})
        -> foundation::ResultValue<CompiledFeatureGraph> {
        FeatureGraphDiagnostic diagnostic{result.message, node != nullptr ? node->node_id : missing_node,
                                           port, node != nullptr ? node->owner_path : OwnerPath{}};
        CompiledFeatureGraph rejected;
        rejected.diagnostics.push_back(std::move(diagnostic));
        return {std::move(result), std::move(rejected)};
    };
    if (graph.asset_uuid.empty() || graph.schema_version == 0 || graph.graph_version == 0) {
        return fail(invalid("FeatureGraph identity and schema version are required"));
    }
    if (graph.schema_version != 1)
        return fail(invalid("Unsupported FeatureGraph schema; migrate explicitly before compilation"));
    if (graph.contract_digest != 0 && graph.contract_digest != registry.contract_digest()) {
        return fail(invalid("FeatureGraph contract digest is stale"));
    }
    const auto lookup = [&registry](FeatureDefinitionId id) { return registry.find_definition(id); };
    CompiledFeatureGraph compiled{};
    std::unordered_map<FeatureNodeId, std::size_t, StableIdHash<FeatureNodeIdTag>> indices;
    std::unordered_map<ClockId, std::size_t, StableIdHash<ClockIdTag>> plan_indices;
    for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
        const auto& node = graph.nodes[index];
        if (!node.node_id.is_valid() || !indices.emplace(node.node_id, index).second) {
            return fail(invalid("FeatureGraph node ids must be stable and unique"), &node);
        }
    }
    for (const auto& node : graph.nodes) {
        const auto* definition = registry.find_definition(node.definition_id);
        if (definition == nullptr || definition->version != node.definition_version) {
            return fail(invalid("FeatureGraph node definition is missing or stale"), &node);
        }
        if (const auto result = validate_feature_parameter_blob(node.parameter_blob,
                                                                definition->contract.parameters);
            !result) {
            return fail(result, &node);
        }
        if (std::unordered_set<FeatureNodeId, StableIdHash<FeatureNodeIdTag>>{
                node.owner_path.nodes.begin(), node.owner_path.nodes.end()}.size() != node.owner_path.nodes.size()) {
            return fail(invalid("FeatureGraph owner paths must not contain cycles"), &node);
        }
        const auto expanded = registry.expand(node.definition_id, node.owner_path);
        if (!expanded)
            return fail(expanded.result, &node);
        for (const auto& mapping : definition->contract.child_port_mappings) {
            if (const auto result = validate_port_projection(lookup, definition->id, mapping.parent_port,
                                                             mapping.direction);
                !result) {
                return fail(result, &node, mapping.parent_port);
            }
        }
        const auto clock = definition->contract.clock.id;
        auto [plan_it, inserted] = plan_indices.emplace(clock, compiled.plans.size());
        if (inserted) {
            compiled.plans.push_back(ExecutionPlan{clock, {}});
        }
        compiled.plans[plan_it->second].instances.emplace_back(FeatureInstanceId(node.node_id.value()));
        compiled.provenance.push_back({definition->id, node.node_id,
                                       FeatureInstanceId(node.node_id.value()), {}, {},
                                   node.owner_path, clock, expanded.value.domains,
                                   definition->owner_module});
    }

    std::unordered_map<FeatureNodeId, std::vector<FeatureNodeId>, StableIdHash<FeatureNodeIdTag>> adjacency;
    std::unordered_map<FeatureNodeId, uint32_t, StableIdHash<FeatureNodeIdTag>> indegree;
    for (const auto& edge : graph.edges) {
        const auto* source = find_node(graph, edge.source_node);
        const auto* target = find_node(graph, edge.target_node);
        if (source == nullptr || target == nullptr) {
            const bool missing_source = source == nullptr;
            return fail(invalid("FeatureGraph edge references an unknown node"), nullptr,
                        missing_source ? edge.source_port : edge.target_port,
                        missing_source ? edge.source_node : edge.target_node);
        }
        const auto* source_definition = registry.find_definition(source->definition_id);
        const auto* target_definition = registry.find_definition(target->definition_id);
        if (source_definition == nullptr || target_definition == nullptr ||
            edge.source_clock != source_definition->contract.clock.id ||
            edge.target_clock != target_definition->contract.clock.id) {
            const bool bad_source = source_definition == nullptr ||
                                    edge.source_clock != source_definition->contract.clock.id;
            return fail(invalid("FeatureGraph edge clocks must match definition contracts"),
                        bad_source ? source : target, bad_source ? edge.source_port : edge.target_port);
        }
        const auto* source_port = source_definition == nullptr
                                      ? nullptr
                                      : find_port(source_definition->contract, edge.source_port,
                                                  PortDirection::kOutput);
        const auto* target_port = target_definition == nullptr
                                      ? nullptr
                                      : find_port(target_definition->contract, edge.target_port,
                                                  PortDirection::kInput);
        if (source_port == nullptr || target_port == nullptr || source_port->schema != target_port->schema ||
            source_port->version != target_port->version || edge.version == 0 ||
            edge.version != source_port->version) {
            const bool bad_source = source_port == nullptr || (target_port != nullptr &&
                (edge.version == 0 || edge.version != source_port->version));
            return fail(invalid("FeatureGraph edge ports must have matching versioned schemas"),
                        bad_source ? source : target, bad_source ? edge.source_port : edge.target_port);
        }
        if (const auto result = validate_port_projection(lookup, source_definition->id, edge.source_port,
                                                         PortDirection::kOutput);
            !result) {
            return fail(result, source, edge.source_port);
        }
        if (const auto result = validate_port_projection(lookup, target_definition->id, edge.target_port,
                                                         PortDirection::kInput);
            !result) {
            return fail(result, target, edge.target_port);
        }
        if (edge.source_clock != edge.target_clock && edge.kind == EdgeKind::kData) {
            return fail(invalid("cross-clock data edges require an explicit transport policy"), target, edge.target_port);
        }
        const auto source_domains = port_domains(lookup, source_definition->id, edge.source_port,
                                                 PortDirection::kOutput);
        const auto target_domains = port_domains(lookup, target_definition->id, edge.target_port,
                                                 PortDirection::kInput);
        if (source_domains != target_domains && edge.kind == EdgeKind::kData) {
            return fail(invalid("cross-domain data edges require an explicit transport policy"), target, edge.target_port);
        }
        if ((edge.kind == EdgeKind::kQueue && edge.capacity == 0) ||
            (edge.kind == EdgeKind::kSnapshot && edge.deadline_ticks == 0)) {
            return fail(invalid("clock transport requires capacity or deadline"), source, edge.source_port);
        }
        if (edge.source_clock == edge.target_clock &&
            (edge.kind == EdgeKind::kData || edge.kind == EdgeKind::kCommand || edge.kind == EdgeKind::kReceipt)) {
            adjacency[edge.source_node].push_back(edge.target_node);
            ++indegree[edge.target_node];
        }
    }

    std::unordered_set<FeatureNodeId, StableIdHash<FeatureNodeIdTag>> visiting;
    std::unordered_set<FeatureNodeId, StableIdHash<FeatureNodeIdTag>> visited;
    std::function<bool(FeatureNodeId)> visit = [&](FeatureNodeId node) {
        if (visiting.contains(node)) {
            return false;
        }
        if (visited.contains(node)) {
            return true;
        }
        visiting.insert(node);
        for (const auto child : adjacency[node]) {
            if (!visit(child)) {
                return false;
            }
        }
        visiting.erase(node);
        visited.insert(node);
        return true;
    };
    for (const auto& node : graph.nodes) {
        if (!visit(node.node_id)) {
            return fail(invalid("same-clock unbuffered FeatureGraph cycle is not executable"), &node);
        }
    }
    for (auto& plan : compiled.plans) {
        std::vector<FeatureInstanceId> ordered;
        std::unordered_set<FeatureNodeId, StableIdHash<FeatureNodeIdTag>> emitted;
        ordered.reserve(plan.instances.size());
        while (ordered.size() < plan.instances.size()) {
            bool advanced = false;
            for (const auto instance : plan.instances) {
                const FeatureNodeId node_id(instance.value());
                if (emitted.contains(node_id) || indegree[node_id] != 0) continue;
                emitted.insert(node_id);
                ordered.push_back(instance);
                for (const auto child : adjacency[node_id]) {
                    if (indegree[child] != 0) --indegree[child];
                }
                advanced = true;
            }
            if (!advanced) break;
        }
        if (ordered.size() != plan.instances.size()) {
            return fail(invalid("FeatureGraph execution plan could not be topologically ordered"),
                        find_node(graph, FeatureNodeId(plan.instances.front().value())));
        }
        plan.instances = std::move(ordered);
    }
    for (const auto& process : graph.process_subgraphs) {
        const auto* owner = find_node(graph, process.owner_node);
        const auto* owner_definition = owner == nullptr ? nullptr : registry.find_definition(owner->definition_id);
        if (owner == nullptr || owner_definition == nullptr ||
            (owner_definition->contract.execution != ExecutionKind::kProcess &&
             owner_definition->contract.state != StatePolicy::kProcessState))
            return fail(invalid("process subgraph requires a Process-capable owner"), owner, {}, process.owner_node);
        if (const auto machine = make_process_state_machine(process, owner_definition->contract); !machine)
            return fail(machine.result, owner);
    }
    std::unordered_set<FeatureNodeId, StableIdHash<FeatureNodeIdTag>> process_owners;
    for (const auto& process : graph.process_subgraphs) {
        if (!process_owners.insert(process.owner_node).second)
            return fail(invalid("each Feature node may declare only one Process subgraph"),
                        find_node(graph, process.owner_node), {}, process.owner_node);
    }
    return {foundation::Result::success(), std::move(compiled)};
}
}  // namespace aeg::feature
