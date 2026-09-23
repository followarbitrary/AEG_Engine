/** @brief Fullscreen triangle without vertex buffers. */
float4 main(uint vertex_id : SV_VertexID) : SV_Position {
    const float2 uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    return float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}
