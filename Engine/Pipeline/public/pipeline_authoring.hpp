/** @file pipeline_authoring.hpp @brief Catalog and validation APIs for authoring values. */
#pragma once
#include <concepts>
#include <map>
#include <span>
#include <type_traits>
#include <vector>

#include "pipeline.hpp"
#include "pipeline_configuration.hpp"
#include "pipeline_document.hpp"
#include "render_pass.hpp"
namespace aeg::pipeline {
template <class T>
concept SupportedSingleDomainFeature =
    std::derived_from<T, LogicFeature> || std::derived_from<T, PhysicsFeature> || std::derived_from<T, RenderFeature> ||
    std::derived_from<T, AudioFeature> || std::derived_from<T, StreamFeature>;
// CompositeFeature registration is reviewed separately. This concept only checks
// single-domain bases; composites use an explicit non-template registration path
// and require review of their parts and domain contracts.
template <class T>
concept SupportedGameFeature = SupportedSingleDomainFeature<std::remove_cvref_t<T>>;
class TypeCatalog final {
public:
    [[nodiscard]] foundation::Result add(const TypeCatalogEntry& entry);
    [[nodiscard]] const TypeCatalogEntry* find(AuthoringTypeKey key) const noexcept;
    /** Borrowed ordered contiguous snapshot; invalidated by the next successful add. */
    [[nodiscard]] std::span<const TypeCatalogEntry> entries() const;

private:
    void rebuild_entries_snapshot() const;
    std::map<AuthoringTypeKey, TypeCatalogEntry> entries_by_key_{};
    mutable std::vector<TypeCatalogEntry> entries_snapshot_{};
    mutable bool snapshot_dirty_{true};
};
[[nodiscard]] foundation::Result validate_parameter_block(const ParameterBlock& values,
                                                          std::span<const ParameterDescriptor> schema);
struct DocumentDiagnostic final {
    std::string code{};
    std::string message{};
    std::string node_id{};
    std::string edge_id{};
    uint64_t port_id{};
    uint64_t parameter_id{};
};
[[nodiscard]] std::vector<DocumentDiagnostic> validate_pipeline_document(const PipelineDocument&, const TypeCatalog&);
[[nodiscard]] std::string document_semantic_digest(const PipelineDocument&);
struct PreparedPipelineDocument final {
    std::vector<std::shared_ptr<const PipelineInstanceConfiguration>> configurations{};
    std::vector<DocumentEdge> composition_edges{};
    std::map<std::string, std::string> source_map{};
    std::string semantic_digest{};
};
[[nodiscard]] foundation::ResultValue<PreparedPipelineDocument> prepare_pipeline_document(const PipelineDocument&,
                                                                                          const TypeCatalog&,
                                                                                          const PipelineRegistry&,
                                                                                          const RenderPassRegistry&);
}  // namespace aeg::pipeline
