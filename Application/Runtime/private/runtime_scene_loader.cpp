#include "runtime_scene_loader.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "Components/component_bundle.hpp"
#include "Components/component_render_extraction.hpp"
#include "scene_payload.hpp"
#include "serialization.hpp"

namespace aeg::application::runtime {
namespace {
scene::SceneAssetId scene_asset_id_from_runtime(foundation::AssetId id) noexcept {
    return scene::SceneAssetId::from_uuid(id.value);
}

foundation::Result with_scene(foundation::Result result, foundation::AssetId id, const char* phase) {
    if (result)
        return result;
    std::string message = "startup scene " + id.to_string() + " " + phase;
    if (!result.message.empty()) {
        message += ": ";
        message += result.message;
    }
    return foundation::Result::failure(result.code, std::move(message));
}

struct WireField final {
    uint64_t id{};
    std::size_t data_offset{};
    uint32_t size{};
};

foundation::Result parse_component_fields(const scene::SceneComponentRecord& component,
                                          std::vector<WireField>& fields) {
    const auto& bytes = component.payload;
    if (bytes.size() < 16)
        return foundation::invalid_argument("Scene component reflection payload is truncated");
    uint64_t type = 0;
    uint32_t version = 0, count = 0;
    std::memcpy(&type, bytes.data(), sizeof(type));
    std::memcpy(&version, bytes.data() + 8, sizeof(version));
    std::memcpy(&count, bytes.data() + 12, sizeof(count));
    if (type != component.schema_type.value() || version != component.schema_version)
        return foundation::invalid_argument("Scene component reflection payload metadata does not match schema");
    std::size_t offset = 16;
    fields.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        if (offset > bytes.size() || bytes.size() - offset < 12)
            return foundation::invalid_argument("Scene component field is truncated");
        WireField field{};
        std::memcpy(&field.id, bytes.data() + offset, sizeof(field.id));
        std::memcpy(&field.size, bytes.data() + offset + 8, sizeof(field.size));
        offset += 12;
        if (field.size > bytes.size() - offset)
            return foundation::invalid_argument("Scene component field payload is truncated");
        field.data_offset = offset;
        fields.push_back(field);
        offset += field.size;
    }
    if (offset != bytes.size())
        return foundation::invalid_argument("Scene component payload has trailing bytes");
    return foundation::Result::success();
}

foundation::Result apply_scene_references(scene::SceneAsset& asset, const asset::ScenePayload& payload,
                                          reflection::TypeRegistry& reflection_registry,
                                          RuntimeAssetResolver* resolver) {
    if (!payload.references.empty() && resolver == nullptr)
        return foundation::invalid_state("Scene asset references require a runtime asset resolver");
    std::vector<bool> consumed(payload.references.size(), false);
    const auto mesh_schema = reflection::TypeId("Game.MeshRenderer");
    (void)reflection_registry;
    for (const auto& component : asset.components()) {
        if (component.schema_type != mesh_schema)
            continue;
        std::vector<WireField> fields;
        if (const auto result = parse_component_fields(component, fields); !result)
            return result;
        auto mutable_payload = component.payload;
        for (std::size_t index = 0; index < payload.references.size(); ++index) {
            const auto& reference = payload.references[index];
            if (RuntimeSceneLoader::scene_entity_id(foundation::Uuid{reference.scene_entity_uuid}) != component.entity)
                continue;
            if (reference.component_type != "MeshRenderer" || reference.component_version != component.schema_version)
                return foundation::invalid_argument("Scene reference component identity does not match archive");
            const bool mesh_field = reference.field_id == reflection::FieldId("mesh_asset_id").value();
            const bool material_field = reference.field_id == reflection::FieldId("material_asset_id").value();
            if (!mesh_field && !material_field)
                return foundation::invalid_argument("Scene reference field is not supported by MeshRenderer");
            const auto field = std::find_if(fields.begin(), fields.end(),
                                            [&](const WireField& value) { return value.id == reference.field_id; });
            if (field == fields.end() || field->size != sizeof(uint64_t))
                return foundation::invalid_argument("Scene reference field is missing or has invalid width");
            uint64_t source_value = 0;
            std::memcpy(&source_value, mutable_payload.data() + field->data_offset, sizeof(source_value));
            if (source_value == 0)
                return foundation::invalid_argument("Scene reference points at an empty asset field");
            foundation::ResultValue<uint64_t> bound{foundation::Result::success(), 0};
            if (mesh_field) {
                if (reference.kind != foundation::AssetKind::kMesh)
                    return foundation::invalid_argument("MeshRenderer mesh reference has the wrong AssetKind");
                const auto result = resolver->bind_mesh(reference.asset);
                if (!result)
                    return result.result;
                bound.value = result.value.value();
            } else {
                if (reference.kind != foundation::AssetKind::kMaterial)
                    return foundation::invalid_argument("MeshRenderer material reference has the wrong AssetKind");
                const auto result = resolver->bind_material(reference.asset);
                if (!result)
                    return result.result;
                bound.value = result.value.value();
            }
            std::memcpy(mutable_payload.data() + field->data_offset, &bound.value, sizeof(bound.value));
            consumed[index] = true;
        }
        for (const WireField& field : fields) {
            if (field.id != reflection::FieldId("mesh_asset_id").value() &&
                field.id != reflection::FieldId("material_asset_id").value())
                continue;
            if (field.size != sizeof(uint64_t))
                return foundation::invalid_argument("MeshRenderer asset field width is invalid");
            uint64_t value = 0;
            std::memcpy(&value, mutable_payload.data() + field.data_offset, sizeof(value));
            if (value == 0)
                continue;
            std::size_t matches = 0;
            for (std::size_t index = 0; index < payload.references.size(); ++index) {
                const auto& reference = payload.references[index];
                if (RuntimeSceneLoader::scene_entity_id(foundation::Uuid{reference.scene_entity_uuid}) == component.entity &&
                    reference.component_type == "MeshRenderer" &&
                    reference.field_id == field.id)
                    ++matches;
            }
            if (matches != 1)
                return foundation::invalid_argument("MeshRenderer asset field has missing or duplicate reference");
        }
        if (const auto result =
                asset.replace_component_payload(component.entity, component.component_type, std::move(mutable_payload));
            !result)
            return result;
    }
    for (std::size_t index = 0; index < consumed.size(); ++index)
        if (!consumed[index])
            return foundation::invalid_argument("Scene asset reference does not match an archive component");
    return foundation::Result::success();
}
}  // namespace

scene::SceneEntityId RuntimeSceneLoader::scene_entity_id(foundation::Uuid uuid) noexcept {
    return scene::SceneEntityId::from_uuid(uuid);
}

foundation::ResultValue<RuntimeSceneLoadResult> RuntimeSceneLoader::load_startup_scene(
    const asset::AssetPackageReader& package, foundation::AssetId startup_scene,
    reflection::TypeRegistry& reflection_registry, RuntimeAssetResolver* resolver) {
    if (startup_scene == foundation::AssetId{})
        return {foundation::invalid_argument("startup scene UUID is invalid"), {}};

    const auto cooked = package.read_cooked(startup_scene);
    if (!cooked)
        return {with_scene(cooked.result, startup_scene, "could not be read"), {}};
    if (cooked.value.kind != foundation::AssetKind::kScene)
        return {with_scene(foundation::invalid_argument("asset kind is not Scene"), startup_scene, "has invalid kind"),
                {}};
    if (cooked.value.payload.empty())
        return {
            with_scene(foundation::invalid_argument("Scene payload is empty"), startup_scene, "has invalid payload"),
            {}};

    const auto payload = asset::decode_scene_payload(cooked.value.payload);
    if (!payload)
        return {with_scene(payload.result, startup_scene, "payload decode failed"), {}};
    if (payload.value.schema_version != asset::kScenePayloadSchemaVersion)
        return {with_scene(foundation::unsupported("Scene payload schema version is unsupported"), startup_scene,
                           "schema validation failed"),
                {}};
    if (payload.value.serialized_scene.empty())
        return {with_scene(foundation::invalid_argument("serialized Scene archive is empty"), startup_scene,
                           "archive decode failed"),
                {}};

    const std::span<const uint8_t> archive(reinterpret_cast<const uint8_t*>(payload.value.serialized_scene.data()),
                                           payload.value.serialized_scene.size());
    serialization::BinaryReader reader(archive);
    auto decoded = scene::SceneSerializer::deserialize(reader);
    if (!decoded)
        return {with_scene(decoded.result, startup_scene, "Scene archive decode failed"), {}};
    const auto expected_scene_id = scene_asset_id_from_runtime(startup_scene);
    if (decoded.value.id() != expected_scene_id) {
        return {with_scene(foundation::invalid_argument("Scene archive asset UUID " +
                                                        std::to_string(decoded.value.id().high()) + ":" +
                                                        std::to_string(decoded.value.id().low()) +
                                                        " does not match cooked UUID " + startup_scene.to_string()),
                           startup_scene, "identity validation failed"),
                {}};
    }

    if (const auto relocated = apply_scene_references(decoded.value, payload.value, reflection_registry, resolver);
        !relocated)
        return {with_scene(relocated, startup_scene, "asset reference validation failed"), {}};

    auto manager = std::make_shared<ecs::WorldManager>();
    const auto handle = manager->create_world(ecs::WorldDesc{"Runtime Startup Scene", true});
    if (!handle)
        return {with_scene(handle.result, startup_scene, "World creation failed"), {}};
    const auto world_result = manager->find(handle.value);
    if (!world_result)
        return {with_scene(world_result.result, startup_scene, "World lookup failed"), {}};
    auto world_state = world_result.value;
    if (const foundation::Result registered =
            engine::components::register_render_component_bundle(*world_state, reflection_registry);
        !registered) {
        return {with_scene(registered, startup_scene, "component registration failed"), {}};
    }

    auto scope = std::make_unique<world::WorldScope>(world_state);
    if (const foundation::Result initialized = scope->initialize(); !initialized)
        return {with_scene(initialized, startup_scene, "World Scope initialization failed"), {}};
    const auto instance = decoded.value.instantiate(*world_state, reflection_registry);
    if (!instance) {
        (void)scope->shutdown();
        return {with_scene(instance.result, startup_scene, "World instantiation failed"), {}};
    }
    const auto snapshot = scope->commit(0, 0);
    if (!snapshot) {
        (void)scope->shutdown();
        return {with_scene(snapshot.result, startup_scene, "initial snapshot commit failed"), {}};
    }

    RuntimeSceneLoadResult output{};
    output.asset = decoded.value;
    output.instance = instance.value;
    const auto committed_snapshot = scope->latest_snapshot();
    if (!committed_snapshot) {
        (void)scope->shutdown();
        return {with_scene(committed_snapshot.result, startup_scene, "initial snapshot lookup failed"), {}};
    }
    committed_snapshot.value->for_each<engine::components::Camera>(
        [&output](ecs::EntityId entity, const engine::components::Camera&) {
            if (!output.startup_camera.is_valid())
                output.startup_camera = entity;
        });
    output.world = std::move(world_state);
    output.scope = std::move(scope);
    output.world_manager = std::move(manager);
    return {foundation::Result::success(), std::move(output)};
}
}  // namespace aeg::application::runtime
