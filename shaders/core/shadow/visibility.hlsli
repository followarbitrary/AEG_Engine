/** @brief Depth-comparison PCF in the directional light's fitted projection. */
float directional_shadow(Texture2D<float> map, float4 camera_position,
                         float4x4 clip_to_shadow, float4 parameters) {
    float4 clip = float4(camera_position.xy * parameters.xy * 2.0f - 1.0f, camera_position.z, 1.0f);
    float4 light_clip = mul(clip_to_shadow, clip);
    if (abs(light_clip.w) < 0.000001f) return 1.0f;
    float3 ndc = light_clip.xyz / light_clip.w;
    if (any(abs(ndc.xy) > 1.0f) || ndc.z < 0.0f || ndc.z > 1.0f) return 1.0f;
    uint width, height;
    map.GetDimensions(width, height);
    float2 uv = ndc.xy * 0.5f + 0.5f;
    float2 uv_dx = ddx(uv), uv_dy = ddy(uv);
    float depth_dx = ddx(ndc.z), depth_dy = ddy(ndc.z);
    float determinant = uv_dx.x * uv_dy.y - uv_dx.y * uv_dy.x;
    // Compare each PCF tap against the receiver plane at that tap, avoiding slope self-shadowing.
    float2 gradient = abs(determinant) > 0.00000001f ?
        float2(depth_dx * uv_dy.y - depth_dy * uv_dx.y,
               depth_dy * uv_dx.x - depth_dx * uv_dy.x) / determinant : float2(0,0);
    int2 pixel = int2(uv * float2(width, height));
    float visibility = 0.0f;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            int2 tap = pixel + int2(x, y);
            float receiver_depth = ndc.z + dot(gradient, (float2(tap) + 0.5f) / float2(width,height) - uv);
            visibility += any(tap < 0) || any(tap >= int2(width, height)) ? 1.0f :
                (receiver_depth - parameters.z <= map.Load(int3(tap, 0)) ? 1.0f : 0.0f);
        }
    }
    return visibility / 9.0f;
}
