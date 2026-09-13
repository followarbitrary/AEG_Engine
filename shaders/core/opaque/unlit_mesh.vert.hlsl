/**
 * @file unlit_mesh.vert.hlsl
 * @brief Transforms an opaque mesh with view-local object constants.
 */
struct VertexInput {
    float4 position : POSITION;
    float2 uv : TEXCOORD0;
};

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

cbuffer OpaqueObjectConstants : register(b0, space0) {
    float4x4 model_view_projection;
};

VertexOutput main(VertexInput input) {
    VertexOutput output;
    output.position = mul(model_view_projection, input.position);
    output.uv = input.uv;
    return output;
}
