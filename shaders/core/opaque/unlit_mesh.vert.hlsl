/**
 * @file unlit_mesh.vert.hlsl
 * @brief Transforms an opaque mesh and forwards view-space normals and UVs.
 */
struct VertexInput {
    float4 position : POSITION;
    float2 uv : TEXCOORD0;
    float3 normal : NORMAL;
};

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float3 normal : NORMAL;
};

cbuffer OpaqueObjectConstants : register(b0, space0) {
    float4x4 model_view_projection;
    float4x4 normal_matrix;
};

VertexOutput main(VertexInput input) {
    VertexOutput output;
    output.position = mul(model_view_projection, input.position);
    output.uv = input.uv;
    output.normal = normalize(mul(normal_matrix, float4(input.normal, 0.0f)).xyz);
    return output;
}
