/** @brief Shades GBuffer surfaces with the shared directional/ambient light contract. */
Texture2D<float4> gbuffer_albedo : register(t0, space0);
Texture2D<float4> gbuffer_normal : register(t1, space0);
Texture2D<float> scene_depth : register(t2, space0);

cbuffer OpaqueLightingConstants : register(b3, space0) {
    float4 directional_direction_intensity;
    float4 directional_color;
    float4 ambient_color;
};

float4 main(float4 position : SV_Position) : SV_Target0 {
    const int3 pixel = int3(int2(position.xy), 0);
    // Background is left transparent for the subsequent skybox/composition pass.
    if (scene_depth.Load(pixel) >= 1.0f) return float4(0, 0, 0, 0);
    const float4 albedo = gbuffer_albedo.Load(pixel);
    const float3 normal = normalize(gbuffer_normal.Load(pixel).xyz * 2.0f - 1.0f);
    const float directional = max(dot(normal, normalize(-directional_direction_intensity.xyz)), 0.0f);
    const float3 lighting = ambient_color.rgb +
        directional_color.rgb * directional_direction_intensity.w * directional;
    return float4(albedo.rgb * lighting, albedo.a);
}
