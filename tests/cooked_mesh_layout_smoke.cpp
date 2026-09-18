#include <cassert>
#include <cstring>
#include <iostream>

#include "mesh_payload.hpp"

int main() {
    std::vector<std::byte> payload(16 + 32 + 12, std::byte{0});
    auto write_u32 = [&](std::size_t offset, uint32_t value) {
        std::memcpy(payload.data() + offset, &value, sizeof(value));
    };
    auto write_f32 = [&](std::size_t offset, float value) {
        std::memcpy(payload.data() + offset, &value, sizeof(value));
    };
    write_u32(0, 1);
    write_u32(4, 3);
    write_u32(8, 32);
    write_u32(12, 0);
    write_f32(16, 1.0f);
    write_f32(20, 2.0f);
    write_f32(24, 3.0f);
    write_f32(28, 0.0f);
    write_f32(32, 1.0f);
    write_f32(36, 0.0f);
    write_f32(40, 0.25f);
    write_f32(44, 0.75f);
    write_u32(48, 0);
    write_u32(52, 0);
    write_u32(56, 0);
    const auto mesh = aeg::asset::decode_mesh_payload(payload);
    if (!mesh || mesh.value.positions.size() != 1 || mesh.value.indices.size() != 3)
        return 1;
    const auto vertices = aeg::application::runtime::make_renderer_vertices(mesh.value);
    if (!vertices || vertices.value.size() != 1 || vertices.value[0].position[3] != 1.0f ||
        vertices.value[0].uv[0] != 0.25f || vertices.value[0].normal[1] != 1.0f)
        return 2;
    payload[8] = std::byte{0x10};
    if (aeg::asset::decode_mesh_payload(payload))
        return 3;
    std::cout << "Cooked mesh layout smoke passed\n";
    return 0;
}
