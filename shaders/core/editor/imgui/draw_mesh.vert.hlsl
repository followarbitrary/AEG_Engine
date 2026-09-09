/**
 * @file draw_mesh.vert.hlsl
 * @brief Transforms one Dear ImGui vertex into clip space.
 */
struct VertexInput {
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
    uint color : COLOR0;
};

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

cbuffer ImGuiConstants : register(b0, space0) {
    float4x4 projection;
};

VertexOutput main(VertexInput input) {
    VertexOutput output;
    output.position = mul(projection, float4(input.position, 0.0f, 1.0f));
    output.uv = input.uv;
    output.color = float4(
        (input.color & 0xffu) / 255.0f,
        ((input.color >> 8u) & 0xffu) / 255.0f,
        ((input.color >> 16u) & 0xffu) / 255.0f,
        ((input.color >> 24u) & 0xffu) / 255.0f);
    return output;
}
