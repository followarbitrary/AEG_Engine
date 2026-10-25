#include "editor_feature_graph_model.hpp"

#include <algorithm>

namespace aeg::application {

bool feature_palette_matches(const FeaturePaletteEntry& entry,
                             const FeaturePaletteFilter& filter) noexcept {
    return (!filter.domain || (entry.domains & aeg::feature::to_mask(*filter.domain)) != 0) &&
           (!filter.execution || entry.execution == *filter.execution) &&
           (!filter.clock || entry.clock == *filter.clock) &&
           (!filter.structure || entry.structure == *filter.structure) &&
           (!filter.scope || entry.scope == *filter.scope);
}

aeg::foundation::Result EditorFeatureGraphModel::replace_catalog(
    const aeg::feature::FeatureCatalog& catalog) {
    entries_.clear();
    entries_.reserve(catalog.entries().size());
    for (const auto& entry : catalog.entries()) {
        entries_.push_back({entry.definition_id, entry.domains, entry.execution, entry.clock,
                            entry.contract.structure, entry.contract.scope.id});
    }
    return aeg::foundation::Result::success();
}

std::vector<FeaturePaletteEntry> EditorFeatureGraphModel::palette(
    const FeaturePaletteFilter& filter) const {
    std::vector<FeaturePaletteEntry> result;
    for (const auto& entry : entries_) {
        if (feature_palette_matches(entry, filter)) {
            result.push_back(entry);
        }
    }
    return result;
}

aeg::foundation::Result EditorFeatureGraphModel::open_document(
    const aeg::feature::FeatureGraphDocument& document) {
    if (document.asset_uuid.empty() || document.schema_version == 0 || document.graph_version == 0) {
        return aeg::foundation::Result::failure(aeg::foundation::ResultCode::kInvalidArgument,
                                                "FeatureGraph document identity is required");
    }
    document_ = document;
    return aeg::foundation::Result::success();
}

aeg::foundation::ResultValue<const aeg::feature::ProcessSubgraph*> EditorFeatureGraphModel::open_process(
    aeg::feature::FeatureNodeId owner) const {
    if (!document_) {
        return {aeg::foundation::Result::failure(aeg::foundation::ResultCode::kNotReady,
                                                 "no FeatureGraph document is open"), nullptr};
    }
    const auto it = std::find_if(document_->process_subgraphs.begin(), document_->process_subgraphs.end(),
                                 [owner](const auto& subgraph) { return subgraph.owner_node == owner; });
    if (it == document_->process_subgraphs.end()) {
        return {aeg::foundation::Result::failure(aeg::foundation::ResultCode::kNotFound,
                                                 "process subgraph was not found"), nullptr};
    }
    return {aeg::foundation::Result::success(), &*it};
}

RuntimeInspectPath EditorFeatureGraphModel::inspect(
    const aeg::feature::FeatureNodeProvenance& provenance) {
    return {provenance.definition_id, provenance.node_id, provenance.instance_id,
            provenance.process_id, provenance.invocation_id, provenance.owner_path};
}
}  // namespace aeg::application
