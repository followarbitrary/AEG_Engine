#include <filesystem>
#include <iostream>

#include "../Application/Tooling/public/cooked_asset.hpp"
#include "Components/component_ids.hpp"
#include "Components/component_reflection.hpp"
#include "Components/render_components.hpp"
#include "asset_package.hpp"
#include "cooked_asset.hpp"
#include "runtime_asset_resolver.hpp"
#include "runtime_asset_upload_host.hpp"
#include "runtime_scene_loader.hpp"
#include "scene_payload.hpp"

int main() {
    namespace fs = std::filesystem;
    using namespace aeg;
    const auto scene_id = foundation::AssetId::parse("00000000-0000-4000-8000-0000000000a1");
    if (!scene_id)
        return 1;

    scene::SceneAsset document(scene::SceneAssetId{0x4000, 0x80000000000000a1ull}, "RuntimeSmoke", 1);
    const scene::SceneEntityId entity_id{3, 4};
    if (!document.add_entity({entity_id, {}, "Camera", scene::SceneEntityFlags::kNone, {}}))
        return 2;
    reflection::TypeRegistry authored_reflection;
    if (!engine::components::register_render_component_reflection(authored_reflection))
        return 2;
    engine::components::Transform camera_transform{};
    camera_transform.translation = {0.0f, 1.0f, 3.0f};
    engine::components::Camera camera{};
    if (!document.add_reflected_component(entity_id, ecs::ComponentTypeId(engine::components::kTransformName),
                                          authored_reflection, reflection::TypeId(engine::components::kTransformName),
                                          &camera_transform) ||
        !document.add_reflected_component(entity_id, ecs::ComponentTypeId(engine::components::kCameraName),
                                          authored_reflection, reflection::TypeId(engine::components::kCameraName),
                                          &camera))
        return 2;
    serialization::BinaryWriter archive;
    if (!scene::SceneSerializer::serialize(document, archive))
        return 3;
    asset::ScenePayload payload{};
    payload.serialized_scene.assign(reinterpret_cast<const std::byte*>(archive.data().data()),
                                    reinterpret_cast<const std::byte*>(archive.data().data() + archive.data().size()));
    const auto encoded_payload = asset::encode_scene_payload(payload);
    if (!encoded_payload)
        return 4;

    asset::CookedAsset cooked{};
    cooked.id = scene_id.value;
    cooked.kind = foundation::AssetKind::kScene;
    cooked.schema_version = 1;
    cooked.payload = encoded_payload.value;
    const auto envelope = tooling::CookedAssetCodec{}.encode(cooked);
    if (!envelope)
        return 5;
    const auto root = fs::temp_directory_path() / "aeg_runtime_scene_boot_smoke";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    if (!tooling::AssetPackageWriter{}.write(root / "scene.aegbin",
                                             {{cooked.id, cooked.kind, cooked.schema_version, envelope.value}}))
        return 6;
    const auto package = asset::AssetPackageReader::open(root / "scene.aegbin");
    if (!package)
        return 7;

    reflection::TypeRegistry reflection;
    const auto loaded = application::runtime::RuntimeSceneLoader::load_startup_scene(package.value, scene_id.value,
                                                                                     reflection);
    if (!loaded || !loaded.value.world || !loaded.value.scope || loaded.value.instance.entity_count() != 1 ||
        loaded.value.world->entity_count() != 1 || !loaded.value.startup_camera.is_valid())
        return 8;
    const auto runtime_entity = loaded.value.instance.runtime_entity(entity_id);
    if (!runtime_entity || !loaded.value.world->is_alive(runtime_entity.value) ||
        loaded.value.startup_camera != runtime_entity.value)
        return 9;

    // A cooked MeshRenderer carries UUID references; runtime loading must bind
    // those UUIDs to session renderer identities before instantiating the World.
    const auto mesh_asset = foundation::AssetId::parse("10000000-0000-4000-8000-000000000001");
    const auto material_asset = foundation::AssetId::parse("10000000-0000-4000-8000-000000000002");
    if (!mesh_asset || !material_asset)
        return 16;
    reflection::TypeRegistry bound_reflection;
    if (!engine::components::register_render_component_reflection(bound_reflection))
        return 17;
    scene::SceneAsset render_document(scene::SceneAssetId{0x4000, 0x80000000000000a1ull}, "RenderRuntime", 1);
    const scene::SceneEntityId render_entity{5, 6};
    if (!render_document.add_entity({render_entity, {}, "Mesh", scene::SceneEntityFlags::kNone, {}}))
        return 18;
    engine::components::MeshRenderer authored_mesh{};
    authored_mesh.mesh_asset_id = 0xabc;
    authored_mesh.material_asset_id = 0xdef;
    if (!render_document.add_reflected_component(
            render_entity, ecs::ComponentTypeId(engine::components::kMeshRendererName), bound_reflection,
            reflection::TypeId(engine::components::kMeshRendererName), &authored_mesh))
        return 19;
    serialization::BinaryWriter render_archive;
    if (!scene::SceneSerializer::serialize(render_document, render_archive))
        return 20;
    asset::ScenePayload render_payload{};
    render_payload.serialized_scene.assign(
        reinterpret_cast<const std::byte*>(render_archive.data().data()),
        reinterpret_cast<const std::byte*>(render_archive.data().data() + render_archive.data().size()));
    asset::SceneAssetReference mesh_ref{};
    for (int i = 0; i != 8; ++i)
        mesh_ref.scene_entity_uuid[static_cast<std::size_t>(i)] =
            static_cast<uint8_t>(render_entity.high() >> (56 - i * 8));
    for (int i = 0; i != 8; ++i)
        mesh_ref.scene_entity_uuid[static_cast<std::size_t>(8 + i)] =
            static_cast<uint8_t>(render_entity.low() >> (56 - i * 8));
    mesh_ref.component_type = "MeshRenderer";
    mesh_ref.component_version = 1;
    mesh_ref.field_id = reflection::FieldId("mesh_asset_id").value();
    mesh_ref.asset = mesh_asset.value;
    mesh_ref.kind = foundation::AssetKind::kMesh;
    auto material_ref = mesh_ref;
    material_ref.field_id = reflection::FieldId("material_asset_id").value();
    material_ref.asset = material_asset.value;
    material_ref.kind = foundation::AssetKind::kMaterial;
    render_payload.references = {mesh_ref, material_ref};
    const auto render_encoded = asset::encode_scene_payload(render_payload);
    if (!render_encoded)
        return 21;
    asset::CookedAsset render_cooked{
        scene_id.value, foundation::AssetKind::kScene, 1, {mesh_asset.value, material_asset.value}, render_encoded.value};
    auto encode_entry = [](foundation::AssetId id, foundation::AssetKind kind) {
        return asset::encode_cooked_asset({id, kind, 1, {}, {std::byte{1}}});
    };
    const auto mesh_entry = encode_entry(mesh_asset.value, foundation::AssetKind::kMesh);
    const auto material_entry = encode_entry(material_asset.value, foundation::AssetKind::kMaterial);
    const auto render_envelope = tooling::CookedAssetCodec{}.encode(render_cooked);
    if (!mesh_entry || !material_entry || !render_envelope ||
        !tooling::AssetPackageWriter{}.write(
            root / "render_scene.aegbin",
            {{render_cooked.id, render_cooked.kind, 1, render_envelope.value},
             {mesh_asset.value, foundation::AssetKind::kMesh, 1, mesh_entry.value},
             {material_asset.value, foundation::AssetKind::kMaterial, 1, material_entry.value}}))
        return 22;
    const auto render_package = asset::AssetPackageReader::open(root / "render_scene.aegbin");
    if (!render_package)
        return 23;
    application::runtime::RuntimeAssetResolver resolver{render_package.value};
    const auto bound_scene = application::runtime::RuntimeSceneLoader::load_startup_scene(
        render_package.value, scene_id.value, bound_reflection, &resolver);
    if (!bound_scene)
        return 24;
    const auto bound_entity = bound_scene.value.instance.runtime_entity(render_entity);
    if (!bound_entity)
        return 25;
    const auto runtime_mesh = bound_scene.value.world->get<engine::components::MeshRenderer>(bound_entity.value);
    if (!runtime_mesh || runtime_mesh->mesh_asset_id == authored_mesh.mesh_asset_id ||
        runtime_mesh->material_asset_id == authored_mesh.material_asset_id || runtime_mesh->mesh_asset_id == 0 ||
        runtime_mesh->material_asset_id == 0)
        return 26;

    // A wrapped archive with a nonzero MeshRenderer field and no relocation
    // records must fail before instantiation instead of silently using editor IDs.
    render_payload.references.clear();
    const auto empty_reference_payload = asset::encode_scene_payload(render_payload);
    if (!empty_reference_payload)
        return 27;
    render_cooked.payload = empty_reference_payload.value;
    const auto empty_reference_envelope = tooling::CookedAssetCodec{}.encode(render_cooked);
    if (!empty_reference_envelope ||
        !tooling::AssetPackageWriter{}.write(
            root / "render_scene_empty_refs.aegbin",
            {{render_cooked.id, render_cooked.kind, 1, empty_reference_envelope.value},
             {mesh_asset.value, foundation::AssetKind::kMesh, 1, mesh_entry.value},
             {material_asset.value, foundation::AssetKind::kMaterial, 1, material_entry.value}}))
        return 28;
    const auto empty_reference_package = asset::AssetPackageReader::open(root / "render_scene_empty_refs.aegbin");
    if (!empty_reference_package)
        return 29;
    application::runtime::RuntimeAssetResolver empty_reference_resolver{empty_reference_package.value};
    reflection::TypeRegistry empty_reference_reflection;
    if (application::runtime::RuntimeSceneLoader::load_startup_scene(
            empty_reference_package.value, scene_id.value, empty_reference_reflection,
            &empty_reference_resolver))
        return 30;

    // The package UUID is authoritative; an archive carrying another scene
    // identity must be rejected before a World becomes visible.
    scene::SceneAsset wrong_document(scene::SceneAssetId{9, 10}, "WrongIdentity", 1);
    serialization::BinaryWriter wrong_archive;
    if (!scene::SceneSerializer::serialize(wrong_document, wrong_archive))
        return 10;
    asset::ScenePayload wrong_payload{};
    wrong_payload.serialized_scene.assign(
        reinterpret_cast<const std::byte*>(wrong_archive.data().data()),
        reinterpret_cast<const std::byte*>(wrong_archive.data().data() + wrong_archive.data().size()));
    const auto wrong_encoded = asset::encode_scene_payload(wrong_payload);
    if (!wrong_encoded)
        return 11;
    asset::CookedAsset wrong_cooked = cooked;
    wrong_cooked.payload = wrong_encoded.value;
    const auto wrong_envelope = tooling::CookedAssetCodec{}.encode(wrong_cooked);
    if (!wrong_envelope || !tooling::AssetPackageWriter{}.write(root / "wrong_identity.aegbin",
                                                                {{wrong_cooked.id, wrong_cooked.kind,
                                                                  wrong_cooked.schema_version, wrong_envelope.value}}))
        return 12;
    const auto wrong_package = asset::AssetPackageReader::open(root / "wrong_identity.aegbin");
    if (!wrong_package || application::runtime::RuntimeSceneLoader::load_startup_scene(
                              wrong_package.value, scene_id.value, reflection))
        return 13;

    asset::CookedAsset malformed = cooked;
    malformed.payload.clear();
    const auto malformed_envelope = tooling::CookedAssetCodec{}.encode(malformed);
    if (!malformed_envelope || !tooling::AssetPackageWriter{}.write(
                                   root / "malformed.aegbin", {{malformed.id, malformed.kind, malformed.schema_version,
                                                                malformed_envelope.value}}))
        return 14;
    const auto malformed_package = asset::AssetPackageReader::open(root / "malformed.aegbin");
    if (!malformed_package || application::runtime::RuntimeSceneLoader::load_startup_scene(
                                  malformed_package.value, scene_id.value, reflection))
        return 15;
    fs::remove_all(root, ec);
    std::cout << "runtime Scene package creates an isolated World and rejects malformed payloads\n";
    return 0;
}
