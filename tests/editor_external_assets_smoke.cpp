/**
 * @file editor_external_assets_smoke.cpp
 * @brief Verifies external mesh/material/texture import through viewport pixels.
 */
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "Components/component_ids.hpp"
#include "Components/render_components.hpp"
#include "editor_scene_runtime.hpp"
#include "rhi.hpp"

namespace {
template <typename Value>
bool require(const Value& condition, const char* message) {
    if (!static_cast<bool>(condition)) {
        std::cerr << message << '\n';
    }
    return static_cast<bool>(condition);
}

template <typename Value>
std::span<const std::byte> bytes_of(const Value& value) {
    return std::as_bytes(std::span{&value, 1});
}

bool write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(file);
}

bool write_tga(const std::filesystem::path& path, std::array<uint8_t, 4> rgba) {
    const std::array<uint8_t, 18> header{
        0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 32, 0x20,
    };
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    for (uint32_t index = 0; index < 4; ++index) {
        const std::array<uint8_t, 4> bgra{rgba[2], rgba[1], rgba[0], rgba[3]};
        file.write(reinterpret_cast<const char*>(bgra.data()), static_cast<std::streamsize>(bgra.size()));
    }
    return static_cast<bool>(file);
}

bool write_bmp(const std::filesystem::path& path, std::array<uint8_t, 4> rgba) {
    constexpr uint32_t width = 2;
    constexpr uint32_t height = 2;
    constexpr uint32_t row_size = 8;
    constexpr uint32_t image_size = row_size * height;
    constexpr uint32_t pixel_offset = 54;
    std::array<uint8_t, pixel_offset> header{};
    const auto put_u16 = [&header](std::size_t offset, uint16_t value) {
        header[offset + 0] = static_cast<uint8_t>(value & 0xffu);
        header[offset + 1] = static_cast<uint8_t>((value >> 8u) & 0xffu);
    };
    const auto put_u32 = [&header](std::size_t offset, uint32_t value) {
        header[offset + 0] = static_cast<uint8_t>(value & 0xffu);
        header[offset + 1] = static_cast<uint8_t>((value >> 8u) & 0xffu);
        header[offset + 2] = static_cast<uint8_t>((value >> 16u) & 0xffu);
        header[offset + 3] = static_cast<uint8_t>((value >> 24u) & 0xffu);
    };
    header[0] = 'B';
    header[1] = 'M';
    put_u32(2, pixel_offset + image_size);
    put_u32(10, pixel_offset);
    put_u32(14, 40);
    put_u32(18, width);
    put_u32(22, height);
    put_u16(26, 1);
    put_u16(28, 24);
    put_u32(34, image_size);
    put_u32(38, 2835);
    put_u32(42, 2835);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const std::array<uint8_t, 3> bgr{rgba[2], rgba[1], rgba[0]};
            file.write(reinterpret_cast<const char*>(bgr.data()), static_cast<std::streamsize>(bgr.size()));
        }
        const std::array<uint8_t, 2> padding{};
        file.write(reinterpret_cast<const char*>(padding.data()), static_cast<std::streamsize>(padding.size()));
    }
    return static_cast<bool>(file);
}

struct PixelStats final {
    uint64_t red{0};
    uint64_t green{0};
    uint64_t blue{0};
    uint64_t colored{0};
};

PixelStats stats(std::span<const std::byte> pixels, uint32_t width, uint32_t height, uint32_t x_begin, uint32_t x_end) {
    PixelStats result{};
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = x_begin; x < x_end; ++x) {
            const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4u;
            const uint64_t red = std::to_integer<uint8_t>(pixels[offset + 0]);
            const uint64_t green = std::to_integer<uint8_t>(pixels[offset + 1]);
            const uint64_t blue = std::to_integer<uint8_t>(pixels[offset + 2]);
            result.red += red;
            result.green += green;
            result.blue += blue;
            result.colored += (red + green + blue > 24u) ? 1u : 0u;
        }
    }
    return result;
}

const aeg::application::EditorResourceDescriptor* find_resource(
    const std::vector<aeg::application::EditorResourceDescriptor>& resources, std::string_view path,
    aeg::application::EditorResourceKind kind) {
    for (const auto& resource : resources) {
        if (resource.kind == kind && resource.path == path) {
            return &resource;
        }
    }
    return nullptr;
}
}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "aeg_editor_external_assets_smoke";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "Assets", error);
    if (!require(!error, "external asset smoke project directory creation failed")) {
        return 1;
    }

    const std::string triangle_obj =
        "v -1 -1 0\n"
        "v 1 -1 0\n"
        "v 0 1 0\n"
        "vt 0 0\n"
        "vt 1 0\n"
        "vt 0.5 1\n"
        "vn 0 0 1\n"
        "f 1/1/1 2/2/1 3/3/1\n";
    const std::string quad_obj =
        "v -1 -1 0\n"
        "v 1 -1 0\n"
        "v 1 1 0\n"
        "v -1 1 0\n"
        "vt 0 0\n"
        "vt 1 0\n"
        "vt 1 1\n"
        "vt 0 1\n"
        "vn 0 0 1\n"
        "f 1/1/1 2/2/1 3/3/1\n"
        "f 1/1/1 3/3/1 4/4/1\n";
    if (!require(write_text(root / "Assets" / "triangle.obj", triangle_obj), "triangle OBJ write failed") ||
        !require(write_text(root / "Assets" / "quad.obj", quad_obj), "quad OBJ write failed") ||
        !require(write_tga(root / "Assets" / "red.tga", {240, 24, 24, 255}), "red TGA write failed") ||
        !require(write_bmp(root / "Assets" / "blue.bmp", {24, 48, 240, 255}), "blue BMP write failed") ||
        !require(
            write_text(root / "Assets" / "red.aegmat", "texture = \"Assets/red.tga\"\nTint = [1.0, 1.0, 1.0, 1.0]\n"),
            "red material write failed") ||
        !require(write_text(root / "Assets" / "blue.aegmat",
                            "Texture = \"Assets/blue.bmp\"\nTint = [0.75, 1.0, 0.9, 1.0]\n"),
                 "blue material write failed")) {
        std::filesystem::remove_all(root, error);
        return 2;
    }

    const aeg::rhi::DeviceCreateDesc device_desc{
        aeg::rhi::BackendType::kVulkan,
        "AEG Editor External Assets Smoke",
        std::numeric_limits<uint32_t>::max(),
        true,
        true,
    };
    auto device_result = aeg::rhi::create_device(device_desc);
    if (!require(device_result, "external asset smoke device creation failed")) {
        std::filesystem::remove_all(root, error);
        return 3;
    }
    std::unique_ptr<aeg::rhi::Device> device = std::move(device_result.value);
    aeg::application::EditorSceneRuntime runtime;
    if (!require(runtime.initialize(*device), "external asset smoke runtime initialization failed") ||
        !require(runtime.new_project(root, "External Assets Smoke"), "external asset smoke project creation failed") ||
        !require(runtime.refresh_assets(), "external asset smoke asset refresh failed") ||
        !require(runtime.request_viewport_extent(128, 96), "external asset smoke viewport request failed")) {
        std::filesystem::remove_all(root, error);
        return 4;
    }

    const auto resources = runtime.resource_descriptors();
    const auto* triangle = find_resource(resources, "Assets/triangle.obj", aeg::application::EditorResourceKind::kMesh);
    const auto* quad = find_resource(resources, "Assets/quad.obj", aeg::application::EditorResourceKind::kMesh);
    const auto* red = find_resource(resources, "Assets/red.aegmat", aeg::application::EditorResourceKind::kMaterial);
    const auto* blue = find_resource(resources, "Assets/blue.aegmat", aeg::application::EditorResourceKind::kMaterial);
    const auto* blue_texture =
        find_resource(resources, "Assets/blue.bmp", aeg::application::EditorResourceKind::kTexture);
    if (!require(triangle != nullptr && triangle->valid, "triangle external mesh was not indexed") ||
        !require(quad != nullptr && quad->valid, "quad external mesh was not indexed") ||
        !require(red != nullptr && red->valid, "red external material was not indexed") ||
        !require(blue != nullptr && blue->valid, "blue external material was not indexed") ||
        !require(blue_texture != nullptr && blue_texture->valid, "blue external texture was not indexed")) {
        std::filesystem::remove_all(root, error);
        return 5;
    }
    const uint64_t blue_material_id = blue->id;
    const uint64_t blue_texture_id = blue_texture->id;
    const std::string blue_material_source = "Texture = \"Assets/blue.bmp\"\nTint = [0.75, 1.0, 0.9, 1.0]\n";

    if (!require(runtime.entities().empty(), "external asset smoke blank project inherited sample geometry")) {
        std::filesystem::remove_all(root, error);
        return 7;
    }

    const auto transform_type = aeg::ecs::ComponentTypeId(aeg::engine::components::kTransformName);
    const auto light_type = aeg::ecs::ComponentTypeId(aeg::engine::components::kDirectionalLightName);
    if (!require(runtime.create_scene_entity({}, "Test Light"), "external asset smoke light creation failed")) {
        std::filesystem::remove_all(root, error);
        return 7;
    }
    const auto light_id = runtime.selected_entity();
    if (!require(runtime.add_component(std::span<const aeg::scene::SceneEntityId>(&light_id, 1), light_type),
                 "external asset smoke light component creation failed")) {
        std::filesystem::remove_all(root, error);
        return 7;
    }
    aeg::engine::components::Transform left_transform{};
    left_transform.translation.x = -0.62f;
    left_transform.scale = {0.52f, 0.78f, 1.0f};
    aeg::engine::components::Transform right_transform{};
    right_transform.translation.x = 0.62f;
    right_transform.scale = {0.52f, 0.78f, 1.0f};
    const auto mesh_type = aeg::ecs::ComponentTypeId(aeg::engine::components::kMeshRendererName);
    if (!require(runtime.create_scene_entity({}, "Left Mesh"), "external asset smoke left entity creation failed")) {
        std::filesystem::remove_all(root, error);
        return 8;
    }
    const aeg::scene::SceneEntityId left_id = runtime.selected_entity();
    aeg::engine::components::MeshRenderer left_renderer{};
    left_renderer.mesh_asset_id = triangle->id;
    left_renderer.material_asset_id = red->id;
    if (!require(runtime.add_component(std::span<const aeg::scene::SceneEntityId>(&left_id, 1), mesh_type),
                 "external asset smoke left renderer creation failed") ||
        !require(runtime.queue_component_update(left_id, mesh_type, bytes_of(left_renderer)),
                 "external asset smoke left renderer update failed") ||
        !require(runtime.queue_component_update(left_id, transform_type, bytes_of(left_transform)),
                 "external asset smoke left transform update failed") ||
        !require(runtime.create_scene_entity({}, "Right Mesh"), "external asset smoke right entity creation failed")) {
        std::filesystem::remove_all(root, error);
        return 8;
    }
    const aeg::scene::SceneEntityId right_id = runtime.selected_entity();
    aeg::engine::components::MeshRenderer right_renderer{};
    right_renderer.mesh_asset_id = quad->id;
    right_renderer.material_asset_id = blue->id;
    if (!require(runtime.add_component(std::span<const aeg::scene::SceneEntityId>(&right_id, 1), mesh_type),
                 "external asset smoke right renderer creation failed") ||
        !require(runtime.queue_component_update(right_id, mesh_type, bytes_of(right_renderer)),
                 "external asset smoke right renderer update failed") ||
        !require(runtime.queue_component_update(right_id, transform_type, bytes_of(right_transform)),
                 "external asset smoke right transform update failed")) {
        std::filesystem::remove_all(root, error);
        return 8;
    }

    const auto left = std::find_if(runtime.entities().begin(), runtime.entities().end(),
                                   [](const auto& entity) { return entity.name == "Left Mesh"; });
    const auto right = std::find_if(runtime.entities().begin(), runtime.entities().end(),
                                    [](const auto& entity) { return entity.name == "Right Mesh"; });
    if (!require(left != runtime.entities().end() && right != runtime.entities().end(),
                 "external asset smoke test mesh entities are missing")) {
        std::filesystem::remove_all(root, error);
        return 8;
    }

    for (uint64_t frame = 0; frame < 8; ++frame) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        if (!require(runtime.render_frame(frame), "external asset smoke asset stabilization frame failed")) {
            std::filesystem::remove_all(root, error);
            return 9;
        }
    }
    const auto external_readback = runtime.request_viewport_readback();
    const auto external_frame = external_readback ? runtime.render_frame(8) : external_readback;
    if (!external_frame) {
        std::cerr << "external asset smoke frame diagnostic: " << external_frame.message << '\n';
    }
    if (!require(external_readback, "external asset smoke readback request failed") ||
        !require(external_frame, "external asset smoke external frame failed")) {
        std::filesystem::remove_all(root, error);
        return 9;
    }
    const auto first_pixels = runtime.read_viewport_pixels();
    if (!require(first_pixels, "external asset smoke first pixel readback failed")) {
        std::filesystem::remove_all(root, error);
        return 10;
    }
    const std::span<const std::byte> first_pixel_span(first_pixels.value.data(), first_pixels.value.size());
    const PixelStats left_pixels = stats(first_pixel_span, 128, 96, 0, 64);
    const PixelStats right_pixels = stats(first_pixel_span, 128, 96, 64, 128);
    if (left_pixels.colored <= 100u || right_pixels.colored <= 100u) {
        const auto* left_mesh = static_cast<const aeg::engine::components::MeshRenderer*>(
            runtime.component_value(left_id, mesh_type));
        const auto* right_mesh = static_cast<const aeg::engine::components::MeshRenderer*>(
            runtime.component_value(right_id, mesh_type));
        std::cerr << "external asset smoke colored pixels: left=" << left_pixels.colored
                  << " right=" << right_pixels.colored << " diagnostic=" << runtime.diagnostic()
                  << " leftMesh=" << (left_mesh == nullptr ? 0 : left_mesh->mesh_asset_id)
                  << " leftMaterial=" << (left_mesh == nullptr ? 0 : left_mesh->material_asset_id)
                  << " rightMesh=" << (right_mesh == nullptr ? 0 : right_mesh->mesh_asset_id)
                  << " rightMaterial=" << (right_mesh == nullptr ? 0 : right_mesh->material_asset_id) << '\n';
    }
    if (!require(left_pixels.colored > 100u && right_pixels.colored > 100u,
                 "external asset smoke did not render both external meshes") ||
        !require(left_pixels.red > left_pixels.blue * 2u, "external red texture did not reach the viewport") ||
        !require(right_pixels.blue > right_pixels.red * 2u, "external blue texture did not reach the viewport") ||
        !require(left_pixels.red != right_pixels.red || left_pixels.blue != right_pixels.blue,
                 "different external materials produced identical pixels")) {
        std::filesystem::remove_all(root, error);
        return 11;
    }

    if (!require(write_text(root / "Assets" / "blue.aegmat",
                            "Texture = \"Assets/does-not-exist.tga\"\nTint = [0.75, 1.0, 0.9, 1.0]\n"),
                 "external asset smoke invalid texture reference write failed") ||
        !require(runtime.reimport_asset(blue_material_id), "external asset smoke invalid material reimport failed") ||
        !require(runtime.request_viewport_readback(),
                 "external asset smoke invalid reference readback request failed") ||
        !require(runtime.render_frame(2), "external asset smoke invalid reference frame failed")) {
        std::filesystem::remove_all(root, error);
        return 29;
    }
    const auto invalid_reference_pixels = runtime.read_viewport_pixels();
    if (!require(invalid_reference_pixels, "external asset smoke invalid reference pixel readback failed")) {
        std::filesystem::remove_all(root, error);
        return 30;
    }
    const PixelStats invalid_reference_right =
        stats(std::span<const std::byte>(invalid_reference_pixels.value.data(), invalid_reference_pixels.value.size()),
              128, 96, 64, 128);
    if (!require(invalid_reference_right.blue < right_pixels.blue * 3u / 4u,
                 "invalid material texture reference did not select the fallback texture")) {
        std::filesystem::remove_all(root, error);
        return 31;
    }
    if (!require(write_text(root / "Assets" / "blue.aegmat", blue_material_source),
                 "external asset smoke material reference restore failed") ||
        !require(runtime.reimport_asset(blue_material_id), "external asset smoke material restore reimport failed")) {
        std::filesystem::remove_all(root, error);
        return 32;
    }
    bool restored_material = false;
    for (uint64_t frame = 3; frame < 19 && !restored_material; ++frame) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        if (!runtime.request_viewport_readback() || !runtime.render_frame(frame)) {
            break;
        }
        const auto pixels = runtime.read_viewport_pixels();
        restored_material = pixels && pixels.value == first_pixels.value;
    }
    if (!require(restored_material, "restored material reference did not return to the original viewport output")) {
        std::filesystem::remove_all(root, error);
        return 33;
    }

    // Exercise the silent watcher/bake path. The render thread must keep the
    // old texture until a completed bake is published, then hot-swap it.
    if (!require(write_bmp(root / "Assets" / "blue.bmp", {24, 240, 48, 255}),
                 "external asset smoke watched texture update failed")) {
        std::filesystem::remove_all(root, error);
        return 12;
    }
    std::vector<std::byte> hot_pixels;
    bool hot_reloaded = false;
    for (uint64_t frame = 6; frame < 50 && !hot_reloaded; ++frame) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        if (!runtime.request_viewport_readback() || !runtime.render_frame(frame)) {
            break;
        }
        const auto pixels = runtime.read_viewport_pixels();
        if (pixels && pixels.value != first_pixels.value) {
            hot_pixels = pixels.value;
            hot_reloaded = true;
        }
    }
    if (!require(hot_reloaded, "external asset smoke watcher did not hot-reload the texture")) {
        std::filesystem::remove_all(root, error);
        return 13;
    }
    const PixelStats hot_right =
        stats(std::span<const std::byte>(hot_pixels.data(), hot_pixels.size()), 128, 96, 64, 128);
    if (!require(hot_right.green > hot_right.blue / 2u,
                 "external asset smoke hot-reloaded texture did not reach the viewport")) {
        std::filesystem::remove_all(root, error);
        return 14;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    if (!require(write_bmp(root / "Assets" / "blue.bmp", {24, 48, 240, 255}),
                 "external asset smoke watched texture restore failed") ||
        !require(runtime.reimport_asset(blue_texture_id), "external asset smoke texture restore reimport failed")) {
        std::filesystem::remove_all(root, error);
        return 15;
    }
    bool hot_restore = false;
    std::vector<std::byte> last_restore_pixels;
    for (uint64_t frame = 50; frame < 90 && !hot_restore; ++frame) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        if (!runtime.request_viewport_readback() || !runtime.render_frame(frame)) {
            break;
        }
        const auto pixels = runtime.read_viewport_pixels();
        if (pixels) {
            last_restore_pixels = pixels.value;
        }
        hot_restore = pixels && pixels.value == first_pixels.value;
    }
    if (!hot_restore && !last_restore_pixels.empty()) {
        const PixelStats restored_right = stats(
            std::span<const std::byte>(last_restore_pixels.data(), last_restore_pixels.size()), 128, 96, 64, 128);
        std::cerr << "external asset smoke restore stats: red=" << restored_right.red
                  << " green=" << restored_right.green << " blue=" << restored_right.blue
                  << " colored=" << restored_right.colored << '\n';
    }
    if (!require(hot_restore, "external asset smoke watcher did not restore the last texture revision")) {
        std::filesystem::remove_all(root, error);
        return 16;
    }

    if (!require(runtime.save_scene(), "external asset smoke save failed") ||
        !require(runtime.open_scene(runtime.scene_path()), "external asset smoke reopen failed") ||
        !require(runtime.request_viewport_readback(), "external asset smoke reopen readback request failed") ||
        !require(runtime.render_frame(2), "external asset smoke reopened frame failed")) {
        std::filesystem::remove_all(root, error);
        return 17;
    }
    const auto reopened_pixels = runtime.read_viewport_pixels();
    if (!require(reopened_pixels, "external asset smoke reopened pixel readback failed") ||
        !require(reopened_pixels.value == first_pixels.value, "external assets changed after save and reopen")) {
        std::filesystem::remove_all(root, error);
        return 18;
    }

    const auto* triangle_source = triangle;
    if (!require(write_text(root / "Assets" / "triangle.obj", "not an OBJ anymore\n"),
                 "external asset smoke invalidation write failed") ||
        !require(runtime.reimport_asset(triangle_source->id), "external asset smoke reimport request failed") ||
        !require(runtime.request_viewport_readback(), "external asset smoke last-good readback request failed") ||
        !require(runtime.render_frame(3), "external asset smoke last-good frame failed")) {
        std::filesystem::remove_all(root, error);
        return 19;
    }
    const auto retained_pixels = runtime.read_viewport_pixels();
    if (!require(retained_pixels, "external asset smoke last-good pixel readback failed") ||
        !require(retained_pixels.value == first_pixels.value,
                 "failed external reimport released the last-good GPU mesh")) {
        std::filesystem::remove_all(root, error);
        return 20;
    }

    if (!require(runtime.play(), "external asset smoke Play failed") ||
        !require(runtime.request_viewport_readback(), "external asset smoke Play readback request failed") ||
        !require(runtime.render_frame(4), "external asset smoke Play frame failed") ||
        !require(runtime.stop(), "external asset smoke Stop failed") ||
        !require(runtime.mode() == aeg::application::EditorSceneRuntime::Mode::kEdit,
                 "external asset smoke Stop did not restore Edit mode") ||
        !require(runtime.request_viewport_readback(), "external asset smoke post-Stop readback request failed") ||
        !require(runtime.render_frame(5), "external asset smoke post-Stop frame failed")) {
        std::filesystem::remove_all(root, error);
        return 21;
    }
    const auto stopped_pixels = runtime.read_viewport_pixels();
    if (!require(stopped_pixels, "external asset smoke post-Stop pixel readback failed") ||
        !require(stopped_pixels.value == retained_pixels.value,
                 "Play/Stop changed the external asset viewport output")) {
        std::filesystem::remove_all(root, error);
        return 22;
    }

    const aeg::ecs::ComponentTypeId text_type(aeg::engine::components::kTextRendererName);
    if (!require(runtime.create_scene_entity({}, "World Text"),
                 "external asset smoke world-text entity creation failed")) {
        std::filesystem::remove_all(root, error);
        return 23;
    }
    const aeg::scene::SceneEntityId text_entity = runtime.selected_entity();
    if (!require(runtime.add_component(std::span<const aeg::scene::SceneEntityId>(&text_entity, 1), text_type),
                 "external asset smoke TextRenderer component creation failed")) {
        std::filesystem::remove_all(root, error);
        return 24;
    }
    const auto* default_text =
        static_cast<const aeg::engine::components::TextRenderer*>(runtime.component_value(text_entity, text_type));
    if (!require(default_text != nullptr, "external asset smoke TextRenderer value is missing")) {
        std::filesystem::remove_all(root, error);
        return 25;
    }
    if (!require(runtime.request_viewport_readback(), "external asset smoke text readback request failed") ||
        !require(runtime.render_frame(55), "external asset smoke text frame failed")) {
        std::filesystem::remove_all(root, error);
        return 26;
    }
    const auto text_pixels = runtime.read_viewport_pixels();
    if (!require(text_pixels, "external asset smoke text pixel readback failed") ||
        !require(text_pixels.value != stopped_pixels.value,
                 "TextRenderer did not change the world-space viewport output")) {
        std::filesystem::remove_all(root, error);
        return 27;
    }

    std::filesystem::remove_all(root, error);
    if (!require(runtime.shutdown(), "external asset smoke shutdown failed")) {
        return 34;
    }
    std::cout << "Editor external assets smoke passed\n";
    return 0;
}
