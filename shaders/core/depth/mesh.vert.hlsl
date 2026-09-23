/** @file mesh.vert.hlsl @brief Depth-only geometry using the renderer object binding. */
cbuffer OpaqueObjectConstants : register(b0, space0) {
    float4x4 model_view_projection;
};

float4 main(float4 position : POSITION) : SV_Position {
    return mul(model_view_projection, position);
}
