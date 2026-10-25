#include "feature_world_commands.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include "Components/component_ids.hpp"
#include "Components/component_reflection.hpp"
#include "../../Feature/private/feature_input.hpp"
#include "../../Feature/private/feature_outbox.hpp"
#include "concurrency.hpp"
#include "feature_world_transform.hpp"
#include "world_generation_access.hpp"
#include "serialization.hpp"

namespace aeg::engine::detail {
namespace {
using Transform = components::Transform;
using OutboxAccess = feature::detail::FeatureOutboxAccess;

foundation::ResultValue<std::shared_ptr<const reflection::TypeDescriptor>> transform_schema(
    const reflection::TypeRegistry& registry) {
    const auto expected = components::transform_reflection_descriptor();
    if (!expected) return {expected.result, {}};
    const auto actual = registry.find_type(expected.value.id);
    if (!actual) return {actual.result, {}};
    const auto& value = *actual.value;
    const auto& required = expected.value;
    if (value.id != required.id || value.name != required.name || value.version != required.version ||
        value.kind != required.kind || value.size != required.size || value.alignment != required.alignment ||
        value.fields.size() != required.fields.size())
        return {foundation::invalid_state("World Transform reflection metadata does not match its actual Runtime schema"), {}};
    for (std::size_t index = 0; index < required.fields.size(); ++index) {
        const auto& field = value.fields[index];
        const auto& expected_field = required.fields[index];
        if (field.id != expected_field.id || field.name != expected_field.name || field.type != expected_field.type ||
            field.flags != expected_field.flags || field.value_kind != expected_field.value_kind ||
            !field.accessor.is_valid() || field.accessor.value_kind != expected_field.accessor.value_kind ||
            field.accessor.value_size != expected_field.accessor.value_size ||
            field.accessor.value_alignment != expected_field.accessor.value_alignment)
            return {foundation::invalid_state("World Transform reflected fields do not match the Runtime schema"), {}};
    }
    return actual;
}

bool finite_transform(const Transform& value) noexcept {
    return math::is_finite(value.translation) && math::is_finite(value.rotation) && math::is_finite(value.scale);
}

bool same_transform(const Transform& left, const Transform& right) noexcept {
    return left.translation == right.translation && left.rotation == right.rotation && left.scale == right.scale;
}

foundation::ResultValue<Transform*> transform_target(ecs::World& world, ecs::EntityId entity,
                                                    const reflection::TypeRegistry& registry) {
    if (!world.is_owner_thread())
        return {foundation::invalid_state("World Transform access requires the ECS owner thread"), nullptr};
    const auto schema = transform_schema(registry);
    if (!schema) return {schema.result, nullptr};
    const ecs::ComponentTypeId type(components::kTransformName);
    if (world.component_type_id<Transform>() != type)
        return {foundation::invalid_state("World Transform C++ storage has another component identity"), nullptr};
    const auto component = world.find_component_descriptor(type);
    if (!component) return {component.result, nullptr};
    const auto& metadata = component.value;
    if (metadata.id != type || metadata.name != components::kTransformName ||
        metadata.size != sizeof(Transform) || metadata.alignment != alignof(Transform) || !metadata.snapshotable ||
        metadata.schema_type != schema.value->id || metadata.schema_version != schema.value->version)
        return {foundation::invalid_state("World Transform component storage does not satisfy its reflected schema"), nullptr};
    auto* target = world.get<Transform>(entity);
    if (!target)
        return {foundation::not_found("World Transform target entity or component is missing or retired"), nullptr};
    return {foundation::Result::success(), target};
}

foundation::Result write_blob(serialization::BinaryWriter& writer, std::span<const uint8_t> blob) {
    if (blob.size() > (std::numeric_limits<uint32_t>::max)())
        return foundation::invalid_argument("World Transform reflected payload is too large");
    if (const auto result = writer.write_u32(static_cast<uint32_t>(blob.size())); !result) return result;
    return writer.write_bytes(blob);
}

foundation::ResultValue<std::span<const uint8_t>> read_blob(serialization::BinaryReader& reader) {
    const auto size = reader.read_u32();
    if (!size) return {size.result, {}};
    if (size.value > feature::kMaxOutboxIntentBytes)
        return {foundation::invalid_argument("World Transform reflected payload exceeds the command limit"), {}};
    return reader.read_bytes(size.value);
}

struct PreparedTransform final {
    Transform* destination{nullptr};
    Transform replacement{};
};
static_assert(std::is_nothrow_copy_assignable_v<Transform>);

void apply_transform(void* opaque, const feature::OutboxIntent&) noexcept {
    const auto& prepared = *static_cast<const PreparedTransform*>(opaque);
    *prepared.destination = prepared.replacement;
}
}  // namespace

foundation::ResultValue<feature::OutboxIntent> encode_world_transform_command(
    const reflection::TypeRegistry& registry, const WorldTransformCommand& command) {
    try {
        const auto schema = transform_schema(registry);
        if (!schema) return {schema.result, {}};
        if (!finite_transform(command.expected) || !finite_transform(command.replacement))
            return {foundation::invalid_argument("World Transform command requires finite semantic components"), {}};
        serialization::BinaryWriter expected;
        if (const auto result = serialization::serialize_object(registry, schema.value->id, &command.expected, expected); !result)
            return {result, {}};
        serialization::BinaryWriter replacement;
        if (const auto result = serialization::serialize_object(registry, schema.value->id, &command.replacement, replacement); !result)
            return {result, {}};
        serialization::BinaryWriter writer;
        if (const auto result = writer.write_u64(command.expected_world_revision); !result) return {result, {}};
        if (const auto result = write_blob(writer, expected.data()); !result) return {result, {}};
        if (const auto result = write_blob(writer, replacement.data()); !result) return {result, {}};
        if (writer.size() > feature::kMaxOutboxIntentBytes)
            return {foundation::invalid_argument("World Transform command exceeds the outbox intent limit"), {}};
        return {foundation::Result::success(),
                {kWorldTransformCommandEffect, kWorldTransformCommandVersion, {writer.data().begin(), writer.data().end()}}};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("World Transform command encoding allocation failed"), {}};
    } catch (const std::exception& error) {
        return {foundation::unknown_exception(error), {}};
    } catch (...) {
        return {foundation::invalid_state("World Transform command encoding threw an exception"), {}};
    }
}

foundation::ResultValue<WorldTransformCommand> decode_world_transform_command(
    const reflection::TypeRegistry& registry, const feature::OutboxIntent& intent) {
    try {
        if (intent.effect != kWorldTransformCommandEffect || intent.version != kWorldTransformCommandVersion ||
            intent.payload.size() > feature::kMaxOutboxIntentBytes)
            return {foundation::invalid_argument("World Transform command protocol or payload size is invalid"), {}};
        const auto schema = transform_schema(registry);
        if (!schema) return {schema.result, {}};
        serialization::BinaryReader reader(intent.payload);
        const auto revision = reader.read_u64();
        if (!revision) return {revision.result, {}};
        const auto expected = read_blob(reader);
        if (!expected) return {expected.result, {}};
        const auto replacement = read_blob(reader);
        if (!replacement) return {replacement.result, {}};
        if (reader.remaining() != 0)
            return {foundation::invalid_argument("World Transform command contains trailing bytes"), {}};
        WorldTransformCommand command;
        command.expected_world_revision = revision.value;
        serialization::BinaryReader expected_reader(expected.value);
        if (const auto result = serialization::deserialize_object(registry, schema.value->id, expected_reader, &command.expected); !result)
            return {result, {}};
        serialization::BinaryReader replacement_reader(replacement.value);
        if (const auto result = serialization::deserialize_object(registry, schema.value->id, replacement_reader, &command.replacement); !result)
            return {result, {}};
        if (expected_reader.remaining() != 0 || replacement_reader.remaining() != 0)
            return {foundation::invalid_argument("World Transform reflected payload contains trailing bytes"), {}};
        if (!finite_transform(command.expected) || !finite_transform(command.replacement))
            return {foundation::invalid_argument("World Transform command contains non-finite semantic components"), {}};
        const auto canonical = encode_world_transform_command(registry, command);
        if (!canonical) return {canonical.result, {}};
        if (canonical.value.payload != intent.payload)
            return {foundation::invalid_argument("World Transform command is not a complete canonical reflected record"), {}};
        return {foundation::Result::success(), std::move(command)};
    } catch (const std::bad_alloc&) {
        return {foundation::out_of_memory("World Transform command decoding allocation failed"), {}};
    } catch (const std::exception& error) {
        return {foundation::unknown_exception(error), {}};
    } catch (...) {
        return {foundation::invalid_state("World Transform command decoding threw an exception"), {}};
    }
}

struct FeatureWorldCommandConsumer::Impl final {
    struct Route final {
        const world::WorldScope* scope{nullptr};
        world::detail::WorldGenerationBinding generation{};
        feature::detail::FeatureOutboxBinding source{};
        ecs::EntityId entity{};
        const reflection::TypeRegistry* registry{nullptr};
        feature::PortId observation_port{};
        feature::detail::FeatureInputBinding observation{};
        double frame_delta_seconds{0};
        bool cohort_active{false};

        foundation::ResultValue<feature::detail::FeatureInputProjection> project(
            const feature::detail::FeatureInputContext& context) {
            try {
                const auto* published = source.source();
                if (!published || context.owner != published->instance_id || context.generation != source.generation() ||
                    context.clock != published->clock)
                    return {foundation::invalid_state("World observation belongs to a retired or foreign Feature owner"), {}};
                // Core supplies canonical Pending protocols. Neither the latch nor this projector
                // re-enters Runtime; the latch only serializes admissions across a World boundary.
                if (cohort_active || std::any_of(context.pending_effects.begin(), context.pending_effects.end(),
                    [&](const feature::SideEffectContract& pending) {
                        return pending.id == source.effect() && pending.version == source.version();
                    }))
                    return {foundation::Result::success(), {false, {}}};
                const auto owner = world::detail::WorldGenerationAccess::lock(*scope, generation);
                if (!owner) return {owner.result, {}};
                const auto target = transform_target(*owner.value, entity, *registry);
                if (!target) return {target.result, {}};
                const auto& before = *target.value;
                feature::WorldTransformInput input{};
                input.observation.world_revision = owner.value->revision();
                input.observation.before.translation = {before.translation.x, before.translation.y, before.translation.z};
                input.observation.before.rotation = {before.rotation.x, before.rotation.y, before.rotation.z, before.rotation.w};
                input.observation.before.scale = {before.scale.x, before.scale.y, before.scale.z};
                input.observation.elapsed_seconds = context.elapsed_seconds;
                input.observation.frame_delta_seconds = frame_delta_seconds;
                input.channel_input.assign(context.routed_input.begin(), context.routed_input.end());
                auto encoded = feature::encode_world_transform_input(input);
                if (!encoded) return {encoded.result, {}};
                cohort_active = true;
                return {foundation::Result::success(), {true, std::move(encoded.value)}};
            } catch (const std::bad_alloc&) {
                return {foundation::out_of_memory("World observation projection allocation failed"), {}};
            } catch (const std::exception& error) {
                return {foundation::unknown_exception(error), {}};
            } catch (...) {
                return {foundation::invalid_state("World observation projection threw an exception"), {}};
            }
        }
    };
    feature::FeatureRuntime& runtime;
    const reflection::TypeRegistry& registry;
    const concurrency::ThreadId owner_thread{concurrency::current_thread_id()};
    double frame_delta_seconds{0};
    bool observation_waiting{false};
    std::vector<std::shared_ptr<Route>> routes{};

    Impl(feature::FeatureRuntime& source, const reflection::TypeRegistry& schemas) : runtime(source), registry(schemas) {}
};

FeatureWorldCommandConsumer::FeatureWorldCommandConsumer(feature::FeatureRuntime& runtime,
                                                       const reflection::TypeRegistry& registry)
    : impl_(std::make_unique<Impl>(runtime, registry)) {}
FeatureWorldCommandConsumer::~FeatureWorldCommandConsumer() = default;

foundation::Result FeatureWorldCommandConsumer::bind(feature::FeatureInstanceId instance, world::WorldScope& scope,
                                                    ecs::EntityId entity, feature::SideEffectId effect, uint32_t version,
                                                    feature::PortId observation_port) {
    try {
        if (concurrency::current_thread_id() != impl_->owner_thread)
            return foundation::invalid_state("World command binding requires its Consumer owner thread");
        if (effect != kWorldTransformCommandEffect || version != kWorldTransformCommandVersion)
            return foundation::invalid_argument("World command route requires the supported Transform protocol");
        const auto source = OutboxAccess::bind(impl_->runtime, instance, effect, version);
        if (!source) return source.result;
        for (const auto& route : impl_->routes)
            if (route->source.source() == source.value.source() && route->source.effect() == effect)
                return foundation::invalid_state("World command source and protocol already have an explicit route");
        const auto generation = world::detail::WorldGenerationAccess::capture(scope);
        if (!generation) return generation.result;
        const auto owner = world::detail::WorldGenerationAccess::lock(scope, generation.value);
        if (!owner) return owner.result;
        const auto target = transform_target(*owner.value, entity, impl_->registry);
        if (!target) return target.result;
        auto route = std::make_shared<Impl::Route>();
        route->scope = &scope; route->generation = generation.value; route->source = source.value;
        route->entity = entity; route->registry = &impl_->registry; route->observation_port = observation_port;
        route->frame_delta_seconds = impl_->frame_delta_seconds;
        // Reserve before installing the Core lease so failure leaves no installed projector.
        impl_->routes.reserve(impl_->routes.size() + 1);
        if (observation_port.is_valid()) {
            std::weak_ptr<Impl::Route> weak_route = route;
            auto binding = feature::detail::FeatureInputAccess::bind(impl_->runtime, instance, observation_port,
                feature::kWorldTransformInputSchema, feature::kWorldTransformInputVersion,
                [weak_route](const feature::detail::FeatureInputContext& context)
                    -> foundation::ResultValue<feature::detail::FeatureInputProjection> {
                    const auto route = weak_route.lock();
                    if (!route) return {foundation::invalid_state("World observation route has retired"), {}};
                    return route->project(context);
                });
            if (!binding) return binding.result;
            route->observation = std::move(binding.value);
        }
        impl_->routes.push_back(std::move(route));
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("World command route allocation failed");
    } catch (const std::exception& error) {
        return foundation::unknown_exception(error);
    } catch (...) {
        return foundation::invalid_state("World command binding threw an exception");
    }
}

foundation::Result FeatureWorldCommandConsumer::set_frame_delta(double seconds) {
    if (concurrency::current_thread_id() != impl_->owner_thread)
        return foundation::invalid_state("World observation frame duration requires its Consumer owner thread");
    if (!std::isfinite(seconds) || seconds < 0)
        return foundation::invalid_argument("World observation frame duration must be finite and nonnegative");
    impl_->frame_delta_seconds = seconds;
    for (const auto& route : impl_->routes) route->frame_delta_seconds = seconds;
    impl_->observation_waiting = false;
    return foundation::Result::success();
}

bool FeatureWorldCommandConsumer::observation_waiting() const noexcept {
    return impl_->observation_waiting;
}

foundation::Result FeatureWorldCommandConsumer::consume(world::WorldScope& scope) {
    try {
        if (concurrency::current_thread_id() != impl_->owner_thread)
            return foundation::invalid_state("World command consumption requires its Consumer owner thread");
        impl_->observation_waiting = false;
        struct PreparedRoute final {
            std::shared_ptr<Impl::Route> route{};
            std::vector<feature::detail::FeatureOutboxHandle> entries{};
        };
        std::vector<PreparedRoute> prepared_routes;
        prepared_routes.reserve(impl_->routes.size());
        // Inspect every observed source before applying any route. A deferred admission,
        // clock backlog or empty cohort cannot itself pin a World snapshot publication.
        for (const auto& route : impl_->routes) {
            if (route->scope != &scope) continue;
            const auto owner = world::detail::WorldGenerationAccess::lock(scope, route->generation);
            if (!owner) return owner.result;
            auto entries = OutboxAccess::capture(impl_->runtime, route->source);
            if (!entries) return entries.result;
            if (route->observation_port.is_valid()) {
                const auto* source = route->source.source();
                const auto invocations = impl_->runtime.inspect_invocations(source->instance_id);
                if (!invocations) return invocations.result;
                if (invocations.value.retry || std::any_of(invocations.value.open_attempts.begin(),
                    invocations.value.open_attempts.end(), [](const auto& attempt) { return attempt.committable; })) {
                    impl_->observation_waiting = true;
                    return foundation::not_ready("World observation still has a committable invocation or clock-owned retry");
                }
            }
            prepared_routes.push_back({route, std::move(entries.value)});
        }
        for (const auto& prepared_route : prepared_routes) {
            const auto& route = *prepared_route.route;
            for (const auto& entry : prepared_route.entries) {
                const auto command = decode_world_transform_command(impl_->registry, *entry.intent());
                if (!command) return command.result;
                const auto current_owner = world::detail::WorldGenerationAccess::lock(scope, route.generation);
                if (!current_owner) return current_owner.result;
                const auto target = transform_target(*current_owner.value, route.entity, impl_->registry);
                if (!target) return target.result;
                if (current_owner.value->revision() != command.value.expected_world_revision ||
                    !finite_transform(*target.value) || !same_transform(*target.value, command.value.expected))
                    return foundation::invalid_state("World Transform command expected revision or value is stale");
                PreparedTransform prepared{target.value, command.value.replacement};
                if (const auto result = OutboxAccess::consume(impl_->runtime, route.source, entry, apply_transform, &prepared); !result)
                    return result;
            }
        }
        // Clear the whole scope only after every route succeeds. Partial Applied receipts
        // survive failures without allowing another observation into the failed cohort.
        for (const auto& prepared_route : prepared_routes) prepared_route.route->cohort_active = false;
        return foundation::Result::success();
    } catch (const std::bad_alloc&) {
        return foundation::out_of_memory("World command preflight allocation failed");
    } catch (const std::exception& error) {
        return foundation::unknown_exception(error);
    } catch (...) {
        return foundation::invalid_state("World command consumption threw an exception");
    }
}

}  // namespace aeg::engine::detail
