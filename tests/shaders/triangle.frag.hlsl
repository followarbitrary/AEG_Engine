Texture2D<float4> texture_image : register(t0, space0);
SamplerState texture_sampler : register(s1, space0);

struct FragmentInput {
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};

float4 main(FragmentInput input) : SV_Target0 {
    return input.color * texture_image.Sample(texture_sampler, input.uv);
}
