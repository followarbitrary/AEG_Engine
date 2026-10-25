#include "../public/dynamic_module.hpp"

#include <chrono>
#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <mutex>
#include <new>

#include "dynamic_library.hpp"
#include "module_abi.hpp"
#include "../abi/feature_module_api.h"
#include "feature_callback_codec.hpp"

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
struct CallbackPacket final {
    std::vector<uint8_t> bytes{};
    bool rejected{};
    static AegAbiResult AEG_ABI_CALL write(void* data, AegAbiBytes chunk) noexcept {
        auto& packet = *static_cast<CallbackPacket*>(data);
        try {
            if (packet.rejected || (!chunk.data && chunk.size) ||
                chunk.size > AEG_FEATURE_ABI_MAX_BYTES - packet.bytes.size()) {
                packet.rejected = true;
                return {sizeof(AegAbiResult), AEG_ABI_INVALID_ARGUMENT, {nullptr, 0}};
            }
            if (chunk.size) packet.bytes.insert(packet.bytes.end(), chunk.data, chunk.data + chunk.size);
            return {sizeof(AegAbiResult), AEG_ABI_OK, {nullptr, 0}};
        } catch (...) {
            packet.rejected = true;
            return {sizeof(AegAbiResult), AEG_ABI_INVALID_STATE, {nullptr, 0}};
        }
    }
};
template <typename Call>
foundation::ResultValue<std::vector<uint8_t>> receive_callback(Call&& call) {
    CallbackPacket packet{};
    try {
        const auto result = call(AegAbiByteSink{&packet, CallbackPacket::write});
        if (packet.rejected) return {foundation::invalid_argument("dynamic Feature response exceeds sink bounds"), {}};
        if (result.struct_size < sizeof(AegAbiResult) || result.code > AEG_ABI_INVALID_STATE)
            return {foundation::invalid_argument("dynamic Feature ABI response is invalid"), {}};
        if (result.code != AEG_ABI_OK) {
            const auto code = result.code == AEG_ABI_UNSUPPORTED ? foundation::ResultCode::kUnsupported
                            : result.code == AEG_ABI_INVALID_ARGUMENT ? foundation::ResultCode::kInvalidArgument
                            : foundation::ResultCode::kInvalidState;
            return {foundation::Result::failure(code, "dynamic Feature ABI callback failed"), {}};
        }
        return {foundation::Result::success(), std::move(packet.bytes)};
    } catch (...) {
        return {foundation::Result::failure(foundation::ResultCode::kUnknown, "dynamic Feature ABI callback threw"), {}};
    }
}
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

    const auto validated = validate_module_bundle(bundle.manifest_path);
    if (!validated) return validated.result;
    ModuleBundle verified = validated.value;
    if (bundle.binary_path != verified.binary_path || bundle.metadata_path != verified.metadata_path ||
        bundle.binary_sha256 != verified.binary_sha256 || bundle.metadata_sha256 != verified.metadata_sha256 ||
        bundle.metadata.module.id != verified.metadata.module.id ||
        bundle.metadata.build_id != verified.metadata.build_id || bundle.metadata.target != verified.metadata.target ||
        bundle.metadata.configuration != verified.metadata.configuration ||
        bundle.metadata.contract_digest != verified.metadata.contract_digest)
        return foundation::invalid_state("Selected module bundle changed before loading");
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

foundation::ResultValue<FeatureModuleDefinition> DynamicModuleLoader::make_feature_module(
    const DynamicModuleHandle& handle) const {
    try {
        auto acquired = acquire(handle);
        if (!acquired) return {acquired.result, {}};
        auto code_lease = std::make_shared<DynamicModuleLease>(std::move(acquired.value));
        if (!(handle.api.required_capabilities & AEG_CAP_FEATURES) || !handle.api.features)
            return {foundation::unsupported("dynamic module has no Feature execution API"), {}};
        if (const auto valid = validate_module_api(handle.api, capabilities_); !valid) return {valid, {}};
        const auto table = *handle.api.features;
        const auto bytes = receive_callback([&](AegAbiByteSink sink) {
            return table.write_contracts(table.user_data, table.context, sink);
        });
        if (!bytes) return {bytes.result, {}};
        const auto actual = decode_module_metadata({reinterpret_cast<const char*>(bytes.value.data()), bytes.value.size()});
        if (!actual) return {actual.result, {}};
        if (actual.value.feature_catalog.entries().size() != table.feature_count ||
            actual.value.feature_catalog.entries().size() != handle.bundle.metadata.feature_catalog.entries().size())
            return {foundation::invalid_argument("dynamic Feature definitions do not match the bundle"), {}};

        // Order is presentation, not identity. Preserve the DLL's entire contract
        // in the verified sidecar order before using the existing full SHA codec.
        auto normalized = actual.value;
        normalized.feature_catalog = {};
        for (const auto& entry : handle.bundle.metadata.feature_catalog.entries()) {
            const auto& candidates = actual.value.feature_catalog.entries();
            const auto found = std::find_if(candidates.begin(), candidates.end(), [&](const auto& candidate) {
                return candidate.definition_id == entry.definition_id;
            });
            if (found == candidates.end()) return {foundation::invalid_argument("dynamic Feature identity is missing"), {}};
            if (const auto added = normalized.feature_catalog.add_entry(*found); !added) return {added, {}};
        }
        const auto actual_digest = module_contract_sha256(normalized);
        const auto verified_digest = module_contract_sha256(handle.bundle.metadata);
        if (!actual_digest || !verified_digest || actual_digest.value != verified_digest.value)
            return {foundation::invalid_argument("DLL Feature contracts differ from the verified sidecar"), {}};

        FeatureModuleDefinition definition{};
        definition.descriptor = normalized.module;
        definition.feature_catalog = normalized.feature_catalog;
        for (uint32_t index = 0; index < table.feature_count; ++index) {
            AegFeatureCallbackInfo info{sizeof(info), 0, 0};
            const auto result = table.describe_feature(table.user_data, table.context, index, &info);
            if (result.struct_size < sizeof(result) || result.code != AEG_ABI_OK || info.struct_size < sizeof(info) ||
                (info.flags & ~(AEG_FEATURE_HAS_EXECUTE | AEG_FEATURE_HAS_STATE_VALIDATOR)))
                return {foundation::invalid_argument("dynamic Feature callback description is invalid"), {}};
            const auto& entries = normalized.feature_catalog.entries();
            const auto entry = std::find_if(entries.begin(), entries.end(), [&](const auto& candidate) {
                return candidate.definition_id.value() == info.definition_id;
            });
            if (entry == entries.end() || (entry->contract.structure == feature::FeatureStructure::kAtomic &&
                                           !(info.flags & AEG_FEATURE_HAS_EXECUTE)))
                return {foundation::invalid_argument("dynamic atomic Feature callback is missing"), {}};
            feature::FeatureDefinition item{};
            item.id = entry->definition_id;
            item.version = entry->version;
            item.owner_module = entry->owner_module;
            item.contract = entry->contract;
            if (info.flags & AEG_FEATURE_HAS_EXECUTE) {
                item.execute = [table, code_lease, id = info.definition_id](const feature::InvocationToken& token,
                    const feature::StateSnapshot& state, std::span<const uint8_t> input, std::span<const uint8_t> parameters) {
                    const AegFeatureInvocation invocation{sizeof(AegFeatureInvocation), AEG_FEATURE_EXECUTION_API_VERSION,
                        token.invocation_id.value(), token.attempt_id.value(), token.generation.value(), token.input_version,
                        token.process_revision.value(), token.state_version.value(), state.version.value(),
                        state.schema_version.major, state.schema_version.minor, state.schema_version.patch, 0,
                        {state.payload.data(), state.payload.size()}, {state.current_state.data(), state.current_state.size()},
                        {input.data(), input.size()}, {parameters.data(), parameters.size()}};
                    const auto response = receive_callback([&](AegAbiByteSink sink) {
                        return table.execute(table.user_data, table.context, id, &invocation, sink);
                    });
                    return response ? detail::decode_feature_callback_result(response.value)
                                    : foundation::ResultValue<feature::StateDelta>{response.result, {}};
                };
            }
            if (info.flags & AEG_FEATURE_HAS_STATE_VALIDATOR) {
                item.validate_state_payload = [table, code_lease, id = info.definition_id](std::span<const uint8_t> payload) {
                    const auto response = receive_callback([&](AegAbiByteSink sink) {
                        return table.validate_state(table.user_data, table.context, id, {payload.data(), payload.size()}, sink);
                    });
                    return response ? detail::decode_feature_callback_result(response.value).result : response.result;
                };
            }
            definition.feature_definitions.push_back(std::move(item));
        }
        if (const auto valid = validate_feature_module(definition); !valid) return {valid, {}};
        return {foundation::Result::success(), std::move(definition)};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("dynamic Feature import allocation failed"), {}};
    } catch (...) {
        return {foundation::Result::failure(foundation::ResultCode::kUnknown, "dynamic Feature import failed"), {}};
    }
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
