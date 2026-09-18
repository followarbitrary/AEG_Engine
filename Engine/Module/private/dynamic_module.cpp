#include "../public/dynamic_module.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <mutex>
#include <new>

#include "dynamic_library.hpp"
#include "module_abi.hpp"

namespace aeg::module {
struct DynamicModuleLeaseState final {
    std::mutex mutex{};
    std::condition_variable released{};
    std::size_t active_leases{};
    bool retiring{};
    bool loaded{};
};

namespace {
void release_lease(const std::shared_ptr<DynamicModuleLeaseState>& state) noexcept {
    if (!state)
        return;
    std::lock_guard lock(state->mutex);
    if (state->active_leases != 0)
        --state->active_leases;
    if (state->active_leases == 0)
        state->released.notify_all();
}
}  // namespace

DynamicModuleLease::~DynamicModuleLease() {
    release_lease(state_);
}
DynamicModuleLease::DynamicModuleLease(DynamicModuleLease&& other) noexcept : state_(std::move(other.state_)) {}
DynamicModuleLease& DynamicModuleLease::operator=(DynamicModuleLease&& other) noexcept {
    if (this == &other)
        return *this;
    release_lease(state_);
    state_ = std::move(other.state_);
    return *this;
}

namespace {
foundation::ResultValue<std::string> read_binary(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {foundation::not_found("dynamic module binary is missing"), {}};
    return {foundation::Result::success(), std::string((std::istreambuf_iterator<char>(input)), {})};
}
}  // namespace

foundation::Result DynamicModuleLoader::load(const ModuleBundle& bundle, DynamicModuleHandle& handle) {
    if (!registry_ || bundle.manifest_path.empty() || bundle.metadata.build_id.empty())
        return foundation::invalid_argument("dynamic module bundle is invalid");
    if (handle.loaded)
        return foundation::invalid_state("dynamic module handle is already loaded");
    std::shared_ptr<DynamicModuleLeaseState> lifetime;
    try {
        lifetime = std::make_shared<DynamicModuleLeaseState>();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("dynamic module lifetime allocation failed");
    }

    ModuleBundle verified = bundle;
    // Structured bundles are revalidated immediately before loading. Legacy
    // metadata-only fixtures intentionally keep the old no-DLL development path.
    if (bundle.metadata_path != bundle.manifest_path || !bundle.binary_sha256.empty()) {
        const auto result = validate_module_bundle(bundle.manifest_path);
        if (!result)
            return result.result;
        verified = result.value;
    }
    if (!verified.binary_path.empty() && !verified.binary_sha256.empty()) {
        const auto bytes = read_binary(verified.binary_path);
        if (!bytes)
            return bytes.result;
        const auto digest = module_content_sha256(bytes.value);
        if (!digest || digest.value != verified.binary_sha256)
            return foundation::invalid_state("dynamic module binary digest mismatch");
    }

    AegAbiModuleApi negotiated_api{};
    if (!verified.binary_path.empty()) {
        auto library = detail::DynamicLibrary::open(verified.binary_path);
        if (!library)
            return library.result;
        const auto symbol = library.value.symbol("aeg_get_module_api");
        if (!symbol)
            return symbol.result;
        const auto entry = reinterpret_cast<AegAbiGetModuleApi>(symbol.value);
        AegAbiModuleApi api{};
        const auto abi_result =
            entry(AEG_MODULE_ABI_MAJOR, AEG_MODULE_ABI_MINOR, static_cast<uint32_t>(sizeof(api)), &api);
        if (abi_result.code != AEG_ABI_OK)
            return foundation::invalid_argument("module ABI negotiation failed");
        if (const auto result = validate_module_api(api, capabilities_); !result)
            return result;
        negotiated_api = api;
        handle.native_handle = library.value.detach();
    }

    handle.bundle = std::move(verified);
    handle.api = negotiated_api;
    handle.generation += 1;
    handle.loaded = true;
    lifetime->loaded = true;
    handle.lifetime = std::move(lifetime);
    return foundation::Result::success();
}

foundation::ResultValue<DynamicModuleLease> DynamicModuleLoader::acquire(const DynamicModuleHandle& handle) const {
    if (!handle.loaded || !handle.lifetime)
        return {foundation::not_found("dynamic module handle is not loaded"), {}};
    std::lock_guard lock(handle.lifetime->mutex);
    if (!handle.lifetime->loaded || handle.lifetime->retiring)
        return {foundation::invalid_state("dynamic module is retiring"), {}};
    ++handle.lifetime->active_leases;
    return {foundation::Result::success(), DynamicModuleLease(handle.lifetime)};
}

foundation::Result DynamicModuleLoader::unload(DynamicModuleHandle& handle, uint64_t timeout_ms) {
    if (!handle.loaded)
        return foundation::not_found("dynamic module handle is not loaded");
    const auto lifetime = handle.lifetime;
    if (lifetime) {
        std::unique_lock lock(lifetime->mutex);
        lifetime->retiring = true;
        const auto ready = [&] { return lifetime->active_leases == 0; };
        if (timeout_ms == 0 ? !ready()
                            : !lifetime->released.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready)) {
            lifetime->retiring = false;
            return foundation::Result::failure(foundation::ResultCode::kTimeout,
                                               "dynamic module still has active leases");
        }
        lifetime->loaded = false;
    }
    if (handle.native_handle != nullptr) {
        auto library = detail::DynamicLibrary::adopt(handle.native_handle);
        const auto result = library.close();
        handle.native_handle = nullptr;
        if (!result) {
            if (lifetime) {
                std::lock_guard lock(lifetime->mutex);
                lifetime->loaded = true;
                lifetime->retiring = false;
            }
            return result;
        }
    }
    handle.loaded = false;
    handle.bundle = {};
    handle.api = {};
    handle.lifetime.reset();
    return foundation::Result::success();
}
}  // namespace aeg::module
