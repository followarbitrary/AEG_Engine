#include "visibility.hlsli"
Texture2D material_color : register(t0, space1);
SamplerState material_sampler : register(s1, space1);
cbuffer TextureMaterialConstants : register(b2, space1) { float4 material_tint; };
cbuffer OpaqueLightingConstants : register(b0, space2) {
    float4 directional_direction_intensity;
    float4 directional_color;
    float4 ambient_color;
};
Texture2D<float> shadow_map : register(t0, space3);
cbuffer ShadowConstants : register(b1, space3) {
    float4x4 clip_to_shadow;
    float4 shadow_parameters;
};
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0, float3 normal : NORMAL) : SV_Target0 {
    float4 albedo = material_color.Sample(material_sampler, uv) * material_tint;
    float directional = max(dot(normalize(normal), normalize(-directional_direction_intensity.xyz)), 0.0f);
    float visibility = directional_shadow(shadow_map, position, clip_to_shadow, shadow_parameters);
    float3 lighting = ambient_color.rgb + directional_color.rgb * directional_direction_intensity.w * directional * visibility;
    return float4(albedo.rgb * lighting, albedo.a);
}
