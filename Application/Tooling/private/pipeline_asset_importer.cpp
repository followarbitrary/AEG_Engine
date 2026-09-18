#include "pipeline_asset_importer.hpp"

#include <array>
#include <fstream>
#include <iterator>

#include "pipeline_asset.hpp"
#include "pipeline_asset_encoder.hpp"
#include "pipeline_document_io.hpp"

namespace aeg::tooling {

PipelineAssetImporter::PipelineAssetImporter(std::shared_ptr<const pipeline::TypeCatalog> catalog)
    : catalog_(std::move(catalog)) {}

bool PipelineAssetImporter::supports(const std::filesystem::path& source) const noexcept {
    return source.extension() == ".aegpipe";
}

foundation::ResultValue<std::vector<CookedAsset>> PipelineAssetImporter::cook(const std::filesystem::path& source,
                                                                              const AssetMeta& meta) const {
    if (!catalog_)
        return {foundation::invalid_state("Pipeline importer has no TypeCatalog"), {}};
    const auto document = ::aeg::application::project::load_pipeline_document(source);
    if (!document)
        return {document.result, {}};
    const auto diagnostics = pipeline::validate_pipeline_document(document.value, *catalog_);
    if (!diagnostics.empty())
        return {foundation::invalid_argument(diagnostics.front().message), {}};
    const auto payload = asset::encode_pipeline_asset(document.value);
    if (!payload)
        return {payload.result, {}};
    const auto document_id = AssetId::parse(document.value.asset_uuid);
    if (!document_id || meta.products.size() != 1 || meta.products.front().kind != AssetKind::kPipeline ||
        meta.products.front().id.value.bytes == std::array<uint8_t, 16>{} ||
        document_id.value != meta.products.front().id)
        return {foundation::invalid_argument("Pipeline .meta must declare exactly one pipeline product"), {}};
    CookedAsset result;
    result.id = meta.products.front().id;
    result.kind = AssetKind::kPipeline;
    result.schema_version = 1;
    result.payload = payload.value;
    return {foundation::Result::success(), {std::move(result)}};
}

}  // namespace aeg::tooling
