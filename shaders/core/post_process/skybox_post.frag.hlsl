Texture2D<float4> scene_color : register(t0, space0);
Texture2D<float> scene_depth : register(t1, space0);
cbuffer PostProcessConstants : register(b2, space0) {
    float4x4 inverse_projection;
    float4x4 inverse_view;
    float4 sky_zenith;
    float4 sky_horizon;
    float4 sky_ground;
    float4 parameters; // exposure, inverse gamma, tone mapping, sky enabled
    float4 viewport; // width, height, reversed Z
};

float4 main(float4 position : SV_Position) : SV_Target0 {
    int3 pixel = int3(int2(position.xy), 0);
    float4 color = scene_color.Load(pixel);
    float depth = scene_depth.Load(pixel);
    bool background = viewport.z != 0 ? depth <= 0 : depth >= 1;
    if (background && parameters.w != 0) {
        float2 ndc = position.xy / viewport.xy * 2.0f - 1.0f;
        float4 near_h = mul(inverse_projection, float4(ndc, viewport.z != 0 ? 1 : 0, 1));
        float4 far_h = mul(inverse_projection, float4(ndc, viewport.z != 0 ? 0 : 1, 1));
        // Homogeneous subtraction also supports an infinite far plane (far_h.w == 0).
        float3 ray = far_h.xyz * near_h.w - near_h.xyz * far_h.w;
        float3 direction = normalize(mul((float3x3)inverse_view, ray));
        color = float4(direction.y >= 0 ? lerp(sky_horizon.rgb, sky_zenith.rgb, direction.y) :
                                         lerp(sky_horizon.rgb, sky_ground.rgb, -direction.y), 1);
    }
    float3 exposed = max(color.rgb, 0.0f) * parameters.x;
    if (parameters.z != 0) exposed = 1.0f - rcp(1.0f + exposed);
    return float4(pow(exposed, parameters.y), color.a);
}
