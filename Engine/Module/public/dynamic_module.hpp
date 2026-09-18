#pragma once
#include <memory>

#include "../abi/module_api.h"
#include "module_bundle.hpp"
namespace aeg::module {
inline constexpr uint64_t kDynamicV1Capabilities = AEG_CAP_METADATA | AEG_CAP_PIPELINE_DECLARATION | AEG_CAP_LOGIC |
                                                   AEG_CAP_RENDER | AEG_CAP_VALUE_REFLECTION | AEG_CAP_RENDER_PROGRAMS;

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
    explicit DynamicModuleLoader(ModuleRegistry& registry, uint64_t capabilities = kDynamicV1Capabilities) noexcept
        : registry_(&registry), capabilities_(capabilities) {}
    [[nodiscard]] foundation::Result load(const ModuleBundle& bundle, DynamicModuleHandle& handle);
    [[nodiscard]] foundation::Result unload(DynamicModuleHandle& handle, uint64_t timeout_ms = 0);
    /** Acquire a borrow for callbacks, plans or instances that may outlive a frame. */
    [[nodiscard]] foundation::ResultValue<DynamicModuleLease> acquire(const DynamicModuleHandle& handle) const;
    [[nodiscard]] bool is_loaded(const DynamicModuleHandle& handle) const noexcept {
        return handle.loaded && handle.generation != 0;
    }
    [[nodiscard]] uint64_t generation(const DynamicModuleHandle& handle) const noexcept { return handle.generation; }

private:
    ModuleRegistry* registry_{};
    uint64_t capabilities_{kDynamicV1Capabilities};
};
}  // namespace aeg::module
