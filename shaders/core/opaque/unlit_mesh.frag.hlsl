/**
 * @file unlit_mesh.frag.hlsl
 * @brief Applies minimal Lambert lighting to the sampled opaque material.
 */
Texture2D material_color : register(t0, space1);
SamplerState material_sampler : register(s1, space1);

cbuffer TextureMaterialConstants : register(b2, space1) {
    float4 material_tint;
};

cbuffer OpaqueLightingConstants : register(b0, space2) {
    float4 directional_direction_intensity;
    float4 directional_color;
    float4 ambient_color;
};

float4 main(float2 uv : TEXCOORD0, float3 normal : NORMAL) : SV_Target0 {
    const float3 albedo = material_color.Sample(material_sampler, uv).rgb * material_tint.rgb;
    const float directional_factor = max(
        dot(normalize(normal), normalize(-directional_direction_intensity.xyz)), 0.0f);
    const float3 lighting = ambient_color.rgb +
                            directional_color.rgb * directional_direction_intensity.w * directional_factor;
    return float4(albedo * lighting, material_tint.a);
}
