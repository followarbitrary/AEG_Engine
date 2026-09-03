/**
 * @file textured_triangle.vert.hlsl
 * @brief Applies the test transform and forwards textured vertex attributes.
 */
struct VertexInput {
    float2 position : POSITION;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};

struct VertexOutput {
    float4 position : SV_Position;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};

cbuffer TransformConstants : register(b0, space1) {
    float4x4 model_view_projection;
};

VertexOutput main(VertexInput input) {
    VertexOutput output;
    output.position = mul(model_view_projection,
                          float4(input.position, 0.0, 1.0));
    output.color = input.color;
    output.uv = input.uv;
    return output;
}
