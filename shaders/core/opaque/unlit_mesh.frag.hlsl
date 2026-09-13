/**
 * @file unlit_mesh.frag.hlsl
 * @brief Samples the material color texture for the opaque output.
 */
Texture2D material_color : register(t0, space1);
SamplerState material_sampler : register(s1, space1);

cbuffer TextureMaterialConstants : register(b2, space1) {
    float4 material_tint;
};

float4 main(float2 uv : TEXCOORD0) : SV_Target0 {
    return material_color.Sample(material_sampler, uv) * material_tint;
}
