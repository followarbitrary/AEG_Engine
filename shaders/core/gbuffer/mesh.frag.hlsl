/**
 * @file mesh.frag.hlsl
 * @brief Writes albedo, encoded normal, and material data to the deferred MRT.
 */
Texture2D material_color : register(t0, space1);
SamplerState material_sampler : register(s1, space1);

cbuffer GBufferMaterialConstants : register(b2, space1) {
    float4 material_tint;
};

struct GBufferOutput {
    float4 albedo : SV_Target0;
    float4 normal : SV_Target1;
    float4 material : SV_Target2;
};

GBufferOutput main(float2 uv : TEXCOORD0, float3 normal : NORMAL) {
    const float4 sampled = material_color.Sample(material_sampler, uv);
    const float3 albedo = sampled.rgb * material_tint.rgb;
    const float3 encoded_normal = normalize(normal) * 0.5f + 0.5f;

    GBufferOutput output;
    output.albedo = float4(albedo, sampled.a * material_tint.a);
    output.normal = float4(encoded_normal, 1.0f);
    output.material = float4(material_tint.a, 0.0f, 0.0f, 1.0f);
    return output;
}
