#include <cassert>
#include <cstddef>
#include <iostream>
#include <type_traits>

#include "renderer_shader_contract.hpp"

int main() {
    using aeg::renderer::RenderLightingConstants;
    using aeg::renderer::RenderMeshVertexLayout;
    using aeg::renderer::RenderObjectConstants;
    using aeg::renderer::RenderShadowConstants;

    static_assert(std::is_standard_layout_v<RenderShadowConstants>);
    static_assert(std::is_standard_layout_v<RenderMeshVertexLayout>);
    static_assert(std::is_standard_layout_v<RenderObjectConstants>);
    static_assert(std::is_standard_layout_v<RenderLightingConstants>);
    static_assert(sizeof(RenderShadowConstants) == 80u);
    static_assert(offsetof(RenderObjectConstants, normal_matrix) == sizeof(aeg::math::Mat4));
    static_assert(sizeof(RenderObjectConstants) == sizeof(aeg::math::Mat4) * 2u);
    static_assert(sizeof(RenderLightingConstants) == sizeof(aeg::math::Vec4) * 3u);

    const RenderMeshVertexLayout layout{};
    assert(layout.position_format == aeg::rhi::Format::kRgba32Float);
    assert(layout.uv_offset == sizeof(float) * 4u);
    assert(layout.normal_offset == sizeof(float) * 6u);

    const RenderLightingConstants lighting{};
    assert(lighting.is_valid());
    std::cout << "renderer shader contract boundary smoke passed\n";
    return 0;
}
