#include "../public/feature_module_export.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "../abi/feature_module_api.h"
#include "../public/module_bundle.hpp"
#include "feature_callback_codec.hpp"
#include "module_context_table.hpp"

namespace aeg::module::detail {
namespace {
AegAbiResult abi_result(uint32_t code = AEG_ABI_OK) noexcept {
    return {sizeof(AegAbiResult), code, {nullptr, 0}};
}
bool valid_bytes(AegAbiBytes bytes) noexcept {
    return bytes.size <= AEG_FEATURE_ABI_MAX_BYTES && (bytes.data || bytes.size == 0);
}
AegAbiResult emit(std::span<const uint8_t> bytes, AegAbiByteSink sink) {
    if (!sink.write || bytes.size() > AEG_FEATURE_ABI_MAX_BYTES)
        return abi_result(AEG_ABI_INVALID_ARGUMENT);
    return sink.write(sink.user_data, {bytes.data(), bytes.size()});
}
AegAbiResult emit_result(const foundation::ResultValue<feature::StateDelta>& result, AegAbiByteSink sink) {
    const auto encoded = encode_feature_callback_result(result);
    return encoded ? emit(encoded.value, sink) : abi_result(AEG_ABI_INVALID_STATE);
}
}  // namespace

struct FeatureModuleExport::Impl final {
    ModuleContextTable contexts{};
    ModuleContextLease context{};
    FeatureModuleDefinition definition;
    std::string metadata;
    AegFeatureExecutionApi api{};

    explicit Impl(FeatureModuleDefinition source) : definition(std::move(source)) {
        const auto valid = validate_feature_module(definition);
        if (!valid || definition.feature_definitions.size() > AEG_FEATURE_ABI_MAX_DEFINITIONS)
            throw std::invalid_argument("dynamic Feature module definition is invalid");
        for (const auto& item : definition.feature_definitions)
            if (item.contract.structure == feature::FeatureStructure::kAtomic && !item.execute)
                throw std::invalid_argument("dynamic atomic Feature has no executable callback");
        ModuleMetadata description{};
        description.module = definition.descriptor;
        for (const auto& item : definition.feature_definitions)
            if (!description.feature_catalog.export_definition(item))
                throw std::invalid_argument("dynamic Feature catalog export failed");
        const auto encoded = encode_module_metadata(description);
        if (!encoded)
            throw std::invalid_argument("dynamic Feature metadata encoding failed");
        metadata = encoded.value;
        auto lease = contexts.acquire(ModuleContextKind::kModule);
        if (!lease)
            throw std::runtime_error("dynamic Feature context allocation failed");
        context = std::move(lease.value);
        api = {sizeof(api), AEG_FEATURE_EXECUTION_API_VERSION, this, context.context(),
               static_cast<uint32_t>(definition.feature_definitions.size()), 0,
               write_contracts, describe_feature, execute, validate_state};
    }

    static Impl* checked(void* data, AegAbiContext context) {
        auto* self = static_cast<Impl*>(data);
        return self && self->contexts.validate(context, ModuleContextKind::kModule) ? self : nullptr;
    }
    const feature::FeatureDefinition* find(uint64_t id) const noexcept {
        const auto iterator = std::find_if(definition.feature_definitions.begin(), definition.feature_definitions.end(),
                                           [id](const auto& value) { return value.id.value() == id; });
        return iterator == definition.feature_definitions.end() ? nullptr : &*iterator;
    }
    static AegAbiResult AEG_ABI_CALL write_contracts(void* data, AegAbiContext context, AegAbiByteSink sink) noexcept {
        try {
            const auto* self = checked(data, context);
            if (!self) return abi_result(AEG_ABI_INVALID_STATE);
            return emit({reinterpret_cast<const uint8_t*>(self->metadata.data()), self->metadata.size()}, sink);
        } catch (...) { return abi_result(AEG_ABI_INVALID_STATE); }
    }
    static AegAbiResult AEG_ABI_CALL describe_feature(void* data, AegAbiContext context, uint32_t index,
                                                    AegFeatureCallbackInfo* output) noexcept {
        try {
            const auto* self = checked(data, context);
            if (!self) return abi_result(AEG_ABI_INVALID_STATE);
            if (!output || output->struct_size < sizeof(*output) || index >= self->definition.feature_definitions.size())
                return abi_result(AEG_ABI_INVALID_ARGUMENT);
            const auto& item = self->definition.feature_definitions[index];
            *output = {sizeof(*output), (item.execute ? AEG_FEATURE_HAS_EXECUTE : 0u) |
                                           (item.validate_state_payload ? AEG_FEATURE_HAS_STATE_VALIDATOR : 0u), item.id.value()};
            return abi_result();
        } catch (...) { return abi_result(AEG_ABI_INVALID_STATE); }
    }
    static AegAbiResult AEG_ABI_CALL execute(void* data, AegAbiContext context, uint64_t id,
                                           const AegFeatureInvocation* invocation, AegAbiByteSink sink) noexcept {
        try {
            const auto* self = checked(data, context);
            if (!self) return abi_result(AEG_ABI_INVALID_STATE);
            const auto* item = self->find(id);
            if (!item || !item->execute) return abi_result(AEG_ABI_UNSUPPORTED);
            if (!invocation || invocation->struct_size < sizeof(*invocation) ||
                invocation->version != AEG_FEATURE_EXECUTION_API_VERSION || invocation->reserved != 0 ||
                !valid_bytes(invocation->state_payload) || !valid_bytes(invocation->input) ||
                !valid_bytes(invocation->parameters) || invocation->current_state.size > AEG_FEATURE_ABI_MAX_BYTES ||
                (!invocation->current_state.data && invocation->current_state.size != 0) || !sink.write)
                return abi_result(AEG_ABI_INVALID_ARGUMENT);
            const feature::InvocationToken token{feature::InvocationId(invocation->invocation_id),
                feature::AttemptId(invocation->attempt_id), feature::RuntimeGeneration(invocation->generation),
                invocation->input_version, feature::ProcessRevision(invocation->process_revision),
                feature::StateVersion(invocation->state_version)};
            feature::StateSnapshot state{};
            state.version = feature::StateVersion(invocation->snapshot_version);
            state.schema_version = {invocation->schema_major, invocation->schema_minor, invocation->schema_patch};
            if (invocation->state_payload.size)
                state.payload.assign(invocation->state_payload.data,
                                     invocation->state_payload.data + invocation->state_payload.size);
            if (invocation->current_state.size)
                state.current_state.assign(invocation->current_state.data, invocation->current_state.size);
            foundation::ResultValue<feature::StateDelta> result{};
            try {
                result = item->execute(token, state, {invocation->input.data, static_cast<size_t>(invocation->input.size)},
                                      {invocation->parameters.data, static_cast<size_t>(invocation->parameters.size)});
            } catch (...) {
                result.result = foundation::Result::failure(foundation::ResultCode::kUnknown, "Feature callback threw");
            }
            return emit_result(result, sink);
        } catch (...) { return abi_result(AEG_ABI_INVALID_STATE); }
    }
    static AegAbiResult AEG_ABI_CALL validate_state(void* data, AegAbiContext context, uint64_t id,
                                                  AegAbiBytes payload, AegAbiByteSink sink) noexcept {
        try {
            const auto* self = checked(data, context);
            if (!self) return abi_result(AEG_ABI_INVALID_STATE);
            const auto* item = self->find(id);
            if (!item || !item->validate_state_payload) return abi_result(AEG_ABI_UNSUPPORTED);
            if (!valid_bytes(payload) || !sink.write) return abi_result(AEG_ABI_INVALID_ARGUMENT);
            foundation::Result result{};
            try { result = item->validate_state_payload({payload.data, static_cast<size_t>(payload.size)}); }
            catch (...) { result = foundation::Result::failure(foundation::ResultCode::kUnknown, "Feature state validator threw"); }
            return emit_result({std::move(result), {}}, sink);
        } catch (...) { return abi_result(AEG_ABI_INVALID_STATE); }
    }
};

FeatureModuleExport::FeatureModuleExport(FeatureModuleDefinition definition)
    : impl_(std::make_unique<Impl>(std::move(definition))) {}
FeatureModuleExport::~FeatureModuleExport() = default;
AegAbiResult FeatureModuleExport::negotiate(uint32_t major, uint32_t minor, uint32_t size,
                                           AegAbiModuleApi* output) const noexcept {
    const auto request = aeg_check_module_api_request(major, minor, size, output);
    if (request.code != AEG_ABI_OK) return request;
    const AegAbiModuleApi api{sizeof(api), AEG_MODULE_ABI_MAJOR, AEG_MODULE_ABI_MINOR, 0, AEG_CAP_FEATURES,
                             nullptr, &impl_->api, nullptr};
    std::memcpy(output, &api, (std::min)(size, static_cast<uint32_t>(sizeof(api))));
    return abi_result();
}
}  // namespace aeg::module::detail
