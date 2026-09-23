#include "visibility.hlsli"
Texture2D<float4> gbuffer_albedo : register(t0, space0);
Texture2D<float4> gbuffer_normal : register(t1, space0);
Texture2D<float> scene_depth : register(t2, space0);
cbuffer OpaqueLightingConstants : register(b3, space0) {
    float4 directional_direction_intensity;
    float4 directional_color;
    float4 ambient_color;
};
Texture2D<float> shadow_map : register(t4, space0);
cbuffer ShadowConstants : register(b5, space0) {
    float4x4 clip_to_shadow;
    float4 shadow_parameters;
};
float4 main(float4 position : SV_Position) : SV_Target0 {
    int3 pixel = int3(int2(position.xy), 0);
    float depth = scene_depth.Load(pixel);
    if (depth >= 1.0f) return float4(0,0,0,0);
    float4 albedo = gbuffer_albedo.Load(pixel);
    float3 normal = normalize(gbuffer_normal.Load(pixel).xyz * 2.0f - 1.0f);
    float directional = max(dot(normal, normalize(-directional_direction_intensity.xyz)), 0.0f);
    float visibility = directional_shadow(shadow_map, float4(position.xy, depth, 1), clip_to_shadow, shadow_parameters);
    float3 lighting = ambient_color.rgb + directional_color.rgb * directional_direction_intensity.w * directional * visibility;
    return float4(albedo.rgb * lighting, albedo.a);
}
