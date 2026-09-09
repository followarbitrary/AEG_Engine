/**
 * @file draw_mesh.frag.hlsl
 * @brief Samples and modulates one Dear ImGui texture.
 */
Texture2D<float4> imgui_texture : register(t1, space0);
SamplerState imgui_sampler : register(s2, space0);

struct FragmentInput {
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

float4 main(FragmentInput input) : SV_Target0 {
    return input.color * imgui_texture.Sample(imgui_sampler, input.uv);
}
