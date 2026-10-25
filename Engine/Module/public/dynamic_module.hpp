#pragma once
#include <memory>

#include "../abi/module_api.h"
#include "module.hpp"
#include "module_bundle.hpp"
#include "feature_module.hpp"
namespace aeg::module {
inline constexpr uint64_t kDynamicModuleCapabilities = AEG_CAP_ALL;

struct DynamicModuleLeaseState;

/** A scoped host borrow that keeps a dynamic module resident until release. */
class DynamicModuleLease final {
public:
    DynamicModuleLease() noexcept = default;
    ~DynamicModuleLease();
    DynamicModuleLease(const DynamicModuleLease&) = delete;
    DynamicModuleLease& operator=(const DynamicModuleLease&) = delete;
    DynamicModuleLease(DynamicModuleLease&& other) noexcept;
    DynamicModuleLease& operator=(DynamicModuleLease&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(state_); }

private:
    explicit DynamicModuleLease(std::shared_ptr<DynamicModuleLeaseState> state) noexcept : state_(std::move(state)) {}
    std::shared_ptr<DynamicModuleLeaseState> state_{};
    friend class DynamicModuleLoader;
};

struct DynamicModuleHandle final {
    uint64_t generation{};
    ModuleBundle bundle{};
    /** Negotiated fixed ABI table copied from the module export. */
    AegAbiModuleApi api{};
    void* native_handle{};
    bool loaded{};
    std::shared_ptr<DynamicModuleLeaseState> lifetime{};
};

class DynamicModuleLoader final {
public:
    explicit DynamicModuleLoader(ModuleRegistry& registry, uint64_t capabilities = kDynamicModuleCapabilities) noexcept
        : registry_(&registry), capabilities_(capabilities) {}
    /** Revalidates the selected bundle and rejects changed build/content/contract
     * facts before opening code. A different build requires explicit selection.
     */
    [[nodiscard]] foundation::Result load(const ModuleBundle& bundle, DynamicModuleHandle& handle);
    [[nodiscard]] foundation::Result unload(DynamicModuleHandle& handle, uint64_t timeout_ms = 0);
    /** Acquire a borrow for callbacks, plans or instances that may outlive a frame. */
    [[nodiscard]] foundation::ResultValue<DynamicModuleLease> acquire(const DynamicModuleHandle& handle) const;
    /** Owning host callbacks automatically borrow the verified DLL's code. */
    [[nodiscard]] foundation::ResultValue<FeatureModuleDefinition> make_feature_module(const DynamicModuleHandle&) const;
    [[nodiscard]] bool is_loaded(const DynamicModuleHandle& handle) const noexcept {
        return handle.loaded && handle.generation != 0;
    }
    [[nodiscard]] uint64_t generation(const DynamicModuleHandle& handle) const noexcept { return handle.generation; }

private:
    ModuleRegistry* registry_{};
    uint64_t capabilities_{kDynamicModuleCapabilities};
};
}  // namespace aeg::module
