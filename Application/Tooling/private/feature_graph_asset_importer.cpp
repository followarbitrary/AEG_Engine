#include "feature_graph_asset_importer.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

#include "asset_identity.hpp"
#include "feature_graph_asset.hpp"
#include "feature_graph_io.hpp"

namespace aeg::tooling {

bool FeatureGraphAssetImporter::supports(const std::filesystem::path& source) const noexcept {
    return source.extension() == ".aegfeature";
}

foundation::ResultValue<std::vector<CookedAsset>> FeatureGraphAssetImporter::cook(
    const std::filesystem::path& source, const AssetMeta& meta) const {
    auto document = aeg::application::authoring::load_feature_graph_document(source);
    if (!document) {
        return {document.result, {}};
    }
    // Process/Feature canvas metadata belongs to the source document. Layout-only edits
    // must not change the runtime payload or remove unrelated extension records.
    std::erase_if(document.value.extensions, [](const std::string& extension) {
        return extension.starts_with("aeg.editor.process-layout.") ||
               extension.starts_with("aeg.editor.feature-layout.v1:");
    });
    document.value.layout_revision = 0;
    const auto payload = aeg::asset::encode_feature_graph_asset(document.value);
    if (!payload) {
        return {payload.result, {}};
    }
    const auto document_id = foundation::AssetId::parse(document.value.asset_uuid);
    if (!document_id || meta.products.size() != 1 ||
        meta.products.front().kind != foundation::AssetKind::kFeatureGraph ||
        meta.products.front().id.value.bytes == std::array<uint8_t, 16>{} ||
        document_id.value != meta.products.front().id) {
        return {foundation::invalid_argument(
                    "FeatureGraph .meta must declare exactly one FeatureGraph product"),
                {}};
    }
    CookedAsset cooked{};
    cooked.id = meta.products.front().id;
    cooked.kind = foundation::AssetKind::kFeatureGraph;
    cooked.schema_version = document.value.schema_version;
    cooked.payload = payload.value;
    return {foundation::Result::success(), {std::move(cooked)}};
}

}  // namespace aeg::tooling
