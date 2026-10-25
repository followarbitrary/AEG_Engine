#pragma once
#include <memory>

#include "../abi/module_api.h"
#include "feature_module.hpp"

namespace aeg::module::detail {
/** DLL-local bridge used by the standard export macro, never a C++ DLL ABI. */
class FeatureModuleExport final {
public:
    explicit FeatureModuleExport(FeatureModuleDefinition definition);
    ~FeatureModuleExport();
    FeatureModuleExport(const FeatureModuleExport&) = delete;
    FeatureModuleExport& operator=(const FeatureModuleExport&) = delete;
    [[nodiscard]] AegAbiResult negotiate(uint32_t major, uint32_t minor, uint32_t size,
                                         AegAbiModuleApi* output) const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace aeg::module::detail
