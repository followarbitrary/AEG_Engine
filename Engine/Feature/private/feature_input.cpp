#include "feature_input.hpp"

#include <algorithm>
#include <exception>
#include <new>
#include <utility>

namespace aeg::feature::detail {

struct FeatureInputLease final {};

foundation::ResultValue<FeatureInputBinding> FeatureInputAccess::bind(
    FeatureRuntime& runtime, FeatureInstanceId instance, PortId port,
    ValueSchemaId schema, uint32_t version, FeatureInputProjector projector) {
    if (!runtime.on_owner_thread() || runtime.execution_closed_ || runtime.state_callback_active_)
        return {foundation::invalid_state("input binding requires its open, non-reentrant Runtime owner"), {}};
    if (!instance.is_valid() || !port.is_valid() || !schema.is_valid() || version == 0 || !projector)
        return {foundation::invalid_argument("input binding requires an owner, versioned input port and projector"), {}};
    const auto owner = runtime.instances_.find(instance);
    if (owner == runtime.instances_.end())
        return {foundation::not_found("input binding owner was not found"), {}};
    const auto* definition = runtime.registry_.find_definition(owner->second.definition_id);
    if (!definition || owner->second.definition_digest != feature_contract_digest(*definition))
        return {foundation::invalid_state("input binding owner has a retired definition"), {}};
    const auto input = std::find_if(definition->contract.ports.begin(), definition->contract.ports.end(),
        [port](const PortContract& candidate) { return candidate.id == port; });
    if (input == definition->contract.ports.end() || input->direction != PortDirection::kInput ||
        input->schema != schema || input->version != version)
        return {foundation::invalid_argument("input binding does not match the declared input port schema/version"), {}};
    const auto existing = runtime.input_->slots.find(instance);
    if (existing != runtime.input_->slots.end() && !existing->second.lease.expired() &&
        existing->second.generation == runtime.generation_ &&
        existing->second.definition_digest == owner->second.definition_digest)
        return {foundation::invalid_state("Feature owner already has an active input binding"), {}};
    try {
        auto lease = std::make_shared<const FeatureInputLease>();
        FeatureInputState::Slot slot{lease, runtime.generation_, owner->second.definition_digest,
                                     port, schema, version, std::move(projector)};
        runtime.input_->slots.insert_or_assign(instance, std::move(slot));
        return {foundation::Result::success(), FeatureInputBinding(std::move(lease))};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("input binding allocation failed"), {}};
    } catch (const std::exception& error) {
        return {foundation::unknown_exception(error), {}};
    } catch (...) {
        return {foundation::Result::failure(foundation::ResultCode::kUnknown,
                                          "input binding threw an exception"), {}};
    }
}

}  // namespace aeg::feature::detail
