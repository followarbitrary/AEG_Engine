#include "pipeline_asset_encoder.hpp"

#include "pipeline_asset.hpp"

namespace aeg::tooling {

foundation::ResultValue<std::vector<std::byte>> encode_pipeline_asset(const pipeline::PipelineDocument& document) {
    return asset::encode_pipeline_asset(document);
}

}  // namespace aeg::tooling
